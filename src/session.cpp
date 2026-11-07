#include "m68k/session.hpp"

#include "session_impl.hpp"

namespace m68k {

// ---- Session ------------------------------------------------------------------------------------------------

Session::Session(SessionOptions options)
    : options_(options), memory_(std::make_unique<Memory>()), cpu_(std::make_unique<Cpu>(*memory_)) {
    resetMachine(false);
}

Session::~Session() = default;

void Session::applyWatches() {
    cpu_->clearWatchpoints();
    for (const Watch& w : watches_) cpu_->addWatchpoint(w.start, w.length, w.read, w.write);
}

void Session::resetMachine(bool clearMemory) {
    if (clearMemory) hunks_.clear();
    if (clearMemory) memory_->clear();
    Registers& r = cpu_->registers();
    r = Registers{};
    r.sr = 0x2700;
    r.pc = 0;
    r.a[7] = amiga_ ? amiga_->config().chipKb * 1024U - 16 : kGenericStackTop;
    CpuState st = cpu_->state();
    st.regs = r;
    st.halted = st.stopped = false;
    st.instructions = st.exceptions = 0;
    st.lastException = 0;
    st.depth = 0;
    st.irqLevel = 0;
    st.irqVector = -1;
    cpu_->setState(st);
    if (amiga_) amiga_->reset(*cpu_);
    if (os_) {
        os_->reset();
        cpu_->registers().a[7] = os_->stackTop();
    }
}

Json Session::handle(const Json& request) {
    Impl impl(*this, request);
    try {
        if (!request.isObject()) throw SessionError("request must be a JSON object");
        const Json body = impl.run();
        Json reply = Json::object();
        reply.set("ok", true);
        for (const auto& [k, v] : body.members()) reply.set(k, v);
        return reply;
    } catch (const std::exception& e) {
        Json err = Json::object();
        err.set("ok", false).set("error", e.what());
        if (impl.detailed.isArray()) err.set("errors", impl.detailed);
        return err;
    }
}

std::string Session::handleLine(std::string_view line) {
    try {
        return handle(Json::parse(line)).dump();
    } catch (const std::exception& e) {
        Json err = Json::object();
        err.set("ok", false).set("error", e.what());
        return err.dump();
    }
}

// ---- command dispatch -----------------------------------------------------------------------------------------

Json Session::Impl::run() {
    const std::string cmd = str("cmd");
    Cpu& cpu = *s.cpu_;
    if (cmd == "help") return cmdHelp();
    if (cmd == "asm") return cmdAsm();
    if (cmd == "dis") return cmdDis();
    if (cmd == "read") return cmdRead();
    if (cmd == "write") return cmdWrite();
    if (cmd == "load_file") return cmdLoadFile();
    if (cmd == "regs") {
        Json j = Json::object();
        j.set("regs", regsJson());
        return j;
    }
    if (cmd == "set") return cmdSet();
    if (cmd == "step" || cmd == "step_over" || cmd == "run" || cmd == "run_until_return") return cmdStep();
    if (cmd == "call") return cmdCall();
    if (cmd == "track_writes" || cmd == "written" || cmd == "run_until_unpacked" || cmd == "save_memory") return cmdUnpack(cmd);
    if (cmd == "break") {
        const Address a = address("addr");
        cpu.addBreakpoint(a);
        Json j = Json::object();
        j.set("addr", hexAddr(a));
        return j;
    }
    if (cmd == "unbreak") {
        cpu.removeBreakpoint(address("addr"));
        return Json::object();
    }
    if (cmd == "breaks" || cmd == "clear_breaks") return cmdBreaks();
    if (cmd == "watch") return cmdWatch(true);
    if (cmd == "unwatch") return cmdWatch(false);
    if (cmd == "checkpoint" || cmd == "rollback" || cmd == "drop" || cmd == "checkpoints") return cmdCheckpoint(cmd);
    if (cmd == "diff") return cmdDiff();
    if (cmd == "state") return cmdState();
    if (cmd == "set_state") return cmdSetState();
    if (cmd == "save_state") {
        requireFiles("save_state");
        Snapshot snap = capture(cpu);
        writeBinaryFile(str("file"), serialize(snap));
        return Json::object();
    }
    if (cmd == "load_state") {
        requireFiles("load_state");
        const Snapshot snap = deserializeSnapshot(readBinaryFile(str("file")));
        restore(cpu, snap);
        Json j = Json::object();
        j.set("regs", regsJson());
        return j;
    }
    if (cmd == "interrupt") {
        const std::int64_t level = number(need("level"));
        if (level < 1 || level > 7) throw SessionError("interrupt level must be 1..7");
        const Json* v = arg("vector");
        cpu.interrupt(static_cast<unsigned>(level), v ? static_cast<int>(number(*v)) : -1);
        return Json::object();
    }
    if (cmd == "reset") {
        s.resetMachine(flag("memory"));
        Json j = Json::object();
        j.set("regs", regsJson());
        return j;
    }
    if (cmd == "symbols") {
        Json j = Json::object();
        j.set("symbols", symbolsJson(s.symbols_));
        return j;
    }
    if (isAnalysisCommand(cmd)) return cmdAnalysis(cmd);
    if (cmd == "machine") return cmdMachine();
    if (cmd == "io") return cmdIo();
    if (cmd == "libcalls" || cmd == "console") return cmdOs(cmd);
    if (cmd == "load_rom") {
        requireFiles("load_rom");
        if (!s.amiga_) throw SessionError("load_rom needs an amiga500 machine (use the 'machine' command first)");
        const std::vector<Byte> rom = readBinaryFile(str("file"));
        if (rom.empty()) throw SessionError("the ROM file is empty");
        s.amiga_->loadRom(rom);
        s.amiga_->reset(cpu);
        Json j = Json::object();
        j.set("size", rom.size());
        j.set("regs", regsJson());
        return j;
    }
    throw SessionError("unknown command '" + cmd + "' (see 'help')");
}

Json Session::Impl::cmdHelp() {
    static const char* names[] = {
        "help", "machine", "reset", "asm", "dis", "read", "write", "load_file", "load_rom", "regs", "set", "step", "step_over",
        "run", "run_until_return", "call", "track_writes", "written", "run_until_unpacked", "save_memory", "break", "unbreak", "breaks", "clear_breaks", "watch", "unwatch", "checkpoint",
        "rollback", "drop", "checkpoints", "diff", "state", "set_state", "save_state", "load_state", "interrupt", "io", "libcalls", "console", "symbols",
        "libcalls", "console", "analyze", "jump_tables", "data_regions", "routines", "listing", "ann_label", "ann_routine", "ann_variable", "ann_data", "ann_comment", "ann_remove",
        "ann_title", "ann_list", "ann_save", "ann_load"};
    Json arr = Json::array();
    for (const char* n : names) arr.push(n);
    Json j = Json::object();
    j.set("commands", arr);
    return j;
}

Json Session::Impl::cmdMachine() {
    const std::string type = arg("type") ? str("type") : "generic";
    if (type == "generic") {
        s.os_.reset();
        s.amiga_.reset();
        s.cpu_->readOverride = nullptr;
        s.cpu_->writeOverride = nullptr;
    } else if (type == "amiga500") {
        AmigaConfig c;
        if (const Json* j = arg("chip_kb")) c.chipKb = static_cast<unsigned>(number(*j));
        if (const Json* j = arg("slow_kb")) c.slowKb = static_cast<unsigned>(number(*j));
        if (const Json* j = arg("fast_kb")) c.fastKb = static_cast<unsigned>(number(*j));
        if (c.chipKb < 64 || c.chipKb > 2048) throw SessionError("chip_kb must be 64..2048");
        if (c.slowKb > 1792 || c.fastKb > 8192) throw SessionError("slow_kb must be <= 1792, fast_kb <= 8192");
        s.os_.reset();
        s.cpu_->executeHook = nullptr;
        s.amiga_ = std::make_unique<Amiga500>(*s.memory_, c);
        s.amiga_->setMaxLog(s.options_.maxIoLog);
        s.amiga_->install(*s.cpu_);
        if (flag("os")) {
            s.os_ = std::make_unique<AmigaOs>(*s.memory_, *s.amiga_);
            if (!s.os_->supported()) {
                s.os_.reset();
                throw SessionError("os needs chip_kb >= 512");
            }
            s.os_->install(*s.cpu_);
        }
    } else {
        throw SessionError("unknown machine type '" + type + "' (generic, amiga500)");
    }
    s.symbols_.clear();
    s.checkpoints_.clear();
    s.ann_.clear();
    s.analysis_.reset();
    s.analysisView_.reset();
    s.analysisEntries_.clear();
    s.programRange_.reset();
    s.resetMachine(true);
    Json j = Json::object();
    j.set("machine", type);
    if (s.amiga_) {
        j.set("chip_kb", s.amiga_->config().chipKb);
        j.set("slow_kb", s.amiga_->config().slowKb);
        j.set("fast_kb", s.amiga_->config().fastKb);
        j.set("os", s.os_ != nullptr);
    }
    j.set("regs", regsJson());
    return j;
}

Json Session::Impl::cmdAsm() {
    const std::string source = str("source");
    AssemblerOptions opt;
    opt.origin = address("origin", 0x1000);
    opt.predefined = s.symbols_;
    opt.optimize = flag("optimize");
    const AssemblyResult r = assemble(source, opt);
    if (r.ok() && r.hunk)
        throw SessionError("a program with SECTIONs is relocatable: assemble it to a hunk file with 'm68ktool asm --hunk' and load it with load_file");
    if (!r.ok()) {
        std::vector<std::string> lines;
        std::istringstream in(source);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
        detailed = Json::array();
        for (const Diagnostic& d : r.errors) {
            Json e = Json::object();
            e.set("line", d.line).set("message", d.message);
            if (d.line >= 1 && static_cast<std::size_t>(d.line) <= lines.size()) e.set("source", lines[static_cast<size_t>(d.line - 1)]);
            detailed.push(e);
        }
        throw SessionError("assembly failed: " + r.errors[0].message + " (line " + std::to_string(r.errors[0].line) + ")");
    }
    // check first, write afterwards: a failed asm must not change anything
    for (const Segment& seg : r.segments)
        for (std::size_t i = 0; i < seg.bytes.size(); ++i)
            if (!writable(static_cast<Address>(seg.address + i)))
                throw SessionError("address " + hexAddr(static_cast<Address>(seg.address + i)) + " is not writable (ROM or unmapped)");
    std::size_t total = 0;
    Json segs = Json::array();
    for (const Segment& seg : r.segments) {
        if (seg.bytes.empty()) continue;
        writeBytes(seg.address, seg.bytes);
        total += seg.bytes.size();
        Json sj = Json::object();
        sj.set("start", hexAddr(seg.address)).set("size", seg.bytes.size());
        segs.push(sj);
    }
    for (const auto& [k, v] : r.symbols) s.symbols_[k] = v;
    Json j = Json::object();
    j.set("size", total);
    if (total) {
        Address hi = 0;
        for (const Segment& seg : r.segments)
            if (!seg.bytes.empty()) hi = std::max(hi, static_cast<Address>(seg.address + seg.bytes.size() - 1));
        j.set("start", hexAddr(r.lowest())).set("end", hexAddr(hi));
    }
    if (total) {
        Address hi = 0;
        for (const Segment& seg : r.segments)
            if (!seg.bytes.empty()) hi = std::max(hi, static_cast<Address>(seg.address + seg.bytes.size()));
        rememberRange(r.lowest(), hi);
    }
    j.set("segments", segs);
    j.set("symbols", symbolsJson(r.symbols));
    if (const Json* e = arg("entry")) {
        const Address a = address(*e);
        s.cpu_->registers().pc = a;
        if (s.os_) s.os_->prepareProgram(*s.cpu_);  // started like AmigaDOS starts a program
        j.set("entry", hexAddr(a));
    }
    return j;
}

Json Session::Impl::cmdDis() {
    Address a = address("addr", s.cpu_->registers().pc);
    const std::int64_t count = arg("count") ? number(need("count")) : 8;
    if (count < 1 || count > 1000) throw SessionError("count must be 1..1000");
    // decode from a scratch copy of what the processor sees (ROM, overlay)
    Memory view;
    const std::vector<Byte> raw = readBytes(a, static_cast<std::size_t>(count) * 10 + 10);
    view.load(a, raw);
    DecodeOptions opt;
    opt.symbolizer = [this](Address x) { return symbolAt(x); };
    Json lines = Json::array();
    for (std::int64_t i = 0; i < count; ++i) {
        const Instruction in = decode(view, a, opt);
        Json l = Json::object();
        l.set("addr", hexAddr(a));
        const std::string label = symbolAt(a);
        if (!label.empty()) l.set("label", label);
        l.set("bytes", bytesToHex(in.bytes));
        l.set("text", in.text());
        if (in.target) l.set("target", hexAddr(*in.target));
        lines.push(l);
        a = (a + static_cast<Address>(in.length())) & kAddressMask;
    }
    Json j = Json::object();
    j.set("lines", lines);
    return j;
}

Json Session::Impl::cmdRead() {
    const Address a = address("addr");
    const std::int64_t len = arg("len") ? number(need("len")) : 16;
    if (len < 1 || static_cast<std::size_t>(len) > kMaxRead) throw SessionError("len must be 1.." + std::to_string(kMaxRead));
    const std::vector<Byte> b = readBytes(a, static_cast<std::size_t>(len));
    std::string ascii;
    for (const Byte x : b) ascii += (x >= 32 && x < 127) ? static_cast<char>(x) : '.';
    Json j = Json::object();
    j.set("addr", hexAddr(a)).set("hex", bytesToHex(b)).set("ascii", ascii);
    return j;
}

Json Session::Impl::cmdWrite() {
    const Address a = address("addr");
    std::vector<Byte> b;
    if (const Json* h = arg("hex")) {
        std::string t;
        for (const char c : h->asString())
            if (!std::isspace(static_cast<unsigned char>(c))) t += c;
        if (t.size() % 2) throw SessionError("hex must have an even number of digits");
        for (std::size_t i = 0; i < t.size(); i += 2) {
            const std::string pair = t.substr(i, 2);
            if (!std::isxdigit(static_cast<unsigned char>(pair[0])) || !std::isxdigit(static_cast<unsigned char>(pair[1])))
                throw SessionError("bad hexadecimal digit");
            b.push_back(static_cast<Byte>(std::stoi(pair, nullptr, 16)));
        }
    } else if (const Json* arr = arg("bytes")) {
        if (!arr->isArray()) throw SessionError("'bytes' must be an array");
        for (const Json& x : arr->items()) b.push_back(static_cast<Byte>(number(x)));
    } else {
        throw SessionError("write needs 'hex' or 'bytes'");
    }
    if (b.size() > kMaxWrite) throw SessionError("too many bytes");
    writeBytes(a, b);
    Json j = Json::object();
    j.set("addr", hexAddr(a)).set("size", b.size());
    return j;
}

Json Session::Impl::cmdLoadFile() {
    requireFiles("load_file");
    const std::string file = str("file");
    std::string type = arg("type") ? str("type") : "auto";
    const std::vector<Byte> bytes = readBinaryFile(file);
    if (looksLikeHunkObject(bytes) && type != "raw") throw SessionError(kHunkObjectMessage);
    if (type == "auto") type = looksLikeSRecordFile(file) ? "srec" : looksLikeHunkFile(bytes) ? "hunk" : "raw";
    Json j = Json::object();
    Registers& r = s.cpu_->registers();
    if (type == "raw") {
        if (!arg("addr")) throw SessionError("a raw file needs 'addr'");
        const Address a = address("addr");
        writeBytes(a, bytes);
        rememberRange(a, static_cast<Address>(a + bytes.size()));
        j.set("addr", hexAddr(a)).set("size", bytes.size());
    } else if (type == "srec") {
        const Image img = parseSRecords(std::string(bytes.begin(), bytes.end()));
        std::size_t total = 0;
        Address lo = 0xFFFFFFFF, hi = 0;
        for (const Segment& seg : img.segments) {
            writeBytes(seg.address, seg.bytes);
            total += seg.bytes.size();
            if (!seg.bytes.empty()) {
                lo = std::min(lo, seg.address);
                hi = std::max(hi, static_cast<Address>(seg.address + seg.bytes.size()));
            }
        }
        if (total) rememberRange(lo, hi);
        j.set("size", total);
        if (img.entry) {
            r.pc = *img.entry;
            j.set("entry", hexAddr(*img.entry));
        }
    } else if (type == "hunk") {
        if (!looksLikeHunkFile(bytes)) throw SessionError("not an AmigaDOS hunk file");
        const Address base = address("addr", kDefaultHunkBase);
        const HunkImage img = loadHunkFile(bytes, base);
        for (const Segment& seg : img.segments) writeBytes(seg.address, seg.bytes);
        if (!img.hunks.empty()) rememberRange(img.hunks.front().address, img.end);
        s.hunks_ = img.hunks;
        Json hunks = Json::array();
        for (const HunkInfo& h : img.hunks) {
            Json hj = Json::object();
            hj.set("type", h.type).set("addr", hexAddr(h.address)).set("size", h.size);
            hunks.push(hj);
        }
        std::map<std::string, std::int64_t> syms;
        for (const HunkSymbol& sym : img.symbols) {
            std::string name = asmSymbolName(sym.name);
            if (syms.count(name) || s.symbols_.count(name)) name += "_" + hexDigits(sym.address, 6);
            syms[name] = sym.address;
            s.symbols_[name] = sym.address;
        }
        // a program starts like AmigaDOS would start it: no arguments, stack at the top of chip RAM
        r.pc = img.entry;
        r.d[0] = 0;
        r.a[0] = 0;
        r.a[7] = s.amiga_ ? s.amiga_->config().chipKb * 1024U - 16 : kGenericStackTop;
        if (s.os_) {
            if (img.end > s.os_->heapBase()) throw SessionError("the program reaches into the heap of the emulated OS (above " + hexAddr(s.os_->heapBase()) + "); load it lower with 'addr'");
            s.os_->prepareProgram(*s.cpu_);
        }
        j.set("hunks", hunks).set("entry", hexAddr(img.entry)).set("end", hexAddr(img.end));
        j.set("symbols", symbolsJson(syms));
    } else {
        throw SessionError("unknown type '" + type + "' (auto, raw, srec, hunk)");
    }
    j.set("regs", regsJson());
    return j;
}

Json Session::Impl::cmdSet() {
    const Json& regs = need("regs");
    if (!regs.isObject()) throw SessionError("'regs' must be an object");
    for (const auto& [name, v] : regs.members()) setRegister(name, number(v));
    Json j = Json::object();
    j.set("regs", regsJson());
    return j;
}

// ---- self-extracting programs: which memory did the program write, and where does it jump into it ------------------------------------------------------
Json Session::Impl::cmdUnpack(const std::string& cmd) {
    Memory& mem = *s.memory_;
    Cpu& cpu = *s.cpu_;
    auto rangesJson = [&](std::size_t minLength, std::size_t most) {
        auto ranges = mem.writtenRanges(minLength);
        std::stable_sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.second - a.first > b.second - b.first; });
        Json arr = Json::array();
        for (std::size_t i = 0; i < ranges.size() && i < most; ++i) {
            Json e = Json::object();
            e.set("start", hexAddr(ranges[i].first)).set("end", hexAddr(ranges[i].second)).set("size", static_cast<std::int64_t>(ranges[i].second - ranges[i].first));
            arr.push(e);
        }
        return arr;
    };
    if (cmd == "track_writes") {
        const bool on = flag("on", true);
        if (on && (flag("clear", true) || !mem.tracking())) mem.trackWrites(true);
        if (!on) mem.trackWrites(false);
        s.unpackIgnore_.clear();
        s.lastUnpackHit_.reset();
        Json j = Json::object();
        j.set("tracking", mem.tracking());
        return j;
    }
    if (cmd == "written") {
        Json j = Json::object();
        j.set("tracking", mem.tracking()).set("bytes", static_cast<std::int64_t>(mem.writtenBytes()));
        const std::int64_t minLength = arg("min_length") ? number(need("min_length")) : 16;
        const std::int64_t most = arg("limit") ? number(need("limit")) : 20;
        j.set("ranges", rangesJson(static_cast<std::size_t>(std::max<std::int64_t>(1, minLength)), static_cast<std::size_t>(std::clamp<std::int64_t>(most, 1, 200))));
        return j;
    }
    if (cmd == "save_memory") {
        requireFiles("save_memory");
        const Address a = address("addr");
        const std::int64_t len = number(need("len"));
        if (len < 1 || len > 0x1000000) throw SessionError("len must be 1..16777216");
        const std::string file = str("file");
        writeBinaryFile(file, mem.readBytes(a, static_cast<std::size_t>(len)));
        Json j = Json::object();
        j.set("file", file).set("addr", hexAddr(a)).set("bytes", len);
        return j;
    }
    // run_until_unpacked
    const std::int64_t minWritten = arg("min_written") ? number(need("min_written")) : 4096;
    if (minWritten < 1) throw SessionError("min_written must be at least 1");
    if (!flag("continue") || !mem.tracking()) {
        mem.trackWrites(true);
        s.unpackIgnore_.clear();
        s.lastUnpackHit_.reset();
    } else if (s.lastUnpackHit_) {
        s.unpackIgnore_.push_back(*s.lastUnpackHit_);   // the stage that was seen last does not count again
    }
    std::vector<std::pair<Address, Address>> ignore = s.unpackIgnore_;
    if (s.os_) ignore.push_back(s.os_->systemRange());
    if (const Json* extra = arg("ignore")) {
        if (!extra->isArray()) throw SessionError("ignore must be a list of [first, end] pairs");
        for (const Json& pair : extra->items()) {
            if (!pair.isArray() || pair.items().size() != 2) throw SessionError("ignore must be a list of [first, end] pairs");
            ignore.emplace_back(address(pair.items()[0]), address(pair.items()[1]));
        }
    }
    const std::uint64_t n = arg("max_instructions") ? limit() : std::uint64_t{50000000};
    const std::uint64_t exBefore = cpu.exceptionCount();
    s.applyWatches();
    EmptyFetchGuard guard(*this);
    cpu.watchWrittenCode(true, static_cast<std::size_t>(minWritten), ignore);
    const RunResult r = cpu.run(n);
    cpu.watchWrittenCode(false);
    Json j = runReply(r, exBefore);
    guard.report(j);
    if (r.reason == StopReason::WrittenCode) {
        const Address pc = cpu.registers().pc & kAddressMask;
        for (const auto& [first, end] : mem.writtenRanges(1))
            if (pc >= first && pc < end) {
                s.lastUnpackHit_ = std::make_pair(first, end);
                Json region = Json::object();
                region.set("start", hexAddr(first)).set("end", hexAddr(end)).set("size", static_cast<std::int64_t>(end - first));
                j.set("region", region);
                break;
            }
    }
    j.set("written_bytes", static_cast<std::int64_t>(mem.writtenBytes()));
    j.set("ranges", rangesJson(64, 8));
    return j;
}

