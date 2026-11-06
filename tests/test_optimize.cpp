// The optional size optimisations (AssemblerOptions::optimize): the optimised program must do exactly what the plain one does.
#include <random>
#include <string>

#include "m68k/assembler.hpp"
#include "m68k/cpu.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

AssemblyResult build(const std::string& src, bool optimize) {
    AssemblerOptions o;
    o.origin = 0x1000;
    o.optimize = optimize;
    return assemble(src, o);
}

std::vector<Byte> bytesOf(const std::string& line, bool optimize) {
    const AssemblyResult r = build("        " + line + "\n", optimize);
    return r.ok() ? r.flatten() : std::vector<Byte>{};
}

std::string hex(const std::vector<Byte>& b) {
    std::string s;
    for (const Byte x : b) s += hexDigits(x, 2);
    return s;
}

struct Outcome {
    std::array<Long, 8> d, a;
    Word sr;
    std::vector<Byte> window;  // memory $2000-$20FF
    bool operator==(const Outcome& o) const { return d == o.d && a == o.a && sr == o.sr && window == o.window; }
};

// Runs the program (one or several straight-line instructions, ending in STOP) from a fixed random state.
Outcome run(const AssemblyResult& r, unsigned seed) {
    Memory mem;
    std::mt19937 rng(seed);
    for (Address a = 0x2000; a < 0x2100; ++a) mem.write8(a, static_cast<Byte>(rng()));
    mem.write32(0, 0x8000);
    mem.write32(4, 0x1000);
    for (const Segment& s : r.segments)
        for (std::size_t i = 0; i < s.bytes.size(); ++i) mem.write8(s.address + static_cast<Address>(i), s.bytes[i]);
    Cpu cpu(mem);
    cpu.reset();
    Registers& g = cpu.registers();
    for (auto& d : g.d) d = static_cast<Long>(rng());
    for (int i = 0; i < 7; ++i) g.a[static_cast<size_t>(i)] = 0x2000 + static_cast<Long>(rng() % 0x80) * 2;
    cpu.setSr(static_cast<Word>(0x2700 | (rng() % 32)));
    g.pc = r.lowest();
    cpu.run(200);
    Outcome o;
    o.d = g.d;
    o.a = g.a;
    o.sr = g.sr;
    for (Address a = 0x2000; a < 0x2100; ++a) o.window.push_back(mem.read8(a));
    return o;
}

}  // namespace

