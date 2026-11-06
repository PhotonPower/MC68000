#pragma once

#include <functional>
#include <string>

#include "m68k/format.hpp"
#include "m68k/instruction.hpp"
#include "m68k/memory.hpp"

namespace m68k {

struct DecodeOptions {
    NumberStyle style = NumberStyle::Dollar;
    /// Optional: returns a symbol name for an address ("" = none). Used for branch targets,
    /// absolute addresses and PC-relative operands.
    std::function<std::string(Address)> symbolizer;
};

/// Decodes the instruction at `address` (stateless). Never fails: anything that is not a valid
/// MC68000 instruction - including Line-A/Line-F words, MC68010+ opcodes, odd branch targets and
/// non-canonical extension words - becomes `DC.W $xxxx` (or `DC.B` at an odd address).
///
/// "Non-canonical" means an extension word the assembler would never produce, e.g. a byte
/// immediate with a non-zero upper byte or an index word with the 68020 scale bits set. Treating
/// these as data keeps `assemble(disassemble(x)) == x` exact.
[[nodiscard]] Instruction decode(const Memory& memory, Address address, const DecodeOptions& options = {});

}  // namespace m68k
