#include "m68k/cpu.hpp"

#include <cstring>
#include <map>

#include "forms.hpp"
#include "m68k/decoder.hpp"

namespace m68k {

using detail::Form;
using detail::Op;
using detail::SizeKind;

struct Cpu::Fault {
    unsigned vector = 0;
    Address pc = 0;          // PC to stack (after an instruction-level fault: what the architecture stacks)
    bool group0 = false;     // address error: long frame
    bool read = true;
    Address address = 0;
    bool instructionSpace = false;
};

namespace {

// ---- opcode table --------------------------------------------------------------------------------

enum class Mn : std::uint8_t {
    Invalid, Ori, Andi, Eori, Subi, Addi, Cmpi, OriCcr, OriSr, AndiCcr, AndiSr, EoriCcr, EoriSr,
    BtstD, BchgD, BclrD, BsetD, BtstS, BchgS, BclrS, BsetS, Movep, Movea, Move, MoveFromSr, MoveToCcr, MoveToSr,
    MoveToUsp, MoveFromUsp, Negx, Clr, Neg, Not, Nbcd, Swap, Pea, Ext, MovemToMem, MovemToReg, Illegal, Tas,
    Tst, Chk, Lea, Trap, Link, Unlk, Reset, Nop, Stop, Rte, Rts, Trapv, Rtr, Jsr, Jmp, Dbcc, Scc, Addq, Subq,
    Bcc, Moveq, SbcdD, SbcdM, Divu, Divs, OrToD, OrToEa, Suba, SubxD, SubxM, SubToD, SubToEa, Cmpa, Cmpm, Eor,
    Cmp, AbcdD, AbcdM, ExgDD, ExgAA, ExgDA, Mulu, Muls, AndToD, AndToEa, Adda, AddxD, AddxM, AddToD, AddToEa,
    ShiftImm, ShiftReg, ShiftMem
};

struct Entry {
    Mn mn = Mn::Invalid;
    Size size = Size::W;
    const Form* form = nullptr;
};

Mn classify(const Form& f) {
    const std::string n = f.name;
    const Op a = f.op[0], b = f.op[1];
    if (f.mask == 0xFFF8 && (f.value & 0xF0F8) == 0x50C8) return Mn::Dbcc;
    if (f.mask == 0xFFC0 && (f.value & 0xF0FF) == 0x50C0) return Mn::Scc;
    if (f.mask == 0xFF00 && (f.value & 0xF000) == 0x6000) return Mn::Bcc;
    if (n == "ORI") return b == Op::Ccr ? Mn::OriCcr : b == Op::Sr ? Mn::OriSr : Mn::Ori;
    if (n == "ANDI") return b == Op::Ccr ? Mn::AndiCcr : b == Op::Sr ? Mn::AndiSr : Mn::Andi;
    if (n == "EORI") return b == Op::Ccr ? Mn::EoriCcr : b == Op::Sr ? Mn::EoriSr : Mn::Eori;
    if (n == "SUBI") return Mn::Subi;
    if (n == "ADDI") return Mn::Addi;
    if (n == "CMPI") return Mn::Cmpi;
    if (n == "BTST") return a == Op::Dn9 ? Mn::BtstD : Mn::BtstS;
    if (n == "BCHG") return a == Op::Dn9 ? Mn::BchgD : Mn::BchgS;
    if (n == "BCLR") return a == Op::Dn9 ? Mn::BclrD : Mn::BclrS;
    if (n == "BSET") return a == Op::Dn9 ? Mn::BsetD : Mn::BsetS;
    if (n == "MOVEP") return Mn::Movep;
    if (n == "MOVEA") return Mn::Movea;
    if (n == "MOVE") {
        if (a == Op::Sr) return Mn::MoveFromSr;
        if (b == Op::Ccr) return Mn::MoveToCcr;
        if (b == Op::Sr) return Mn::MoveToSr;
        if (b == Op::Usp) return Mn::MoveToUsp;
        if (a == Op::Usp) return Mn::MoveFromUsp;
        return Mn::Move;
    }
    if (n == "NEGX") return Mn::Negx;
    if (n == "CLR") return Mn::Clr;
    if (n == "NEG") return Mn::Neg;
    if (n == "NOT") return Mn::Not;
    if (n == "NBCD") return Mn::Nbcd;
    if (n == "SWAP") return Mn::Swap;
    if (n == "PEA") return Mn::Pea;
    if (n == "EXT") return Mn::Ext;
    if (n == "MOVEM") return a == Op::RegList ? Mn::MovemToMem : Mn::MovemToReg;
    if (n == "ILLEGAL") return Mn::Illegal;
    if (n == "TAS") return Mn::Tas;
    if (n == "TST") return Mn::Tst;
    if (n == "CHK") return Mn::Chk;
    if (n == "LEA") return Mn::Lea;
    if (n == "TRAP") return Mn::Trap;
    if (n == "LINK") return Mn::Link;
    if (n == "UNLK") return Mn::Unlk;
    if (n == "RESET") return Mn::Reset;
    if (n == "NOP") return Mn::Nop;
    if (n == "STOP") return Mn::Stop;
    if (n == "RTE") return Mn::Rte;
    if (n == "RTS") return Mn::Rts;
    if (n == "TRAPV") return Mn::Trapv;
    if (n == "RTR") return Mn::Rtr;
    if (n == "JSR") return Mn::Jsr;
    if (n == "JMP") return Mn::Jmp;
    if (n == "ADDQ") return Mn::Addq;
    if (n == "SUBQ") return Mn::Subq;
    if (n == "MOVEQ") return Mn::Moveq;
    if (n == "SBCD") return a == Op::Dn0 ? Mn::SbcdD : Mn::SbcdM;
    if (n == "ABCD") return a == Op::Dn0 ? Mn::AbcdD : Mn::AbcdM;
    if (n == "DIVU") return Mn::Divu;
    if (n == "DIVS") return Mn::Divs;
    if (n == "MULU") return Mn::Mulu;
    if (n == "MULS") return Mn::Muls;
    if (n == "OR") return a == Op::Ea ? Mn::OrToD : Mn::OrToEa;
    if (n == "AND") return a == Op::Ea ? Mn::AndToD : Mn::AndToEa;
    if (n == "SUB") return a == Op::Ea ? Mn::SubToD : Mn::SubToEa;
    if (n == "ADD") return a == Op::Ea ? Mn::AddToD : Mn::AddToEa;
    if (n == "SUBA") return Mn::Suba;
    if (n == "ADDA") return Mn::Adda;
    if (n == "SUBX") return a == Op::Dn0 ? Mn::SubxD : Mn::SubxM;
    if (n == "ADDX") return a == Op::Dn0 ? Mn::AddxD : Mn::AddxM;
    if (n == "CMPA") return Mn::Cmpa;
    if (n == "CMPM") return Mn::Cmpm;
    if (n == "CMP") return Mn::Cmp;
    if (n == "EOR") return Mn::Eor;
    if (n == "EXG") return a == Op::Dn9 && b == Op::Dn0 ? Mn::ExgDD : a == Op::An9 ? Mn::ExgAA : Mn::ExgDA;
    if (n == "ASL" || n == "ASR" || n == "LSL" || n == "LSR" || n == "ROXL" || n == "ROXR" || n == "ROL" || n == "ROR") {
        return a == Op::ImmQ3 ? Mn::ShiftImm : a == Op::Dn9 ? Mn::ShiftReg : Mn::ShiftMem;
    }
    return Mn::Invalid;
}

// One entry per opcode word; built once from the shared instruction table.
const std::vector<Entry>& table() {
    static const std::vector<Entry> t = [] {
        std::vector<Entry> out(0x10000);
        for (unsigned w = 0; w < 0x10000; ++w) {
            for (const Form& f : detail::forms()) {
                if ((w & f.mask) != f.value) continue;
                Size size = Size::W;
                bool ok = true;
                switch (f.sk) {
                    case SizeKind::None:
                    case SizeKind::Any: break;
                    case SizeKind::B76: {
                        const unsigned s = (w >> 6) & 3U;
                        if (s == 3) ok = false;
                        size = s == 0 ? Size::B : s == 1 ? Size::W : Size::L;
                        break;
                    }
                    case SizeKind::Move: {
                        const unsigned s = (w >> 12) & 3U;
                        if (s == 0) ok = false;
                        size = s == 1 ? Size::B : s == 3 ? Size::W : Size::L;
                        break;
                    }
                    case SizeKind::W8: size = (w & 0x100U) ? Size::L : Size::W; break;
                    case SizeKind::W6: size = (w & 0x40U) ? Size::L : Size::W; break;
                    case SizeKind::Fixed: size = f.fixed; break;
                }
                if (!ok) continue;  // another form may take this word
                bool legal = true;
                for (int i = 0; i < 2; ++i) {
                    if (f.op[i] != Op::Ea && f.op[i] != Op::EaDst) continue;
                    const unsigned mode = f.op[i] == Op::Ea ? (w >> 3) & 7U : (w >> 6) & 7U;
                    const unsigned reg = f.op[i] == Op::Ea ? w & 7U : (w >> 9) & 7U;
                    const unsigned bit = detail::eaBitOf(mode, reg);
                    if (bit == 0 || !(f.set[i] & bit)) legal = false;
                    if (f.noAnByte && size == Size::B && bit == detail::kAn) legal = false;
                }
                if (legal) {
                    out[w].mn = classify(f);
                    out[w].size = size;
                    out[w].form = &f;
                }
                break;  // first matching form decides, like the decoder
            }
        }
        return out;
    }();
    return t;
}

// ---- small helpers --------------------------------------------------------------------------------

constexpr Long maskOf(Size s) { return s == Size::B ? 0xFFU : s == Size::W ? 0xFFFFU : 0xFFFFFFFFU; }
constexpr Long msbOf(Size s) { return s == Size::B ? 0x80U : s == Size::W ? 0x8000U : 0x80000000U; }
constexpr unsigned bitsOf(Size s) { return static_cast<unsigned>(s) * 8U; }
constexpr std::int32_t sext(Long v, Size s) {
    return s == Size::B ? static_cast<std::int8_t>(v) : s == Size::W ? static_cast<std::int16_t>(v) : static_cast<std::int32_t>(v);
}
constexpr Long sext16to32(Word w) { return static_cast<Long>(static_cast<std::int32_t>(static_cast<std::int16_t>(w))); }

}  // namespace

// ---- the executor ---------------------------------------------------------------------------------

class Exec {
public:
    explicit Exec(Cpu& c) : c_(c), r_(c.r_), mem_(c.mem_) {}