Json Session::Impl::cmdStep() {
    Cpu& cpu = *s.cpu_;
    const std::string cmd = str("cmd");
    const std::uint64_t exBefore = cpu.exceptionCount();
    s.applyWatches();
    EmptyFetchGuard guard(*this);
    if (cmd == "step") {
        const std::int64_t n = arg("n") ? number(need("n")) : 1;
        if (n < 1) throw SessionError("n must be at least 1");
        if (n > kMaxLimit) throw SessionError("n must be at most " + std::to_string(kMaxLimit));
        RunResult total;
        total.reason = StopReason::None;
        Json trace = Json::array();
        const bool wantTrace = flag("trace");
        DecodeOptions opt;
        opt.symbolizer = [this](Address x) { return symbolAt(x); };
        for (std::int64_t i = 0; i < n; ++i) {
            if (wantTrace && i < 1000) {
                Memory view;
                const Address pc = cpu.registers().pc;
                view.load(pc, readBytes(pc, 12));
                const Instruction in = decode(view, pc, opt);
                Json t = Json::object();
                t.set("addr", hexAddr(pc)).set("bytes", bytesToHex(in.bytes)).set("text", in.text());
                trace.push(t);
            }
            const StepResult sr = cpu.step();
            if (sr.reason == StopReason::Halted || sr.reason == StopReason::Stopped) {
                total.reason = sr.reason;
                break;
            }
            ++total.instructions;
        }
        Json j = runReply(total, exBefore);
        if (wantTrace) j.set("trace", trace);
        guard.report(j);
        return j;
    }
    Json j;
    if (cmd == "step_over") j = runReply(cpu.stepOver(limit()), exBefore);
    else if (cmd == "run_until_return") j = runReply(cpu.runUntilReturn(limit()), exBefore);
    else if (const Json* u = arg("until")) j = runReply(cpu.runUntil(address(*u), limit()), exBefore);
    else j = runReply(cpu.run(limit()), exBefore);
    guard.report(j);
    return j;
}

