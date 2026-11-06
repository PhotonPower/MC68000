#include "m68k/decoder.hpp"

#include <initializer_list>

#include "test_util.hpp"

using namespace m68k;

namespace {

constexpr Address kBase = 0x1000;

Instruction decodeWords(std::initializer_list<Word> words, const DecodeOptions& opt = {}, Address at = kBase) {
    Memory mem;
    Address a = at;
    for (const Word w : words) {
        mem.write16(a, w);
        a += 2;
    }
    return decode(mem, at, opt);
}

void expect(std::initializer_list<Word> words, const std::string& text, std::size_t length) {
    const Instruction in = decodeWords(words);
    CHECK_EQ(in.text(), text);
    CHECK_EQ(in.length(), length);
    CHECK(in.valid);
}

void expectInvalid(std::initializer_list<Word> words) {
    const Instruction in = decodeWords(words);
    CHECK(!in.valid);
    CHECK_EQ(in.mnemonic, std::string("DC.W"));
    CHECK_EQ(in.length(), std::size_t{2});
}

}  // namespace

int main() {
    // data movement
    expect({0x4E75}, "RTS", 2);
    expect({0x2040}, "MOVEA.L D0,A0", 2);
    expect({0x2200}, "MOVE.L D0,D1", 2);
    expect({0x1018}, "MOVE.B (A0)+,D0", 2);
    expect({0x3F20}, "MOVE.W -(A0),-(A7)", 2);
    expect({0x303C, 0x1234}, "MOVE.W #$1234,D0", 4);
    expect({0x203C, 0x1234, 0x5678}, "MOVE.L #$12345678,D0", 6);
    expect({0x103C, 0x00FF}, "MOVE.B #$FF,D0", 4);
    expect({0x2C79, 0x0000, 0x0004}, "MOVEA.L $00000004.L,A6", 6);
    expect({0x7005}, "MOVEQ #$05,D0", 2);
    expect({0x7EFB}, "MOVEQ #$FB,D7", 2);
    expect({0x3028, 0xFFFE}, "MOVE.W -$2(A0),D0", 4);
    expect({0x3030, 0x1004}, "MOVE.W $4(A0,D1.W),D0", 4);
    expect({0x3030, 0x9804}, "MOVE.W $4(A0,A1.L),D0", 4);
    expect({0x3038, 0x8000}, "MOVE.W $8000.W,D0", 4);
    expect({0x40C0}, "MOVE SR,D0", 2);
    expect({0x44C0}, "MOVE D0,CCR", 2);
    expect({0x46C0}, "MOVE D0,SR", 2);
    expect({0x4E60}, "MOVE A0,USP", 2);
    expect({0x4E6B}, "MOVE USP,A3", 2);
    expect({0x48E7, 0xFFFE}, "MOVEM.L D0-D7/A0-A6,-(A7)", 4);
    expect({0x48E7, 0x8080}, "MOVEM.L D0/A0,-(A7)", 4);  // predecrement: mask is bit-reversed
    expect({0x4CDF, 0x7FFF}, "MOVEM.L (A7)+,D0-D7/A0-A6", 4);
    expect({0x4890, 0x0005}, "MOVEM.W D0/D2,(A0)", 4);
    expect({0x4DFA, 0x0010}, "LEA $001012(PC),A6", 4);
    expect({0x0108, 0x0004}, "MOVEP.W $4(A0),D0", 4);

    // arithmetic and logic
    expect({0xD0C1}, "ADDA.W D1,A0", 2);
    expect({0xD1C1}, "ADDA.L D1,A0", 2);
    expect({0xD441}, "ADD.W D1,D2", 2);
    expect({0xD350}, "ADD.W D1,(A0)", 2);
    expect({0xD140}, "ADDX.W D0,D0", 2);
    expect({0x0640, 0x0064}, "ADDI.W #$0064,D0", 4);
    expect({0x5E88}, "ADDQ.L #7,A0", 2);
    expect({0x5088}, "ADDQ.L #8,A0", 2);
    expect({0x5341}, "SUBQ.W #1,D1", 2);
    expect({0xB108}, "CMPM.B (A0)+,(A0)+", 2);
    expect({0xC0C1}, "MULU D1,D0", 2);
    expect({0x81C1}, "DIVS D1,D0", 2);
    expect({0x4880}, "EXT.W D0", 2);
    expect({0x48C0}, "EXT.L D0", 2);
    expect({0x4840}, "SWAP D0", 2);
    expect({0xC141}, "EXG D0,D1", 2);
    expect({0xC149}, "EXG A0,A1", 2);
    expect({0xC189}, "EXG D0,A1", 2);
    expect({0xE548}, "LSL.W #2,D0", 2);
    expect({0xE2A8}, "LSR.L D1,D0", 2);
    expect({0xE0D0}, "ASR.W (A0)", 2);
    expect({0xE7D0}, "ROL.W (A0)", 2);
    expect({0x0800, 0x0007}, "BTST #7,D0", 4);
    expect({0x0300}, "BTST D1,D0", 2);
    expect({0x023C, 0x00FE}, "ANDI #$FE,CCR", 4);
    expect({0x007C, 0x0700}, "ORI #$0700,SR", 4);
    expect({0x4E50, 0xFFFC}, "LINK A0,#-$4", 4);
    expect({0x4E72, 0x2700}, "STOP #$2700", 4);
    expect({0x4E4F}, "TRAP #15", 2);
    expect({0x57C0}, "SEQ D0", 2);
    expect({0x50C0}, "ST D0", 2);

    // control flow: targets are absolute, sizes explicit
    {
        const Instruction in = decodeWords({0x6008});
        CHECK_EQ(in.text(), std::string("BRA.S $00100A"));
        CHECK(in.flow == FlowKind::Jump);
        CHECK(in.target.has_value() && *in.target == 0x100A);
    }
    {
        const Instruction in = decodeWords({0x6700, 0x0010});
        CHECK_EQ(in.text(), std::string("BEQ.W $001012"));
        CHECK(in.flow == FlowKind::CondJump);
        CHECK_EQ(in.length(), std::size_t{4});
        CHECK(in.target && *in.target == 0x1012);
    }
    {
        const Instruction in = decodeWords({0x6100, 0xFFFE});
        CHECK_EQ(in.text(), std::string("BSR.W $001000"));
        CHECK(in.flow == FlowKind::Call);
    }
    {
        const Instruction in = decodeWords({0x51C8, 0xFFFC});
        CHECK_EQ(in.text(), std::string("DBRA D0,$000FFE"));
        CHECK(in.flow == FlowKind::CondJump);
        CHECK(in.target && *in.target == 0xFFE);
    }
    {
        const Instruction in = decodeWords({0x4EB9, 0x0000, 0x2000});
        CHECK_EQ(in.text(), std::string("JSR $00002000.L"));
        CHECK(in.flow == FlowKind::Call);
        CHECK(in.target && *in.target == 0x2000);
    }
    {
        const Instruction in = decodeWords({0x4ED0});
        CHECK(in.flow == FlowKind::Jump);
        CHECK(!in.target);  // JMP (A0): unknown target
    }
    {
        const Instruction in = decodeWords({0x4E75});
        CHECK(in.flow == FlowKind::Return);
    }
    {
        const Instruction in = decodeWords({0x41FA, 0x0010});
        CHECK(in.flow == FlowKind::Normal);
        CHECK(!in.target);
        CHECK(in.reference && *in.reference == 0x1012);
    }
    {
        const Instruction in = decodeWords({0x33FC, 0x0001, 0x00DF, 0xF180});
        CHECK_EQ(in.text(), std::string("MOVE.W #$0001,$00DFF180.L"));
        CHECK(in.reference && *in.reference == 0xDFF180);
    }

    // things that are not MC68000 instructions
    expectInvalid({0xA000});          // Line A
    expectInvalid({0xF000});          // Line F
    expectInvalid({0x4E74, 0x0000});  // RTD (68010)
    expectInvalid({0x42C0});          // MOVE from CCR (68010)
    expectInvalid({0x4E7A, 0x0801});  // MOVEC (68010)
    expectInvalid({0x4808});          // LINK.L (68020)
    expectInvalid({0x7100});          // MOVEQ with bit 8 set
    expectInvalid({0x1008});          // MOVE.B A0,D0: byte access to an address register
    expectInvalid({0x5008});          // ADDQ.B #8,A0
    expectInvalid({0x4A08});          // TST.B A0
    expectInvalid({0x4A3C, 0x0000});  // TST.B #imm (68020)
    expectInvalid({0x4880 | 0x0010, 0x0000});  // MOVEM with an empty register mask
    expectInvalid({0x6001});          // BRA.S to an odd address
    expectInvalid({0x60FF});          // 68020 long branch
    expectInvalid({0x6000, 0x0001});  // BRA.W to an odd address
    expectInvalid({0x51C8, 0x0001});  // DBRA to an odd address
    // non-canonical extension words are data (see decode())
    expectInvalid({0x103C, 0x01FF});  // byte immediate with a non-zero upper byte
    expectInvalid({0x0800, 0x0107});  // bit number with a non-zero upper byte
    expectInvalid({0x3030, 0x1304});  // index word with 68020 scale bits

    // odd addresses: one byte of data
    {
        const Instruction in = decodeWords({0x4E75, 0x4E71}, {}, 0x1001);
        CHECK(!in.valid);
        CHECK_EQ(in.mnemonic, std::string("DC.B"));
        CHECK_EQ(in.length(), std::size_t{1});
    }
    // addresses wrap at 16 MiB
    {
        Memory mem;
        mem.write16(0xFFFFFE, 0x4E75);
        CHECK_EQ(decode(mem, 0xFFFFFE).text(), std::string("RTS"));
        CHECK_EQ(decode(mem, 0xFFFFFE).address, Address{0xFFFFFE});
    }

    // number styles
    {
        DecodeOptions opt;
        opt.style = NumberStyle::CPrefix;
        CHECK_EQ(decodeWords({0x303C, 0x1234}, opt).text(), std::string("MOVE.W #0x1234,D0"));
        opt.style = NumberStyle::HSuffix;
        CHECK_EQ(decodeWords({0x303C, 0xFFFF}, opt).text(), std::string("MOVE.W #0FFFFH,D0"));
    }

    // symbolizer: branch targets, absolute and PC-relative operands
    {
        DecodeOptions opt;
        opt.symbolizer = [](Address a) { return a == 0x1012 ? std::string("msg") : std::string(); };
        CHECK_EQ(decodeWords({0x41FA, 0x0010}, opt).text(), std::string("LEA msg(PC),A0"));
        CHECK_EQ(decodeWords({0x6700, 0x0010}, opt).text(), std::string("BEQ.W msg"));
        CHECK_EQ(decodeWords({0x4EB9, 0x0000, 0x1012}, opt).text(), std::string("JSR msg.L"));
        CHECK_EQ(decodeWords({0x4EB8, 0x1012}, opt).text(), std::string("JSR msg.W"));
        CHECK_EQ(decodeWords({0x6700, 0x0020}, opt).text(), std::string("BEQ.W $001022"));
        // a label stands for one value only: an upper byte or a sign-extended .W address is never hidden
        CHECK_EQ(decodeWords({0x4EB9, 0x6700, 0x1012}, opt).text(), std::string("JSR $67001012.L"));
        CHECK_EQ(decodeWords({0x4EB8, 0x9012}, opt).text(), std::string("JSR $9012.W"));
    }

    // PC-relative operands with a second extension word use the address of *their* word
    {
        const Instruction in = decodeWords({0x11FA, 0x0010, 0x2000});  // MOVE.B d16(PC),$2000.W
        CHECK_EQ(in.text(), std::string("MOVE.B $001012(PC),$2000.W"));
        const Instruction in2 = decodeWords({0x0C3A, 0x0001, 0x0010});  // CMPI.B #1,d16(PC) - 68020 only
        CHECK(!in2.valid);
    }

    return finish("decoder");
}
