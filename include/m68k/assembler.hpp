#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "m68k/section.hpp"
#include "m68k/types.hpp"

namespace m68k {

struct AssemblerOptions {
    Address origin = 0;                                  ///< start address before the first ORG
    std::map<std::string, std::int64_t> predefined;      ///< symbols defined before assembly
    /// Shorter equivalent forms (never changes registers, flags or memory): `ADD/SUB #1..8` -> ADDQ/SUBQ, `MOVE.L #-128..127,Dn` -> MOVEQ,
    /// `ADD/SUB/MOVE/CMP.L #x,An` with x in 16 bits -> `.W` form. Off by default so that source and output stay one-to-one.
    bool optimize = false;
    bool exportAllSymbols = false;                       ///< SECTION programs: put every label into the hunk symbol table (not only XDEF ones)
};

struct Diagnostic {
    int line = 0;  ///< 1-based source line
    std::string message;
};

/// A contiguous block of output bytes.
struct Segment {
    Address address = 0;
    std::vector<Byte> bytes;
};

/// A longword inside a hunk that the loader must adjust by the base address of another hunk.
struct HunkReloc {
    Address offset = 0;   ///< offset inside this section
    unsigned target = 0;  ///< 0-based index of the section whose base is added
};

/// One section of a SECTION program (it becomes one hunk).
struct HunkSection {
    enum class Kind { Code, Data, Bss };
    std::string name;
    Kind kind = Kind::Code;
    bool chip = false;   ///< `_C`: must be loaded into chip RAM
    bool fast = false;   ///< `_F`: fast RAM preferred
    std::vector<Byte> bytes;          ///< contents (empty for BSS)
    Address bssSize = 0;              ///< size of a BSS section in bytes
    std::vector<HunkReloc> relocs;
    std::vector<std::pair<std::string, Address>> symbols;  ///< exported labels (XDEF, or all with exportAllSymbols)

    [[nodiscard]] Address size() const { return kind == Kind::Bss ? bssSize : static_cast<Address>(bytes.size()); }
};

struct ListingLine {
    int line = 0;
    int section = -1;   ///< SECTION programs: index of the section, else -1
    Address address = 0;  ///< address; offset inside the section for SECTION programs
    std::vector<Byte> bytes;
    std::string source;
};

struct AssemblyResult {
    std::vector<Segment> segments;
    std::vector<Diagnostic> errors;
    std::map<std::string, std::int64_t> symbols;
    std::vector<ListingLine> listing;
    /// SECTION programs ("relocatable"): the sections in order of first appearance. `segments` is then empty, `symbols`
    /// holds offsets inside the section named in `symbolSection` (-1 = plain number, e.g. EQU).
    bool hunk = false;
    std::vector<HunkSection> sections;
    std::map<std::string, int> symbolSection;

    [[nodiscard]] bool ok() const { return errors.empty(); }

    /// Lowest address of any output byte (0 if there is none).
    [[nodiscard]] Address lowest() const;
    /// All segments as one raw image starting at lowest(); gaps are filled with zero bytes.
    [[nodiscard]] std::vector<Byte> flatten() const;
    /// The listing as text: line number, address, bytes, source.
    [[nodiscard]] std::string formatListing() const;
    /// The symbol table as text ("NAME = $XXXXXXXX"), sorted by value, then name.
    [[nodiscard]] std::string formatSymbols() const;
};

/// Assembles Motorola-syntax source text; see docs/assembler-syntax.md.
///
/// The assembler runs several passes until symbol values and the sizes chosen for unsized branches
/// and absolute addresses stop changing; sizes only ever grow, so this converges. Errors do not stop
/// the run; all errors of the final pass are collected (the bytes of faulty lines are omitted).
[[nodiscard]] AssemblyResult assemble(const std::string& source, const AssemblerOptions& options = {});

}  // namespace m68k
