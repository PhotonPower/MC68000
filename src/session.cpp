#include "m68k/session.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <sstream>

#include "m68k/assembler.hpp"
#include "m68k/decoder.hpp"
#include "m68k/expression.hpp"
#include "m68k/format.hpp"
#include "m68k/hunk.hpp"
#include "m68k/image_io.hpp"

namespace m68k {

namespace {

constexpr std::uint64_t kDefaultLimit = 1000000;
constexpr Address kGenericStackTop = 0x80000;
constexpr Address kDefaultHunkBase = 0x20000;
constexpr Address kReturnSentinel = 0xFFFFF0;
constexpr std::size_t kMaxRead = 4096;
constexpr std::size_t kMaxWrite = 65536;

struct SessionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string hex(std::uint64_t v, int digits) {
    if (digits < 1) digits = 1;
    if (digits > 16) digits = 16;
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%0*llX", digits, static_cast<unsigned long long>(v));
    return buf;
}
std::string hex32(Long v) { return hex(v, 8); }
std::string hexAddr(Address a) { return hex(a & kAddressMask, 6); }

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string bytesToHex(const std::vector<Byte>& b) {
    std::string s;
    for (const Byte x : b) s += hexDigits(x, 2);
    return s;
}

const char* stopName(StopReason r) {
    switch (r) {
        case StopReason::None: return "stepped";
        case StopReason::Limit: return "instruction_limit";
        case StopReason::Breakpoint: return "breakpoint";
        case StopReason::Watchpoint: return "watchpoint";
        case StopReason::Reached: return "address";
        case StopReason::Returned: return "returned";
        case StopReason::Stopped: return "stopped";
        case StopReason::Halted: return "halted";
    }
    return "?";
}

}  // namespace

// ---- the per-request worker ---------------------------------------------------------------------------

struct Session::Impl {
    Session& s;
    const Json& req;
    Json detailed;  // "errors" array for asm

    Impl(Session& session, const Json& r) : s(session), req(r) {}

    // ---- argument helpers ----
    const Json* arg(const char* name) const { return req.find(name); }
    const Json& need(const char* name) const {
        const Json* j = req.find(name);
        if (!j) throw SessionError(std::string("missing argument '") + name + "'");
        return *j;
    }
    std::string str(const char* name) const {
        const Json& j = need(name);
        if (!j.isString()) throw SessionError(std::string("argument '") + name + "' must be a string");
        return j.asString();
    }

    bool lookup(const std::string& name, std::int64_t& v) const {
        const std::string u = upper(name);
        const Registers& r = s.cpu_->registers();
        if (u.size() == 2 && (u[0] == 'D' || u[0] == 'A') && u[1] >= '0' && u[1] <= '7') {
            v = (u[0] == 'D' ? r.d : r.a)[static_cast<size_t>(u[1] - '0')];
            return true;
        }
        if (u == "SP") { v = r.a[7]; return true; }
        if (u == "PC") { v = r.pc; return true; }
        if (u == "SR") { v = r.sr; return true; }
        if (u == "USP") { v = r.usp(); return true; }
        if (u == "SSP") { v = r.ssp(); return true; }
        if (auto it = s.symbols_.find(name); it != s.symbols_.end()) {
            v = it->second;
            return true;
        }
        return false;
    }
    std::int64_t number(const Json& j) const {
        if (j.isNumber()) return j.asInt();
        if (!j.isString()) throw SessionError("expected a number or an expression string");
        const ExpressionEvaluator ev([this](const std::string& n, std::int64_t& v) { return lookup(n, v); },
                                     s.cpu_->registers().pc);
        const ExprResult r = ev.evaluate(j.asString());
        if (!r.ok) throw SessionError("bad expression '" + j.asString() + "': " + r.error);
        if (r.undefined) throw SessionError("unknown symbol '" + r.undefinedName + "'");
        return r.value;
    }
    Address address(const Json& j) const { return static_cast<Address>(number(j)) & kAddressMask; }
    Address address(const char* name) const { return address(need(name)); }
    Address address(const char* name, Address fallback) const {
        const Json* j = arg(name);
        return j ? address(*j) : fallback;
    }
    std::uint64_t limit() const {
        const Json* j = arg("max_instructions");
        if (!j) return kDefaultLimit;
        const std::int64_t n = number(*j);
        if (n < 0) throw SessionError("max_instructions must not be negative");
        return static_cast<std::uint64_t>(n);
    }
    bool flag(const char* name, bool fallback = false) const {
        const Json* j = arg(name);
        return j && j->isBool() ? j->asBool() : fallback;
    }
    void requireFiles(const char* cmd) const {
        if (!s.options_.allowFiles) throw SessionError(std::string("'") + cmd + "' needs file access: start the server with --allow-files");
    }

