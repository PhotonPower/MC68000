#include "m68k/hunk.hpp"

#include "m68k/disassembler.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

struct Builder {
    std::vector<Byte> out;
    void u32(std::uint32_t v) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<Byte>(v >> (8 * i)));
    }
    void bytes(std::initializer_list<Byte> b) {
        for (const Byte x : b) out.push_back(x);
    }
    void name(const std::string& s) {  // length in longs, then the padded string
        const std::size_t longs = (s.size() + 3) / 4;
        u32(static_cast<std::uint32_t>(longs));
        for (std::size_t i = 0; i < longs * 4; ++i) out.push_back(i < s.size() ? static_cast<Byte>(s[i]) : Byte{0});
    }
};

// 3 hunks: CODE (JSR to the DATA hunk, relocated), DATA, BSS (with a chip-memory flag in its size)
std::vector<Byte> sampleProgram() {
    Builder b;
    b.u32(0x3F3);
    b.u32(0);                 // no resident libraries
    b.u32(3);                 // table size
    b.u32(0);
    b.u32(2);                 // first, last
    b.u32(4);                 // CODE: 4 longs
    b.u32(1);                 // DATA: 1 long
    b.u32(0x40000002);        // BSS: 2 longs, chip memory
    b.u32(0x3E9);             // HUNK_CODE
    b.u32(4);
    b.bytes({0x4E, 0xB9, 0x00, 0x00, 0x00, 0x00,    // JSR $00000000.L  (relocated to the DATA hunk)
             0x4E, 0x75,                            // RTS
             0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71});
    b.u32(0x3EC);             // HUNK_RELOC32
    b.u32(1);                 // one offset ...
    b.u32(1);                 // ... pointing into hunk 1
    b.u32(2);                 // at offset 2
    b.u32(0);
    b.u32(0x3F0);             // HUNK_SYMBOL
    b.name("main");
    b.u32(0);
    b.name("x.y-z");
    b.u32(6);
    b.u32(0);
    b.u32(0x3F1);             // HUNK_DEBUG (skipped)
    b.u32(1);
    b.u32(0x12345678);
    b.u32(0x3F2);
    b.u32(0x3EA);             // HUNK_DATA
    b.u32(1);
    b.bytes({0xDE, 0xAD, 0xBE, 0xEF});
    b.u32(0x3F2);
    b.u32(0x3EB);             // HUNK_BSS
    b.u32(2);
    b.u32(0x3F2);
    return b.out;
}

bool loadFails(const std::vector<Byte>& file) {
    try {
        (void)loadHunkFile(file, 0x1000);
    } catch (const IoError&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const std::vector<Byte> file = sampleProgram();
    CHECK(looksLikeHunkFile(file));
    CHECK(!looksLikeHunkFile({0x4E, 0x75}));

    const HunkImage img = loadHunkFile(file, 0x1000);
    CHECK_EQ(img.hunks.size(), std::size_t{3});
    CHECK(img.hunks[0].type == "CODE" && img.hunks[0].address == 0x1000 && img.hunks[0].size == 16);
    CHECK(img.hunks[1].type == "DATA" && img.hunks[1].address == 0x1010 && img.hunks[1].size == 4);
    CHECK(img.hunks[2].type == "BSS" && img.hunks[2].address == 0x1014 && img.hunks[2].size == 8);
    CHECK_EQ(img.entry, Address{0x1000});
    CHECK_EQ(img.end, Address{0x101C});

    Memory mem;
    loadSegments(mem, img.segments);
    CHECK_EQ(mem.read32(0x1002), Long{0x1010});   // relocated: the base of the DATA hunk
    CHECK_EQ(mem.read32(0x1010), Long{0xDEADBEEF});
    CHECK_EQ(mem.read32(0x1014), Long{0});

    CHECK_EQ(img.symbols.size(), std::size_t{2});
    CHECK(img.symbols[0].name == "main" && img.symbols[0].address == 0x1000);
    CHECK(img.symbols[1].name == "x.y-z" && img.symbols[1].address == 0x1006);

    // another base address, aligned to 4
    const HunkImage img2 = loadHunkFile(file, 0x20001);
    CHECK_EQ(img2.hunks[0].address, Address{0x20004});
    Memory mem2;
    loadSegments(mem2, img2.segments);
    CHECK_EQ(mem2.read32(0x20006), Long{0x20014});

    // the disassembler follows the relocated call
    {
        Disassembler dis(mem, 0x1000, img.end);
        dis.addEntry(img.entry);
        dis.addSymbol(0x1000, "main");
        dis.analyzeFlow();
        const std::string text = dis.render();
        CHECK(text.find("JSR      L_001010.L") != std::string::npos);
        CHECK(text.find("main:") != std::string::npos);
    }

    // symbol names for the assembler
    CHECK_EQ(asmSymbolName("main"), std::string("main"));
    CHECK_EQ(asmSymbolName("x.y-z"), std::string("x_y_z"));
    CHECK_EQ(asmSymbolName("1abc"), std::string("_1abc"));
    CHECK_EQ(asmSymbolName("d0"), std::string("_d0"));
    CHECK_EQ(asmSymbolName("SP"), std::string("_SP"));
    CHECK_EQ(asmSymbolName(""), std::string("_"));

    // malformed files are rejected, never read out of bounds
    CHECK(loadFails({}));
    CHECK(loadFails({0, 0, 3, 0xF3}));
    for (std::size_t n = 0; n < file.size(); ++n) {  // every truncation
        const std::vector<Byte> cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK(loadFails(cut));
    }
    {
        std::vector<Byte> bad = file;
        bad[0] = 1;  // not a hunk file
        CHECK(loadFails(bad));
    }
    {
        std::vector<Byte> bad = file;  // relocation offset outside the hunk
        for (std::size_t i = 0; i + 3 < bad.size(); ++i)
            if (bad[i] == 0 && bad[i + 1] == 0 && bad[i + 2] == 0 && bad[i + 3] == 2 && i > 40 && i < 80) {
                bad[i + 3] = 100;
                break;
            }
        CHECK(loadFails(bad));
    }
    {
        Builder b;  // unsupported block type (HUNK_OVERLAY)
        b.u32(0x3F3); b.u32(0); b.u32(1); b.u32(0); b.u32(0); b.u32(1);
        b.u32(0x3F4);
        CHECK(loadFails(b.out));
    }
    {
        Builder b;  // block larger than announced
        b.u32(0x3F3); b.u32(0); b.u32(1); b.u32(0); b.u32(0); b.u32(1);
        b.u32(0x3E9); b.u32(2); b.u32(0); b.u32(0); b.u32(0x3F2);
        CHECK(loadFails(b.out));
    }
    {
        Builder b;  // HUNK_END missing between hunks
        b.u32(0x3F3); b.u32(0); b.u32(2); b.u32(0); b.u32(1); b.u32(1); b.u32(1);
        b.u32(0x3E9); b.u32(1); b.u32(0); b.u32(0x3E9); b.u32(1); b.u32(0); b.u32(0x3F2);
        CHECK(loadFails(b.out));
    }
    {
        Builder b;  // random garbage after a valid header must not crash
        b.u32(0x3F3); b.u32(0); b.u32(1000000); b.u32(0); b.u32(999999);
        CHECK(loadFails(b.out));
    }

    return finish("hunk");
}