Session::Impl::EmptyFetchGuard::EmptyFetchGuard(Impl& i) : impl(i) {
    if (!impl.s.amiga_) return;
    previous = impl.s.cpu_->traceHook;
    const Amiga500* amiga = impl.s.amiga_.get();
    impl.s.cpu_->traceHook = [this, amiga](const Registers& r, Address pc, Word word) {
        if (previous) previous(r, pc, word);
        const Amiga500::Region region = amiga->classify(pc);
        if (region == Amiga500::Region::Unmapped || (region == Amiga500::Region::Rom && !amiga->hasRom())) {
            if (count == 0) first = pc;
            ++count;
        }
    };
}

Session::Impl::EmptyFetchGuard::~EmptyFetchGuard() {
    if (impl.s.amiga_) impl.s.cpu_->traceHook = previous;
}

void Session::Impl::EmptyFetchGuard::report(Json& j) const {
    if (count == 0) return;
    j.set("fetched_from_empty_memory", count).set("first_empty_fetch", hexAddr(first));
}

Json Session::Impl::cmdCall() {
    Cpu& cpu = *s.cpu_;
    const Address entry = address("addr");
    const Address ret = address("return_address", kReturnSentinel);
    if (const Json* regs = arg("regs")) {
        if (!regs->isObject()) throw SessionError("'regs' must be an object");
        for (const auto& [name, v] : regs->members()) setRegister(name, number(v));
    }
    s.applyWatches();
    wake();
    const std::uint64_t exBefore = cpu.exceptionCount();
    const Address spBefore = cpu.registers().a[7];
    EmptyFetchGuard guard(*this);
    const RunResult r = cpu.call(entry, ret, limit());
    Json j = runReply(r, exBefore);
    const bool atSentinel = r.reason == StopReason::Reached && cpu.registers().pc == ret;
    // Reaching the return address is not a return: a jump into empty memory (JSR through an uninitialised pointer) runs through zero words and
    // can come across it. A real return leaves the stack pointer where it was before the call.
    const bool returned = atSentinel && cpu.registers().a[7] == spBefore;
    j.set("returned", returned);
    if (returned) j.set("stop", "returned");
    else if (atSentinel)
        j.set("note", "the PC reached the return address but the routine did not return: A7 is " + hexAddr(cpu.registers().a[7]) + ", after a return it would be " +
                          hexAddr(spBefore) + " (a jump into empty memory, e.g. JSR through a pointer or library base that is still 0?)");
    guard.report(j);
    return j;
}

