#include "m68k/monitor.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "m68k/assembler.hpp"
#include "m68k/decoder.hpp"
#include "m68k/encoder.hpp"
#include "m68k/expression.hpp"
#include "m68k/format.hpp"
#include "m68k/hunk.hpp"
#include "m68k/image_io.hpp"
#include "m68k/version.hpp"

namespace m68k {

namespace {

struct MonitorError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr Address kGenericStackTop = 0x80000;
constexpr std::size_t kMaxSpan = 0x100000;  // fill/compare/copy/search/save work on at most 1 MiB per command

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string h6(Address v) { return hexDigits(v & kAddressMask, 6); }
std::string h8(Long v) { return hexDigits(v, 8); }
std::string pad(std::string s, std::size_t w) {
    if (s.size() < w) s.append(w - s.size(), ' ');
    return s;
}

// Splits a line into words; "quoted text" stays one word (quotes kept).
std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (const char c : line) {
        if (quoted) {
            cur += c;
            if (c == '"') quoted = false;
        } else if (c == '"') {
            cur += c;
            quoted = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

}  // namespace

struct Monitor::Impl {
    Monitor& m;
    explicit Impl(Monitor& monitor) : m(monitor) {}

    Cpu& cpu() { return *m.cpu_; }
    Registers& regs() { return m.cpu_->registers(); }

    // ---- memory as the processor sees it ----
    Byte rd(Address a) const { return m.amiga_ ? m.amiga_->readByte(a) : m.memory_->read8(a); }
    bool writable(Address a) const {
        if (!m.amiga_) return true;
        const auto r = m.amiga_->classify(a);
        return r != Amiga500::Region::Rom && r != Amiga500::Region::Unmapped;
    }
    void wr(Address a, Byte v) {
        a &= kAddressMask;
        if (!writable(a)) throw MonitorError("address " + h6(a) + " is not writable (ROM or unmapped)");
        if (m.amiga_) m.amiga_->writeByte(a, v);
        else m.memory_->write8(a, v);
    }
    Memory view(Address a, std::size_t n) const {
        Memory v;
        for (std::size_t i = 0; i < n; ++i) v.write8(static_cast<Address>(a + i), rd(static_cast<Address>(a + i)));
        return v;
    }

    // ---- numbers and expressions (hexadecimal by default) ----
    std::string substituteRegisters(const std::string& text) const {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '@') {
                out += text[i];
                continue;
            }
            std::size_t j = i + 1;
            while (j < text.size() && std::isalnum(static_cast<unsigned char>(text[j]))) ++j;
            const std::string name = upper(text.substr(i + 1, j - i - 1));
            std::int64_t v;
            if (!registerValue(name, v)) throw MonitorError("unknown register '" + name + "'");
            out += "$" + hexDigits(static_cast<unsigned>(v), 1);
            i = j - 1;
        }
        return out;
    }
    bool registerValue(const std::string& u, std::int64_t& v) const {
        const Registers& r = m.cpu_->registers();
        if (u.size() == 2 && (u[0] == 'D' || u[0] == 'A') && u[1] >= '0' && u[1] <= '7') {
            v = (u[0] == 'D' ? r.d : r.a)[static_cast<size_t>(u[1] - '0')];
            return true;
        }
        if (u == "SP") { v = r.a[7]; return true; }
        if (u == "PC") { v = r.pc; return true; }
        if (u == "SR") { v = r.sr; return true; }
        if (u == "CCR") { v = r.sr & 0x1F; return true; }
        if (u == "USP") { v = r.usp(); return true; }
        if (u == "SSP") { v = r.ssp(); return true; }
        return false;
    }
    std::int64_t value(const std::string& text) const {
        const std::string t = substituteRegisters(text);
        const ExpressionEvaluator ev(
            [this](const std::string& n, std::int64_t& v) {
                if (const auto it = m.symbols_.find(n); it != m.symbols_.end()) {
                    v = it->second;
                    return true;
                }
                return false;
            },
            m.cpu_->registers().pc, true);
        const ExprResult r = ev.evaluate(t);
        if (!r.ok) throw MonitorError("bad number or expression '" + text + "': " + r.error);
        if (r.undefined) throw MonitorError("unknown symbol '" + r.undefinedName + "'");
        return r.value;
    }
    Address address(const std::string& text) const { return static_cast<Address>(value(text)) & kAddressMask; }
    Long number(const std::string& text) const { return static_cast<Long>(value(text)); }

    // bytes from arguments: numbers and "text"
    std::vector<Byte> byteList(const std::vector<std::string>& args, std::size_t from, unsigned unit = 1) const {
        std::vector<Byte> out;
        for (std::size_t i = from; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a.size() >= 2 && a.front() == '"' && a.back() == '"') {
                for (std::size_t k = 1; k + 1 < a.size(); ++k) out.push_back(static_cast<Byte>(a[k]));
                continue;
            }
            const std::int64_t v = value(a);
            if (unit == 1 && (v < -128 || v > 255)) throw MonitorError("'" + a + "' does not fit in a byte");
            if (unit == 2 && (v < -32768 || v > 65535)) throw MonitorError("'" + a + "' does not fit in a word");
            for (int b = static_cast<int>(unit) - 1; b >= 0; --b) out.push_back(static_cast<Byte>((v >> (8 * b)) & 0xFF));
        }
        if (out.empty()) throw MonitorError("no data given");
        return out;
    }

