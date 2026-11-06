#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "m68k/memory.hpp"
#include "m68k/types.hpp"

namespace m68k {

// Status register bits.
inline constexpr Word kSrTrace = 0x8000;
inline constexpr Word kSrSupervisor = 0x2000;
inline constexpr Word kSrIntMask = 0x0700;
inline constexpr Word kFlagX = 0x10, kFlagN = 0x08, kFlagZ = 0x04, kFlagV = 0x02, kFlagC = 0x01;
inline constexpr Word kSrValidBits = 0xA71F;  // T, S, interrupt mask, X N Z V C

/// Exception vector numbers.
enum Vector : unsigned {
    kVecReset = 0, kVecBusError = 2, kVecAddressError = 3, kVecIllegal = 4, kVecZeroDivide = 5, kVecChk = 6,
    kVecTrapV = 7, kVecPrivilege = 8, kVecTrace = 9, kVecLineA = 10, kVecLineF = 11, kVecAutovector = 24,
    kVecTrap0 = 32
};

struct Registers {
    std::array<Long, 8> d{};
    std::array<Long, 8> a{};  ///< a[7] is the stack pointer in use (USP in user mode, SSP in supervisor mode)
    Long inactiveSp = 0;      ///< the stack pointer that is not in use
    Address pc = 0;
    Word sr = 0x2700;

    [[nodiscard]] bool supervisor() const { return (sr & kSrSupervisor) != 0; }
    [[nodiscard]] Long usp() const { return supervisor() ? inactiveSp : a[7]; }
    [[nodiscard]] Long ssp() const { return supervisor() ? a[7] : inactiveSp; }
};

enum class StopReason {
    None,        ///< one instruction was executed
    Limit,       ///< the instruction limit was reached
    Breakpoint,  ///< pc is at a breakpoint (the instruction there has not run)
    Watchpoint,  ///< the last instruction touched a watched address
    Reached,     ///< pc reached the requested address
    Returned,    ///< the call depth dropped below its starting value (RTS/RTR/RTE)
    Stopped,     ///< STOP instruction: waiting for an interrupt
    Halted,      ///< double fault: the processor has stopped
    Requested    ///< an execute hook asked to stop (see Cpu::stopMessage(), e.g. the program called Exit())
};

struct StepResult {
    StopReason reason = StopReason::None;
    unsigned exception = 0;  ///< vector number if the instruction (or its fetch) raised an exception, else 0
};

struct RunResult {
    StopReason reason = StopReason::None;
    std::uint64_t instructions = 0;  ///< instructions executed by this call
    Address watchAddress = 0;        ///< for Watchpoint: the address that was touched
};

/// Everything that defines the processor's state besides the memory (breakpoints and hooks are not part of it).
struct CpuState {
    Registers regs;
    bool halted = false, stopped = false;
    std::uint64_t instructions = 0;
    std::uint64_t exceptions = 0;   ///< exceptions and interrupts taken so far
    unsigned lastException = 0;     ///< vector of the most recent one
    int depth = 0;                  ///< call depth (see Cpu::runUntilReturn)
    unsigned irqLevel = 0;          ///< pending interrupt request
    int irqVector = -1;
};

/// MC68000 processor core.
///
/// Functional emulation: every instruction yields the correct registers, flags and memory contents,
/// including exceptions, privilege checks, address errors and interrupts. There is **no timing**: no
/// cycle counts, no prefetch queue, no bus cycles. Opcode validity comes from the same instruction
/// table as the disassembler, so both agree on what an MC68000 instruction is.
///
/// Known simplifications (docs/emulator.md): the PC saved in an address-error frame is the PC after
/// the words fetched so far; the bus-error exception is never raised by the core; MOVEM does not do
/// the extra read cycle of the real chip.
class Cpu {
public:
    explicit Cpu(Memory& memory);

    Registers& registers() { return r_; }
    [[nodiscard]] const Registers& registers() const { return r_; }
    Memory& memory() { return mem_; }
    [[nodiscard]] const Memory& memory() const { return mem_; }
    /// Address of the instruction being (or last) executed; valid inside hooks.
    [[nodiscard]] Address instructionAddress() const { return startPc_; }

    /// Writes the status register; switches the active stack pointer if the S bit changes.
    void setSr(Word value);
    void setFlags(Word ccrBits) { r_.sr = static_cast<Word>((r_.sr & ~0x1FU) | (ccrBits & 0x1FU)); }

    /// Reset: supervisor mode, interrupt mask 7, SSP and PC loaded from vectors 0 and 1.
    void reset();