    // ---- memory as the processor sees it ----
    Byte readByte(Address a) const { return s.amiga_ ? s.amiga_->readByte(a) : s.memory_->read8(a); }
    std::vector<Byte> readBytes(Address a, std::size_t n) const {
        std::vector<Byte> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) out.push_back(readByte(static_cast<Address>(a + i)));
        return out;
    }
    // Plain RAM and device registers are writable; ROM and unmapped addresses are not.
    bool writable(Address a) const {
        if (!s.amiga_) return true;
        switch (s.amiga_->classify(a)) {
            case Amiga500::Region::Rom:
            case Amiga500::Region::Unmapped: return false;
            default: return true;
        }
    }
    void writeBytes(Address a, const std::vector<Byte>& b) {
        for (std::size_t i = 0; i < b.size(); ++i)
            if (!writable(static_cast<Address>(a + i)))
                throw SessionError("address " + hexAddr(static_cast<Address>(a + i)) + " is not writable (ROM or unmapped)");
        for (std::size_t i = 0; i < b.size(); ++i) {
            const Address at = static_cast<Address>(a + i) & kAddressMask;
            if (s.amiga_) s.amiga_->writeByte(at, b[i]);
            else s.memory_->write8(at, b[i]);
        }
    }

    // ---- registers ----
    // Setting the PC (or starting a call) takes the processor out of STOP and of the halted state.
    void wake() {
        CpuState st = s.cpu_->state();
        st.stopped = false;
        st.halted = false;
        s.cpu_->setState(st);
    }
    Json regsJson() const {
        const Registers& r = s.cpu_->registers();
        Json j = Json::object();
        for (int i = 0; i < 8; ++i) j.set("D" + std::to_string(i), hex32(r.d[static_cast<size_t>(i)]));
        for (int i = 0; i < 8; ++i) j.set("A" + std::to_string(i), hex32(r.a[static_cast<size_t>(i)]));
        j.set("PC", hexAddr(r.pc));
        j.set("SR", hex(r.sr, 4));
        j.set("USP", hex32(r.usp()));
        j.set("SSP", hex32(r.ssp()));
        std::string f;
        f += (r.sr & kFlagX) ? 'X' : '.';
        f += (r.sr & kFlagN) ? 'N' : '.';
        f += (r.sr & kFlagZ) ? 'Z' : '.';
        f += (r.sr & kFlagV) ? 'V' : '.';
        f += (r.sr & kFlagC) ? 'C' : '.';
        j.set("flags", f);
        j.set("S", r.supervisor());
        j.set("T", (r.sr & kSrTrace) != 0);
        j.set("IPL", (r.sr & kSrIntMask) >> 8);
        j.set("stopped", s.cpu_->stopped());
        j.set("halted", s.cpu_->halted());
        j.set("instructions", s.cpu_->instructionCount());
        j.set("exceptions", s.cpu_->exceptionCount());
        return j;
    }

    void setRegister(const std::string& rawName, std::int64_t value) {
        const std::string n = upper(rawName);
        Registers& r = s.cpu_->registers();
        const Long v = static_cast<Long>(value);
        if (n.size() == 2 && (n[0] == 'D' || n[0] == 'A') && n[1] >= '0' && n[1] <= '7') {
            (n[0] == 'D' ? r.d : r.a)[static_cast<size_t>(n[1] - '0')] = v;
        } else if (n == "SP") {
            r.a[7] = v;
        } else if (n == "PC") {
            r.pc = v & kAddressMask;
            wake();  // a debugger that sets the PC wants to run from there
        } else if (n == "SR") {
            s.cpu_->setSr(static_cast<Word>(v));
        } else if (n == "CCR") {
            s.cpu_->setFlags(static_cast<Word>(v));
        } else if (n == "USP") {
            (r.supervisor() ? r.inactiveSp : r.a[7]) = v;
        } else if (n == "SSP") {
            (r.supervisor() ? r.a[7] : r.inactiveSp) = v;
        } else {
            throw SessionError("unknown register '" + rawName + "'");
        }
    }

    // ---- results of running commands ----
    Json runReply(const RunResult& r, std::uint64_t exceptionsBefore) const {
        Json j = Json::object();
        j.set("stop", stopName(r.reason));
        j.set("instructions", r.instructions);
        j.set("pc", hexAddr(s.cpu_->registers().pc));
        const std::uint64_t ex = s.cpu_->exceptionCount() - exceptionsBefore;
        j.set("exceptions", ex);
        if (ex) j.set("last_exception", s.cpu_->lastException());
        if (r.reason == StopReason::Watchpoint) {
            Json w = Json::object();
            w.set("addr", hexAddr(r.watchAddress));
            j.set("watch", w);
        }
        j.set("regs", regsJson());
        return j;
    }