    std::string symbolAt(Address a) const {
        std::string best;
        for (const auto& [k, v] : m.symbols_)
            if (static_cast<Address>(v) == a && (best.empty() || k < best)) best = k;
        return best;
    }

    // ---- output helpers ----
    std::string instructionLine(Address a, bool marker, bool labels) const {
        DecodeOptions opt;
        opt.symbolizer = [this](Address x) { return symbolAt(x); };
        const Memory v = view(a, 12);
        const Instruction in = decode(v, a, opt);
        std::ostringstream out;
        if (labels) {
            const std::string name = symbolAt(a);
            if (!name.empty()) out << name << ":\n";
        }
        std::string bytes;
        for (std::size_t i = 0; i < in.bytes.size(); ++i) {
            bytes += hexDigits(in.bytes[i], 2);
            if (i % 2 == 1 && i + 1 < in.bytes.size()) bytes += ' ';
        }
        out << (marker ? "> " : "  ") << h6(a) << "  " << pad(bytes, 22) << in.text() << "\n";
        return out.str();
    }

    std::string regsText() const {
        const Registers& r = m.cpu_->registers();
        std::ostringstream o;
        std::string flags;
        flags += (r.sr & kFlagX) ? 'X' : '.';
        flags += (r.sr & kFlagN) ? 'N' : '.';
        flags += (r.sr & kFlagZ) ? 'Z' : '.';
        flags += (r.sr & kFlagV) ? 'V' : '.';
        flags += (r.sr & kFlagC) ? 'C' : '.';
        o << " PC=" << h6(r.pc) << " SR=" << hexDigits(r.sr, 4) << "  " << flags << "  " << (r.supervisor() ? "S" : "U")
          << " IPL" << ((r.sr & kSrIntMask) >> 8) << ((r.sr & kSrTrace) ? " T" : "") << (m.cpu_->stopped() ? "  STOPPED" : "")
          << (m.cpu_->halted() ? "  HALTED" : "") << "  USP=" << h8(r.usp()) << " SSP=" << h8(r.ssp()) << "\n";
        for (int row = 0; row < 2; ++row) {
            o << " ";
            for (int i = 0; i < 4; ++i) o << (i ? " " : "") << "D" << (row * 4 + i) << "=" << h8(r.d[static_cast<size_t>(row * 4 + i)]);
            o << "\n";
        }
        for (int row = 0; row < 2; ++row) {
            o << " ";
            for (int i = 0; i < 4; ++i) o << (i ? " " : "") << "A" << (row * 4 + i) << "=" << h8(r.a[static_cast<size_t>(row * 4 + i)]);
            o << "\n";
        }
        return o.str();
    }

    std::string stopText(const RunResult& r) const {
        std::ostringstream o;
        const Registers& rg = m.cpu_->registers();
        o << "stopped: ";
        switch (r.reason) {
            case StopReason::Breakpoint: o << "breakpoint at " << h6(rg.pc); break;
            case StopReason::Watchpoint: o << "watchpoint at " << h6(r.watchAddress); break;
            case StopReason::Reached: o << "reached " << h6(rg.pc); break;
            case StopReason::Returned: o << "returned"; break;
            case StopReason::Stopped: o << "STOP instruction (waiting for an interrupt)"; break;
            case StopReason::Halted: o << "halted (double fault)"; break;
            case StopReason::Requested: o << m.cpu_->stopMessage(); break;
            case StopReason::Limit: o << "instruction limit (" << m.limit_ << ")"; break;
            case StopReason::None: o << "stepped"; break;
        }
        o << "  [" << r.instructions << " instructions]\n";
        return o.str();
    }
    std::string afterRun(const RunResult& r, std::uint64_t exBefore) {
        std::ostringstream o;
        o << stopText(r);
        const std::uint64_t ex = m.cpu_->exceptionCount() - exBefore;
        if (ex) o << " exceptions: " << ex << " (last: vector " << m.cpu_->lastException() << ")\n";
        o << regsText();
        o << instructionLine(m.cpu_->registers().pc, true, true);
        m.nextDisassembly_ = m.cpu_->registers().pc;
        return o.str();
    }

    void resetMachine(bool clearMemory) {
        if (clearMemory) m.memory_->clear();
        Registers r;
        r.sr = 0x2700;
        r.a[7] = m.amiga_ ? m.amiga_->config().chipKb * 1024U - 16 : kGenericStackTop;
        CpuState st = m.cpu_->state();
        st = CpuState{};
        st.regs = r;
        m.cpu_->setState(st);
        if (m.amiga_) m.amiga_->reset(*m.cpu_);
        if (m.os_) {
            m.os_->reset();
            regs().a[7] = m.os_->stackTop();
        }
    }
    void wake() {
        CpuState st = m.cpu_->state();
        st.stopped = false;
        st.halted = false;
        m.cpu_->setState(st);
    }

    void requireFiles() const {
        if (!m.options_.allowFiles) throw MonitorError("file commands are disabled (--no-files)");
    }

    // ---- watchpoints ----
    using WatchEntry = Monitor::Watch;
    std::vector<WatchEntry>& watches() { return m.watches_; }
    void applyWatches() {
        cpu().clearWatchpoints();
        for (const WatchEntry& w : m.watches_) cpu().addWatchpoint(w.start, w.length, w.read, w.write);
    }
    std::vector<std::string> watchList_() const {
        std::vector<std::string> out;
        for (const WatchEntry& w : m.watches_)
            out.push_back("  watch " + h6(w.start) + " len " + hexDigits(w.length, 1) + (w.read && w.write ? " rw" : w.read ? " r" : " w") + "\n");
        return out;
    }
    static bool isMnemonicWord(const std::string& w) { return isMnemonic(upper(w)); }

