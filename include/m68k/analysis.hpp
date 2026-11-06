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

/// A table of code addresses reached through an indirect jump.
struct JumpTable {
    enum class Kind {
        Branch,    ///< `JMP/JSR d(PC,Dn)` into a row of BRA (or JMP abs) instructions: every entry is code
        Offset,    ///< `MOVE.W tab(PC,Dn),Dm ; JMP base(PC,Dm)`: words, target = base + signed word
        Pointers   ///< a row of longword addresses of code
    };
    Kind kind = Kind::Branch;
    Address jump = 0;               ///< the JMP/JSR instruction (0 if the table was not found through one)
    Address table = 0;              ///< first byte of the table
    Address base = 0;               ///< Offset: the address the words are relative to
    unsigned bytes = 0;             ///< size of the table in bytes
    bool followed = true;           ///< the targets were analysed as code (false: only a suggestion, mark the region as pointers to follow it)
    std::vector<Address> slots;     ///< address of each entry
    std::vector<Address> targets;   ///< code address each entry leads to
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

    /// Declares [table, table+bytes) a table of longword code addresses (an annotated `pointers` region): every in-range even address becomes an entry point.
    /// Call before analyzeFlow().
    void addPointerTable(Address table, std::size_t bytes);

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
    /// Jump tables found (branch tables and offset tables are followed; pointer tables found by scanning are only suggested).
    [[nodiscard]] const std::vector<JumpTable>& jumpTables() const { return tables_; }
    /// True for the bytes of an offset table (data between the code).
    [[nodiscard]] bool isTableData(Address a) const;

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
    void detectJumpTable(Address a, const Instruction& in, std::vector<Address>& work);
    void scanPointerTables();

    const Memory& mem_;
    Address from_, to_;
    std::function<bool(Address)> isData_;
    std::vector<std::uint8_t> state_;  // 0 unknown, 1 instruction start, 2 instruction body, 3 jump table data
    std::vector<JumpTable> tables_;
    std::map<Address, Instruction> code_;
    std::set<Address> entries_, callTargets_, jumpTargets_, refs_;
    std::map<Address, std::set<Address>> callers_;  // call target -> call instructions
};

}  // namespace m68k
