#include "m68k/cpu.hpp"

#include <sstream>

#include "m68k/assembler.hpp"
#include "m68k/decoder.hpp"
#include "m68k/image_io.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

constexpr Address kCode = 0x1000;
constexpr Address kSsp = 0x8000;

// A processor with its memory, a reset vector (SSP $8000, PC $1000) and helpers to run assembled code.
struct Rig {
    Memory mem;
    Cpu cpu{mem};
    std::map<std::string, std::int64_t> symbols;

    Rig() {
        mem.write32(0, kSsp);
        mem.write32(4, kCode);
        for (unsigned v = 2; v < 256; ++v) mem.write32(v * 4, 0x3000);  // default handler: STOP
        load("STOP #$2700", 0x3000);
        cpu.reset();
    }

    // Lines that do not start with a label are indented, so tests can write "MOVEQ #1,D0" directly.
    static std::string indent(const std::string& src) {
        std::string out, line;
        std::istringstream in(src);
        while (std::getline(in, line)) {
            const std::size_t sp = line.find_first_of(" \t");
            const std::string first = line.substr(0, sp);
            const bool label = !first.empty() && first.back() == ':';
            out += (label || line.empty() || line[0] == ' ' ? "" : "    ") + line + "\n";
        }
        return out;
    }

    void load(const std::string& src, Address at) {
        AssemblerOptions opt;
        opt.origin = at;
        const AssemblyResult r = assemble(indent(src), opt);
        if (!r.ok()) {
            std::cerr << "test program does not assemble (line " << r.errors[0].line << "): " << r.errors[0].message << "\n"
                      << src << "\n";
            std::exit(2);
        }
        loadSegments(mem, r.segments);
        for (const auto& [k, v] : r.symbols) symbols[k] = v;
    }
    // Installs an exception handler for `vector`.
    void handler(unsigned vector, const std::string& src) {
        const Address at = 0x4000 + vector * 0x40;
        load(src, at);
        mem.write32(vector * 4, at);
    }
    // Runs `src` (followed by a label `done`) until `done` is reached.
    RunResult exec(const std::string& src, std::uint64_t limit = 100000) {
        load(src + "\ndone: NOP", kCode);
        r().pc = kCode;  // every exec() starts at the beginning, registers and memory are kept
        return cpu.runUntil(static_cast<Address>(symbols.at("done")), limit);
    }
    Registers& r() { return cpu.registers(); }
    Word ccr() const { return cpu.registers().sr & 0x1F; }
};

constexpr Word X = kFlagX, N = kFlagN, Z = kFlagZ, V = kFlagV, C = kFlagC;