    // ---- commands ----
    std::string run(const std::vector<std::string>& a);
    std::string assembleLine(const std::string& line);
    std::string cmdDump(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdEdit(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdRegisters(const std::vector<std::string>& a);
    std::string cmdFiles(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdSymbols(const std::vector<std::string>& a);
    std::string cmdBreaks(const std::string& cmd, const std::vector<std::string>& a);
    std::string cmdMachine(const std::vector<std::string>& a);
    static std::string help();
};

std::string Monitor::banner() {
    return std::string("m68ktool monitor ") + kVersion + " - MC68000 (type ? for help, q to quit; numbers are hexadecimal)\n";
}

Monitor::Monitor(MonitorOptions options)
    : options_(options), memory_(std::make_unique<Memory>()), cpu_(std::make_unique<Cpu>(*memory_)), limit_(options.maxInstructions) {
    Impl(*this).resetMachine(false);
}

Monitor::~Monitor() = default;

std::string Monitor::prompt() const { return assembleAt_ ? hexDigits(*assembleAt_, 6) + ":" : "-"; }

std::string Monitor::execute(std::string_view line) {
    Impl impl(*this);
    try {
        if (assembleAt_) return impl.assembleLine(std::string(line));
        const std::vector<std::string> args = tokenize(line);
        if (args.empty()) return "";
        return impl.run(args);
    } catch (const MonitorError& e) {
        return std::string("? ") + e.what() + "\n";
    } catch (const std::exception& e) {
        return std::string("? ") + e.what() + "\n";
    }
}

std::string Monitor::Impl::help() {
    return
        "numbers are hexadecimal: 1000 FF; 100. = decimal; $FF 0xFF %1010; symbols and expressions (start+4); @A0 @PC = registers\n"
        "  d [adr [n]]            disassemble (n instructions, default 12; no address: continue)\n"
        "  m [adr [n]]            memory as hex + ASCII (n bytes, default 128); m.w / m.l for words / longs\n"
        "  e adr b...             write bytes (numbers and \"text\"); e.w / e.l write words / longs\n"
        "  f from to b...         fill (the pattern is repeated)      c from to dest   compare\n"
        "  t from to dest         copy (overlap-safe)                  h from to b...   search bytes or \"text\"\n"
        "  a [adr]                assemble lines (assembler syntax, decimal numbers); an empty line ends\n"
        "  r [name=value ...]     show / set registers: d0-d7 a0-a7 sp pc sr ccr usp ssp (r pc=1000 d0=@a1)\n"
        "  n [count]              single step(s)        o   step over BSR/JSR        u   run until the routine returns\n"
        "  g [adr]                run until a breakpoint / STOP / limit        ru adr   run until PC == adr\n"
        "  b [adr...]             set breakpoints (no argument: list)          bc [adr...]   clear (no argument: all)\n"
        "  w adr [len] [r|w|rw]   watchpoint (default: 1 byte, write)          limit n   instructions per run\n"
        "  sym [name value | -name]   symbols       symload file / symsave file\n"
        "  asm file [org]         assemble a source file into memory           l file [adr]   load raw / S-record / hunk\n"
        "  sv file from to        save memory (inclusive)                      ck name / rb name   checkpoint / roll back\n"
        "  savestate file / loadstate file    snapshot files                   int level [vector]   interrupt\n"
        "  machine [generic|amiga500 [chipkb] [os]]   memory map (os: emulated exec/dos calls)   rom file   Kickstart ROM (amiga500)\n"
        "  io                     writes to custom/CIA registers (amiga500)    reset [mem]   reset the CPU (and memory)\n"
        "  libcalls / console     library calls and text printed through dos Write (machine amiga500 [chipkb] os)\n"
        "  prog adr               start a program like AmigaDOS (PC, D0=A0=0, return address on the stack; needs os)\n"
        "  ?  help      q  quit\n";
}

std::string Monitor::Impl::run(const std::vector<std::string>& a) {
    const std::string cmd = lower(a[0]);
    Cpu& c = cpu();
    applyWatches();
    if (cmd == "?" || cmd == "help") return help();
    if (cmd == "q" || cmd == "quit" || cmd == "exit") {
        m.finished_ = true;
        return "";
    }
    if (cmd == "d") {
        const Address from = a.size() > 1 ? address(a[1]) : m.nextDisassembly_;
        const std::int64_t n = a.size() > 2 ? value(a[2]) : 12;
        if (n < 1 || n > 4096) throw MonitorError("count must be 1..1000 (hex)");
        std::string out;
        Address at = from;
        const Address pc = regs().pc;
        for (std::int64_t i = 0; i < n; ++i) {
            DecodeOptions opt;
            opt.symbolizer = [this](Address x) { return symbolAt(x); };
            const Memory v = view(at, 12);
            const Instruction in = decode(v, at, opt);
            out += instructionLine(at, at == pc, true);
            at = (at + static_cast<Address>(in.length())) & kAddressMask;
        }
        m.nextDisassembly_ = at;
        return out;
    }
    if (cmd == "m" || cmd == "m.b" || cmd == "m.w" || cmd == "m.l") return cmdDump(cmd, a);
    if (cmd == "e" || cmd == "e.b" || cmd == "e.w" || cmd == "e.l" || cmd == "f" || cmd == "c" || cmd == "t" || cmd == "h")
        return cmdEdit(cmd, a);
    if (cmd == "a") {
        m.assembleAt_ = a.size() > 1 ? address(a[1]) : m.nextDisassembly_;
        return "assemble mode: type assembler lines (decimal numbers, $ for hex), empty line to finish\n";
    }
    if (cmd == "r") return cmdRegisters(a);
    if (cmd == "n") {
        const std::int64_t n = a.size() > 1 ? value(a[1]) : 1;
        if (n < 1 || n > 1000000) throw MonitorError("count must be 1..1000000 (hex)");
        const std::uint64_t exBefore = c.exceptionCount();
        std::string shown;
        RunResult r;
        r.reason = StopReason::None;
        for (std::int64_t i = 0; i < n; ++i) {
            if (n >= 2 && n <= 20) shown += instructionLine(regs().pc, false, false);
            const StepResult s = c.step();
            if (s.reason == StopReason::Halted || s.reason == StopReason::Stopped) {
                r.reason = s.reason;
                break;
            }
            ++r.instructions;
        }
        if (n >= 2 && n <= 20) return shown + afterRun(r, exBefore);
        return afterRun(r, exBefore);
    }
    if (cmd == "o" || cmd == "u" || cmd == "g" || cmd == "ru") {
        const std::uint64_t exBefore = c.exceptionCount();
        RunResult r;
        if (cmd == "g" && a.size() > 1) {
            regs().pc = address(a[1]);
            wake();
        }
        if (cmd == "o") r = c.stepOver(m.limit_);
        else if (cmd == "u") r = c.runUntilReturn(m.limit_);
        else if (cmd == "ru") {
            if (a.size() < 2) throw MonitorError("usage: ru address");
            r = c.runUntil(address(a[1]), m.limit_);
        } else r = c.run(m.limit_);
        return afterRun(r, exBefore);
    }
    if (cmd == "b" || cmd == "bc" || cmd == "w") return cmdBreaks(cmd, a);
    if (cmd == "limit") {
        if (a.size() < 2) return "limit " + std::to_string(m.limit_) + "\n";
        const std::int64_t n = value(a[1]);
        if (n < 1) throw MonitorError("limit must be at least 1");
        m.limit_ = static_cast<std::uint64_t>(n);
        return "ok\n";
    }
    if (cmd == "sym" || cmd == "symload" || cmd == "symsave") {
        if (cmd == "sym") return cmdSymbols(a);
        return cmdFiles(cmd, a);
    }
    if (cmd == "asm" || cmd == "l" || cmd == "sv" || cmd == "savestate" || cmd == "loadstate" || cmd == "rom") return cmdFiles(cmd, a);
    if (cmd == "ck" || cmd == "rb") {
        if (a.size() < 2) throw MonitorError("usage: " + cmd + " name");
        if (cmd == "ck") {
            m.checkpoints_[a[1]] = capture(c);
            m.amigaCheckpoints_.erase(a[1]);
            m.osCheckpoints_.erase(a[1]);
            if (m.amiga_) m.amigaCheckpoints_.emplace(a[1], m.amiga_->state());
            if (m.os_) m.osCheckpoints_.emplace(a[1], m.os_->state());
            return "ok\n";
        }
        const auto it = m.checkpoints_.find(a[1]);
        if (it == m.checkpoints_.end()) throw MonitorError("no checkpoint named '" + a[1] + "'");
        restore(c, it->second);
        if (m.amiga_ && m.amigaCheckpoints_.count(a[1])) m.amiga_->setState(m.amigaCheckpoints_.at(a[1]));
        if (m.os_ && m.osCheckpoints_.count(a[1])) m.os_->setState(m.osCheckpoints_.at(a[1]));
        return "ok\n" + regsText();
    }
    if (cmd == "int") {
        if (a.size() < 2) throw MonitorError("usage: int level [vector]");
        const std::int64_t level = value(a[1]);
        if (level < 1 || level > 7) throw MonitorError("level must be 1..7");
        c.interrupt(static_cast<unsigned>(level), a.size() > 2 ? static_cast<int>(value(a[2])) : -1);
        return "ok\n";
    }
    if (cmd == "io") {
        if (!m.amiga_) return "no device map (use: machine amiga500)\n";
        std::string out;
        for (const DeviceWrite& w : m.amiga_->log()) {
            const char* kind = w.kind == DeviceWrite::Kind::Custom ? "custom" : w.kind == DeviceWrite::Kind::Cia ? "cia"
                               : w.kind == DeviceWrite::Kind::Rom ? "rom" : "unmapped";
            const std::string name = Amiga500::customRegisterName(w.address);
            out += "  " + h6(w.address) + " <- " + hexDigits(w.value, static_cast<int>(w.size) * 2) + "  (" + kind +
                   (name.empty() ? "" : " " + name) + ")  by the instruction at " + h6(w.pc) + "\n";
        }
        m.amiga_->clearLog();
        return out.empty() ? "no writes\n" : out;
    }
    if (cmd == "prog") {
        if (!m.os_) throw MonitorError("prog needs the emulated OS (machine amiga500 [chipkb] os)");
        if (a.size() < 2) throw MonitorError("usage: prog address");
        regs().pc = address(a[1]);
        m.os_->prepareProgram(cpu());
        wake();
        return "ok: PC=" + h6(regs().pc) + " (a return to the system ends the run)\n";
    }
    if (cmd == "libcalls") {
        if (!m.os_) return "no emulated OS (use: machine amiga500 chipkb os)\n";
        std::string out;
        for (const LibCall& lc : m.os_->log()) {
            out += "  " + lc.library + " " + std::to_string(lc.offset) + (lc.function.empty() ? "" : " " + lc.function) +
                   (lc.emulated ? "" : "  (NOT emulated: returned 0)") + (lc.minVersion >= 36 ? "  (V36+: not in Kickstart 1.x)" : "") + "  D0=" + h8(lc.d0) + " D1=" + h8(lc.d1) + " A0=" + h8(lc.a0) +
                   " A1=" + h8(lc.a1) + " -> D0=" + h8(lc.result) + (lc.note.empty() ? "" : "  [" + lc.note + "]") + "  called from " + h6(lc.caller) + "\n";
        }
        m.os_->clearLog();
        return out.empty() ? "no library calls\n" : out;
    }
    if (cmd == "console") {
        if (!m.os_) return "no emulated OS (use: machine amiga500 chipkb os)\n";
        const std::string text = m.os_->console();
        m.os_->clearConsole();
        return text.empty() ? "(empty)\n" : text + (text.back() == '\n' ? "" : "\n");
    }
    if (cmd == "reset") {
        resetMachine(a.size() > 1 && lower(a[1]) == "mem");
        return "ok\n";
    }
    if (cmd == "machine") return cmdMachine(a);
    throw MonitorError("unknown command '" + a[0] + "' (? for help)");
}

std::string Monitor::Impl::cmdDump(const std::string& cmd, const std::vector<std::string>& a) {
    const unsigned unit = cmd == "m.w" ? 2 : cmd == "m.l" ? 4 : 1;
    const Address from = a.size() > 1 ? address(a[1]) : m.nextDump_;
    std::int64_t n = a.size() > 2 ? value(a[2]) : 128;
    if (n < 1 || n > 0x10000) throw MonitorError("count must be 1..10000 (hex)");
    n = (n + unit - 1) / unit * unit;
    const std::size_t perLine = 16;
    std::string out;
    for (std::int64_t off = 0; off < n; off += static_cast<std::int64_t>(perLine)) {
        const std::size_t len = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(perLine), n - off));
        const Address base = static_cast<Address>(from + off);
        std::string hex, ascii;
        for (std::size_t i = 0; i < len; ++i) {
            const Byte b = rd(static_cast<Address>(base + i));
            hex += hexDigits(b, 2);
            if ((i + 1) % unit == 0) hex += ' ';
            ascii += (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
        }
        out += h6(base) + "  " + pad(hex, perLine * 2 + perLine / unit) + " |" + ascii + "|\n";
    }
    m.nextDump_ = static_cast<Address>(from + n);
    return out;
}

std::string Monitor::Impl::cmdEdit(const std::string& cmd, const std::vector<std::string>& a) {
    if (cmd == "e" || cmd == "e.b" || cmd == "e.w" || cmd == "e.l") {
        if (a.size() < 3) throw MonitorError("usage: " + cmd + " address data...");
        const Address at = address(a[1]);
        const unsigned unit = cmd == "e.w" ? 2 : cmd == "e.l" ? 4 : 1;
        if (unit > 1 && (at & 1U)) throw MonitorError("word and long data should start at an even address");
        const std::vector<Byte> data = byteList(a, 2, unit);
        for (std::size_t i = 0; i < data.size(); ++i)
            if (!writable(static_cast<Address>(at + i))) throw MonitorError("address " + h6(static_cast<Address>(at + i)) + " is not writable (ROM or unmapped)");
        for (std::size_t i = 0; i < data.size(); ++i) wr(static_cast<Address>(at + i), data[i]);
        m.nextDump_ = at;
        return "ok\n";
    }
    if (a.size() < 4) throw MonitorError("usage: " + cmd + " from to ...");
    const Address from = address(a[1]);
    const Address to = address(a[2]);
    if (to < from) throw MonitorError("'to' is below 'from'");
    const std::size_t len = static_cast<std::size_t>(to - from) + 1;
    if (len > kMaxSpan) throw MonitorError("the range is larger than 100000 (hex) bytes");
    if (cmd == "f") {
        const std::vector<Byte> pattern = byteList(a, 3);
        for (std::size_t i = 0; i < len; ++i)
            if (!writable(static_cast<Address>(from + i))) throw MonitorError("address " + h6(static_cast<Address>(from + i)) + " is not writable");
        for (std::size_t i = 0; i < len; ++i) wr(static_cast<Address>(from + i), pattern[i % pattern.size()]);
        return "ok\n";
    }
    if (cmd == "c") {
        const Address dest = address(a[3]);
        std::string out;
        std::size_t diffs = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const Byte x = rd(static_cast<Address>(from + i)), y = rd(static_cast<Address>(dest + i));
            if (x != y && diffs++ < 32)
                out += "  " + h6(static_cast<Address>(from + i)) + ": " + hexDigits(x, 2) + "  " + h6(static_cast<Address>(dest + i)) + ": " + hexDigits(y, 2) + "\n";
        }
        if (diffs > 32) out += "  ... " + std::to_string(diffs - 32) + " more\n";
        return diffs == 0 ? "identical\n" : out;
    }
    if (cmd == "t") {
        const Address dest = address(a[3]);
        std::vector<Byte> tmp;
        for (std::size_t i = 0; i < len; ++i) tmp.push_back(rd(static_cast<Address>(from + i)));  // via a buffer: overlap-safe
        for (std::size_t i = 0; i < len; ++i)
            if (!writable(static_cast<Address>(dest + i))) throw MonitorError("address " + h6(static_cast<Address>(dest + i)) + " is not writable");
        for (std::size_t i = 0; i < len; ++i) wr(static_cast<Address>(dest + i), tmp[i]);
        return "ok\n";
    }
    // h: search
    const std::vector<Byte> needle = byteList(a, 3);
    std::string out;
    std::size_t hits = 0;
    for (std::size_t i = 0; i + needle.size() <= len; ++i) {
        bool same = true;
        for (std::size_t k = 0; same && k < needle.size(); ++k) same = rd(static_cast<Address>(from + i + k)) == needle[k];
        if (same && hits++ < 64) out += "  " + h6(static_cast<Address>(from + i)) + "\n";
    }
    if (hits > 64) out += "  ... " + std::to_string(hits - 64) + " more\n";
    return hits == 0 ? "not found\n" : out;
}

std::string Monitor::Impl::cmdRegisters(const std::vector<std::string>& a) {
    if (a.size() == 1) return regsText();
    for (std::size_t i = 1; i < a.size(); ++i) {
        const std::size_t eq = a[i].find('=');
        if (eq == std::string::npos) throw MonitorError("usage: r name=value ...");
        const std::string name = upper(a[i].substr(0, eq));
        const Long v = number(a[i].substr(eq + 1));
        Registers& r = regs();
        if (name.size() == 2 && (name[0] == 'D' || name[0] == 'A') && name[1] >= '0' && name[1] <= '7') {
            (name[0] == 'D' ? r.d : r.a)[static_cast<size_t>(name[1] - '0')] = v;
        } else if (name == "SP") r.a[7] = v;
        else if (name == "PC") {
            r.pc = v & kAddressMask;
            wake();
        } else if (name == "SR") cpu().setSr(static_cast<Word>(v));
        else if (name == "CCR") cpu().setFlags(static_cast<Word>(v));
        else if (name == "USP") (r.supervisor() ? r.inactiveSp : r.a[7]) = v;
        else if (name == "SSP") (r.supervisor() ? r.a[7] : r.inactiveSp) = v;
        else throw MonitorError("unknown register '" + a[i].substr(0, eq) + "'");
    }
    return regsText();
}

std::string Monitor::Impl::cmdBreaks(const std::string& cmd, const std::vector<std::string>& a) {
    Cpu& c = cpu();
    if (cmd == "b") {
        if (a.size() == 1) {
            std::string out;
            for (const Address x : c.breakpoints()) out += "  break " + h6(x) + (symbolAt(x).empty() ? "" : "  (" + symbolAt(x) + ")") + "\n";
            for (const auto& w : watchList_()) out += w;
            return out.empty() ? "no breakpoints\n" : out;
        }
        for (std::size_t i = 1; i < a.size(); ++i) c.addBreakpoint(address(a[i]));
        return "ok\n";
    }
    if (cmd == "bc") {
        if (a.size() == 1) {
            c.clearBreakpoints();
            c.clearWatchpoints();
            watches().clear();
            return "ok\n";
        }
        for (std::size_t i = 1; i < a.size(); ++i) {
            const Address x = address(a[i]);
            c.removeBreakpoint(x);
            auto& ws = watches();
            ws.erase(std::remove_if(ws.begin(), ws.end(), [&](const WatchEntry& w) { return w.start == x; }), ws.end());
        }
        applyWatches();
        return "ok\n";
    }
    // w adr [len] [r|w|rw]
    if (a.size() < 2) throw MonitorError("usage: w address [length] [r|w|rw]");
    WatchEntry w;
    w.start = address(a[1]);
    w.length = 1;
    w.read = false;
    w.write = true;
    for (std::size_t i = 2; i < a.size(); ++i) {
        const std::string t = lower(a[i]);
        if (t == "r") w.read = true, w.write = false;
        else if (t == "w") w.read = false, w.write = true;
        else if (t == "rw") w.read = w.write = true;
        else w.length = static_cast<Address>(value(a[i]));
    }
    if (w.length < 1) throw MonitorError("length must be at least 1");
    watches().push_back(w);
    applyWatches();
    return "ok\n";
}

std::string Monitor::Impl::cmdSymbols(const std::vector<std::string>& a) {
    if (a.size() == 1) {
        std::string out;
        for (const auto& [k, v] : m.symbols_) out += "  " + pad(k, 20) + " = $" + h8(static_cast<Long>(v)) + "\n";
        return out.empty() ? "no symbols\n" : out;
    }
    if (a[1][0] == '-') {
        if (m.symbols_.erase(a[1].substr(1)) == 0) throw MonitorError("no symbol '" + a[1].substr(1) + "'");
        return "ok\n";
    }
    if (a.size() < 3) throw MonitorError("usage: sym name value   |   sym -name");
    const std::string name = a[1];
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) throw MonitorError("a symbol name starts with a letter or '_'");
    m.symbols_[name] = value(a[2]);
    return "ok\n";
}

