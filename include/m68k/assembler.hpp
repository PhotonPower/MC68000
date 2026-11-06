#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "m68k/types.hpp"

namespace m68k {

struct AssemblerOptions {
    Address origin = 0;                                  ///< start address before the first ORG
    std::map<std::string, std::int64_t> predefined;      ///< symbols defined before assembly
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

struct ListingLine {
    int line = 0;
    Address address = 0;
    std::vector<Byte> bytes;
    std::string source;
};

struct AssemblyResult {
    std::vector<Segment> segments;
    std::vector<Diagnostic> errors;
    std::map<std::string, std::int64_t> symbols;
    std::vector<ListingLine> listing;

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