void arithmetic() {
    {
        Rig t;  // signed overflow: 5 + $7B = $80 as a byte
        t.exec("MOVEQ #5,D0\nADD.B #$7B,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0x80});
        CHECK_EQ(t.ccr(), Word(N | V));
    }
    {
        Rig t;  // carry out: $FF + 1
        t.exec("MOVE.B #$FF,D0\nADD.B #1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0});
        CHECK_EQ(t.ccr(), Word(X | Z | C));
    }
    {
        Rig t;  // the other bytes of the register are not touched by a .B operation
        t.exec("MOVE.L #$12345678,D0\nADD.B #$10,D0\nADD.W #$1000,D0");
        CHECK_EQ(t.r().d[0], Long{0x12346688});
    }
    {
        Rig t;
        t.exec("MOVE.W #$8000,D0\nNEG.W D0");
        CHECK_EQ(t.r().d[0] & 0xFFFF, Long{0x8000});
        CHECK_EQ(t.ccr(), Word(X | N | V | C));
    }
    {
        Rig t;  // ADDQ/SUBQ on an address register: whole register, no flags
        t.exec("MOVE.W #4,CCR\nMOVEA.L #$FFFFFFFF,A0\nADDQ.W #1,A0\nSUBQ.L #8,A1");
        CHECK_EQ(t.r().a[0], Long{0});
        CHECK_EQ(t.r().a[1], Long{0xFFFFFFF8});
        CHECK_EQ(t.ccr(), Word(Z));
    }
    {
        Rig t;  // MOVEA.W sign-extends, flags unchanged
        t.exec("MOVE.W #$0A,CCR\nMOVEA.W #$8000,A0");
        CHECK_EQ(t.r().a[0], Long{0xFFFF8000});
        CHECK_EQ(t.ccr(), Word(N | V));
    }
    {
        Rig t;  // 64 bit addition with ADDX
        t.exec("MOVE.L #$FFFFFFFF,D0\nMOVEQ #0,D1\nMOVEQ #1,D2\nMOVEQ #0,D3\nADD.L D2,D0\nADDX.L D3,D1");
        CHECK_EQ(t.r().d[0], Long{0});
        CHECK_EQ(t.r().d[1], Long{1});
    }
    {
        Rig t;  // ADDX only clears Z, never sets it
        t.exec("MOVE.W #4,CCR\nMOVEQ #0,D0\nMOVEQ #0,D1\nADDX.B D1,D0");
        CHECK_EQ(t.ccr(), Word(Z));
        t.exec("MOVE.W #4,CCR\nMOVEQ #1,D0\nMOVEQ #0,D1\nADDX.B D1,D0");
        CHECK_EQ(t.ccr(), Word(0));
    }
    {
        Rig t;
        t.exec("MOVEQ #0,D0\nSUBQ.B #1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0xFF});
        CHECK_EQ(t.ccr(), Word(X | N | C));
    }
    {
        Rig t;  // CMP leaves X alone and does not write
        t.exec("MOVE.W #$10,CCR\nMOVEQ #5,D0\nCMP.L #5,D0");
        CHECK_EQ(t.r().d[0], Long{5});
        CHECK_EQ(t.ccr(), Word(X | Z));
    }
    {
        Rig t;  // CMPA compares all 32 bits (the word is sign-extended)
        t.exec("MOVEA.L #$FFFFFFFF,A0\nCMPA.W #$FFFF,A0");
        CHECK_EQ(t.ccr() & Z, Word(Z));
    }
    {
        Rig t;  // multiply
        t.exec("MOVE.W #$FFFF,D0\nMULU #$FFFF,D0\nMOVE.W #-2,D1\nMULS #3,D1");
        CHECK_EQ(t.r().d[0], Long{0xFFFE0001});
        CHECK_EQ(t.r().d[1], Long{0xFFFFFFFA});
    }
    {
        Rig t;  // divide: quotient in the low word, remainder in the high word
        t.exec("MOVE.L #100000,D0\nDIVU #7,D0\nMOVE.L #-100000,D1\nDIVS #7,D1");
        CHECK_EQ(t.r().d[0], Long{0x000537CD});
        CHECK_EQ(t.r().d[1], Long{0xFFFBC833});
    }
    {
        Rig t;  // overflow leaves the register alone and sets V
        t.exec("MOVE.L #$100000,D0\nDIVU #1,D0");
        CHECK_EQ(t.r().d[0], Long{0x100000});
        CHECK_EQ(t.ccr() & V, Word(V));
    }
    {
        Rig t;  // BCD
        t.exec("MOVE.B #$19,D0\nMOVE.B #$28,D1\nMOVE.W #0,CCR\nABCD D1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0x47});
        t.exec("MOVE.B #$99,D0\nMOVE.B #1,D1\nMOVE.W #4,CCR\nABCD D1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0});
        CHECK_EQ(t.ccr() & (X | C | Z), Word(X | C | Z));
        t.exec("MOVE.B #$42,D0\nMOVE.B #$17,D1\nMOVE.W #0,CCR\nSBCD D1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0x25});
        t.exec("MOVE.B #$25,D0\nMOVE.W #0,CCR\nNBCD D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0x75});
    }
}

void logicAndShifts() {
    {
        Rig t;
        t.exec("MOVE.B #$81,D0\nLSR.B #1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0x40});
        CHECK_EQ(t.ccr(), Word(X | C));
    }
    {
        Rig t;  // ASR keeps the sign
        t.exec("MOVE.B #$81,D0\nASR.B #1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0xC0});
        CHECK_EQ(t.ccr(), Word(X | N | C));
    }
    {
        Rig t;  // ASL: V if the sign changed during the shift
        t.exec("MOVE.B #$40,D0\nASL.B #1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0x80});
        CHECK_EQ(t.ccr(), Word(N | V));
    }
    {
        Rig t;  // rotate: X untouched, C = last bit out
        t.exec("MOVE.W #$10,CCR\nMOVE.B #$81,D0\nROL.B #1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0x03});
        CHECK_EQ(t.ccr(), Word(X | C));
    }
    {
        Rig t;  // rotate through X
        t.exec("MOVE.W #$10,CCR\nMOVE.B #$80,D0\nROXL.B #1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0x01});
        CHECK_EQ(t.ccr(), Word(X | C));
    }
    {
        Rig t;  // a register count above the operand size shifts everything out
        t.exec("MOVE.B #$FF,D0\nMOVEQ #9,D1\nLSL.B D1,D0");
        CHECK_EQ(t.r().d[0] & 0xFF, Long{0});
        CHECK_EQ(t.ccr(), Word(Z));
    }
    {
        Rig t;  // count 0 (also 64 = 0 mod 64): C cleared, X unchanged
        t.exec("MOVE.W #$11,CCR\nMOVEQ #64,D1\nMOVEQ #5,D0\nLSL.L D1,D0");
        CHECK_EQ(t.r().d[0], Long{5});
        CHECK_EQ(t.ccr(), Word(X));
    }
    {
        Rig t;  // memory shifts are word sized with count 1
        t.exec("MOVEA.L #$2000,A0\nMOVE.W #$8001,(A0)\nLSR.W (A0)");
        CHECK_EQ(t.mem.read16(0x2000), Word{0x4000});
        CHECK_EQ(t.ccr(), Word(X | C));
    }
    {
        Rig t;
        t.exec("MOVE.L #$12345678,D0\nSWAP D0\nMOVE.W #$80,D1\nEXT.W D1\nMOVEQ #-1,D2\nEXT.L D2\nMOVE.L #$00008000,D3\nEXT.L D3");
        CHECK_EQ(t.r().d[0], Long{0x56781234});
        CHECK_EQ(t.r().d[1] & 0xFFFF, Long{0xFF80});
        CHECK_EQ(t.r().d[3], Long{0xFFFF8000});
        CHECK_EQ(t.ccr(), Word(N));
    }
    {
        Rig t;
        t.exec("MOVEQ #1,D0\nMOVEQ #2,D1\nEXG D0,D1\nMOVEA.L #$1234,A0\nEXG D0,A0");
        CHECK_EQ(t.r().d[1], Long{1});
        CHECK_EQ(t.r().d[0], Long{0x1234});
        CHECK_EQ(t.r().a[0], Long{2});
    }
    {
        Rig t;  // bit operations
        t.exec("MOVEQ #0,D0\nBSET #3,D0\nMOVE.W SR,D6\nBTST #3,D0\nMOVE.W SR,D7\nBCLR #3,D0\nBCHG #31,D0");
        CHECK_EQ(t.r().d[0], Long{0x80000000});
        CHECK_EQ(t.r().d[6] & Z, Long{Z});    // the bit was clear
        CHECK_EQ(t.r().d[7] & Z, Long{0});    // now it is set
        Rig u;  // on memory the bit number is taken modulo 8
        u.exec("MOVEA.L #$2000,A0\nBSET #9,(A0)");
        CHECK_EQ(u.mem.read8(0x2000), Byte{0x02});
    }
    {
        Rig t;  // TAS
        t.exec("MOVEA.L #$2000,A0\nTAS (A0)");
        CHECK_EQ(t.mem.read8(0x2000), Byte{0x80});
        CHECK_EQ(t.ccr() & Z, Word(Z));
    }
    {
        Rig t;  // Scc and the condition codes after a signed/unsigned compare
        t.exec("MOVEQ #5,D0\nCMP.L #3,D0\nSGT D1\nSLT D2\nSHI D3\nSEQ D4\nSNE D5\nSGE D6\nSLE D7");
        CHECK_EQ(t.r().d[1] & 0xFF, Long{0xFF});
        CHECK_EQ(t.r().d[2] & 0xFF, Long{0});
        CHECK_EQ(t.r().d[3] & 0xFF, Long{0xFF});
        CHECK_EQ(t.r().d[4] & 0xFF, Long{0});
        CHECK_EQ(t.r().d[5] & 0xFF, Long{0xFF});
        CHECK_EQ(t.r().d[6] & 0xFF, Long{0xFF});
        CHECK_EQ(t.r().d[7] & 0xFF, Long{0});
        Rig u;  // signed vs unsigned: -1 < 1 signed, but $FFFFFFFF > 1 unsigned
        u.exec("MOVEQ #-1,D0\nCMP.L #1,D0\nSLT D1\nSHI D2");
        CHECK_EQ(u.r().d[1] & 0xFF, Long{0xFF});
        CHECK_EQ(u.r().d[2] & 0xFF, Long{0xFF});
    }
}

void addressingAndData() {
    {
        Rig t;  // (An)+ / -(An) / d16(An) / indexed / absolute / PC-relative
        t.exec("MOVEA.L #$2000,A0\nMOVE.L #$11223344,(A0)+\nMOVE.W #$5566,(A0)+\nMOVE.B #$77,(A0)\n"
               "MOVE.L -(A0),D0\n"        // -(A0): word $5566, then... (A0) is $2006 -> reads $2002..$2005? see below
               "MOVEA.L #$2000,A1\nMOVEQ #2,D1\nMOVE.B 1(A1,D1.W),D2\nMOVE.W $2000.W,D3\nMOVE.W tbl(PC),D4\n"
               "BRA over\ntbl: DC.W $ABCD\nover: NOP");
        CHECK_EQ(t.mem.read32(0x2000), Long{0x11223344});
        CHECK_EQ(t.mem.read16(0x2004), Word{0x5566});
        CHECK_EQ(t.mem.read8(0x2006), Byte{0x77});
        CHECK_EQ(t.r().d[0], Long{0x33445566});   // -(A0) with A0 = $2006: reads $2002..$2005
        CHECK_EQ(t.r().a[0], Long{0x2002});
        CHECK_EQ(t.r().d[2] & 0xFF, Long{0x44});  // $2000 + 2 + 1
        CHECK_EQ(t.r().d[3] & 0xFFFF, Long{0x1122});
        CHECK_EQ(t.r().d[4] & 0xFFFF, Long{0xABCD});
    }
    {
        Rig t;  // byte access through A7 moves the stack pointer by 2
        t.exec("MOVE.B #1,-(A7)\nMOVE.B (A7)+,D0");
        CHECK_EQ(t.r().a[7], Long{kSsp});
        CHECK_EQ(t.mem.read8(kSsp - 2), Byte{1});
    }
    {
        Rig t;  // MOVEM: registers go out in a fixed order and come back sign-extended for words
        t.exec("MOVEQ #1,D0\nMOVEQ #2,D1\nMOVEQ #3,D2\nMOVEQ #4,D3\nMOVEA.L #$1000,A0\nMOVEM.L D0-D3/A0,-(A7)\n"
               "MOVEM.L (A7)+,D4-D7/A1\n"
               "MOVE.W #$8000,D0\nMOVEM.W D0,-(A7)\nMOVEM.W (A7)+,D2");
        CHECK_EQ(t.r().d[4], Long{1});
        CHECK_EQ(t.r().d[5], Long{2});
        CHECK_EQ(t.r().d[6], Long{3});
        CHECK_EQ(t.r().d[7], Long{4});
        CHECK_EQ(t.r().a[1], Long{0x1000});
        CHECK_EQ(t.r().d[2], Long{0xFFFF8000});
        CHECK_EQ(t.r().a[7], Long{kSsp});
    }
    {
        Rig t;  // MOVEM with control addressing, registers in ascending order
        t.exec("MOVEQ #7,D0\nMOVEQ #8,D1\nMOVEA.L #$2000,A0\nMOVEM.L D0-D1,(A0)\nMOVEM.L (A0),D4-D5");
        CHECK_EQ(t.mem.read32(0x2000), Long{7});
        CHECK_EQ(t.mem.read32(0x2004), Long{8});
        CHECK_EQ(t.r().d[5], Long{8});
    }
    {
        Rig t;  // LINK / UNLK
        t.exec("MOVEA.L #$1234,A6\nLINK A6,#-8\nMOVE.L A7,D0\nMOVE.L A6,D1\nUNLK A6");
        CHECK_EQ(t.r().d[1], Long{kSsp - 4});
        CHECK_EQ(t.r().d[0], Long{kSsp - 12});
        CHECK_EQ(t.r().a[6], Long{0x1234});
        CHECK_EQ(t.r().a[7], Long{kSsp});
    }
    {
        Rig t;  // LEA / PEA
        t.exec("MOVEA.L #$2000,A0\nLEA 8(A0),A1\nPEA 4(A0)\nMOVE.L (A7)+,D0");
        CHECK_EQ(t.r().a[1], Long{0x2008});
        CHECK_EQ(t.r().d[0], Long{0x2004});
    }
    {
        Rig t;  // MOVEP
        t.exec("MOVE.L #$11223344,D0\nMOVEA.L #$2000,A0\nMOVEP.L D0,0(A0)\nMOVEP.L 0(A0),D1");
        CHECK_EQ(t.mem.read8(0x2000), Byte{0x11});
        CHECK_EQ(t.mem.read8(0x2002), Byte{0x22});
        CHECK_EQ(t.mem.read8(0x2004), Byte{0x33});
        CHECK_EQ(t.mem.read8(0x2006), Byte{0x44});
        CHECK_EQ(t.mem.read8(0x2001), Byte{0});
        CHECK_EQ(t.r().d[1], Long{0x11223344});
    }
    {
        Rig t;  // 16 MiB wraparound: the upper address byte is ignored
        t.mem.write16(0x2000, 0xBEEF);
        t.exec("MOVEA.L #$55002000,A0\nMOVE.W (A0),D0");
        CHECK_EQ(t.r().d[0] & 0xFFFF, Long{0xBEEF});
    }
}

void controlFlow() {
    {
        Rig t;
        t.exec("MOVEQ #0,D0\nBSR sub\nBSR sub\nJSR sub\nBRA done2\nsub: ADDQ.W #1,D0\nRTS\ndone2: NOP");
        CHECK_EQ(t.r().d[0], Long{3});
        CHECK_EQ(t.r().a[7], Long{kSsp});
        CHECK_EQ(t.cpu.callDepth(), 0);
    }
    {
        Rig t;  // DBRA loop: 9+8+...+0
        t.exec("MOVEQ #0,D0\nMOVE.W #9,D1\nloop: ADD.W D1,D0\nDBRA D1,loop");
        CHECK_EQ(t.r().d[0], Long{45});
        CHECK_EQ(t.r().d[1] & 0xFFFF, Long{0xFFFF});
    }
    {
        Rig t;  // DBcc leaves the loop early when the condition is true
        t.exec("MOVEQ #0,D0\nMOVE.W #9,D1\nloop: ADDQ.W #1,D0\nCMP.W #3,D0\nDBEQ D1,loop");
        CHECK_EQ(t.r().d[0], Long{3});
        CHECK_EQ(t.r().d[1] & 0xFFFF, Long{7});   // decremented twice; the third pass leaves on EQ
    }
    {
        Rig t;  // branch displacements: short, word, backward
        t.exec("MOVEQ #0,D0\nBRA.S a\nMOVEQ #99,D0\na: BRA.W b\nMOVEQ #98,D0\nb: ADDQ.W #1,D0\nBNE c\nMOVEQ #97,D0\nc: NOP");
        CHECK_EQ(t.r().d[0], Long{1});
    }
    {
        Rig t;  // JMP through a register and RTR (which restores only the condition codes)
        t.exec("MOVEA.L #tgt,A0\nJMP (A0)\nMOVEQ #99,D0\ntgt: PEA ret\nMOVE.W #$FF,-(A7)\nRTR\nret: NOP");
        CHECK_EQ(t.ccr(), Word(X | N | Z | V | C));  // RTR restores the condition codes from the stack
        CHECK_EQ(t.r().a[7], Long{kSsp});
    }
}

void exceptions() {
    {
        Rig t;  // TRAP: stack frame is SR then PC, RTE returns
        t.handler(35, "MOVE.W (A7),D5\nMOVE.L 2(A7),D6\nADDQ.W #1,D0\nRTE");
        t.exec("MOVE.W #$2005,SR\nMOVEQ #0,D0\nTRAP #3\nafter: MOVEQ #7,D1");
        CHECK_EQ(t.r().d[0], Long{1});
        CHECK_EQ(t.r().d[1], Long{7});
        CHECK_EQ(t.r().d[5] & 0xFFFF, Long{0x2004});               // SR as it was (MOVEQ #0 set Z)
        CHECK_EQ(t.r().d[6], static_cast<Long>(t.symbols.at("after")));  // return address = next instruction
        CHECK_EQ(t.r().sr & kSrIntMask, Word{0x0000} | (t.r().sr & kSrIntMask));
        CHECK_EQ(t.r().a[7], Long{kSsp});
    }
    {
        Rig t;  // privilege violation in user mode; the stacked PC is the offending instruction
        t.handler(8, "MOVE.L 2(A7),D6\nMOVE.W (A7),D5\nMOVEQ #1,D0\nSTOP #$2700");
        t.load("MOVE.W #$0000,SR\nbad: MOVE.W #$2000,SR\nMOVEQ #2,D0\nSTOP #$2700", kCode);
        t.cpu.run(100);
        CHECK_EQ(t.r().d[0], Long{1});
        CHECK_EQ(t.r().d[6], static_cast<Long>(t.symbols.at("bad")));
        CHECK_EQ(t.r().d[5] & 0xFFFF, Long{0x0000});               // the user-mode SR
        CHECK(t.r().supervisor());
    }
    {
        Rig t;  // illegal instruction and Line A / Line F: stacked PC = address of the word
        t.handler(4, "MOVE.L 2(A7),D6\nSTOP #$2700");
        t.handler(10, "MOVE.L 2(A7),D7\nSTOP #$2700");
        t.load("NOP\nill: DC.W $4AFC", kCode);
        t.cpu.run(100);
        CHECK_EQ(t.r().d[6], static_cast<Long>(t.symbols.at("ill")));
        Rig u;
        u.handler(10, "MOVE.L 2(A7),D7\nSTOP #$2700");
        u.load("NOP\nlinea: DC.W $A123", kCode);
        u.cpu.run(100);
        CHECK_EQ(u.r().d[7], static_cast<Long>(u.symbols.at("linea")));
        Rig w;  // MC68010 opcodes are illegal too
        w.handler(4, "MOVEQ #1,D0\nSTOP #$2700");
        w.load("DC.W $4E7A,$0801", kCode);  // MOVEC
        w.cpu.run(100);
        CHECK_EQ(w.r().d[0], Long{1});
    }
    {
        Rig t;  // CHK, TRAPV, division by zero: PC of the *next* instruction
        t.handler(6, "MOVE.W (A7),D6\nMOVE.L 2(A7),D7\nSTOP #$2700");
        t.load("MOVEQ #5,D0\nCHK #3,D0\nnext: NOP", kCode);
        t.cpu.run(100);
        CHECK_EQ(t.r().d[7], static_cast<Long>(t.symbols.at("next")));
        CHECK_EQ(t.r().d[6] & N, Long{0});  // above the bound: N cleared
        Rig u;
        u.handler(6, "MOVE.W (A7),D6\nSTOP #$2700");
        u.load("MOVEQ #-1,D0\nCHK #3,D0\nNOP", kCode);
        u.cpu.run(100);
        CHECK_EQ(u.r().d[6] & N, Long{N});  // negative: N set
        Rig w;
        w.handler(7, "MOVEQ #1,D0\nSTOP #$2700");
        w.load("MOVE.W #2,CCR\nTRAPV", kCode);
        w.cpu.run(100);
        CHECK_EQ(w.r().d[0], Long{1});
        Rig z;
        z.handler(5, "MOVE.L 2(A7),D7\nSTOP #$2700");
        z.load("MOVEQ #1,D0\nMOVEQ #0,D1\nDIVU D1,D0\nnext: NOP", kCode);
        z.cpu.run(100);
        CHECK_EQ(z.r().d[7], static_cast<Long>(z.symbols.at("next")));
    }
    {
        Rig t;  // address error on a word access: 14 byte frame
        t.handler(3, "MOVE.W (A7),D5\nMOVE.L 2(A7),D6\nMOVE.W 6(A7),D7\nSTOP #$2700");
        t.load("MOVEA.L #$2001,A0\nMOVE.W (A0),D0\nNOP", kCode);
        const RunResult r = t.cpu.run(100);
        CHECK(r.reason == StopReason::Stopped);
        CHECK_EQ(t.r().d[6], Long{0x2001});          // the faulting address
        CHECK_EQ(t.r().d[7] & 0xFFFF, Long{0x3010});  // the instruction word
        CHECK_EQ(t.r().d[5] & 0x1F, Long{0x15});      // read access, supervisor data space
        CHECK_EQ(t.r().a[7], Long{kSsp - 14});
    }
    {
        Rig t;  // jump to an odd address: address error in program space
        t.handler(3, "MOVE.W (A7),D5\nMOVE.L 2(A7),D6\nSTOP #$2700");
        t.load("MOVEA.L #$1001,A0\nJMP (A0)", kCode);
        t.cpu.run(100);
        CHECK_EQ(t.r().d[6], Long{0x1001});
        CHECK_EQ(t.r().d[5] & 0x1F, Long{0x16});  // read access, supervisor program space
    }
    {
        Rig t;  // a fault while stacking an exception frame halts the processor
        t.load("MOVEA.L #$8001,A7\nTRAP #0\nNOP", kCode);
        const RunResult r = t.cpu.run(100);
        CHECK(r.reason == StopReason::Halted);
        CHECK(t.cpu.halted());
        CHECK(t.cpu.step().reason == StopReason::Halted);
        t.cpu.reset();
        CHECK(!t.cpu.halted());
    }
    {
        Rig t;  // user and supervisor stack pointers
        t.handler(32, "MOVE.L A7,D7\nRTE");
        t.exec("MOVEA.L #$6000,A0\nMOVE A0,USP\nANDI #$DFFF,SR\nMOVE.L A7,D0\nTRAP #0\nMOVE.L A7,D1");
        CHECK_EQ(t.r().d[0], Long{0x6000});            // in user mode A7 is the USP
        CHECK_EQ(t.r().d[7], Long{kSsp - 6});          // the handler runs on the supervisor stack
        CHECK_EQ(t.r().d[1], Long{0x6000});            // back in user mode
        CHECK(!t.r().supervisor());
        CHECK_EQ(t.r().usp(), Long{0x6000});
        CHECK_EQ(t.r().ssp(), Long{kSsp});
    }
}

void interruptsStopTrace() {
    {
        Rig t;  // an interrupt is taken when its level is above the mask
        t.handler(27, "MOVE.W (A7),D5\nMOVE.L 2(A7),D6\nMOVE.W SR,D7\nADDQ.W #1,D0\nRTE");
        t.load("MOVE.W #$2400,SR\nNOP\nNOP\nMOVE.W #$2000,SR\nnop3: NOP\nNOP\nSTOP #$2700", kCode);
        t.cpu.interrupt(3);
        t.cpu.step();  // MOVE #$2400,SR (mask 4)
        t.cpu.step();  // NOP: the level-3 request is masked
        CHECK_EQ(t.r().d[0], Long{0});
        t.cpu.step();  // NOP
        t.cpu.step();  // MOVE #$2000,SR: mask 0
        const StepResult s = t.cpu.step();  // takes the interrupt instead of the next instruction
        CHECK(s.reason == StopReason::None);
        CHECK_EQ(t.r().pc, static_cast<Address>(0x4000 + 27 * 0x40));
        CHECK_EQ(t.r().sr & kSrIntMask, Word{0x0300});     // mask raised to the level
        t.cpu.run(10);                                     // handler runs and returns, then STOP
        CHECK_EQ(t.r().d[0], Long{1});
        CHECK_EQ(t.r().d[5] & 0xFFFF, Long{0x2000});
        CHECK_EQ(t.r().d[6], static_cast<Long>(t.symbols.at("nop3")));
        CHECK_EQ(t.r().d[7] & kSrIntMask, Long{0x0300});
    }
    {
        Rig t;  // level 7 is not maskable
        t.handler(31, "MOVEQ #7,D0\nSTOP #$2700");
        t.load("NOP\nNOP", kCode);
        t.cpu.interrupt(7);
        t.cpu.run(10);
        CHECK_EQ(t.r().d[0], Long{7});
    }
    {
        Rig t;  // a vector supplied by the device
        t.handler(64, "MOVEQ #9,D0\nSTOP #$2700");
        t.load("MOVE.W #$2000,SR\nNOP", kCode);
        t.cpu.step();
        t.cpu.interrupt(2, 64);
        t.cpu.run(10);
        CHECK_EQ(t.r().d[0], Long{9});
    }
    {
        Rig t;  // STOP waits for an interrupt and continues after it
        t.handler(26, "MOVEQ #5,D0\nRTE");
        t.load("STOP #$2000\nMOVEQ #6,D1\nSTOP #$2700", kCode);
        const StepResult s = t.cpu.step();
        CHECK(s.reason == StopReason::Stopped);
        CHECK(t.cpu.stopped());
        CHECK(t.cpu.step().reason == StopReason::Stopped);   // nothing happens without an interrupt
        t.cpu.interrupt(2);
        t.cpu.run(10);
        CHECK_EQ(t.r().d[0], Long{5});
        CHECK_EQ(t.r().d[1], Long{6});
    }
    {
        Rig t;  // trace: an exception after every instruction while T is set
        t.handler(9, "ADDQ.W #1,D0\nRTE");
        t.load("ORI #$8000,SR\nMOVEQ #1,D1\nMOVEQ #2,D2\nANDI #$7FFF,SR\nNOP\ndone: NOP", kCode);
        t.cpu.runUntil(static_cast<Address>(t.symbols.at("done")), 100);
        // traced: MOVEQ #1, MOVEQ #2 and ANDI (T was set when it started) - not the ORI that set T
        CHECK_EQ(t.r().d[0], Long{3});
        CHECK_EQ(t.r().d[1], Long{1});
        CHECK_EQ(t.r().d[2], Long{2});
    }
    {
        Rig t;  // RESET is privileged and calls the hook
        int calls = 0;
        t.cpu.resetHook = [&] { ++calls; };
        t.handler(8, "MOVEQ #1,D0\nSTOP #$2700");
        t.load("RESET\nMOVE.W #$0000,SR\nRESET", kCode);
        t.cpu.run(100);
        CHECK_EQ(calls, 1);
        CHECK_EQ(t.r().d[0], Long{1});
    }
}

void runControl() {
    const char* program =
        "start:  MOVEQ #0,D0\n"
        "loop:   ADDQ.W #1,D0\n"
        "        BSR sub\n"
        "        CMP.W #5,D0\n"
        "        BNE loop\n"
        "end:    STOP #$2700\n"
        "sub:    MOVE.W D0,$2000.W\n"
        "        ADDQ.W #1,D1\n"
        "        RTS\n";
    {
        Rig t;
        t.load(program, kCode);
        const RunResult r = t.cpu.run(3);
        CHECK(r.reason == StopReason::Limit);
        CHECK_EQ(r.instructions, std::uint64_t{3});
        CHECK_EQ(t.cpu.instructionCount(), std::uint64_t{3});
    }
    {
        Rig t;  // breakpoint stops before the instruction; the next run() steps off it
        t.load(program, kCode);
        const Address sub = static_cast<Address>(t.symbols.at("sub"));
        t.cpu.addBreakpoint(sub);
        RunResult r = t.cpu.run(100);
        CHECK(r.reason == StopReason::Breakpoint);
        CHECK_EQ(t.r().pc, sub);
        CHECK_EQ(t.r().d[1], Long{0});
        r = t.cpu.run(2);  // starts at the breakpoint without stopping again
        CHECK(r.reason == StopReason::Limit);
        CHECK_EQ(t.r().d[1], Long{1});
        t.cpu.removeBreakpoint(sub);
        r = t.cpu.run(1000);
        CHECK(r.reason == StopReason::Stopped);
        CHECK_EQ(t.r().d[0], Long{5});
        CHECK_EQ(t.r().d[1], Long{5});
    }
    {
        Rig t;  // runUntil, runUntilReturn, stepOver
        t.load(program, kCode);
        const Address loop = static_cast<Address>(t.symbols.at("loop"));
        const Address sub = static_cast<Address>(t.symbols.at("sub"));
        RunResult r = t.cpu.runUntil(sub, 100);
        CHECK(r.reason == StopReason::Reached);
        CHECK_EQ(t.r().pc, sub);
        r = t.cpu.runUntilReturn(100);
        CHECK(r.reason == StopReason::Returned);
        CHECK_EQ(t.r().d[1], Long{1});
        CHECK_EQ(t.cpu.callDepth(), 0);
        t.cpu.reset();
        t.r().d[1] = 0;
        t.cpu.runUntil(loop, 100);
        t.cpu.step();  // ADDQ
        const Address bsr = t.r().pc;
        r = t.cpu.stepOver(100);  // the whole BSR sub ... RTS
        CHECK(r.reason == StopReason::Reached);
        CHECK_EQ(t.r().pc, bsr + 2);
        CHECK_EQ(t.r().d[1], Long{1});
        r = t.cpu.stepOver(100);  // an ordinary instruction: one step
        CHECK_EQ(r.instructions, std::uint64_t{1});
    }
    {
        Rig t;  // watchpoints
        t.load(program, kCode);
        t.cpu.addWatchpoint(0x2000, 2, false, true);
        const RunResult r = t.cpu.run(1000);
        CHECK(r.reason == StopReason::Watchpoint);
        CHECK_EQ(r.watchAddress, Address{0x2000});
        CHECK_EQ(t.mem.read16(0x2000), Word{1});
        t.cpu.clearWatchpoints();
        t.cpu.addWatchpoint(0x2000, 2, true, false);  // reads only: never hit
        CHECK(t.cpu.run(1000).reason == StopReason::Stopped);
    }
    {
        Rig t;  // call() a subroutine and return to a sentinel address
        t.load("sub: ADD.L D0,D0\nADDQ.L #1,D0\nRTS", 0x2000);
        t.r().d[0] = 20;
        const RunResult r = t.cpu.call(0x2000, 0xF00000, 100);
        CHECK(r.reason == StopReason::Reached);
        CHECK_EQ(t.r().d[0], Long{41});
        CHECK_EQ(t.r().pc, Address{0xF00000});
        CHECK_EQ(t.r().a[7], Long{kSsp});
    }
    {
        Rig t;  // hooks: trace, access log, device registers
        int traced = 0;
        std::vector<std::string> log;
        t.cpu.traceHook = [&](const Registers&, Address, Word) { ++traced; };
        t.cpu.accessHook = [&](Address a, Size s, Long v, bool w) {
            log.push_back(std::string(w ? "W" : "R") + std::to_string(static_cast<int>(s)) + ":" + hexDigits(a, 6) + ":" + hexDigits(v, 1));
        };
        t.cpu.readOverride = [](Address a, Size, Long& v) {
            if (a == 0xDFF006) { v = 0x1234; return true; }  // a "register"
            return false;
        };
        bool swallowed = false;
        t.cpu.writeOverride = [&](Address a, Size, Long) {
            if (a == 0xDFF180) { swallowed = true; return true; }
            return false;
        };
        t.exec("MOVE.W $DFF006,D0\nMOVE.W #$0F0,$DFF180\nMOVE.B #7,$2000");
        CHECK_EQ(t.r().d[0] & 0xFFFF, Long{0x1234});
        CHECK(swallowed);
        CHECK_EQ(t.mem.read16(0xDFF180), Word{0});      // the write did not reach memory
        CHECK_EQ(t.mem.read8(0x2000), Byte{7});
        CHECK(traced >= 3);
        CHECK(!log.empty());
        bool sawWrite = false;
        for (const std::string& l : log) sawWrite = sawWrite || l == "W1:002000:7";
        CHECK(sawWrite);
    }
    {
        Rig t;  // reset loads SSP and PC from the vectors
        t.mem.write32(0, 0x7000);
        t.mem.write32(4, 0x1230);
        t.cpu.reset();
        CHECK_EQ(t.r().a[7], Long{0x7000});
        CHECK_EQ(t.r().pc, Address{0x1230});
        CHECK_EQ(t.r().sr, Word{0x2700});
        CHECK(t.r().supervisor());
    }
}

// What the disassembler calls an instruction, the processor must not call illegal.
void agreesWithDecoder() {
    std::size_t checked = 0;
    for (unsigned w = 0; w < 0x10000; ++w) {
        Rig t;
        const Word pattern[4] = {0x0010, 0x0020, 0x0030, 0x0040};
        t.mem.write16(kCode, static_cast<Word>(w));
        for (int i = 0; i < 4; ++i) t.mem.write16(kCode + 2 + 2 * static_cast<Address>(i), pattern[i]);
        const Instruction in = decode(t.mem, kCode);
        for (int i = 0; i < 8; ++i) t.r().a[static_cast<size_t>(i)] = 0x2000;
        t.r().a[7] = kSsp;
        t.r().pc = kCode;
        const StepResult s = t.cpu.step();
        const bool illegal = s.exception == kVecIllegal || s.exception == kVecLineA || s.exception == kVecLineF;
        if (in.valid && w != 0x4AFC) {  // ILLEGAL itself is a valid instruction that raises the exception
            CHECK(!illegal);
            ++checked;
        }
        if (illegal && in.valid && w != 0x4AFC) {
            std::cerr << "decoder accepts " << hexDigits(w, 4) << " (" << in.text() << ") but the processor says illegal\n";
            break;
        }
        if (w >= 0xA000 && w < 0xB000) CHECK(s.exception == kVecLineA);
        if (w >= 0xF000) CHECK(s.exception == kVecLineF);
    }
    CHECK(checked > 40000);
}

}  // namespace

int main() {
    arithmetic();
    logicAndShifts();
    addressingAndData();
    controlFlow();
    exceptions();
    interruptsStopTrace();
    runControl();
    agreesWithDecoder();
    return finish("cpu");
}
