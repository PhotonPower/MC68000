#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace m68k {

struct ExprResult {
    bool ok = false;               ///< false: syntax error or division by zero (see `error`)
    std::int64_t value = 0;
    bool undefined = false;        ///< an undefined symbol was used (value is then not final)
    std::string undefinedName;     ///< the first undefined symbol
    std::string error;
};

/// Looks up a symbol; returns false if it is not defined (yet).
using SymbolLookup = std::function<bool(const std::string& name, std::int64_t& value)>;

/// Integer expression evaluator (Motorola syntax).
///
/// Numbers: `123`, `$1F`, `0x1F`, `%1010`, `@17` (octal), `'A'` (up to four characters).
/// Operators (C precedence): `* / %`, `+ -`, `<< >>`, `&`, `^`, `|`; unary `- + ~`; parentheses.
/// `*` where a value is expected is the current address (`pc`). Values are 64 bit.
///
/// With `hexDefault` (monitor mode) a bare number such as `4000`, `FF` or `1B` is hexadecimal, `100.` is
/// decimal, and a name that is not a known symbol but consists of hex digits (`DEAD`) is a hex number;
/// known symbols always win.
class ExpressionEvaluator {
public:
    ExpressionEvaluator(SymbolLookup lookup, std::int64_t pc, bool hexDefault = false)
        : lookup_(std::move(lookup)), pc_(pc), hexDefault_(hexDefault) {}
    [[nodiscard]] ExprResult evaluate(const std::string& text) const;

private:
    SymbolLookup lookup_;
    std::int64_t pc_;
    bool hexDefault_;
};

}  // namespace m68k