std::string Monitor::Impl::cmdMachine(const std::vector<std::string>& a) {
    if (a.size() == 1)
        return m.amiga_ ? "amiga500 (" + std::to_string(m.amiga_->config().chipKb) + " KiB chip RAM" + (m.os_ ? ", emulated OS calls" : "") + ")\n"
                        : "generic (16 MiB RAM)\n";
    const std::string type = lower(a[1]);
    if (type == "generic") {
        m.os_.reset();
        m.amiga_.reset();
        cpu().readOverride = nullptr;
        cpu().writeOverride = nullptr;
        cpu().executeHook = nullptr;
    } else if (type == "amiga500") {
        AmigaConfig cfg;
        bool wantOs = false;
        for (std::size_t i = 2; i < a.size(); ++i) {
            if (lower(a[i]) == "os") wantOs = true;
            else cfg.chipKb = static_cast<unsigned>(number(a[i]));
        }
        if (cfg.chipKb < 64 || cfg.chipKb > 2048) throw MonitorError("chip_kb must be 40..800 (hex) = 64..2048");
        m.os_.reset();
        cpu().executeHook = nullptr;
        m.amiga_ = std::make_unique<Amiga500>(*m.memory_, cfg);
        m.amiga_->install(cpu());
        if (wantOs) {
            m.os_ = std::make_unique<AmigaOs>(*m.memory_, *m.amiga_);
            if (!m.os_->supported()) {
                m.os_.reset();
                throw MonitorError("os needs chip RAM of at least 200 (hex) = 512 KiB");
            }
            m.os_->install(cpu());
        }
    } else {
        throw MonitorError("unknown machine '" + a[1] + "' (generic, amiga500)");
    }
    m.symbols_.clear();
    m.checkpoints_.clear();
    m.amigaCheckpoints_.clear();
    m.osCheckpoints_.clear();
    resetMachine(true);
    return "ok\n";
}