    // Executes one instruction; throws Cpu::Fault for exceptions.
    void run();

    // Exception processing (also used for interrupts and trace).
    void processException(const Cpu::Fault& f);
    void processInterrupt(unsigned level, unsigned vector);

private:
    struct Ea {
        enum Kind { Dn, An, Mem, Imm } kind = Mem;
        unsigned reg = 0;
        Address addr = 0;
        Long imm = 0;
    };

    Cpu& c_;
    Registers& r_;
    Memory& mem_;

    // ---- memory ----
    [[noreturn]] void raise(unsigned vector, Address pc) {
        Cpu::Fault f;
        f.vector = vector;
        f.pc = pc;
        throw f;
    }
    [[noreturn]] void addressError(Address a, bool read, bool instr) {
        Cpu::Fault f;
        f.vector = kVecAddressError;
        f.group0 = true;
        f.pc = r_.pc;
        f.read = read;
        f.address = a;
        f.instructionSpace = instr;
        throw f;
    }
    void noteAccess(Address a, Size s, Long v, bool write) {
        if (c_.accessHook) c_.accessHook(a, s, v, write);
        for (const auto& w : c_.watches_) {
            if (((write && w.onWrite) || (!write && w.onRead)) && a < w.end && a + static_cast<Address>(s) > w.start) {
                c_.watchHit_ = true;
                c_.watchAddress_ = a;
            }
        }
    }
    Long read(Address a, Size s) {
        if (s != Size::B && (a & 1U)) addressError(a, true, false);
        a &= kAddressMask;
        Long v;
        if (!(c_.readOverride && c_.readOverride(a, s, v))) {
            v = s == Size::B ? mem_.read8(a) : s == Size::W ? mem_.read16(a) : mem_.read32(a);
        }
        noteAccess(a, s, v, false);
        return v;
    }
    void write(Address a, Size s, Long v) {
        if (s != Size::B && (a & 1U)) addressError(a, false, false);
        a &= kAddressMask;
        v &= maskOf(s);
        noteAccess(a, s, v, true);
        if (c_.writeOverride && c_.writeOverride(a, s, v)) return;
        if (s == Size::B) mem_.write8(a, static_cast<Byte>(v));
        else if (s == Size::W) mem_.write16(a, static_cast<Word>(v));
        else mem_.write32(a, v);
    }
    Word fetch16() {
        if (r_.pc & 1U) addressError(r_.pc, true, true);
        const Word w = mem_.read16(r_.pc & kAddressMask);
        r_.pc = (r_.pc + 2) & kAddressMask;
        return w;
    }
    Long fetchImm(Size s) {
        if (s == Size::L) {
            const Long hi = fetch16();
            return (hi << 16) | fetch16();
        }
        const Word w = fetch16();
        return s == Size::B ? (w & 0xFFU) : w;
    }
    void push16(Word v) {
        r_.a[7] -= 2;
        write(r_.a[7], Size::W, v);
    }
    void push32(Long v) {
        r_.a[7] -= 4;
        write(r_.a[7], Size::L, v);
    }
    Word pop16() {
        const Word v = static_cast<Word>(read(r_.a[7], Size::W));
        r_.a[7] += 2;
        return v;
    }
    Long pop32() {
        const Long v = read(r_.a[7], Size::L);
        r_.a[7] += 4;
        return v;
    }

