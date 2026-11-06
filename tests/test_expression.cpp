#include "m68k/expression.hpp"

#include <map>

#include "test_util.hpp"

using namespace m68k;

namespace {
std::map<std::string, std::int64_t> g_symbols = {{"foo", 100}, {"bar", 7}, {".loc", 5}};
ExprResult eval(const std::string& s, std::int64_t pc = 0x1000) {
    const ExpressionEvaluator ev(
        [](const std::string& n, std::int64_t& v) {
            const auto it = g_symbols.find(n);
            if (it == g_symbols.end()) return false;
            v = it->second;
            return true;
        },
        pc);
    return ev.evaluate(s);
}
std::int64_t val(const std::string& s) {
    const ExprResult r = eval(s);
    return r.ok ? r.value : -999999;
}
}  // namespace

int main() {
    // number formats
    CHECK_EQ(val("123"), 123);
    CHECK_EQ(val("$1F"), 31);
    CHECK_EQ(val("$1f"), 31);
    CHECK_EQ(val("0x1F"), 31);
    CHECK_EQ(val("%1010"), 10);
    CHECK_EQ(val("@17"), 15);
    CHECK_EQ(val("'A'"), 65);
    CHECK_EQ(val("'AB'"), 0x4142);
    CHECK_EQ(val("0"), 0);

    // operators and precedence
    CHECK_EQ(val("1+2*3"), 7);
    CHECK_EQ(val("(1+2)*3"), 9);
    CHECK_EQ(val("10-3-2"), 5);
    CHECK_EQ(val("100/7"), 14);
    CHECK_EQ(val("100%7"), 2);
    CHECK_EQ(val("1<<4"), 16);
    CHECK_EQ(val("$100>>4"), 16);
    CHECK_EQ(val("$F0&$3C"), 0x30);
    CHECK_EQ(val("$F0|$0F"), 0xFF);
    CHECK_EQ(val("$FF^$0F"), 0xF0);
    CHECK_EQ(val("~0"), -1);
    CHECK_EQ(val("-5+2"), -3);
    CHECK_EQ(val("--5"), 5);
    CHECK_EQ(val("1|2&3"), 3);        // & binds tighter than |
    CHECK_EQ(val("1+2<<1"), 6);       // + binds tighter than <<
    CHECK_EQ(val(" 1 + 2 "), 3);
    CHECK_EQ(val("5%3+%11"), 5);      // modulo after a value, binary literal where a value starts

    // location counter and symbols
    CHECK_EQ(val("*"), 0x1000);
    CHECK_EQ(val("*+4"), 0x1004);
    CHECK_EQ(val("2*3"), 6);
    CHECK_EQ(val("foo+bar"), 107);
    CHECK_EQ(val("foo-*"), 100 - 0x1000);
    CHECK_EQ(val(".loc*2"), 10);

    // undefined symbols
    {
        const ExprResult r = eval("nothere+1");
        CHECK(r.ok);
        CHECK(r.undefined);
        CHECK_EQ(r.undefinedName, std::string("nothere"));
        CHECK_EQ(r.value, 1);
    }
    CHECK(!eval("foo+1").undefined);

    // errors
    CHECK(!eval("").ok);
    CHECK(!eval("1+").ok);
    CHECK(!eval("(1+2").ok);
    CHECK(!eval("1/0").ok);
    CHECK(!eval("1%0").ok);
    CHECK(!eval("$").ok);
    CHECK(!eval("1 2").ok);
    CHECK(!eval("'ABCDE'").ok);
    CHECK(!eval("'").ok);
    CHECK(!eval("#5").ok);

    return finish("expression");
}
