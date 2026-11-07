#pragma once

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "m68k/amiga.hpp"
#include "m68k/cpu.hpp"

namespace m68k {

/// A call of a library function made by the program.
struct LibCall {
    std::string library;   ///< "exec.library", "dos.library", ...
    int offset = 0;        ///< the negative vector offset from the library base (JSR -552(A6))
    std::string function;  ///< "OpenLibrary" if known, else ""
    bool emulated = false; ///< false: only logged; the call returned D0 = 0 and did nothing else
    unsigned minVersion = 0; ///< first library version that has the function (33 = Kickstart 1.2, 36 = Kickstart 2.0); 0 = not in the tables
    Address caller = 0;    ///< return address of the call
    Address callSite = 0;  ///< address of the JSR instruction itself (0 if it could not be found): where a comment belongs
    Long d0 = 0, d1 = 0, d2 = 0, d3 = 0, a0 = 0, a1 = 0;  ///< arguments (registers at the call)
    Long result = 0;       ///< D0 afterwards
    std::string note;      ///< e.g. the name given to OpenLibrary, or what a Write printed
};

/// Just enough of AmigaOS to let programs run past their library calls, so that their memory effects can be looked at.
///
/// There is an `ExecBase` (its address is at absolute address 4, as on the real machine) with a jump table, and
/// every opened library gets a base with a jump table below it - real `JMP` instructions that lead to addresses
/// the processor does not execute but hands to this class (Cpu::executeHook). Programs can therefore call
/// `JSR -552(A6)` exactly as they would on an Amiga.
///
/// **Only a handful of functions are emulated** (docs/amiga500.md has the list): exec memory (AllocMem, FreeMem,
/// AllocVec, FreeVec, AvailMem, CopyMem), lists (AddHead, AddTail, Insert, Remove, RemHead, RemTail, Enqueue, FindName),
/// OpenLibrary/CloseLibrary, FindTask, Forbid/Permit/Disable/Enable; dos Output/Input/Write/Read/Close/Open/Lock/IoErr/Delay/Exit. Every other call - known by name or not - is **logged** (library, offset, name,
/// registers) and returns D0 = 0. Nothing blocks, nothing is scheduled, there are no files, no windows, no graphics.
/// The function names and offsets come from the Amiga include files as remembered; check them before relying on them.
///
/// Memory map (chip RAM `chipKb` >= 512 KiB): the program area is below chip/2; the heap for AllocMem runs from
/// chip/2 up to a 16 KiB stack area; the OS structures take the top 24 KiB.
class AmigaOs {
public:
    struct State {
        std::vector<std::pair<Address, Address>> blocks;  ///< allocated (address, size)
        std::vector<std::pair<std::string, Address>> libraries;  ///< opened libraries and their bases
        std::string console;
        Long ioErr = 0;
    };

    AmigaOs(Memory& memory, Amiga500& amiga);

    /// False if the chip RAM is too small (< 512 KiB).
    [[nodiscard]] bool supported() const { return amiga_.config().chipKb >= 512; }

    void install(Cpu& cpu);
    void uninstall(Cpu& cpu);
    /// Builds ExecBase and the structures in memory and forgets allocations, libraries, console and log.
    void reset();
    /// Prepares a freshly loaded program the way AmigaDOS starts it: D0 = A0 = 0, and the return address on the stack leads to
    /// an exit stub (the program returning ends the run with StopReason::Requested).
    void prepareProgram(Cpu& cpu);

    [[nodiscard]] Address execBase() const { return osBase_ + 0xE00; }
    /// [first, end) of the memory that the emulated system keeps its own structures in (ExecBase, library bases with their jump tables, the task).
    [[nodiscard]] std::pair<Address, Address> systemRange() const { return {osBase_, osBase_ + 0x6000}; }
    [[nodiscard]] Address stackTop() const { return osBase_ - 16; }
    [[nodiscard]] Address heapBase() const { return heapBase_; }
    [[nodiscard]] Address heapEnd() const { return heapEnd_; }
    [[nodiscard]] Address exitAddress() const { return kExitStub; }

    [[nodiscard]] const std::deque<LibCall>& log() const { return log_; }
    void clearLog() { log_.clear(); }
    [[nodiscard]] const std::string& console() const { return state_.console; }
    void clearConsole() { state_.console.clear(); }

    [[nodiscard]] State state() const { return state_; }
    void setState(const State& s);

    /// Name of a well-known function ("" if not in the tables): exec.library -552 -> "OpenLibrary".
    [[nodiscard]] static std::string functionName(const std::string& library, int offset);
    /// First library version that has the function (33 = Kickstart 1.2/1.3, 36 = Kickstart 2.0), 0 if it is not in the tables.
    [[nodiscard]] static unsigned functionVersion(const std::string& library, int offset);

private:
    static constexpr Address kStubBase = 0xF00000;
    static constexpr Address kExitStub = 0xF7F000;
    static constexpr unsigned kMaxLibraries = 7;  // exec + six more

    bool onExecute(Cpu& cpu);
    void dispatch(Cpu& cpu, LibCall& call, const std::string& lib);
    Address allocate(Long size, Long requirements);
    void release(Address block);
    Address openLibrary(const std::string& name);
    void buildLibrary(unsigned index, const std::string& name);
    std::string readString(Address a, std::size_t max = 80) const;
    Long rd32(Address a) const;
    void wr32(Address a, Long v);
    void wr16(Address a, Word v);

    Memory& mem_;
    Amiga500& amiga_;
    Address osBase_, heapBase_, heapEnd_;
    State state_;
    std::deque<LibCall> log_;
};

}  // namespace m68k