int main() {
    // ---- the forms ------------------------------------------------------------------------------------------------------------------------
    CHECK_EQ(hex(bytesOf("MOVE.L #5,D0", true)), std::string("7005"));
    CHECK_EQ(hex(bytesOf("MOVE.L #5,D0", false)), std::string("203C00000005"));
    CHECK_EQ(hex(bytesOf("MOVE.L #-128,D3", true)), std::string("7680"));
    CHECK_EQ(hex(bytesOf("MOVE.L #127,D3", true)), std::string("767F"));
    CHECK_EQ(hex(bytesOf("MOVE.L #128,D3", true)), std::string("263C00000080"));    // does not fit MOVEQ
    CHECK_EQ(hex(bytesOf("MOVE.W #5,D0", true)), std::string("303C0005"));          // only the long form becomes MOVEQ
    CHECK_EQ(hex(bytesOf("MOVE.L #5,(A0)", true)), std::string("20BC00000005"));    // MOVEQ needs a data register
    CHECK_EQ(hex(bytesOf("ADD.W #3,D1", true)), std::string("5641"));
    CHECK_EQ(hex(bytesOf("ADD.W #9,D1", true)), std::string("D27C0009"));           // 9 does not fit ADDQ
    CHECK_EQ(hex(bytesOf("ADD.W #0,D1", true)), std::string("D27C0000"));
    CHECK_EQ(hex(bytesOf("SUB.L #8,A0", true)), std::string("5188"));
    CHECK_EQ(hex(bytesOf("ADD.B #2,D2", true)), std::string("5402"));
    CHECK_EQ(hex(bytesOf("ADD.L #4,(A1)+", true)), std::string("5899"));
    CHECK_EQ(hex(bytesOf("ADD.L #4,$2000.W", true)), std::string("58B82000"));
    CHECK_EQ(hex(bytesOf("ADDA.L #3,A5", true)), std::string("568D"));
    CHECK_EQ(hex(bytesOf("MOVEA.L #1000,A1", true)), std::string("327C03E8"));
    CHECK_EQ(hex(bytesOf("MOVEA.L #-2,A1", true)), std::string("327CFFFE"));
    CHECK_EQ(hex(bytesOf("MOVEA.L #40000,A1", true)), std::string("227C00009C40"));  // does not fit 16 bits signed
    CHECK_EQ(hex(bytesOf("CMPA.L #-5,A2", true)), std::string("B4FCFFFB"));
    CHECK_EQ(hex(bytesOf("ADD.L #1000,A2", true)), std::string("D4FC03E8"));
    // explicit forms are left alone
    CHECK_EQ(hex(bytesOf("ADDI.W #3,D1", true)), hex(bytesOf("ADDI.W #3,D1", false)));
    CHECK_EQ(hex(bytesOf("ADDQ.W #3,D1", true)), hex(bytesOf("ADDQ.W #3,D1", false)));
    CHECK_EQ(hex(bytesOf("MOVEQ #5,D0", true)), std::string("7005"));
    CHECK_EQ(hex(bytesOf("CMP.W #3,D1", true)), hex(bytesOf("CMP.W #3,D1", false)));   // CMP has no quick form
    // without the option nothing changes
    CHECK_EQ(hex(bytesOf("ADD.W #3,D1", false)), std::string("D27C0003"));

    // ---- symbols: forward references and labels converge ----------------------------------------------------------------------------------
    {
        const AssemblyResult r = build(
            "        ORG $10\n"
            "start:  MOVE.L #fwd,D0\n"      // a forward reference: the value shrinks together with the code
            "        MOVE.L #fwd2,D1\n"
            "        ADD.W #count,D2\n"      // `count` is defined further down
            "        NOP\n"
            "fwd:    NOP\n"
            "fwd2:   STOP #$2700\n"
            "count   EQU 4\n", true);
        CHECK(r.ok());
        CHECK_EQ(r.symbols.at("fwd"), std::int64_t{0x18});             // 4 instructions of 2 bytes in front of it
        CHECK_EQ(hex(r.flatten()).substr(0, 12), std::string("7018721A5842"));
        const Outcome o = run(r, 1);
        CHECK_EQ(o.d[0], Long{0x18});
        CHECK_EQ(o.d[1], Long{0x1A});
        // MOVEQ is sign-extended: an address that is not below 128 stays long (here: origin 0x1000)
        CHECK_EQ(hex(bytesOf("MOVE.L #$1000,D0", true)), std::string("203C00001000"));
    }
    {
        // a value that stops fitting once code moves is un-optimised and stays so (no oscillation): the result is still correct
        std::string src = "        ORG 100\n        MOVE.L #target,D0\n        ADD.W #3,D1\n";
        for (int i = 0; i < 12; ++i) src += "        MOVE.L #fwd" + std::to_string(i) + ",D2\n";
        for (int i = 0; i < 40; ++i) src += "        NOP\n";
        src += "target: NOP\n";
        for (int i = 0; i < 12; ++i) src += "fwd" + std::to_string(i) + ": NOP\n";
        AssemblerOptions o;
        o.optimize = true;
        const AssemblyResult r = assemble(src, o);
        CHECK(r.ok());
    }

    // ---- random equivalence: the optimised instruction does exactly what the plain one does -------------------------------------------------------
    {
        std::mt19937 rng(777);
        const char* mnem[] = {"ADD", "SUB", "ADDA", "SUBA", "MOVE", "MOVEA", "CMP", "CMPA"};
        const char* sizes[] = {".B", ".W", ".L"};
        const char* dests[] = {"D0", "D3", "D7", "A0", "A3", "A6", "SP", "(A0)", "(A1)+", "-(A2)", "$10(A3)", "$2010.W", "$2020.L", "2(A4,D1.W)"};
        int compared = 0, shorter = 0;
        for (int i = 0; i < 6000; ++i) {
            const char* m = mnem[rng() % 8];
            const char* sz = sizes[rng() % 3];
            const char* d = dests[rng() % 14];
            const std::int64_t values[] = {0, 1, 2, 7, 8, 9, -1, -8, 100, 127, 128, -128, -129, 255, 32767, 32768, -32768, -32769, 65535, 0x12345,
                                           static_cast<std::int64_t>(rng() % 20), static_cast<std::int64_t>(rng() % 70000) - 35000};
            const std::int64_t v = values[rng() % (sizeof values / sizeof values[0])];
            const std::string line = std::string(m) + sz + " #" + std::to_string(v) + "," + d + "\n        STOP #$2700";
            const AssemblyResult plain = build("        " + line + "\n", false);
            if (!plain.ok()) continue;                     // not a valid instruction (e.g. ADD.B #1,A0): then the optimised one must not exist either
            const AssemblyResult opt = build("        " + line + "\n", true);
            CHECK(opt.ok());
            if (!opt.ok()) continue;
            CHECK(opt.flatten().size() <= plain.flatten().size());
            if (opt.flatten().size() < plain.flatten().size()) ++shorter;
            for (unsigned seed = 1; seed <= 2; ++seed) {
                const Outcome a = run(plain, seed + static_cast<unsigned>(i)), b = run(opt, seed + static_cast<unsigned>(i));
                CHECK(a == b);
                if (!(a == b)) std::fprintf(stderr, "  differs: %s\n", line.c_str());
            }
            ++compared;
        }
        CHECK(compared > 1500);
        CHECK(shorter > 300);
    }
    {   // invalid instructions stay invalid
        CHECK(!build("        ADD.B #1,A0\n", true).ok());
        CHECK(!build("        MOVE.L #1,PC\n", true).ok());
        CHECK(!build("        MOVE.L #1,#2\n", true).ok());
    }

    return finish("optimize");
}