    // ---- flags ----
    bool flag(Word f) const { return (r_.sr & f) != 0; }
    void setFlag(Word f, bool on) { r_.sr = static_cast<Word>(on ? (r_.sr | f) : (r_.sr & ~f)); }
    void setNZ(Long v, Size s) {
        setFlag(kFlagN, (v & msbOf(s)) != 0);
        setFlag(kFlagZ, (v & maskOf(s)) == 0);
    }
    void logicFlags(Long v, Size s) {
        setNZ(v, s);
        setFlag(kFlagV, false);
        setFlag(kFlagC, false);
    }
    bool cond(unsigned c) const {
        const bool C = flag(kFlagC), Z = flag(kFlagZ), N = flag(kFlagN), V = flag(kFlagV);
        switch (c) {
            case 0: return true;
            case 1: return false;
            case 2: return !C && !Z;
            case 3: return C || Z;
            case 4: return !C;
            case 5: return C;
            case 6: return !Z;
            case 7: return Z;
            case 8: return !V;
            case 9: return V;
            case 10: return !N;
            case 11: return N;
            case 12: return N == V;
            case 13: return N != V;
            case 14: return !Z && N == V;
            default: return Z || N != V;
        }
    }

    // ---- arithmetic with flags ----
    Long add(Long src, Long dst, Size s, bool withX) {
        const Long m = maskOf(s), sm = msbOf(s);
        src &= m;
        dst &= m;
        const Long x = (withX && flag(kFlagX)) ? 1U : 0U;
        const Long res = static_cast<Long>((static_cast<std::uint64_t>(dst) + src + x) & m);
        const bool S = src & sm, D = dst & sm, R = res & sm;
        const bool carry = (S && D) || (!R && D) || (S && !R);
        setFlag(kFlagV, (S && D && !R) || (!S && !D && R));
        setFlag(kFlagC, carry);
        setFlag(kFlagX, carry);
        setFlag(kFlagN, R);
        setFlag(kFlagZ, withX ? (flag(kFlagZ) && res == 0) : res == 0);
        return res;
    }
    Long sub(Long src, Long dst, Size s, bool withX, bool updateX = true) {  // dst - src - X
        const Long m = maskOf(s), sm = msbOf(s);
        src &= m;
        dst &= m;
        const Long x = (withX && flag(kFlagX)) ? 1U : 0U;
        const Long res = static_cast<Long>((static_cast<std::uint64_t>(dst) - src - x) & m);
        const bool S = src & sm, D = dst & sm, R = res & sm;
        const bool borrow = (S && !D) || (R && !D) || (S && R);
        setFlag(kFlagV, (!S && D && !R) || (S && !D && R));
        setFlag(kFlagC, borrow);
        if (updateX) setFlag(kFlagX, borrow);
        setFlag(kFlagN, R);
        setFlag(kFlagZ, withX ? (flag(kFlagZ) && res == 0) : res == 0);
        return res;
    }

    // ---- registers ----
    Long readD(unsigned n, Size s) const { return r_.d[n] & maskOf(s); }
    void writeD(unsigned n, Size s, Long v) {
        if (s == Size::L) r_.d[n] = v;
        else r_.d[n] = (r_.d[n] & ~maskOf(s)) | (v & maskOf(s));
    }
    Long incr(unsigned reg, Size s) const { return (s == Size::B && reg == 7) ? 2U : static_cast<Long>(s); }

    // ---- effective addresses ----
    Ea decodeEa(unsigned mode, unsigned reg, Size s) {
        Ea e;
        e.reg = reg;
        switch (mode) {
            case 0: e.kind = Ea::Dn; return e;
            case 1: e.kind = Ea::An; return e;
            case 2: e.addr = r_.a[reg]; return e;
            case 3:
                e.addr = r_.a[reg];
                r_.a[reg] += incr(reg, s);
                return e;
            case 4:
                r_.a[reg] -= incr(reg, s);
                e.addr = r_.a[reg];
                return e;
            case 5: {
                const Long d = sext16to32(fetch16());
                e.addr = r_.a[reg] + d;
                return e;
            }
            case 6: e.addr = indexed(r_.a[reg]); return e;
            default: break;
        }
        switch (reg) {
            case 0: e.addr = sext16to32(fetch16()); return e;
            case 1: {
                const Long hi = fetch16();
                e.addr = (hi << 16) | fetch16();
                return e;
            }
            case 2: {
                const Address base = r_.pc;
                e.addr = base + sext16to32(fetch16());
                return e;
            }
            case 3: {
                const Address base = r_.pc;
                e.addr = indexed(base);
                return e;
            }
            default:  // 4: immediate
                e.kind = Ea::Imm;
                e.imm = fetchImm(s);
                return e;
        }
    }
    Long indexed(Long base) {
        const Word ext = fetch16();
        Long idx = (ext & 0x8000U) ? r_.a[(ext >> 12) & 7U] : r_.d[(ext >> 12) & 7U];
        if (!(ext & 0x0800U)) idx = sext16to32(static_cast<Word>(idx));
        return base + static_cast<Long>(static_cast<std::int32_t>(static_cast<std::int8_t>(ext & 0xFFU))) + idx;
    }
    Long readEa(const Ea& e, Size s) {
        switch (e.kind) {
            case Ea::Dn: return readD(e.reg, s);
            case Ea::An: return r_.a[e.reg] & maskOf(s);
            case Ea::Imm: return e.imm & maskOf(s);
            default: return read(e.addr, s);
        }
    }
    void writeEa(const Ea& e, Size s, Long v) {
        switch (e.kind) {
            case Ea::Dn: writeD(e.reg, s, v); break;
            case Ea::An: r_.a[e.reg] = v; break;
            case Ea::Imm: break;
            default: write(e.addr, s, v); break;
        }
    }
    Ea eaOf(const Form& f, int slot, Word w, Size s) {
        if (f.op[slot] == Op::Ea) return decodeEa((w >> 3) & 7U, w & 7U, s);
        return decodeEa((w >> 6) & 7U, (w >> 9) & 7U, s);  // EaDst
    }
    // Address of an effective address that must be in memory (LEA/PEA/JMP/JSR/MOVEM).
    Address controlAddress(Word w) { return decodeEa((w >> 3) & 7U, w & 7U, Size::L).addr; }

    // ---- shifts ----
    Long shift(unsigned type, bool left, Long v, unsigned count, Size s) {
        const Long m = maskOf(s), sm = msbOf(s);
        v &= m;
        bool c = false, vflag = false;
        const bool x0 = flag(kFlagX);
        bool x = x0;
        for (unsigned i = 0; i < count; ++i) {
            switch (type) {
                case 0:  // AS
                    if (left) {
                        c = (v & sm) != 0;
                        const Long nv = (v << 1) & m;
                        if ((nv & sm) != (v & sm)) vflag = true;
                        v = nv;
                    } else {
                        c = (v & 1U) != 0;
                        v = ((v >> 1) | (v & sm)) & m;
                    }
                    x = c;
                    break;
                case 1:  // LS
                    if (left) {
                        c = (v & sm) != 0;
                        v = (v << 1) & m;
                    } else {
                        c = (v & 1U) != 0;
                        v >>= 1;
                    }
                    x = c;
                    break;
                case 2:  // ROX
                    if (left) {
                        c = (v & sm) != 0;
                        v = ((v << 1) | (x ? 1U : 0U)) & m;
                    } else {
                        c = (v & 1U) != 0;
                        v = (v >> 1) | (x ? sm : 0U);
                    }
                    x = c;
                    break;
                default:  // RO
                    if (left) {
                        c = (v & sm) != 0;
                        v = ((v << 1) | (c ? 1U : 0U)) & m;
                    } else {
                        c = (v & 1U) != 0;
                        v = (v >> 1) | (c ? sm : 0U);
                    }
                    break;
            }
        }
        setNZ(v, s);
        setFlag(kFlagV, type == 0 && left && vflag);
        if (count == 0) {
            setFlag(kFlagC, type == 2 ? x0 : false);
        } else {
            setFlag(kFlagC, c);
            if (type != 3) setFlag(kFlagX, x);
        }
        return v;
    }

