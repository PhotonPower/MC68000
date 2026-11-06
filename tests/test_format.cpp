#include "m68k/format.hpp"

#include "test_util.hpp"

using namespace m68k;

int main() {
    CHECK_EQ(hexDigits(0, 2), std::string("00"));
    CHECK_EQ(hexDigits(0xAB, 2), std::string("AB"));
    CHECK_EQ(hexDigits(0x1F, 4), std::string("001F"));
    CHECK_EQ(hexDigits(0xFFFFFF, 6), std::string("FFFFFF"));
    CHECK_EQ(hexDigits(0x12345, 2), std::string("12345"));  // never truncates

    CHECK_EQ(formatHex(0x1F, 2, NumberStyle::Dollar), std::string("$1F"));
    CHECK_EQ(formatHex(0x1F, 2, NumberStyle::CPrefix), std::string("0x1F"));
    CHECK_EQ(formatHex(0x1F, 2, NumberStyle::HSuffix), std::string("1FH"));
    CHECK_EQ(formatHex(0xFF, 2, NumberStyle::HSuffix), std::string("0FFH"));

    NumberStyle s = NumberStyle::CPrefix;
    CHECK(parseNumberStyle("Dollar", s));
    CHECK(s == NumberStyle::Dollar);
    CHECK(parseNumberStyle("0x", s));
    CHECK(s == NumberStyle::CPrefix);
    CHECK(parseNumberStyle("H", s));
    CHECK(s == NumberStyle::HSuffix);
    CHECK(!parseNumberStyle("amp", s));

    return finish("format");
}