    Json symbolsJson(const std::map<std::string, std::int64_t>& m) const {
        Json j = Json::object();
        for (const auto& [k, v] : m) j.set(k, hex32(static_cast<Long>(v)));
        return j;
    }

    std::string symbolAt(Address a) const {
        for (const auto& [k, v] : s.symbols_)
            if (static_cast<Address>(v) == a) return k;
        return "";
    }

    // ---- commands ----
    Json run();
    Json cmdAsm();
    Json cmdDis();
    Json cmdRead();
    Json cmdWrite();
    Json cmdLoadFile();
    Json cmdSet();
    Json cmdStep();
    Json cmdCall();
    Json cmdBreaks();
    Json cmdWatch(bool add);
    Json cmdCheckpoint(const std::string& cmd);
    Json cmdDiff();
    Json cmdState();
    Json cmdSetState();
    Json cmdMachine();
    Json cmdIo();
    Json cmdHelp();
};

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
    if (cmd == "machine") return cmdMachine();
    if (cmd == "io") return cmdIo();
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
        "run", "run_until_return", "call", "break", "unbreak", "breaks", "clear_breaks", "watch", "unwatch", "checkpoint",
        "rollback", "drop", "checkpoints", "diff", "state", "set_state", "save_state", "load_state", "interrupt", "io", "symbols"};
    Json arr = Json::array();
    for (const char* n : names) arr.push(n);
    Json j = Json::object();
    j.set("commands", arr);
    return j;
}

Json Session::Impl::cmdMachine() {
    const std::string type = arg("type") ? str("type") : "generic";
    if (type == "generic") {
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
        s.amiga_ = std::make_unique<Amiga500>(*s.memory_, c);
        s.amiga_->setMaxLog(s.options_.maxIoLog);
        s.amiga_->install(*s.cpu_);
    } else {
        throw SessionError("unknown machine type '" + type + "' (generic, amiga500)");
    }
    s.symbols_.clear();
    s.checkpoints_.clear();
    s.resetMachine(true);
    Json j = Json::object();
    j.set("machine", type);
    if (s.amiga_) {
        j.set("chip_kb", s.amiga_->config().chipKb);
        j.set("slow_kb", s.amiga_->config().slowKb);
        j.set("fast_kb", s.amiga_->config().fastKb);
    }
    j.set("regs", regsJson());
    return j;
}

Json Session::Impl::cmdAsm() {
    const std::string source = str("source");
    AssemblerOptions opt;
    opt.origin = address("origin", 0x1000);
    opt.predefined = s.symbols_;
    const AssemblyResult r = assemble(source, opt);
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
    j.set("segments", segs);
    j.set("symbols", symbolsJson(r.symbols));
    if (const Json* e = arg("entry")) {
        const Address a = address(*e);
        s.cpu_->registers().pc = a;
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
    if (type == "auto") type = looksLikeSRecordFile(file) ? "srec" : looksLikeHunkFile(bytes) ? "hunk" : "raw";
    Json j = Json::object();
    Registers& r = s.cpu_->registers();
    if (type == "raw") {
        if (!arg("addr")) throw SessionError("a raw file needs 'addr'");
        const Address a = address("addr");
        writeBytes(a, bytes);
        j.set("addr", hexAddr(a)).set("size", bytes.size());
    } else if (type == "srec") {
        const Image img = parseSRecords(std::string(bytes.begin(), bytes.end()));
        std::size_t total = 0;
        for (const Segment& seg : img.segments) {
            writeBytes(seg.address, seg.bytes);
            total += seg.bytes.size();
        }
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

Json Session::Impl::cmdStep() {
    Cpu& cpu = *s.cpu_;
    const std::string cmd = str("cmd");
    const std::uint64_t exBefore = cpu.exceptionCount();
    s.applyWatches();
    if (cmd == "step") {
        const std::int64_t n = arg("n") ? number(need("n")) : 1;
        if (n < 1) throw SessionError("n must be at least 1");
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
        return j;
    }
    if (cmd == "step_over") return runReply(cpu.stepOver(limit()), exBefore);
    if (cmd == "run_until_return") return runReply(cpu.runUntilReturn(limit()), exBefore);
    // run
    if (const Json* u = arg("until")) return runReply(cpu.runUntil(address(*u), limit()), exBefore);
    return runReply(cpu.run(limit()), exBefore);
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
    const RunResult r = cpu.call(entry, ret, limit());
    Json j = runReply(r, exBefore);
    const bool returned = r.reason == StopReason::Reached && cpu.registers().pc == ret;
    j.set("returned", returned);
    if (returned) j.set("stop", "returned");
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