Json Session::Impl::cmdBreaks() {
    Cpu& cpu = *s.cpu_;
    if (str("cmd") == "clear_breaks") {
        cpu.clearBreakpoints();
        s.watches_.clear();
        s.applyWatches();
        return Json::object();
    }
    Json b = Json::array();
    for (const Address a : cpu.breakpoints()) b.push(hexAddr(a));
    Json w = Json::array();
    for (const Watch& x : s.watches_) {
        Json wj = Json::object();
        wj.set("addr", hexAddr(x.start)).set("len", x.length).set("read", x.read).set("write", x.write);
        w.push(wj);
    }
    Json j = Json::object();
    j.set("breakpoints", b).set("watchpoints", w);
    return j;
}

Json Session::Impl::cmdWatch(bool add) {
    const Address a = address("addr");
    if (!add) {
        s.watches_.erase(std::remove_if(s.watches_.begin(), s.watches_.end(), [&](const Watch& w) { return w.start == a; }),
                         s.watches_.end());
        s.applyWatches();
        return Json::object();
    }
    const std::string kind = arg("kind") ? str("kind") : "write";
    if (kind != "read" && kind != "write" && kind != "access") throw SessionError("kind must be read, write or access");
    const std::int64_t len = arg("len") ? number(need("len")) : 1;
    if (len < 1 || len > 0x1000000) throw SessionError("bad len");
    s.watches_.push_back({a, static_cast<Address>(len), kind != "write", kind != "read"});
    s.applyWatches();
    Json j = Json::object();
    j.set("addr", hexAddr(a));
    return j;
}