    // ---- BCD ----
    // Decimal add/subtract with extend; Z is only cleared, N and V are "undefined" in the manual
    // and follow the usual hardware behaviour (derived from the intermediate binary result).
    Long abcd(Long src, Long dst) {
        Long res = (src & 0x0F) + (dst & 0x0F) + (flag(kFlagX) ? 1U : 0U);
        const Long vbits = ~res;  // undefined V: derived from the low-nibble sum and the final value
        if (res > 9) res += 6;
        res += (src & 0xF0) + (dst & 0xF0);
        const bool carry = res > 0x99;
        if (carry) res -= 0xA0;
        setFlag(kFlagC, carry);
        setFlag(kFlagX, carry);
        setFlag(kFlagV, (vbits & res & 0x80) != 0);
        setFlag(kFlagN, (res & 0x80) != 0);
        const Long low = res & 0xFF;
        if (low != 0) setFlag(kFlagZ, false);
        return low;
    }
    // NBCD: 0 - dst - X in decimal. A result of 0 (dst = 0, X = 0) clears C and X.
    Long nbcd(Long dst) {
        Long res = (0x9AU - (dst & 0xFF) - (flag(kFlagX) ? 1U : 0U)) & 0xFF;
        if (res != 0x9A) {
            const Long vbits = ~res;
            if ((res & 0x0F) == 0x0A) res = (res & 0xF0) + 0x10;
            res &= 0xFF;
            setFlag(kFlagV, (vbits & res & 0x80) != 0);
            if (res != 0) setFlag(kFlagZ, false);
            setFlag(kFlagC, true);
            setFlag(kFlagX, true);
            setFlag(kFlagN, (res & 0x80) != 0);
            return res;
        }
        setFlag(kFlagV, false);
        setFlag(kFlagC, false);
        setFlag(kFlagX, false);
        setFlag(kFlagN, true);  // undefined N: bit 7 of the intermediate 0x9A
        return dst & 0xFF;  // the destination keeps its value (0 for valid BCD input)
    }
    Long sbcd(Long src, Long dst) {
        std::int32_t res = static_cast<std::int32_t>(dst & 0x0F) - static_cast<std::int32_t>(src & 0x0F) - (flag(kFlagX) ? 1 : 0);
        const std::int32_t first = res;
        if (res > 9 || res < 0) res -= 6;
        res += static_cast<std::int32_t>(dst & 0xF0) - static_cast<std::int32_t>(src & 0xF0);
        const bool carry = res > 0x99 || res < 0;
        if (carry) res += 0xA0;
        const Long low = static_cast<Long>(res) & 0xFF;
        setFlag(kFlagC, carry);
        setFlag(kFlagX, carry);
        setFlag(kFlagN, (low & 0x80) != 0);
        setFlag(kFlagV, (static_cast<Long>(~first) & low & 0x80) != 0);
        if (low != 0) setFlag(kFlagZ, false);
        return low;
    }

    void privileged() {
        if (!r_.supervisor()) raise(kVecPrivilege, c_.startPc_);
    }

