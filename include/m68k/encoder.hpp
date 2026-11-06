#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "m68k/types.hpp"

namespace m68k {

/// Result of evaluating an expression for the encoder.
struct EncodeValue {
    std::int64_t value = 0;
    bool undefined = false;  ///< forward reference that is not resolved yet (value is a placeholder)
    std::string error;       ///< non-empty: evaluation failed
};
using ValueFn = std::function<EncodeValue(const std::string& expression)>;

/// A 32 bit field of an instruction that holds an address inside another (or the same) SECTION and must be
/// relocated by the loader.
struct Relocation {
    unsigned offset = 0;  ///< byte offset of the long inside the instruction
    unsigned section = 0; ///< 0-based index of the target section
};

struct EncodeRequest {
    std::string mnemonic;               ///< without size suffix, any case: "move", "BRA"
    char size = 0;                      ///< 0 (none), 'B', 'W', 'L' or 'S' (short branch)
    std::vector<std::string> operands;  ///< operand texts in Motorola syntax
    Address pc = 0;                     ///< address of the instruction (offset inside its section for SECTION programs)
    bool optimize = false;              ///< use shorter equivalent forms where the result is identical (see AssemblerOptions::optimize)
    int section = 0;                    ///< 0 = absolute program; else the tag of the instruction's section (tagOfSection(), see section.hpp)
    /// Sizing level remembered from earlier passes (0 = smallest encoding). Unsized branches and
    /// absolute addresses only ever grow: level 1 means the larger form.
    int hint = 0;
};

struct EncodeResult {
    bool ok = false;
    std::string error;
    std::vector<Byte> bytes;
    std::vector<Relocation> relocs;  ///< long fields that hold section-relative addresses
    int level = 0;  ///< sizing level used; feed it back as `hint` in the next pass
};

/// Encodes one instruction. Unsized branches choose .S or .W, unsized absolute addresses .W or .L
/// (the shorter form that fits, but never shorter than `hint`).
///
/// Operand order, syntax and the forms that exist are described in docs/assembler-syntax.md.
/// PC-relative operands and branches take the *target address* (`label(PC)`, `BRA label`).
[[nodiscard]] EncodeResult encodeInstruction(const EncodeRequest& request, const ValueFn& value);

/// True if `name` (any case) is an instruction mnemonic known to the encoder.
[[nodiscard]] bool isMnemonic(const std::string& name);

/// Splits an operand list at top-level commas (not inside parentheses or quotes).
[[nodiscard]] std::vector<std::string> splitOperands(const std::string& text);

}  // namespace m68k
