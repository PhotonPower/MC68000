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
class ExpressionEvaluator {
public:
    ExpressionEvaluator(SymbolLookup lookup, std::int64_t pc) : lookup_(std::move(lookup)), pc_(pc) {}
    [[nodiscard]] ExprResult evaluate(const std::string& text) const;

private:
    SymbolLookup lookup_;
    std::int64_t pc_;
};

}  // namespace m68k