    void doMove(Mn mn, const Form& f, Word w, Size s);
    void doMovem(Mn mn, Word w, Size s);
    void doArithmetic(Mn mn, const Form& f, Word w, Size s);
    void doMisc(Mn mn, const Form& f, Word w, Size s);
    void branch(Word w);
};

// ---- instruction groups -------------------------------------------------------------------------------

void Exec::branch(Word w) {
    const unsigned c = (w >> 8) & 15U;
    const std::uint32_t d8 = w & 0xFFU;
    const Address base = r_.pc;  // address of the extension word = start + 2
    Long disp;
    if (d8 == 0) disp = sext16to32(fetch16());
    else disp = static_cast<Long>(static_cast<std::int32_t>(static_cast<std::int8_t>(d8)));
    const Address target = (base + disp) & kAddressMask;
    if (c == 1) {  // BSR
        push32(r_.pc);
        ++c_.depth_;
        r_.pc = target;
        return;
    }
    if (cond(c)) r_.pc = target;
}

void Exec::doMovem(Mn mn, Word w, Size s) {
    const Word mask = fetch16();
    const unsigned mode = (w >> 3) & 7U, reg = w & 7U;
    const Long step = static_cast<Long>(s);
    auto regValue = [&](unsigned i) { return i < 8 ? r_.d[i] : r_.a[i - 8]; };
    auto setReg = [&](unsigned i, Long v) {
        if (i < 8) r_.d[i] = v;
        else r_.a[i - 8] = v;
    };
    if (mn == Mn::MovemToMem) {
        if (mode == 4) {  // -(An): registers are stored A7..D0, the mask is bit-reversed
            Long addr = r_.a[reg];
            const Long initial = r_.a[reg];
            for (unsigned bit = 0; bit < 16; ++bit) {
                if (!(mask & (1U << bit))) continue;
                const unsigned i = 15 - bit;  // bit 0 = A7 ... bit 15 = D0
                Long v = regValue(i);
                if (i == 8 + reg) v = initial;  // the 68000 stores the initial value of An
                addr -= step;
                write(addr, s, v);
            }
            r_.a[reg] = addr;
        } else {
            Address addr = decodeEa(mode, reg, s).addr;
            for (unsigned i = 0; i < 16; ++i) {
                if (!(mask & (1U << i))) continue;
                write(addr, s, regValue(i));
                addr += step;
            }
        }
    } else {
        Address addr;
        if (mode == 3) addr = r_.a[reg];
        else addr = decodeEa(mode, reg, s).addr;
        for (unsigned i = 0; i < 16; ++i) {
            if (!(mask & (1U << i))) continue;
            Long v = read(addr, s);
            if (s == Size::W) v = sext16to32(static_cast<Word>(v));
            setReg(i, v);
            addr += step;
        }
        if (mode == 3) r_.a[reg] = addr;
    }
}

void Exec::doMove(Mn mn, const Form& f, Word w, Size s) {
    switch (mn) {
        case Mn::Move: {
            const Ea src = eaOf(f, 0, w, s);
            const Long v = readEa(src, s);
            const Ea dst = eaOf(f, 1, w, s);
            writeEa(dst, s, v);
            logicFlags(v, s);
            break;
        }
        case Mn::Movea: {
            const Ea src = eaOf(f, 0, w, s);
            Long v = readEa(src, s);
            if (s == Size::W) v = sext16to32(static_cast<Word>(v));
            r_.a[(w >> 9) & 7U] = v;
            break;
        }
        case Mn::Moveq: {
            const Long v = static_cast<Long>(static_cast<std::int32_t>(static_cast<std::int8_t>(w & 0xFFU)));
            r_.d[(w >> 9) & 7U] = v;
            logicFlags(v, Size::L);
            break;
        }
        case Mn::MoveFromSr: {
            const Ea dst = decodeEa((w >> 3) & 7U, w & 7U, Size::W);
            writeEa(dst, Size::W, r_.sr);
            break;
        }
        case Mn::MoveToCcr: {
            const Ea src = decodeEa((w >> 3) & 7U, w & 7U, Size::W);
            const Long v = readEa(src, Size::W);
            c_.setFlags(static_cast<Word>(v));
            break;
        }
        case Mn::MoveToSr: {
            privileged();
            const Ea src = decodeEa((w >> 3) & 7U, w & 7U, Size::W);
            c_.setSr(static_cast<Word>(readEa(src, Size::W)));
            break;
        }
        case Mn::MoveToUsp:
            privileged();
            r_.inactiveSp = r_.a[w & 7U];  // supervisor mode: the inactive pointer is the USP
            break;
        case Mn::MoveFromUsp:
            privileged();
            r_.a[w & 7U] = r_.inactiveSp;
            break;
        case Mn::Lea: r_.a[(w >> 9) & 7U] = controlAddress(w); break;
        case Mn::Pea: push32(controlAddress(w)); break;
        case Mn::Swap: {
            const unsigned n = w & 7U;
            const Long v = (r_.d[n] << 16) | (r_.d[n] >> 16);
            r_.d[n] = v;
            logicFlags(v, Size::L);
            break;
        }
        case Mn::Ext: {
            const unsigned n = w & 7U;
            if (s == Size::W) {
                const Long v = static_cast<Long>(static_cast<std::int32_t>(static_cast<std::int8_t>(r_.d[n] & 0xFFU))) & 0xFFFFU;
                writeD(n, Size::W, v);
                logicFlags(v, Size::W);
            } else {
                const Long v = sext16to32(static_cast<Word>(r_.d[n]));
                r_.d[n] = v;
                logicFlags(v, Size::L);
            }
            break;
        }
        case Mn::ExgDD: std::swap(r_.d[(w >> 9) & 7U], r_.d[w & 7U]); break;
        case Mn::ExgAA: std::swap(r_.a[(w >> 9) & 7U], r_.a[w & 7U]); break;
        case Mn::ExgDA: std::swap(r_.d[(w >> 9) & 7U], r_.a[w & 7U]); break;
        case Mn::Movep: {
            const unsigned dn = (w >> 9) & 7U, an = w & 7U;
            const Address addr = r_.a[an] + sext16to32(fetch16());
            const unsigned n = s == Size::L ? 4U : 2U;
            if (f.op[0] == Op::D16An0) {  // memory to register
                Long v = 0;
                for (unsigned i = 0; i < n; ++i) v = (v << 8) | read(addr + 2 * i, Size::B);
                writeD(dn, s, v);
            } else {
                const Long v = r_.d[dn];
                for (unsigned i = 0; i < n; ++i) write(addr + 2 * i, Size::B, v >> (8 * (n - 1 - i)));
            }
            break;
        }
        default: break;
    }
}

void Exec::doArithmetic(Mn mn, const Form& f, Word w, Size s) {
    const unsigned dn = (w >> 9) & 7U;
    switch (mn) {
        // ---- two operand, <ea> op Dn / Dn op <ea> ----
        case Mn::AddToD: case Mn::SubToD: case Mn::AndToD: case Mn::OrToD: case Mn::Cmp: {
            const Ea src = decodeEa((w >> 3) & 7U, w & 7U, s);
            const Long a = readEa(src, s);
            const Long d = readD(dn, s);
            Long r = 0;
            if (mn == Mn::AddToD) r = add(a, d, s, false);
            else if (mn == Mn::SubToD) r = sub(a, d, s, false);
            else if (mn == Mn::Cmp) { sub(a, d, s, false, false); break; }
            else {
                r = mn == Mn::AndToD ? (a & d) : (a | d);
                logicFlags(r, s);
            }
            writeD(dn, s, r);
            break;
        }
        case Mn::AddToEa: case Mn::SubToEa: case Mn::AndToEa: case Mn::OrToEa: case Mn::Eor: {
            const Ea dst = decodeEa((w >> 3) & 7U, w & 7U, s);
            const Long a = readD(dn, s);
            const Long d = readEa(dst, s);
            Long r;
            if (mn == Mn::AddToEa) r = add(a, d, s, false);
            else if (mn == Mn::SubToEa) r = sub(a, d, s, false);
            else {
                r = mn == Mn::AndToEa ? (a & d) : mn == Mn::OrToEa ? (a | d) : (a ^ d);
                logicFlags(r, s);
            }
            writeEa(dst, s, r);
            break;
        }
        case Mn::Adda: case Mn::Suba: case Mn::Cmpa: {
            const Ea src = decodeEa((w >> 3) & 7U, w & 7U, s);
            Long a = readEa(src, s);
            if (s == Size::W) a = sext16to32(static_cast<Word>(a));
            if (mn == Mn::Adda) r_.a[dn] += a;
            else if (mn == Mn::Suba) r_.a[dn] -= a;
            else sub(a, r_.a[dn], Size::L, false, false);
            break;
        }
        // ---- immediate operations ----
        case Mn::Ori: case Mn::Andi: case Mn::Eori: case Mn::Subi: case Mn::Addi: case Mn::Cmpi: {
            const Long imm = fetchImm(s);
            const Ea dst = decodeEa((w >> 3) & 7U, w & 7U, s);
            const Long d = readEa(dst, s);
            Long r;
            switch (mn) {
                case Mn::Ori: r = imm | d; logicFlags(r, s); break;
                case Mn::Andi: r = imm & d; logicFlags(r, s); break;
                case Mn::Eori: r = imm ^ d; logicFlags(r, s); break;
                case Mn::Subi: r = sub(imm, d, s, false); break;
                case Mn::Addi: r = add(imm, d, s, false); break;
                default: sub(imm, d, s, false, false); return;  // CMPI: no write
            }
            writeEa(dst, s, r);
            break;
        }
        case Mn::Addq: case Mn::Subq: {
            Long q = (w >> 9) & 7U;
            if (q == 0) q = 8;
            const Ea dst = decodeEa((w >> 3) & 7U, w & 7U, s);
            if (dst.kind == Ea::An) {  // whole register, no flags
                r_.a[dst.reg] = mn == Mn::Addq ? r_.a[dst.reg] + q : r_.a[dst.reg] - q;
                break;
            }
            const Long d = readEa(dst, s);
            writeEa(dst, s, mn == Mn::Addq ? add(q, d, s, false) : sub(q, d, s, false));
            break;
        }
        // ---- extended arithmetic ----
        case Mn::AddxD: case Mn::SubxD: {
            const Long a = readD(w & 7U, s), d = readD(dn, s);
            writeD(dn, s, mn == Mn::AddxD ? add(a, d, s, true) : sub(a, d, s, true));
            break;
        }
        case Mn::AddxM: case Mn::SubxM: {
            const unsigned ry = w & 7U;
            r_.a[ry] -= incr(ry, s);
            const Long a = read(r_.a[ry], s);
            r_.a[dn] -= incr(dn, s);
            const Long d = read(r_.a[dn], s);
            write(r_.a[dn], s, mn == Mn::AddxM ? add(a, d, s, true) : sub(a, d, s, true));
            break;
        }
        case Mn::Cmpm: {
            const unsigned ry = w & 7U;
            const Long a = read(r_.a[ry], s);
            r_.a[ry] += incr(ry, s);
            const Long d = read(r_.a[dn], s);
            r_.a[dn] += incr(dn, s);
            sub(a, d, s, false, false);
            break;
        }
        case Mn::Neg: case Mn::Negx: case Mn::Not: case Mn::Clr: case Mn::Tst: {
            const Ea dst = decodeEa((w >> 3) & 7U, w & 7U, s);
            if (mn == Mn::Tst) {
                logicFlags(readEa(dst, s), s);
                break;
            }
            const Long d = mn == Mn::Clr ? 0U : readEa(dst, s);
            Long r;
            if (mn == Mn::Neg) r = sub(d, 0, s, false);
            else if (mn == Mn::Negx) r = sub(d, 0, s, true);
            else if (mn == Mn::Not) { r = ~d & maskOf(s); logicFlags(r, s); }
            else { r = 0; logicFlags(0, s); }
            writeEa(dst, s, r);
            break;
        }
        // ---- multiply and divide ----
        case Mn::Mulu: case Mn::Muls: {
            const Ea src = decodeEa((w >> 3) & 7U, w & 7U, Size::W);
            const Long a = readEa(src, Size::W);
            const Long d = r_.d[dn] & 0xFFFFU;
            Long r;
            if (mn == Mn::Mulu) r = a * d;
            else r = static_cast<Long>(static_cast<std::int32_t>(static_cast<std::int16_t>(a)) * static_cast<std::int32_t>(static_cast<std::int16_t>(d)));
            r_.d[dn] = r;
            logicFlags(r, Size::L);
            break;
        }
        case Mn::Divu: case Mn::Divs: {
            const Ea src = decodeEa((w >> 3) & 7U, w & 7U, Size::W);
            const Long divisor = readEa(src, Size::W);
            if (divisor == 0) {
                raise(kVecZeroDivide, r_.pc);
            }
            const Long dividend = r_.d[dn];
            if (mn == Mn::Divu) {
                const Long q = dividend / divisor, rem = dividend % divisor;
                if (q > 0xFFFFU) {
                    setFlag(kFlagV, true);  // overflow: only V changes (N, Z, C are undefined)
                    break;
                }
                r_.d[dn] = (rem << 16) | q;
                logicFlags(q, Size::W);
            } else {
                const std::int32_t dd = static_cast<std::int32_t>(dividend);
                const std::int32_t dv = static_cast<std::int16_t>(divisor);
                if (dd == INT32_MIN && dv == -1) {  // 2^31 does not fit, but the result is defined as 0
                    r_.d[dn] = 0;
                    setFlag(kFlagZ, true);
                    setFlag(kFlagN, false);
                    setFlag(kFlagV, false);
                    setFlag(kFlagC, false);
                    break;
                }
                const std::int32_t q = dd / dv, rem = dd % dv;
                if (q > 32767 || q < -32768) {
                    setFlag(kFlagV, true);  // overflow: only V changes
                    break;
                }
                r_.d[dn] = (static_cast<Long>(rem & 0xFFFF) << 16) | static_cast<Long>(q & 0xFFFF);
                logicFlags(static_cast<Long>(q), Size::W);
            }
            break;
        }
        case Mn::Chk: {
            const Ea src = decodeEa((w >> 3) & 7U, w & 7U, Size::W);
            const std::int32_t bound = static_cast<std::int16_t>(readEa(src, Size::W));
            const std::int32_t v = static_cast<std::int16_t>(r_.d[dn]);
            setFlag(kFlagZ, v == 0);  // Z, V and C are undefined; these are the values of the reference emulator
            setFlag(kFlagV, false);
            setFlag(kFlagC, false);
            if (v < 0 || v > bound) {
                setFlag(kFlagN, v < 0);
                raise(kVecChk, r_.pc);
            }
            break;
        }
        // ---- BCD ----
        case Mn::AbcdD: case Mn::SbcdD: {
            const Long a = r_.d[w & 7U] & 0xFF, d = r_.d[dn] & 0xFF;
            writeD(dn, Size::B, mn == Mn::AbcdD ? abcd(a, d) : sbcd(a, d));
            break;
        }
        case Mn::AbcdM: case Mn::SbcdM: {
            const unsigned ry = w & 7U;
            r_.a[ry] -= incr(ry, Size::B);
            const Long a = read(r_.a[ry], Size::B);
            r_.a[dn] -= incr(dn, Size::B);
            const Long d = read(r_.a[dn], Size::B);
            write(r_.a[dn], Size::B, mn == Mn::AbcdM ? abcd(a, d) : sbcd(a, d));
            break;
        }
        case Mn::Nbcd: {
            const Ea dst = decodeEa((w >> 3) & 7U, w & 7U, Size::B);
            writeEa(dst, Size::B, nbcd(readEa(dst, Size::B)));
            break;
        }
        // ---- shifts ----
        case Mn::ShiftImm: case Mn::ShiftReg: {
            const unsigned type = (w >> 3) & 3U;
            const bool left = (w & 0x100U) != 0;
            unsigned count;
            if (mn == Mn::ShiftImm) {
                count = (w >> 9) & 7U;
                if (count == 0) count = 8;
            } else {
                count = static_cast<unsigned>(r_.d[dn] & 63U);
            }
            const unsigned reg = w & 7U;
            writeD(reg, s, shift(type, left, readD(reg, s), count, s));
            break;
        }
        case Mn::ShiftMem: {
            const unsigned type = (w >> 9) & 3U;
            const bool left = (w & 0x100U) != 0;
            const Ea dst = decodeEa((w >> 3) & 7U, w & 7U, Size::W);
            writeEa(dst, Size::W, shift(type, left, readEa(dst, Size::W), 1, Size::W));
            break;
        }
        default: break;
    }
    (void)f;
}

void Exec::doMisc(Mn mn, const Form& f, Word w, Size s) {
    (void)f;
    switch (mn) {
        // ---- bit operations ----
        case Mn::BtstD: case Mn::BchgD: case Mn::BclrD: case Mn::BsetD:
        case Mn::BtstS: case Mn::BchgS: case Mn::BclrS: case Mn::BsetS: {
            const bool dynamic = mn == Mn::BtstD || mn == Mn::BchgD || mn == Mn::BclrD || mn == Mn::BsetD;
            Long bit = dynamic ? r_.d[(w >> 9) & 7U] : (fetch16() & 0xFFU);
            const unsigned mode = (w >> 3) & 7U;
            const Size sz = mode == 0 ? Size::L : Size::B;
            bit &= mode == 0 ? 31U : 7U;
            const Ea dst = decodeEa(mode, w & 7U, sz);
            Long v = readEa(dst, sz);
            setFlag(kFlagZ, (v & (1U << bit)) == 0);
            const bool test = mn == Mn::BtstD || mn == Mn::BtstS;
            if (test) break;
            if (mn == Mn::BchgD || mn == Mn::BchgS) v ^= 1U << bit;
            else if (mn == Mn::BclrD || mn == Mn::BclrS) v &= ~(1U << bit);
            else v |= 1U << bit;
            writeEa(dst, sz, v);
            break;
        }
        case Mn::Tas: {
            const Ea dst = decodeEa((w >> 3) & 7U, w & 7U, Size::B);
            const Long v = readEa(dst, Size::B);
            logicFlags(v, Size::B);
            writeEa(dst, Size::B, v | 0x80U);
            break;
        }
        case Mn::Scc: {
            const Ea dst = decodeEa((w >> 3) & 7U, w & 7U, Size::B);
            writeEa(dst, Size::B, cond((w >> 8) & 15U) ? 0xFFU : 0U);
            break;
        }
        case Mn::Dbcc: {
            const Address base = r_.pc;
            const Long disp = sext16to32(fetch16());
            if (!cond((w >> 8) & 15U)) {
                const unsigned n = w & 7U;
                const Word v = static_cast<Word>(r_.d[n] - 1);
                writeD(n, Size::W, v);
                if (v != 0xFFFF) r_.pc = (base + disp) & kAddressMask;
            }
            break;
        }
        case Mn::Bcc: branch(w); break;
        case Mn::Jmp: r_.pc = controlAddress(w) & kAddressMask; break;
        case Mn::Jsr: {
            const Address target = controlAddress(w) & kAddressMask;
            push32(r_.pc);
            ++c_.depth_;
            r_.pc = target;
            break;
        }
        case Mn::Rts:
            r_.pc = pop32() & kAddressMask;
            --c_.depth_;
            break;
        case Mn::Rtr: {
            const Word ccr = pop16();
            c_.setFlags(ccr);
            r_.pc = pop32() & kAddressMask;
            --c_.depth_;
            break;
        }
        case Mn::Rte: {
            privileged();
            const Word sr = pop16();
            const Long pc = pop32();
            c_.setSr(sr);
            r_.pc = pc & kAddressMask;
            --c_.depth_;
            break;
        }
        case Mn::Trap: raise(kVecTrap0 + (w & 15U), r_.pc); break;
        case Mn::Trapv:
            if (flag(kFlagV)) raise(kVecTrapV, r_.pc);
            break;
        case Mn::Link: {
            const unsigned n = w & 7U;
            const Long disp = sext16to32(fetch16());
            if (n == 7) {  // LINK A7: the decremented stack pointer is what gets pushed
                r_.a[7] -= 4;
                write(r_.a[7], Size::L, r_.a[7]);
            } else {
                push32(r_.a[n]);
                r_.a[n] = r_.a[7];
            }
            r_.a[7] += disp;
            break;
        }
        case Mn::Unlk: {
            const unsigned n = w & 7U;
            r_.a[7] = r_.a[n];
            r_.a[n] = pop32();
            break;
        }
        case Mn::Nop: break;
        case Mn::Reset:
            privileged();
            if (c_.resetHook) c_.resetHook();
            break;
        case Mn::Stop: {
            privileged();
            const Word v = fetch16();
            c_.setSr(v);
            c_.stopped_ = true;
            break;
        }
        case Mn::Illegal: raise(kVecIllegal, c_.startPc_); break;
        // ---- status register logic ----
        case Mn::OriCcr: c_.setFlags(static_cast<Word>((r_.sr & 0x1F) | (fetchImm(Size::B) & 0x1F))); break;
        case Mn::AndiCcr: c_.setFlags(static_cast<Word>((r_.sr & 0x1F) & fetchImm(Size::B))); break;
        case Mn::EoriCcr: c_.setFlags(static_cast<Word>((r_.sr & 0x1F) ^ (fetchImm(Size::B) & 0x1F))); break;
        case Mn::OriSr: privileged(); c_.setSr(static_cast<Word>(r_.sr | fetchImm(Size::W))); break;
        case Mn::AndiSr: privileged(); c_.setSr(static_cast<Word>(r_.sr & fetchImm(Size::W))); break;
        case Mn::EoriSr: privileged(); c_.setSr(static_cast<Word>(r_.sr ^ fetchImm(Size::W))); break;
        default: break;
    }
    (void)s;
}

void Exec::run() {
    c_.startPc_ = r_.pc;
    const Word w = fetch16();
    c_.opcode_ = w;
    const Entry& e = table()[w];
    if (e.mn == Mn::Invalid) {
        const unsigned group = w >> 12;
        raise(group == 0xA ? kVecLineA : group == 0xF ? kVecLineF : kVecIllegal, c_.startPc_);
    }
    const Form& f = *e.form;
    switch (e.mn) {
        case Mn::Move: case Mn::Movea: case Mn::Moveq: case Mn::MoveFromSr: case Mn::MoveToCcr: case Mn::MoveToSr:
        case Mn::MoveToUsp: case Mn::MoveFromUsp: case Mn::Lea: case Mn::Pea: case Mn::Swap: case Mn::Ext:
        case Mn::ExgDD: case Mn::ExgAA: case Mn::ExgDA: case Mn::Movep:
            doMove(e.mn, f, w, e.size);
            break;
        case Mn::MovemToMem: case Mn::MovemToReg: doMovem(e.mn, w, e.size); break;
        default: break;
    }
    switch (e.mn) {
        case Mn::AddToD: case Mn::SubToD: case Mn::AndToD: case Mn::OrToD: case Mn::Cmp: case Mn::AddToEa:
        case Mn::SubToEa: case Mn::AndToEa: case Mn::OrToEa: case Mn::Eor: case Mn::Adda: case Mn::Suba:
        case Mn::Cmpa: case Mn::Ori: case Mn::Andi: case Mn::Eori: case Mn::Subi: case Mn::Addi: case Mn::Cmpi:
        case Mn::Addq: case Mn::Subq: case Mn::AddxD: case Mn::SubxD: case Mn::AddxM: case Mn::SubxM: case Mn::Cmpm:
        case Mn::Neg: case Mn::Negx: case Mn::Not: case Mn::Clr: case Mn::Tst: case Mn::Mulu: case Mn::Muls:
        case Mn::Divu: case Mn::Divs: case Mn::Chk: case Mn::AbcdD: case Mn::SbcdD: case Mn::AbcdM: case Mn::SbcdM:
        case Mn::Nbcd: case Mn::ShiftImm: case Mn::ShiftReg: case Mn::ShiftMem:
            doArithmetic(e.mn, f, w, e.size);
            break;
        case Mn::BtstD: case Mn::BchgD: case Mn::BclrD: case Mn::BsetD: case Mn::BtstS: case Mn::BchgS:
        case Mn::BclrS: case Mn::BsetS: case Mn::Tas: case Mn::Scc: case Mn::Dbcc: case Mn::Bcc: case Mn::Jmp:
        case Mn::Jsr: case Mn::Rts: case Mn::Rtr: case Mn::Rte: case Mn::Trap: case Mn::Trapv: case Mn::Link:
        case Mn::Unlk: case Mn::Nop: case Mn::Reset: case Mn::Stop: case Mn::Illegal: case Mn::OriCcr:
        case Mn::AndiCcr: case Mn::EoriCcr: case Mn::OriSr: case Mn::AndiSr: case Mn::EoriSr:
            doMisc(e.mn, f, w, e.size);
            break;
        default: break;
    }
}

// ---- exceptions ---------------------------------------------------------------------------------------

void Exec::processException(const Cpu::Fault& f) {
    const Word oldSr = r_.sr;
    c_.setSr(static_cast<Word>((r_.sr | kSrSupervisor) & ~kSrTrace));
    if (f.group0) {
        const Word status = static_cast<Word>((c_.opcode_ & 0xFFE0U) | (f.read ? 0x10U : 0U) |
                                              ((oldSr & kSrSupervisor) ? 4U : 0U) | (f.instructionSpace ? 2U : 1U));
        push32(f.pc);
        push16(oldSr);
        push16(c_.opcode_);
        push32(f.address);
        push16(status);
    } else {
        push32(f.pc);
        push16(oldSr);
    }
    ++c_.depth_;
    r_.pc = mem_.read32(f.vector * 4U) & kAddressMask;
}

void Exec::processInterrupt(unsigned level, unsigned vector) {
    const Word oldSr = r_.sr;
    c_.setSr(static_cast<Word>(((r_.sr | kSrSupervisor) & ~kSrTrace & ~kSrIntMask) | (level << 8)));
    push32(r_.pc);
    push16(oldSr);
    ++c_.depth_;
    r_.pc = mem_.read32(vector * 4U) & kAddressMask;
}

// ---- Cpu ------------------------------------------------------------------------------------------------

Cpu::Cpu(Memory& memory) : mem_(memory) {}

void Cpu::setSr(Word value) {
    value &= kSrValidBits;
    if ((value ^ r_.sr) & kSrSupervisor) std::swap(r_.a[7], r_.inactiveSp);
    r_.sr = value;
}

void Cpu::reset() {
    r_.sr = 0x2700;
    r_.inactiveSp = 0;
    r_.a[7] = mem_.read32(0);
    r_.pc = mem_.read32(4) & kAddressMask;
    halted_ = stopped_ = false;
    irqLevel_ = 0;
    depth_ = 0;
}

void Cpu::interrupt(unsigned level, int vector) {
    irqLevel_ = level > 7 ? 7 : level;
    irqVector_ = vector;
}

void Cpu::addWatchpoint(Address start, Address length, bool onRead, bool onWrite) {
    watches_.push_back({start & kAddressMask, (start & kAddressMask) + length, onRead, onWrite});
}

Long Cpu::peek(Address a, Size s) const {
    a &= kAddressMask;
    return s == Size::B ? mem_.read8(a) : s == Size::W ? mem_.read16(a) : mem_.read32(a);
}

StepResult Cpu::step() {
    StepResult res;
    if (halted_) {
        res.reason = StopReason::Halted;
        return res;
    }
    Exec ex(*this);
    watchHit_ = false;
    try {
        if (irqLevel_ != 0 && (irqLevel_ == 7 || irqLevel_ > ((r_.sr & kSrIntMask) >> 8))) {
            const unsigned level = irqLevel_;
            const unsigned vec = irqVector_ >= 0 ? static_cast<unsigned>(irqVector_) : kVecAutovector + level;
            irqLevel_ = 0;
            stopped_ = false;
            ex.processInterrupt(level, vec);
            ++instructions_;
            return res;
        }
    } catch (const Fault&) {
        halted_ = true;  // fault while stacking an interrupt frame: double fault
        res.reason = StopReason::Halted;
        return res;
    }
    if (stopped_) {
        res.reason = StopReason::Stopped;
        return res;
    }
    const bool trace = (r_.sr & kSrTrace) != 0;
    try {
        if (traceHook) traceHook(r_, r_.pc, mem_.read16(r_.pc & kAddressMask));
        ex.run();
    } catch (const Fault& f) {
        res.exception = f.vector;
        try {
            ex.processException(f);
        } catch (const Fault&) {
            halted_ = true;  // fault while processing an exception: the processor halts
            res.reason = StopReason::Halted;
            ++instructions_;
            return res;
        }
    }
    ++instructions_;
    if (trace && !res.exception) {
        try {
            Fault t;
            t.vector = kVecTrace;
            t.pc = r_.pc;
            ex.processException(t);
            res.exception = kVecTrace;
        } catch (const Fault&) {
            halted_ = true;
            res.reason = StopReason::Halted;
        }
    }
    if (stopped_ && res.reason == StopReason::None) res.reason = StopReason::Stopped;
    return res;
}

RunResult Cpu::runLoop(std::uint64_t maxInstructions, std::optional<Address> until, bool untilReturn) {
    RunResult out;
    const int startDepth = depth_;
    for (std::uint64_t i = 0; i < maxInstructions; ++i) {
        if (i > 0 || until) {
            if (until && (r_.pc & kAddressMask) == (*until & kAddressMask) && i > 0) {
                out.reason = StopReason::Reached;
                return out;
            }
            if (i > 0 && breakpoints_.count(r_.pc & kAddressMask)) {
                out.reason = StopReason::Breakpoint;
                return out;
            }
        }
        const StepResult s = step();
        if (s.reason == StopReason::Halted || s.reason == StopReason::Stopped) {
            out.reason = s.reason;
            return out;
        }
        ++out.instructions;
        if (watchHit_) {
            out.reason = StopReason::Watchpoint;
            out.watchAddress = watchAddress_;
            return out;
        }
        if (untilReturn && depth_ < startDepth) {
            out.reason = StopReason::Returned;
            return out;
        }
    }
    out.reason = StopReason::Limit;
    return out;
}

RunResult Cpu::run(std::uint64_t maxInstructions) { return runLoop(maxInstructions, std::nullopt, false); }
RunResult Cpu::runUntil(Address address, std::uint64_t maxInstructions) {
    return runLoop(maxInstructions, address, false);
}
RunResult Cpu::runUntilReturn(std::uint64_t maxInstructions) { return runLoop(maxInstructions, std::nullopt, true); }

RunResult Cpu::stepOver(std::uint64_t maxInstructions) {
    RunResult out;
    const Instruction in = decode(mem_, r_.pc);
    if (!(in.valid && in.flow == FlowKind::Call)) {
        const StepResult s = step();
        out.reason = s.reason;
        if (s.reason == StopReason::None) out.instructions = 1;
        return out;
    }
    const Address next = (r_.pc + static_cast<Address>(in.length())) & kAddressMask;
    const int depth = depth_;
    while (out.instructions < maxInstructions) {
        const StepResult s = step();
        if (s.reason == StopReason::Halted || s.reason == StopReason::Stopped) {
            out.reason = s.reason;
            return out;
        }
        ++out.instructions;
        if (watchHit_) {
            out.reason = StopReason::Watchpoint;
            out.watchAddress = watchAddress_;
            return out;
        }
        if ((r_.pc & kAddressMask) == next && depth_ <= depth) {
            out.reason = StopReason::Reached;
            return out;
        }
        if (breakpoints_.count(r_.pc & kAddressMask)) {
            out.reason = StopReason::Breakpoint;
            return out;
        }
    }
    out.reason = StopReason::Limit;
    return out;
}

RunResult Cpu::call(Address entry, Address returnAddress, std::uint64_t maxInstructions) {
    r_.a[7] -= 4;
    mem_.write32(r_.a[7] & kAddressMask, returnAddress);
    r_.pc = entry & kAddressMask;
    ++depth_;
    return runLoop(maxInstructions, returnAddress, false);
}

}  // namespace m68k
