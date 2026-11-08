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
        Pointers   ///< a row of longword addresses of code (followed when found behind `MOVEA.L tab(PC,Dn),An ; JMP (An)` or `LEA tab,Am ; MOVEA.L 0(Am,Dn),An ; JMP (An)`)
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

/// A guess about the type of bytes that are neither code nor annotated.
struct DataGuess {
    enum class Kind {
        Text,      ///< a string (printable characters, usually NUL-terminated)
        Zeros,     ///< a run of zero bytes (padding, an empty buffer)
        Pointers,  ///< a table of longword addresses of strings
        Words,     ///< a table of 16 bit numbers: code walks through it with .W accesses
        Longs      ///< a table of 32 bit numbers: code walks through it with .L accesses
    };
    Kind kind = Kind::Text;
    Address start = 0;
    unsigned length = 0;       ///< includes a terminating NUL (and NUL padding up to an even length)
    std::string reason;        ///< why this was guessed
    std::string preview;       ///< Text: the first characters
};

struct RoutineInfo {
    Address entry = 0;
    Address start = 0, end = 0;          ///< extent: lowest address and the address after the highest instruction
    unsigned instructions = 0;
    unsigned bytes = 0;
    /// The routine's own code as contiguous blocks [start, end). `start`/`end` are only the hull: a shared exit at the end of the program (an error
    /// path that many routines branch to) makes the hull take in the neighbours, the blocks do not.
    std::vector<std::pair<Address, Address>> blocks;
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
    /// Types of the bytes that are not code: strings, zero runs and tables of string pointers. `claimed(a)` is true for bytes that are annotated already.
    /// Guesses are never certain: a string is recognised if it is referenced by an instruction, ends in a NUL, or is long and mostly letters.
    [[nodiscard]] std::vector<DataGuess> guessData(const std::function<bool(Address)>& claimed = nullptr) const;
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
    void detectPointerJump(Address a, const Instruction& in, std::vector<Address>& work);
    [[nodiscard]] bool looksLikeText(Address t) const;
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
