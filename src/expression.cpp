#include "m68k/expression.hpp"

#include <cctype>

namespace m68k {

namespace {

class Parser {
public:
    Parser(const std::string& s, const SymbolLookup& lookup, std::int64_t pc) : s_(s), lookup_(lookup), pc_(pc) {}

    ExprResult run() {
        ExprResult r;
        skip();
        if (i_ >= s_.size()) {
            r.error = "empty expression";
            return r;
        }
        const std::int64_t v = parseOr();
        skip();
        if (!err_.empty()) {
            r.error = err_;
            return r;
        }
        if (i_ != s_.size()) {
            r.error = std::string("unexpected '") + s_[i_] + "' in expression";
            return r;
        }
        r.ok = true;
        r.value = v;
        r.undefined = undefined_;
        r.undefinedName = undefinedName_;
        return r;
    }

private:
    void skip() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool peekIs(char c) {
        skip();
        return i_ < s_.size() && s_[i_] == c;
    }
    bool peekIs2(char a, char b) {
        skip();
        return i_ + 1 < s_.size() && s_[i_] == a && s_[i_ + 1] == b;
    }
    void fail(const std::string& m) {
        if (err_.empty()) err_ = m;
    }

    std::int64_t parseOr() {
        std::int64_t v = parseXor();
        while (err_.empty() && peekIs('|')) {
            ++i_;
            v |= parseXor();
        }
        return v;
    }
    std::int64_t parseXor() {
        std::int64_t v = parseAnd();
        while (err_.empty() && peekIs('^')) {
            ++i_;
            v ^= parseAnd();
        }
        return v;
    }
    std::int64_t parseAnd() {
        std::int64_t v = parseShift();
        while (err_.empty() && peekIs('&')) {
            ++i_;
            v &= parseShift();
        }
        return v;
    }
    std::int64_t parseShift() {
        std::int64_t v = parseAdd();
        while (err_.empty()) {
            if (peekIs2('<', '<')) {
                i_ += 2;
                const std::int64_t n = parseAdd();
                v = (n < 0 || n > 63) ? 0 : static_cast<std::int64_t>(static_cast<std::uint64_t>(v) << n);
            } else if (peekIs2('>', '>')) {
                i_ += 2;
                const std::int64_t n = parseAdd();
                v = (n < 0 || n > 63) ? (v < 0 ? -1 : 0) : (v >> n);
            } else {
                break;
            }
        }
        return v;
    }
    std::int64_t parseAdd() {
        std::int64_t v = parseMul();
        while (err_.empty()) {
            if (peekIs('+')) {
                ++i_;
                v += parseMul();
            } else if (peekIs('-')) {
                ++i_;
                v -= parseMul();
            } else {
                break;
            }
        }
        return v;
    }
    std::int64_t parseMul() {
        std::int64_t v = parseUnary();
        while (err_.empty()) {
            if (peekIs('*')) {
                ++i_;
                v *= parseUnary();
            } else if (peekIs('/')) {
                ++i_;
                const std::int64_t d = parseUnary();
                if (d == 0) {
                    fail("division by zero");
                    return 0;
                }
                v /= d;
            } else if (peekIs('%')) {
                ++i_;
                const std::int64_t d = parseUnary();
                if (d == 0) {
                    fail("division by zero");
                    return 0;
                }
                v %= d;
            } else {
                break;
            }
        }
        return v;
    }

    std::int64_t parseUnary() {
        skip();
        if (peekIs('-')) {
            ++i_;
            return -parseUnary();
        }
        if (peekIs('+')) {
            ++i_;
            return parseUnary();
        }
        if (peekIs('~')) {
            ++i_;
            return ~parseUnary();
        }
        return parsePrimary();
    }

    static int digitValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 99;
    }

    std::int64_t parseRadix(int radix, const char* what) {
        std::int64_t v = 0;
        std::size_t n = 0;
        while (i_ < s_.size() && digitValue(s_[i_]) < radix) {
            v = v * radix + digitValue(s_[i_]);
            ++i_;
            ++n;
        }
        if (n == 0) fail(std::string("missing digits in ") + what + " number");
        return v;
    }

    std::int64_t parsePrimary() {
        skip();
        if (i_ >= s_.size()) {
            fail("unexpected end of expression");
            return 0;
        }
        const char c = s_[i_];
        if (c == '(') {
            ++i_;
            const std::int64_t v = parseOr();
            if (!peekIs(')')) {
                fail("missing ')'");
                return 0;
            }
            ++i_;
            return v;
        }
        if (c == '*') {
            ++i_;
            return pc_;
        }
        if (c == '$') {
            ++i_;
            return parseRadix(16, "hexadecimal");
        }
        if (c == '%') {
            ++i_;
            return parseRadix(2, "binary");
        }
        if (c == '@') {
            ++i_;
            return parseRadix(8, "octal");
        }
        if (c == '\'') {
            ++i_;
            std::int64_t v = 0;
            int n = 0;
            while (i_ < s_.size() && s_[i_] != '\'') {
                v = (v << 8) | static_cast<unsigned char>(s_[i_]);
                ++i_;
                ++n;
            }
            if (i_ >= s_.size() || n == 0 || n > 4) {
                fail("bad character constant");
                return 0;
            }
            ++i_;
            return v;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            if (c == '0' && i_ + 1 < s_.size() && (s_[i_ + 1] == 'x' || s_[i_ + 1] == 'X')) {
                i_ += 2;
                return parseRadix(16, "hexadecimal");
            }
            return parseRadix(10, "decimal");
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
            std::string name;
            while (i_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '_' || s_[i_] == '.')) {
                name += s_[i_++];
            }
            std::int64_t v = 0;
            if (lookup_ && lookup_(name, v)) return v;
            if (!undefined_) undefinedName_ = name;
            undefined_ = true;
            return 0;
        }
        fail(std::string("unexpected '") + c + "' in expression");
        return 0;
    }

    const std::string& s_;
    const SymbolLookup& lookup_;
    std::int64_t pc_;
    std::size_t i_ = 0;
    std::string err_;
    bool undefined_ = false;
    std::string undefinedName_;
};

}  // namespace

ExprResult ExpressionEvaluator::evaluate(const std::string& text) const {
    return Parser(text, lookup_, pc_).run();
}

}  // namespace m68k
