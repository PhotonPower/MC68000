#include "m68k/assembler.hpp"

#include "m68k/decoder.hpp"
#include "m68k/image_io.hpp"

#include <cstdlib>
#include <random>

#include "test_util.hpp"

using namespace m68k;

#include "gnu_verified.inc"

namespace {

std::string hex(const std::vector<Byte>& b) {
    std::string s;
    for (const Byte x : b) s += hexDigits(x, 2);
    return s;
}

// Assembles `src` and returns all output bytes as hex ("" for none); errors are reported through `r`.
std::string asmHex(const std::string& src, AssemblyResult* out = nullptr, Address origin = 0x1000) {
    AssemblerOptions opt;
    opt.origin = origin;
    AssemblyResult r = assemble(src, opt);
    const std::string h = hex(r.flatten());
    if (out) *out = std::move(r);
    return h;
}

void expectHex(const std::string& line, const std::string& expected) {
    AssemblyResult r;
    const std::string got = asmHex("  " + line + "\n", &r);
    if (!r.ok()) {
        ++g_failures;
        std::cerr << "unexpected error for '" << line << "': " << r.errors[0].message << "\n";
        return;
    }
    CHECK_EQ(got, expected);
}

void expectError(const std::string& src, const std::string& fragment) {
    AssemblyResult r;
    asmHex(src, &r);
    ++g_checks;
    if (r.ok()) {
        ++g_failures;
        std::cerr << "expected an error for:\n" << src << "\n";
        return;
    }
    bool found = false;
    for (const Diagnostic& d : r.errors) found = found || d.message.find(fragment) != std::string::npos;
    if (!found) {
        ++g_failures;
        std::cerr << "error for '" << src << "' does not mention '" << fragment << "', got: " << r.errors[0].message << "\n";
    }
}

}  // namespace