std::string Monitor::Impl::assembleLine(const std::string& raw) {
    std::string line = raw;
    // trim
    const std::size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos) {
        m.assembleAt_.reset();
        return "ok\n";
    }
    line = line.substr(b);
    Address at = *m.assembleAt_;
    // "label:" prefix?
    std::string label;
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos && colon > 0 && line.find_first_of(" \t") > colon) {
        label = line.substr(0, colon);
        line = line.substr(colon + 1);
        const std::size_t nb = line.find_first_not_of(" \t");
        line = nb == std::string::npos ? "" : line.substr(nb);
        if (!(std::isalpha(static_cast<unsigned char>(label[0])) || label[0] == '_')) throw MonitorError("bad label '" + label + "'");
    } else if (line.find_first_of(" \t,") == std::string::npos && !isMnemonicWord(line)) {
        throw MonitorError("'" + line + "' is not an instruction (a label needs a colon: " + line + ":)");
    }
    if (!label.empty()) m.symbols_[label] = at;
    if (line.empty()) return label.empty() ? "" : "  " + label + " = " + h6(at) + "\n";
    AssemblerOptions opt;
    opt.origin = at;
    opt.predefined = m.symbols_;
    const AssemblyResult r = assemble("        " + line + "\n", opt);
    if (r.hunk) throw MonitorError("SECTION is not available while typing lines; assemble a source file to a hunk with 'm68ktool asm --hunk'");
    if (!r.ok()) {
        if (!label.empty()) m.symbols_.erase(label);
        std::string msg = r.errors[0].message;
        if (msg.find("undefined symbol") != std::string::npos)
            msg += " (forward references are not possible here: define the symbol first with 'sym name address')";
        throw MonitorError(msg);
    }
    const std::vector<Byte> bytes = r.flatten();
    for (std::size_t i = 0; i < bytes.size(); ++i)
        if (!writable(static_cast<Address>(at + i))) throw MonitorError("address " + h6(static_cast<Address>(at + i)) + " is not writable");
    for (std::size_t i = 0; i < bytes.size(); ++i) wr(static_cast<Address>(at + i), bytes[i]);
    std::string out;
    if (!bytes.empty()) {
        std::string hex;
        for (std::size_t i = 0; i < bytes.size() && i < 10; ++i) {
            hex += hexDigits(bytes[i], 2);
            if (i % 2 == 1) hex += ' ';
        }
        out = "  " + h6(at) + "  " + pad(hex, 22) + line + "\n";
    }
    m.assembleAt_ = static_cast<Address>(at + bytes.size()) & kAddressMask;
    m.nextDisassembly_ = at;
    return out;
}

