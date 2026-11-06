#pragma once

#include <string>

namespace m68k {

/// How numbers are written in disassembler output.
enum class NumberStyle {
    Dollar,   ///< $1F   (Motorola syntax, default)
    CPrefix,  ///< 0x1F
    HSuffix   ///< 1FH (a leading 0 is added if the first digit is a letter)
};

/// Upper-case hexadecimal digits without any prefix, zero-padded to `digits`.
std::string hexDigits(unsigned value, int digits);

/// Hexadecimal number in the requested style, zero-padded to `digits`.
std::string formatHex(unsigned value, int digits, NumberStyle style);

/// Parses "dollar", "c", "h" (case-insensitive). Returns false on unknown names.
bool parseNumberStyle(const std::string& name, NumberStyle& out);

}  // namespace m68k