int main() {
    // ---- every line verified against GNU as ---------------------------------------------------------
    for (const Case& c : kGnuVerified) expectHex(c.source, c.hex);

    // ---- conventions of this assembler -------------------------------------------------------------
    expectHex("MOVE D0,D1", "3200");               // no size = .W
    expectHex("move.l d0,d1", "2200");             // case-insensitive
    expectHex("MOVE.L D0,(SP)", "2E80");           // SP = A7
    expectHex("MOVE.L (A0)+,D1", "2218");
    expectHex("MOVE.W #5,D0", "303C0005");
    expectHex("MOVE.B #-1,D0", "103C00FF");        // negative immediates are fine
    expectHex("MOVE.B #255,D0", "103C00FF");
    expectHex("MOVE.L #-1,D0", "203CFFFFFFFF");
    expectHex("MOVE.W 0(A0),D0", "30280000");      // d16 is kept even if it is 0
    expectHex("MOVE.W (4,A0),D0", "30280004");     // (d,An) syntax
    expectHex("MOVE.W (A0),D0", "3010");
    expectHex("MOVE.W ($1234).W,D0", "30381234");  // parenthesised absolute address
    expectHex("MOVE.W $1234,D0", "30381234");      // unsized absolute: .W if it fits
    expectHex("MOVE.W $8000,D0", "303900008000");  // ... otherwise .L (0x8000 would be sign-extended)
    expectHex("MOVE.W -2,D0", "3038FFFE");
    expectHex("MOVE.W $12345,D0", "303900012345");
    expectHex("MOVE.W 2*(3+1)(A0),D0", "30280008"); // expression displacement
    expectHex("MOVEA.L A0,A1", "2248");
    expectHex("MOVE.L A0,A1", "2248");             // MOVE to An is MOVEA
    expectHex("ADD.W D0,A1", "D2C0");              // ADD to An is ADDA
    expectHex("ADD.W #5,(A0)", "06500005");        // ADD #imm,<ea> without a Dn destination is ADDI
    expectHex("SUB.L #4,A7", "9FFC00000004");      // SUB to An is SUBA
    expectHex("CMP.W #5,(A0)", "0C500005");        // CMP -> CMPI
    expectHex("AND.W #$FF,(A0)", "025000FF");      // AND #imm,<ea> -> ANDI
    expectHex("OR.B #1,D0", "803C0001");           // ... but with a Dn destination the OR form itself exists
    expectHex("ORI.B #1,D0", "00000001");
    expectHex("EOR.W #1,D0", "0A400001");          // EOR #imm -> EORI
    expectHex("DBF D0,*", "51C8FFFE");             // DBF = DBRA; '*' is the current address
    expectHex("BHS.S *+4", "6402");                // HS = CC
    expectHex("BLO.S *+4", "6502");
    expectHex("MOVEM.L D0-D2/D4,-(A7)", "48E7E800");
    expectHex("MOVEM.L D0,-(A7)", "48E78000");
    expectHex("MOVEM.L A0,(A1)", "48D10100");
    expectHex("MOVEM.L (A7)+,D0", "4CDF0001");
    expectHex("TRAP #0", "4E40");
    expectHex("LINK A6,#-8", "4E56FFF8");
    expectHex("DC.W -1", "FFFF");
    expectHex("DC.B -1,255", "FFFF");
    expectHex("DC.L $12345678,2", "1234567800000002");
    expectHex("DC.B 'A',\"BC\"", "414243");
    expectHex("DC.B \"hello\"", "68656C6C6F");
    expectHex("DS.B 3", "000000");
    expectHex("DS.W 2", "00000000");
    expectHex("DS.L 1", "00000000");

    // ---- labels, forward references and sizing -----------------------------------------------------
    {
        AssemblyResult r;
        const std::string h = asmHex(
            "start:  BRA     end\n"       // forward: short
            "        NOP\n"
            "end:    BRA     start\n",    // backward: short
            &r);
        CHECK(r.ok());
        CHECK_EQ(h, std::string("60024E7160FA"));
        CHECK_EQ(r.symbols.at("start"), std::int64_t{0x1000});
        CHECK_EQ(r.symbols.at("end"), std::int64_t{0x1004});
    }
    {
        AssemblyResult r;  // a branch over more than 127 bytes grows to the word form; the rest shifts
        const std::string h = asmHex("  BRA far\n  DS.B 200\nfar: RTS\n", &r);
        CHECK(r.ok());
        CHECK_EQ(h.substr(0, 8), std::string("600000CA"));
        CHECK_EQ(r.symbols.at("far"), std::int64_t{0x1000 + 4 + 200});
    }
    {
        AssemblyResult r;  // cascade: the growth of one branch pushes another one out of range
        std::string src = "  BRA a\n  BRA b\n  DS.B 126\na: NOP\n  DS.B 2\nb: NOP\n";
        asmHex(src, &r);
        CHECK(r.ok());
        const std::int64_t a = r.symbols.at("a"), b = r.symbols.at("b");
        CHECK(b > a);
        // whatever sizes were chosen, both branches must land exactly on their labels
        Memory mem;
        loadSegments(mem, r.segments);
        const Instruction first = decode(mem, 0x1000);
        const Instruction second = decode(mem, 0x1000 + static_cast<Address>(first.length()));
        CHECK(first.target && static_cast<std::int64_t>(*first.target) == a);
        CHECK(second.target && static_cast<std::int64_t>(*second.target) == b);
    }
    {
        AssemblyResult r;  // unsized forward absolute address starts small and grows
        const std::string h = asmHex("  MOVE.W later,D0\n  DS.B 100\nlater: DC.W 1\n", &r, 0x00FE0000);
        CHECK(r.ok());
        CHECK_EQ(h.substr(0, 4), std::string("3039"));  // abs.L because the address is above 32 KiB
    }
    {
        AssemblyResult r;  // forward EQU
        const std::string h = asmHex("  MOVE.W #five,D0\nfive EQU 5\n", &r);
        CHECK(r.ok());
        CHECK_EQ(h, std::string("303C0005"));
    }
    {
        AssemblyResult r;  // local labels are scoped to the preceding global label
        const std::string h = asmHex("a:  BRA.S .x\n    NOP\n.x: NOP\nb:  BRA.S .x\n    NOP\n.x: NOP\n", &r);
        CHECK(r.ok());
        CHECK_EQ(h, std::string("60024E714E71" "60024E714E71"));
    }
    {
        AssemblyResult r;  // expressions with labels, '*' and symbols in data
        asmHex("t: DC.W t-*,*-t\n", &r);
        CHECK(r.ok());
    }
    {
        AssemblyResult r;
        const std::string h = asmHex("  ORG $2000\nx: DC.W 1\n  ORG $3000\ny: DC.W 2\n", &r);
        CHECK(r.ok());
        CHECK_EQ(r.segments.size(), std::size_t{2});
        CHECK_EQ(r.segments[0].address, Address{0x2000});
        CHECK_EQ(r.segments[1].address, Address{0x3000});
        CHECK_EQ(r.lowest(), Address{0x2000});
        CHECK_EQ(r.flatten().size(), std::size_t{0x1002});  // gap filled with zeros
        CHECK_EQ(h.substr(0, 4), std::string("0001"));
    }
    {
        AssemblyResult r;  // EVEN, comments, label forms, case
        const std::string h = asmHex(
            "* comment line\n"
            "; another\n"
            "lab1: DC.B 1 ; trailing comment\n"
            "   EVEN\n"
            "  lab2: DC.W 2\n"
            "lab3\n"
            "   END\n"
            "   DC.B 99\n",
            &r);
        CHECK(r.ok());
        CHECK_EQ(h, std::string("01000002"));
        CHECK_EQ(r.symbols.at("lab2"), std::int64_t{0x1002});
        CHECK_EQ(r.symbols.at("lab3"), std::int64_t{0x1004});
    }
    {
        AssemblerOptions opt;
        opt.predefined["KICK"] = 0xFC0000;
        const AssemblyResult r = assemble("  MOVE.L #KICK,D0\n", opt);
        CHECK(r.ok());
        CHECK_EQ(hex(r.flatten()), std::string("203C00FC0000"));
    }

    // ---- errors --------------------------------------------------------------------------------------
    expectError("  FROBNICATE D0\n", "unknown instruction");
    expectError("  MOVE.L D0\n", "number of operands");
    expectError("  MOVE.L D0,#5\n", "addressing mode");
    expectError("  MOVE.B A0,D0\n", "byte");
    expectError("  ADDQ.W #9,D0\n", "1..8");
    expectError("  MOVEQ #300,D0\n", "range");
    expectError("  MOVE.B #256,D0\n", "range");
    expectError("  MOVE.W #70000,D0\n", "range");
    expectError("  MOVE.W D0,lost\n", "undefined symbol 'lost'");
    expectError("  BRA nowhere\n", "undefined symbol 'nowhere'");
    expectError("x: NOP\nx: NOP\n", "twice");
    expectError("  NOP\n  ORG $1001\n  NOP\n", "odd address");
    expectError("  BRA.S far\n  DS.B 200\nfar: NOP\n", "too far");
    expectError("  BRA.W odd\n  DS.B 1\nodd: NOP\n", "odd address");
    expectError("  MOVE.W 40000(A0),D0\n", "16 bit");
    expectError("  MOVE.W 200(A0,D0.W),D0\n", "8 bit");
    expectError("  MOVE.W $10000.W,D0\n", ".W");
    expectError("  LEA far(PC),A0\n  DS.B 40000\nfar: NOP\n", "PC-relative");
    expectError("  MOVE.W D0,D1.W\n", "");
    expectError("  MOVE.W (A0,X1),D0\n", "index");
    expectError("  DC D0\n", "size");
    expectError("  DS.W 1/0\n", "division");
    expectError("  EVEN 3\nX EQU\n", "");
    expectError("  MULU.L D0,D1\n", "size");
    expectError("  TRAP #16\n", "0..15");
    expectError("  ASL.W #9,D0\n", "1..8");
    expectError("  JMP D0\n", "addressing mode");
    expectError("  MOVEM.L D0,D1\n", "");
    expectError("  LEA (A0),D0\n", "address register");
    expectError("  EXG D0,#1\n", "");
    expectError("  BTST #256,D0\n", "0..255");
    expectError("  ORG $1000000\n", "range");

    // errors from several lines are all reported, sorted by line
    {
        AssemblyResult r;
        asmHex("  FOO\n  NOP\n  BAR\n", &r);
        CHECK_EQ(r.errors.size(), std::size_t{2});
        CHECK_EQ(r.errors[0].line, 1);
        CHECK_EQ(r.errors[1].line, 3);
    }

    // ---- listing and symbol table ---------------------------------------------------------------------
    {
        AssemblyResult r;
        asmHex("lab: MOVE.W #5,D0 ; five\n", &r);
        const std::string listing = r.formatListing();
        CHECK(listing.find("001000") != std::string::npos);
        CHECK(listing.find("303C 0005") != std::string::npos);
        CHECK(listing.find("MOVE.W #5,D0") != std::string::npos);
        CHECK(r.formatSymbols().find("lab = $00001000") != std::string::npos);
    }

    // ---- robustness: random garbage must never crash or hang ----------------------------------------------
    {
        std::mt19937 rng(12345);
        const std::string alphabet = "ABCDMOVEQRTSLNP.#$%()-+*/,:;'\" 0123456789adx\n\t";
        for (int i = 0; i < 3000; ++i) {
            std::string src;
            const int len = static_cast<int>(rng() % 60);
            for (int k = 0; k < len; ++k) src += alphabet[rng() % alphabet.size()];
            const AssemblyResult r = assemble(src);
            (void)r;
        }
        // semi-structured: valid mnemonics with random operands
        const char* mn[] = {"MOVE.L", "ADD.W", "BRA", "LEA", "MOVEM.L", "DBRA", "BTST", "ASL.W", "JMP", "MOVEP.W", "DC.W", "DS.B"};
        const char* op[] = {"D0", "A1", "(A2)", "(A3)+", "-(A4)", "4(A5)", "4(A5,D1.W)", "#5", "$1234", "$1234.L", "label",
                            "label(PC)", "D0-D3/A0", "SR", "CCR", "USP", "(", ")", "", "#", "1+", "(,)"};
        for (int i = 0; i < 4000; ++i) {
            std::string src = "label: ";
            src += mn[rng() % 12];
            src += " ";
            src += op[rng() % 22];
            if (rng() % 2) {
                src += ",";
                src += op[rng() % 22];
            }
            src += "\n";
            const AssemblyResult r = assemble(src);
            (void)r;
        }
        CHECK(true);
    }

    return finish("assembler");
}
