#include "forms.hpp"

#include <deque>
#include <string>

namespace m68k::detail {

namespace {

const char* intern(std::string s) {
    static std::deque<std::string> pool;  // deque: references stay valid
    pool.push_back(std::move(s));
    return pool.back().c_str();
}

std::vector<Form> build() {
    std::vector<Form> t;
    using S = SizeKind;
    constexpr Size kB = Size::B, kW = Size::W, kL = Size::L;

    auto add = [&](const char* name, unsigned value, unsigned mask, SizeKind sk, Size fixed, bool print,
                   Op a = Op::None, unsigned sa = 0, Op b = Op::None, unsigned sb = 0,
                   bool noAnByte = false) {
        Form f;
        f.name = name;
        f.value = static_cast<std::uint16_t>(value);
        f.mask = static_cast<std::uint16_t>(mask);
        f.sk = sk;
        f.fixed = fixed;
        f.print = print;
        f.noAnByte = noAnByte;
        f.op[0] = a;
        f.op[1] = b;
        f.set[0] = static_cast<std::uint16_t>(sa);
        f.set[1] = static_cast<std::uint16_t>(sb);
        t.push_back(f);
    };

    // ---- group 0: immediate operations, bit operations, MOVEP ----------------------------------
    add("ORI", 0x003C, 0xFFFF, S::Fixed, kB, false, Op::Imm, 0, Op::Ccr);
    add("ORI", 0x007C, 0xFFFF, S::Fixed, kW, false, Op::Imm, 0, Op::Sr);
    add("ANDI", 0x023C, 0xFFFF, S::Fixed, kB, false, Op::Imm, 0, Op::Ccr);
    add("ANDI", 0x027C, 0xFFFF, S::Fixed, kW, false, Op::Imm, 0, Op::Sr);
    add("EORI", 0x0A3C, 0xFFFF, S::Fixed, kB, false, Op::Imm, 0, Op::Ccr);
    add("EORI", 0x0A7C, 0xFFFF, S::Fixed, kW, false, Op::Imm, 0, Op::Sr);

    add("MOVEP", 0x0108, 0xF1F8, S::Fixed, kW, true, Op::D16An0, 0, Op::Dn9);
    add("MOVEP", 0x0148, 0xF1F8, S::Fixed, kL, true, Op::D16An0, 0, Op::Dn9);
    add("MOVEP", 0x0188, 0xF1F8, S::Fixed, kW, true, Op::Dn9, 0, Op::D16An0);
    add("MOVEP", 0x01C8, 0xF1F8, S::Fixed, kL, true, Op::Dn9, 0, Op::D16An0);

    add("BTST", 0x0100, 0xF1C0, S::Any, kB, false, Op::Dn9, 0, Op::Ea, kData);
    add("BCHG", 0x0140, 0xF1C0, S::Any, kB, false, Op::Dn9, 0, Op::Ea, kDataAlt);
    add("BCLR", 0x0180, 0xF1C0, S::Any, kB, false, Op::Dn9, 0, Op::Ea, kDataAlt);
    add("BSET", 0x01C0, 0xF1C0, S::Any, kB, false, Op::Dn9, 0, Op::Ea, kDataAlt);
    add("BTST", 0x0800, 0xFFC0, S::Any, kB, false, Op::ImmBit, 0, Op::Ea, kData & ~kImm);
    add("BCHG", 0x0840, 0xFFC0, S::Any, kB, false, Op::ImmBit, 0, Op::Ea, kDataAlt);
    add("BCLR", 0x0880, 0xFFC0, S::Any, kB, false, Op::ImmBit, 0, Op::Ea, kDataAlt);
    add("BSET", 0x08C0, 0xFFC0, S::Any, kB, false, Op::ImmBit, 0, Op::Ea, kDataAlt);

    add("ORI", 0x0000, 0xFF00, S::B76, kB, true, Op::Imm, 0, Op::Ea, kDataAlt);
    add("ANDI", 0x0200, 0xFF00, S::B76, kB, true, Op::Imm, 0, Op::Ea, kDataAlt);
    add("SUBI", 0x0400, 0xFF00, S::B76, kB, true, Op::Imm, 0, Op::Ea, kDataAlt);
    add("ADDI", 0x0600, 0xFF00, S::B76, kB, true, Op::Imm, 0, Op::Ea, kDataAlt);
    add("EORI", 0x0A00, 0xFF00, S::B76, kB, true, Op::Imm, 0, Op::Ea, kDataAlt);
    add("CMPI", 0x0C00, 0xFF00, S::B76, kB, true, Op::Imm, 0, Op::Ea, kDataAlt);

    // ---- groups 1-3: MOVE ---------------------------------------------------------------------
    add("MOVEA", 0x2040, 0xF1C0, S::Fixed, kL, true, Op::Ea, kAll, Op::An9);
    add("MOVEA", 0x3040, 0xF1C0, S::Fixed, kW, true, Op::Ea, kAll, Op::An9);
    add("MOVE", 0x0000, 0xC000, S::Move, kW, true, Op::Ea, kAll, Op::EaDst, kDataAlt, true);

    // ---- group 4: miscellaneous ---------------------------------------------------------------
    add("MOVE", 0x40C0, 0xFFC0, S::Fixed, kW, false, Op::Sr, 0, Op::Ea, kDataAlt);
    add("NEGX", 0x4000, 0xFF00, S::B76, kB, true, Op::Ea, kDataAlt);
    add("CLR", 0x4200, 0xFF00, S::B76, kB, true, Op::Ea, kDataAlt);
    add("NEG", 0x4400, 0xFF00, S::B76, kB, true, Op::Ea, kDataAlt);
    add("NOT", 0x4600, 0xFF00, S::B76, kB, true, Op::Ea, kDataAlt);
    add("MOVE", 0x44C0, 0xFFC0, S::Fixed, kW, false, Op::Ea, kData, Op::Ccr);
    add("MOVE", 0x46C0, 0xFFC0, S::Fixed, kW, false, Op::Ea, kData, Op::Sr);
    add("NBCD", 0x4800, 0xFFC0, S::Fixed, kB, false, Op::Ea, kDataAlt);
    add("SWAP", 0x4840, 0xFFF8, S::Fixed, kW, false, Op::Dn0);
    add("PEA", 0x4840, 0xFFC0, S::Fixed, kL, false, Op::Ea, kControl);
    add("EXT", 0x4880, 0xFFF8, S::Fixed, kW, true, Op::Dn0);
    add("EXT", 0x48C0, 0xFFF8, S::Fixed, kL, true, Op::Dn0);
    add("MOVEM", 0x4880, 0xFF80, S::W6, kW, true, Op::RegList, 0, Op::Ea, kCtlAlt | kPreDec);
    add("MOVEM", 0x4C80, 0xFF80, S::W6, kW, true, Op::Ea, kControl | kPostInc, Op::RegList);
    add("ILLEGAL", 0x4AFC, 0xFFFF, S::None, kW, false);
    add("TAS", 0x4AC0, 0xFFC0, S::Fixed, kB, false, Op::Ea, kDataAlt);
    add("TST", 0x4A00, 0xFF00, S::B76, kB, true, Op::Ea, kDataAlt);
    add("CHK", 0x4180, 0xF1C0, S::Fixed, kW, false, Op::Ea, kData, Op::Dn9);
    add("LEA", 0x41C0, 0xF1C0, S::Fixed, kL, false, Op::Ea, kControl, Op::An9);
    add("TRAP", 0x4E40, 0xFFF0, S::None, kW, false, Op::ImmTrap);
    add("LINK", 0x4E50, 0xFFF8, S::None, kW, false, Op::An0, 0, Op::ImmSWord);
    add("UNLK", 0x4E58, 0xFFF8, S::None, kW, false, Op::An0);
    add("MOVE", 0x4E60, 0xFFF8, S::Fixed, kL, false, Op::An0, 0, Op::Usp);
    add("MOVE", 0x4E68, 0xFFF8, S::Fixed, kL, false, Op::Usp, 0, Op::An0);
    add("RESET", 0x4E70, 0xFFFF, S::None, kW, false);
    add("NOP", 0x4E71, 0xFFFF, S::None, kW, false);
    add("STOP", 0x4E72, 0xFFFF, S::None, kW, false, Op::ImmWord);
    add("RTE", 0x4E73, 0xFFFF, S::None, kW, false);
    add("RTS", 0x4E75, 0xFFFF, S::None, kW, false);
    add("TRAPV", 0x4E76, 0xFFFF, S::None, kW, false);
    add("RTR", 0x4E77, 0xFFFF, S::None, kW, false);
    add("JSR", 0x4E80, 0xFFC0, S::None, kW, false, Op::Ea, kControl);
    add("JMP", 0x4EC0, 0xFFC0, S::None, kW, false, Op::Ea, kControl);

    // ---- group 5: ADDQ/SUBQ/Scc/DBcc ----------------------------------------------------------
    for (unsigned c = 0; c < 16; ++c) {
        const std::string cond = kCondNames[c];
        add(intern(c == 1 ? "DBRA" : "DB" + cond), 0x50C8 | (c << 8), 0xFFF8, S::None, kW, false, Op::Dn0, 0,
            Op::DbDisp);
    }
    for (unsigned c = 0; c < 16; ++c) {
        add(intern(std::string("S") + kCondNames[c]), 0x50C0 | (c << 8), 0xFFC0, S::Fixed, kB, false, Op::Ea,
            kDataAlt);
    }
    add("ADDQ", 0x5000, 0xF100, S::B76, kB, true, Op::ImmQ3, 0, Op::Ea, kAlterable, true);
    add("SUBQ", 0x5100, 0xF100, S::B76, kB, true, Op::ImmQ3, 0, Op::Ea, kAlterable, true);

    // ---- group 6: branches --------------------------------------------------------------------
    for (unsigned c = 0; c < 16; ++c) {
        const char* name = c == 0 ? "BRA" : c == 1 ? "BSR" : intern(std::string("B") + kCondNames[c]);
        add(name, 0x6000 | (c << 8), 0xFF00, S::None, kW, false, Op::Branch);
    }

    // ---- group 7: MOVEQ -----------------------------------------------------------------------
    add("MOVEQ", 0x7000, 0xF100, S::Fixed, kL, false, Op::ImmMoveq, 0, Op::Dn9);

    // ---- group 8: OR/DIV/SBCD -----------------------------------------------------------------
    add("SBCD", 0x8100, 0xF1F8, S::Fixed, kB, false, Op::Dn0, 0, Op::Dn9);
    add("SBCD", 0x8108, 0xF1F8, S::Fixed, kB, false, Op::PreDec0, 0, Op::PreDec9);
    add("DIVU", 0x80C0, 0xF1C0, S::Fixed, kW, false, Op::Ea, kData, Op::Dn9);
    add("DIVS", 0x81C0, 0xF1C0, S::Fixed, kW, false, Op::Ea, kData, Op::Dn9);
    add("OR", 0x8000, 0xF100, S::B76, kB, true, Op::Ea, kData, Op::Dn9);
    add("OR", 0x8100, 0xF100, S::B76, kB, true, Op::Dn9, 0, Op::Ea, kMemAlt);

    // ---- group 9: SUB -------------------------------------------------------------------------
    add("SUBA", 0x90C0, 0xF0C0, S::W8, kW, true, Op::Ea, kAll, Op::An9);
    add("SUBX", 0x9100, 0xF138, S::B76, kB, true, Op::Dn0, 0, Op::Dn9);
    add("SUBX", 0x9108, 0xF138, S::B76, kB, true, Op::PreDec0, 0, Op::PreDec9);
    add("SUB", 0x9000, 0xF100, S::B76, kB, true, Op::Ea, kAll, Op::Dn9, 0, true);
    add("SUB", 0x9100, 0xF100, S::B76, kB, true, Op::Dn9, 0, Op::Ea, kMemAlt);

    // ---- group B: CMP/EOR ---------------------------------------------------------------------
    add("CMPA", 0xB0C0, 0xF0C0, S::W8, kW, true, Op::Ea, kAll, Op::An9);
    add("CMPM", 0xB108, 0xF138, S::B76, kB, true, Op::PostInc0, 0, Op::PostInc9);
    add("EOR", 0xB100, 0xF100, S::B76, kB, true, Op::Dn9, 0, Op::Ea, kDataAlt);
    add("CMP", 0xB000, 0xF100, S::B76, kB, true, Op::Ea, kAll, Op::Dn9, 0, true);

    // ---- group C: AND/MUL/ABCD/EXG ------------------------------------------------------------
    add("ABCD", 0xC100, 0xF1F8, S::Fixed, kB, false, Op::Dn0, 0, Op::Dn9);
    add("ABCD", 0xC108, 0xF1F8, S::Fixed, kB, false, Op::PreDec0, 0, Op::PreDec9);
    add("EXG", 0xC140, 0xF1F8, S::Fixed, kL, false, Op::Dn9, 0, Op::Dn0);
    add("EXG", 0xC148, 0xF1F8, S::Fixed, kL, false, Op::An9, 0, Op::An0);
    add("EXG", 0xC188, 0xF1F8, S::Fixed, kL, false, Op::Dn9, 0, Op::An0);
    add("MULU", 0xC0C0, 0xF1C0, S::Fixed, kW, false, Op::Ea, kData, Op::Dn9);
    add("MULS", 0xC1C0, 0xF1C0, S::Fixed, kW, false, Op::Ea, kData, Op::Dn9);
    add("AND", 0xC000, 0xF100, S::B76, kB, true, Op::Ea, kData, Op::Dn9);
    add("AND", 0xC100, 0xF100, S::B76, kB, true, Op::Dn9, 0, Op::Ea, kMemAlt);

    // ---- group D: ADD -------------------------------------------------------------------------
    add("ADDA", 0xD0C0, 0xF0C0, S::W8, kW, true, Op::Ea, kAll, Op::An9);
    add("ADDX", 0xD100, 0xF138, S::B76, kB, true, Op::Dn0, 0, Op::Dn9);
    add("ADDX", 0xD108, 0xF138, S::B76, kB, true, Op::PreDec0, 0, Op::PreDec9);
    add("ADD", 0xD000, 0xF100, S::B76, kB, true, Op::Ea, kAll, Op::Dn9, 0, true);
    add("ADD", 0xD100, 0xF100, S::B76, kB, true, Op::Dn9, 0, Op::Ea, kMemAlt);

    // ---- group E: shifts and rotates ----------------------------------------------------------
    static constexpr const char* kShift[4] = {"AS", "LS", "ROX", "RO"};
    for (unsigned type = 0; type < 4; ++type) {
        for (unsigned dir = 0; dir < 2; ++dir) {
            const char* name = intern(std::string(kShift[type]) + (dir ? "L" : "R"));
            add(name, 0xE000 | (dir << 8) | (type << 3), 0xF138, S::B76, kB, true, Op::ImmQ3, 0, Op::Dn0);
            add(name, 0xE020 | (dir << 8) | (type << 3), 0xF138, S::B76, kB, true, Op::Dn9, 0, Op::Dn0);
            add(name, 0xE0C0 | (type << 9) | (dir << 8), 0xFFC0, S::Fixed, kW, true, Op::Ea, kMemAlt);
        }
    }
    return t;
}

}  // namespace

const std::vector<Form>& forms() {
    static const std::vector<Form> table = build();
    return table;
}

}  // namespace m68k::detail
