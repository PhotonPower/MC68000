#pragma once

#include <functional>
#include <map>
#include <set>
#include <vector>

#include "m68k/instruction.hpp"
#include "m68k/memory.hpp"

namespace m68k {

/// A call through a library base register: `JSR -552(A6)` (the offset is negative, a multiple of 6).
struct LibraryCall {
    Address address = 0;  ///< the JSR/BSR instruction
    int offset = 0;       ///< displacement from A6
};

struct RoutineInfo {
    Address entry = 0;
    Address start = 0, end = 0;          ///< extent: lowest address and the address after the highest instruction
    unsigned instructions = 0;
    unsigned bytes = 0;
    std::vector<Address> callers;        ///< addresses of the instructions that call it
    std::vector<Address> callees;        ///< entries of the routines it calls (inside the analysed code)
    std::vector<Address> externalCalls;  ///< call targets outside the analysed code
    std::vector<LibraryCall> libraryCalls;
    int depth = 0;                       ///< 0 = calls no other routine; else 1 + the deepest callee
};

/// Code flow analysis over an address range [from, to) of a memory image: which bytes are code, where
/// the routines are, who calls whom. Like the disassembler's analysis (decode(), follow BSR/JSR/BRA/
/// Bcc/DBcc/JMP from entry points) but keeps the instructions and the call graph.
class Analysis {
public:
    Analysis(const Memory& view, Address from, Address to);

    /// Addresses for which `isData` returns true are never decoded as code (declared data regions).
    void setDataBarrier(std::function<bool(Address)> isData) { isData_ = std::move(isData); }

    void addEntry(Address address);
    void analyzeFlow();                               ///< follow the flow from all entry points
    void analyzeLinear(Address from, Address to);     ///< decode sequentially (for code reached by tables etc.)

    [[nodiscard]] Address from() const { return from_; }
    [[nodiscard]] Address to() const { return to_; }
    [[nodiscard]] const Instruction* at(Address a) const;
    /// True if the byte belongs to an analysed instruction (start or middle).
    [[nodiscard]] bool isCode(Address a) const;
    [[nodiscard]] const std::map<Address, Instruction>& code() const { return code_; }
    [[nodiscard]] std::size_t codeBytes() const;
    [[nodiscard]] const std::set<Address>& entries() const { return entries_; }

    [[nodiscard]] const std::set<Address>& callTargets() const { return callTargets_; }
    [[nodiscard]] const std::set<Address>& jumpTargets() const { return jumpTargets_; }
    [[nodiscard]] const std::set<Address>& dataReferences() const { return refs_; }

    /// Routines ordered by address; `depth` is computed over the call graph (cycles are cut).
    [[nodiscard]] std::vector<RoutineInfo> routines() const;

private:
    [[nodiscard]] bool inRange(Address a) const { return a >= from_ && a < to_; }
    void follow(Address a, std::vector<Address>& work);
    bool place(Address a, const Instruction& in);
    void note(Address from, const Instruction& in, std::vector<Address>* work);

    const Memory& mem_;
    Address from_, to_;
    std::function<bool(Address)> isData_;
    std::vector<std::uint8_t> state_;  // 0 unknown, 1 instruction start, 2 instruction body
    std::map<Address, Instruction> code_;
    std::set<Address> entries_, callTargets_, jumpTargets_, refs_;
    std::map<Address, std::set<Address>> callers_;  // call target -> call instructions
};

}  // namespace m68k