std::string Monitor::Impl::cmdFiles(const std::string& cmd, const std::vector<std::string>& a) {
    requireFiles();
    if (a.size() < 2) throw MonitorError("usage: " + cmd + " file ...");
    const std::string file = a[1];
    try {
        if (cmd == "symsave") {
            std::string text;
            for (const auto& [k, v] : m.symbols_) text += k + " = $" + h8(static_cast<Long>(v)) + "\n";
            writeTextFile(file, text);
            return "ok\n";
        }
        if (cmd == "symload") {
            std::istringstream in(readTextFile(file));
            std::string line;
            std::size_t n = 0;
            while (std::getline(in, line)) {
                const std::size_t eq = line.find('=');
                if (line.empty() || line[0] == ';' || eq == std::string::npos) continue;
                std::string name = line.substr(0, eq);
                while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
                const ExpressionEvaluator ev(nullptr, 0);
                const ExprResult r = ev.evaluate(line.substr(eq + 1));
                if (!r.ok || r.undefined) throw MonitorError("bad line in " + file + ": " + line);
                m.symbols_[name] = r.value;
                ++n;
            }
            return "ok: " + std::to_string(n) + " symbols\n";
        }
        if (cmd == "asm") {
            AssemblerOptions opt;
            if (a.size() > 2) opt.origin = address(a[2]);
            opt.predefined = m.symbols_;
            const AssemblyResult r = assemble(readTextFile(file), opt);
            if (r.ok() && r.hunk) throw MonitorError("a program with SECTIONs is relocatable: assemble it to a hunk file with 'm68ktool asm --hunk', then load it with 'l'");
            if (!r.ok()) {
                std::string out;
                for (const Diagnostic& d : r.errors) out += "? " + file + ":" + std::to_string(d.line) + ": " + d.message + "\n";
                return out;
            }
            std::size_t total = 0;
            for (const Segment& seg : r.segments)
                for (std::size_t i = 0; i < seg.bytes.size(); ++i)
                    if (!writable(static_cast<Address>(seg.address + i))) throw MonitorError("address " + h6(static_cast<Address>(seg.address + i)) + " is not writable");
            for (const Segment& seg : r.segments) {
                for (std::size_t i = 0; i < seg.bytes.size(); ++i) wr(static_cast<Address>(seg.address + i), seg.bytes[i]);
                total += seg.bytes.size();
            }
            for (const auto& [k, v] : r.symbols) m.symbols_[k] = v;
            m.nextDisassembly_ = r.lowest();
            return "ok: " + std::to_string(total) + " bytes at " + h6(r.lowest()) + ", " + std::to_string(r.symbols.size()) + " symbols\n";
        }
        if (cmd == "l") {
            const std::vector<Byte> bytes = readBinaryFile(file);
            if (looksLikeSRecordFile(file)) {
                const Image img = parseSRecords(std::string(bytes.begin(), bytes.end()));
                std::size_t total = 0;
                for (const Segment& seg : img.segments) {
                    for (std::size_t i = 0; i < seg.bytes.size(); ++i) wr(static_cast<Address>(seg.address + i), seg.bytes[i]);
                    total += seg.bytes.size();
                }
                if (img.entry) {
                    regs().pc = *img.entry;
                    wake();
                }
                m.nextDisassembly_ = img.entry.value_or(img.segments.empty() ? 0 : img.segments[0].address);
                return "ok: " + std::to_string(total) + " bytes" + (img.entry ? ", PC=" + h6(*img.entry) : "") + "\n";
            }
            if (looksLikeHunkObject(bytes)) throw MonitorError(kHunkObjectMessage);
            if (looksLikeHunkFile(bytes)) {
                const Address base = a.size() > 2 ? address(a[2]) : kDefaultHunkBase;
                const HunkImage img = loadHunkFile(bytes, base);
                for (const Segment& seg : img.segments)
                    for (std::size_t i = 0; i < seg.bytes.size(); ++i) wr(static_cast<Address>(seg.address + i), seg.bytes[i]);
                std::string out;
                for (const HunkInfo& h : img.hunks) out += "  " + h.type + " hunk at " + h6(h.address) + ", " + std::to_string(h.size) + " bytes\n";
                for (const HunkSymbol& s : img.symbols) {
                    std::string name = asmSymbolName(s.name);
                    if (m.symbols_.count(name)) name += "_" + h6(s.address);
                    m.symbols_[name] = s.address;
                }
                regs().pc = img.entry;
                regs().d[0] = 0;
                regs().a[0] = 0;
                regs().a[7] = m.amiga_ ? m.amiga_->config().chipKb * 1024U - 16 : kGenericStackTop;
                if (m.os_) {
                    if (img.end > m.os_->heapBase()) throw MonitorError("the program reaches into the heap of the emulated OS (above " + h6(m.os_->heapBase()) + ")");
                    m.os_->prepareProgram(cpu());
                }
                wake();
                m.nextDisassembly_ = img.entry;
                return out + "ok: PC=" + h6(img.entry) + ", " + std::to_string(img.symbols.size()) + " symbols\n";
            }
            if (a.size() < 3) throw MonitorError("a raw file needs an address: l file address");
            const Address at = address(a[2]);
            for (std::size_t i = 0; i < bytes.size(); ++i) wr(static_cast<Address>(at + i), bytes[i]);
            m.nextDisassembly_ = at;
            return "ok: " + std::to_string(bytes.size()) + " bytes at " + h6(at) + "\n";
        }
        if (cmd == "sv") {
            if (a.size() < 4) throw MonitorError("usage: sv file from to");
            const Address from = address(a[2]), to = address(a[3]);
            if (to < from) throw MonitorError("'to' is below 'from'");
            if (static_cast<std::size_t>(to - from) + 1 > kMaxSpan) throw MonitorError("the range is larger than 100000 (hex) bytes");
            std::vector<Byte> bytes;
            for (Address x = from; x <= to; ++x) bytes.push_back(rd(x));
            writeBinaryFile(file, bytes);
            return "ok: " + std::to_string(bytes.size()) + " bytes\n";
        }
        if (cmd == "savestate") {
            writeBinaryFile(file, serialize(capture(cpu())));
            return "ok\n";
        }
        if (cmd == "loadstate") {
            restore(cpu(), deserializeSnapshot(readBinaryFile(file)));
            return "ok\n" + regsText();
        }
        if (cmd == "rom") {
            if (!m.amiga_) throw MonitorError("rom needs the amiga500 machine (machine amiga500)");
            const std::vector<Byte> rom = readBinaryFile(file);
            if (rom.empty()) throw MonitorError("the ROM file is empty");
            m.amiga_->loadRom(rom);
            m.amiga_->reset(cpu());
            return "ok: " + std::to_string(rom.size()) + " bytes, PC=" + h6(regs().pc) + "\n";
        }
    } catch (const IoError& e) {
        throw MonitorError(e.what());
    }
    throw MonitorError("unknown file command");
}

}  // namespace m68k