Json Session::Impl::cmdCheckpoint(const std::string& cmd) {
    Json j = Json::object();
    if (cmd == "checkpoints") {
        Json names = Json::array();
        for (const auto& [k, v] : s.checkpoints_) names.push(k);
        j.set("checkpoints", names);
        return j;
    }
    const std::string name = str("name");
    if (cmd == "checkpoint") {
        Checkpoint c;
        c.machine = capture(*s.cpu_);
        if (s.amiga_) c.amiga = s.amiga_->state();
        if (s.os_) c.os = s.os_->state();
        s.checkpoints_[name] = std::move(c);
        return j;
    }
    const auto it = s.checkpoints_.find(name);
    if (it == s.checkpoints_.end()) throw SessionError("no checkpoint named '" + name + "'");
    if (cmd == "drop") {
        s.checkpoints_.erase(it);
        return j;
    }
    restore(*s.cpu_, it->second.machine);  // rollback
    if (s.amiga_ && it->second.amiga) s.amiga_->setState(*it->second.amiga);
    if (s.os_ && it->second.os) s.os_->setState(*it->second.os);
    j.set("regs", regsJson());
    return j;
}

Json Session::Impl::cmdDiff() {
    const auto it = s.checkpoints_.find(str("name"));
    if (it == s.checkpoints_.end()) throw SessionError("no checkpoint named '" + str("name") + "'");
    std::size_t total = 0;
    const std::vector<MemoryChange> changes = diffMemory(it->second.machine.memory, s.cpu_->memory(), 8, 1000, &total);
    Json ranges = Json::array();
    for (std::size_t i = 0; i < changes.size() && i < 50; ++i) {
        const MemoryChange& c = changes[i];
        const std::size_t shown = std::min<std::size_t>(c.before.size(), 64);
        const std::vector<Byte> before(c.before.begin(), c.before.begin() + static_cast<std::ptrdiff_t>(shown));
        const std::vector<Byte> after(c.after.begin(), c.after.begin() + static_cast<std::ptrdiff_t>(shown));
        Json r = Json::object();
        r.set("addr", hexAddr(c.address)).set("len", c.before.size()).set("before", bytesToHex(before)).set("after", bytesToHex(after));
        ranges.push(r);
    }
    Json j = Json::object();
    j.set("changed_bytes", total).set("ranges", ranges);
    if (changes.size() > 50) j.set("more_ranges", changes.size() - 50);
    return j;
}