    // ---- running ----------------------------------------------------------------------------------
    /// Executes one instruction (or processes a pending interrupt). Breakpoints are ignored.
    StepResult step();
    /// Runs at most `maxInstructions`; stops at breakpoints and watchpoints. The instruction at the
    /// starting pc runs even if a breakpoint is there, so a stopped program can be continued.
    RunResult run(std::uint64_t maxInstructions);
    /// Runs until pc == `address` (checked before each instruction).
    RunResult runUntil(Address address, std::uint64_t maxInstructions);
    /// Runs until the current subroutine returns (RTS/RTR/RTE with the call depth below its start).
    RunResult runUntilReturn(std::uint64_t maxInstructions);
    /// Like step(), but a BSR/JSR is run to completion (until it returns to the next instruction).
    RunResult stepOver(std::uint64_t maxInstructions);
    /// Calls a subroutine: pushes `returnAddress`, sets pc and runs until pc == `returnAddress`.
    RunResult call(Address entry, Address returnAddress, std::uint64_t maxInstructions);

    // ---- debugging --------------------------------------------------------------------------------
    void addBreakpoint(Address a) { breakpoints_.insert(a & kAddressMask); }
    void removeBreakpoint(Address a) { breakpoints_.erase(a & kAddressMask); }
    void clearBreakpoints() { breakpoints_.clear(); }
    [[nodiscard]] const std::set<Address>& breakpoints() const { return breakpoints_; }

    /// Watches [start, start+length) for reads and/or writes; run() stops after the instruction.
    void addWatchpoint(Address start, Address length, bool onRead, bool onWrite);
    void clearWatchpoints() { watches_.clear(); }

    /// Called before every instruction with its address and opcode word.
    std::function<void(const Registers&, Address, Word)> traceHook;
    /// Called for every data access: address, size, value, true for writes.
    std::function<void(Address, Size, Long, bool)> accessHook;
    /// Replaces a read (return true and set `value` to handle it, e.g. device registers or a ROM overlay).
    /// Used for data reads, instruction fetches and exception vector reads; only data accesses are
    /// reported to accessHook and watchpoints.
    std::function<bool(Address, Size, Long& value)> readOverride;
    /// Intercepts a write (return true to swallow it).
    std::function<bool(Address, Size, Long)> writeOverride;
    /// RESET instruction (resets external devices).
    std::function<void()> resetHook;
    /// Called instead of executing an instruction when the PC is in [hookLow, hookHigh). Return true if it
    /// handled the instruction (it must then have moved the PC itself, e.g. with returnFromSubroutine()).
    /// Used to emulate operating-system functions (docs/amiga500.md).
    std::function<bool(Cpu&)> executeHook;
    Address hookLow = 0, hookHigh = 0;

    // ---- interrupts and state ----------------------------------------------------------------------
    /// Requests an interrupt of level 1..7 (0 clears it). It is taken before the next instruction if
    /// its level is above the mask in SR (level 7 is non-maskable). `vector` < 0 selects the autovector.
    void interrupt(unsigned level, int vector = -1);

    [[nodiscard]] CpuState state() const;
    void setState(const CpuState& s);
    /// Exceptions and interrupts taken since the last reset of the counters (part of the state).
    [[nodiscard]] std::uint64_t exceptionCount() const { return exceptions_; }
    [[nodiscard]] unsigned lastException() const { return lastException_; }

    /// For execute hooks: performs an RTS (pops the PC, adjusts the call depth).
    void returnFromSubroutine();
    /// For execute hooks: ends the current run() with StopReason::Requested.
    void requestStop(std::string message) {
        stopRequested_ = true;
        stopMessage_ = std::move(message);
    }
    [[nodiscard]] const std::string& stopMessage() const { return stopMessage_; }

    [[nodiscard]] bool halted() const { return halted_; }
    [[nodiscard]] bool stopped() const { return stopped_; }
    [[nodiscard]] std::uint64_t instructionCount() const { return instructions_; }
    [[nodiscard]] int callDepth() const { return depth_; }

    // ---- direct memory access with the processor's rules (address errors, hooks) ------------------------
    [[nodiscard]] Long peek(Address a, Size s) const;  ///< no hooks, no faults

private:
    friend class Exec;
    struct Watch {
        Address start, end;
        bool onRead, onWrite;
    };
    struct Fault;  // internal exception signal

    Memory& mem_;
    Registers r_;
    std::set<Address> breakpoints_;
    std::vector<Watch> watches_;
    bool watchHit_ = false;
    Address watchAddress_ = 0;
    bool halted_ = false, stopped_ = false;
    bool stopRequested_ = false;
    std::string stopMessage_;
    std::uint64_t instructions_ = 0;
    std::uint64_t exceptions_ = 0;
    unsigned lastException_ = 0;
    int depth_ = 0;
    unsigned irqLevel_ = 0;
    int irqVector_ = -1;
    Address startPc_ = 0;
    Word opcode_ = 0;

    RunResult runLoop(std::uint64_t maxInstructions, std::optional<Address> until, bool untilReturn);
    [[nodiscard]] Long rawRead(Address a, Size s) const;  // instruction fetch / vector read: readOverride, else memory
};

}  // namespace m68k
