#pragma once

// Internal: the per-request worker of Session (shared by session.cpp and session_analysis.cpp).

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
#include "m68k/session.hpp"

namespace m68k {

namespace session_detail {

inline constexpr std::uint64_t kDefaultLimit = 1000000;
/// Upper bound for max_instructions and for `step n`: a client must not be able to hang the server with an endless loop and a huge limit.
inline constexpr std::int64_t kMaxLimit = 100000000;
inline constexpr Address kGenericStackTop = 0x80000;
inline constexpr Address kReturnSentinel = 0xFFFFF0;
inline constexpr std::size_t kMaxRead = 4096;
inline constexpr std::size_t kMaxWrite = 65536;

struct SessionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::string hex(std::uint64_t v, int digits) {
    if (digits < 1) digits = 1;
    if (digits > 16) digits = 16;
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%0*llX", digits, static_cast<unsigned long long>(v));
    return buf;
}
inline std::string hex32(Long v) { return hex(v, 8); }
inline std::string hexAddr(Address a) { return hex(a & kAddressMask, 6); }

inline std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline std::string bytesToHex(const std::vector<Byte>& b) {
    std::string s;
    for (const Byte x : b) s += hexDigits(x, 2);
    return s;
}

inline const char* stopName(StopReason r) {
    switch (r) {
        case StopReason::None: return "stepped";
        case StopReason::Limit: return "instruction_limit";
        case StopReason::Breakpoint: return "breakpoint";
        case StopReason::Watchpoint: return "watchpoint";
        case StopReason::Reached: return "address";
        case StopReason::Returned: return "returned";
        case StopReason::WrittenCode: return "unpacked";
        case StopReason::Stopped: return "stopped";
        case StopReason::Halted: return "halted";
        case StopReason::Requested: return "exit";
    }
    return "?";
}

}  // namespace session_detail

using namespace session_detail;

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
        if (const auto a = s.ann_.addressOf(name)) {
            v = *a;
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
        if (n > kMaxLimit) throw SessionError("max_instructions must be at most " + std::to_string(kMaxLimit) + " (call run again to go on)");
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
        if (r.reason == StopReason::Requested) j.set("message", s.cpu_->stopMessage());
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
        if (const auto n = s.ann_.nameAt(a)) return *n;
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
    Json cmdUnpack(const std::string& cmd);
    Json cmdCall();
    Json cmdBreaks();
    Json cmdWatch(bool add);
    Json cmdCheckpoint(const std::string& cmd);
    Json cmdDiff();
    Json cmdState();
    Json cmdSetState();
    Json cmdMachine();
    Json cmdIo();
    Json cmdOs(const std::string& cmd);
    /// Counts instructions fetched from empty memory (unmapped, or the ROM area without a ROM) while it is alive; only on the Amiga machine.
    struct EmptyFetchGuard {
        Impl& impl;
        std::function<void(const Registers&, Address, Word)> previous;
        unsigned count = 0;
        Address first = 0;
        explicit EmptyFetchGuard(Impl& i);
        ~EmptyFetchGuard();
        void report(Json& j) const;
    };
    Json cmdHelp();
    // reverse engineering (session_analysis.cpp)
    bool isAnalysisCommand(const std::string& cmd) const;
    Json cmdAnalysis(const std::string& cmd);
    void rememberRange(Address from, Address to) {
        if (to > from) s.programRange_ = std::make_pair(from, to);
        s.hunks_.clear();   // a hunk load sets them again right afterwards
    }
};

}  // namespace m68k