Json Session::Impl::cmdState() {
    Json j = Json::object();
    j.set("state", regsJson());
    return j;
}

Json Session::Impl::cmdSetState() {
    const Json& st = need("state");
    if (!st.isObject()) throw SessionError("'state' must be an object");
    // registers first (SR before the stack pointers, so that USP/SSP land in the right slot)
    if (const Json* sr = st.find("SR")) setRegister("SR", number(*sr));
    for (const auto& [name, v] : st.members()) {
        const std::string n = upper(name);
        const bool reg = (n.size() == 2 && (n[0] == 'D' || n[0] == 'A') && n[1] >= '0' && n[1] <= '7') || n == "PC" ||
                         n == "USP" || n == "SSP";
        if (reg) setRegister(name, number(v));
    }
    return Json::object();
}

Json Session::Impl::cmdOs(const std::string& cmd) {
    Json j = Json::object();
    if (!s.os_) {
        j.set("note", "no emulated OS: use machine type amiga500 with os:true");
        if (cmd == "libcalls") j.set("calls", Json::array());
        else j.set("text", "");
        return j;
    }
    if (cmd == "console") {
        j.set("text", s.os_->console());
        if (flag("clear", true)) s.os_->clearConsole();
        return j;
    }
    Json arr = Json::array();
    for (const LibCall& c : s.os_->log()) {
        Json e = Json::object();
        e.set("library", c.library).set("offset", c.offset);
        if (!c.function.empty()) e.set("function", c.function);
        if (c.minVersion) e.set("min_version", c.minVersion);
        e.set("emulated", c.emulated).set("caller", hexAddr(c.caller));
        if (c.callSite) e.set("call_site", hexAddr(c.callSite));
        Json args = Json::object();
        args.set("D0", hex32(c.d0)).set("D1", hex32(c.d1)).set("D2", hex32(c.d2)).set("D3", hex32(c.d3));
        args.set("A0", hex32(c.a0)).set("A1", hex32(c.a1));
        e.set("args", args).set("result", hex32(c.result));
        if (!c.note.empty()) e.set("note", c.note);
        arr.push(e);
    }
    j.set("calls", arr);
    if (flag("clear", true)) s.os_->clearLog();
    return j;
}

Json Session::Impl::cmdIo() {
    Json arr = Json::array();
    Json j = Json::object();
    if (!s.amiga_) {
        j.set("output", arr);
        j.set("note", "the generic machine has no devices; use machine type amiga500");
        return j;
    }
    for (const DeviceWrite& w : s.amiga_->log()) {
        Json e = Json::object();
        e.set("addr", hexAddr(w.address)).set("size", static_cast<int>(w.size)).set("value", hex(w.value, static_cast<int>(w.size) * 2));
        e.set("pc", hexAddr(w.pc));
        e.set("kind", w.kind == DeviceWrite::Kind::Custom ? "custom" : w.kind == DeviceWrite::Kind::Cia ? "cia"
                       : w.kind == DeviceWrite::Kind::Rom ? "rom" : "unmapped");
        const std::string name = Amiga500::customRegisterName(w.address);
        if (!name.empty()) e.set("what", name);
        arr.push(e);
    }
    if (flag("clear", true)) s.amiga_->clearLog();
    j.set("output", arr);
    j.set("overlay", s.amiga_->overlay());
    return j;
}

}  // namespace m68k
