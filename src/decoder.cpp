#include "m68k/decoder.hpp"

#include <cstring>

#include "forms.hpp"

namespace m68k {

namespace {

using detail::Form;
using detail::Op;
using detail::SizeKind;

class Decoder {
public:
    Decoder(const Memory& mem, Address address, const DecodeOptions& opt)
        : mem_(mem), opt_(opt), start_(address), pos_(address) {}

    Instruction run() {
        Instruction in;
        in.address = start_;
        if (start_ & 1U) return invalid(in, true);
        const Word w = mem_.read16(start_);
        pos_ = start_ + 2;
        for (const Form& f : detail::forms()) {
            if ((w & f.mask) != f.value) continue;
            Size size = Size::W;
            bool hasSize = true;
            switch (f.sk) {
                case SizeKind::None:
                case SizeKind::Any:
                    hasSize = false;
                    break;
                case SizeKind::B76: {
                    const unsigned s = (w >> 6) & 3U;
                    if (s == 3) continue;
                    size = s == 0 ? Size::B : s == 1 ? Size::W : Size::L;
                    break;
                }
                case SizeKind::Move: {
                    const unsigned s = (w >> 12) & 3U;
                    if (s == 0) continue;
                    size = s == 1 ? Size::B : s == 3 ? Size::W : Size::L;
                    break;
                }
                case SizeKind::W8:
                    size = (w & 0x0100U) ? Size::L : Size::W;
                    break;
                case SizeKind::W6:
                    size = (w & 0x0040U) ? Size::L : Size::W;
                    break;
                case SizeKind::Fixed:
                    size = f.fixed;
                    break;
            }
            if (!decodeForm(f, w, size, in)) return invalid(in, false);
            in.valid = true;
            std::string mn = f.name;
            if (branchSuffix_) {
                mn += branchSuffix_;
            } else if (f.print && hasSize) {
                mn += size == Size::B ? ".B" : size == Size::W ? ".W" : ".L";
            }
            in.mnemonic = mn;
            in.operands = operands_;
            in.flow = flowOf(f.name);
            if (in.flow == FlowKind::Jump || in.flow == FlowKind::CondJump || in.flow == FlowKind::Call) {
                if (target_) in.target = target_;
            } else if (ref_) {
                in.reference = ref_;
            }
            if (!in.target && eaTarget_ && (std::strcmp(f.name, "JMP") == 0 || std::strcmp(f.name, "JSR") == 0)) {
                in.target = eaTarget_;
            }
            fillBytes(in);
            return in;
        }
        return invalid(in, false);
    }

private:
    // ---- helpers ----------------------------------------------------------------------------
    std::string hex(unsigned long long v, int digits) const {
        return formatHex(static_cast<unsigned>(v), digits, opt_.style);
    }
    std::string sgn(long v) const { return v < 0 ? "-" + hex(static_cast<unsigned long long>(-v), 1) : hex(static_cast<unsigned long long>(v), 1); }
    std::string sym(Address a) const { return opt_.symbolizer ? opt_.symbolizer(a & kAddressMask) : std::string(); }

    Word next() {
        const Word w = mem_.read16(pos_);
        pos_ += 2;
        return w;
    }

    void fillBytes(Instruction& in) const {
        in.bytes.clear();
        for (Address a = start_; a < pos_; ++a) in.bytes.push_back(mem_.read8(a));
    }

    Instruction invalid(Instruction in, bool oddByte) const {
        in.valid = false;
        in.flow = FlowKind::Normal;
        in.target.reset();
        in.reference.reset();
        in.bytes.clear();
        if (oddByte) {
            in.bytes.push_back(mem_.read8(start_));
            in.mnemonic = "DC.B";
            in.operands = hex(in.bytes[0], 2);
        } else {
            in.bytes.push_back(mem_.read8(start_));
            in.bytes.push_back(mem_.read8(start_ + 1));
            in.mnemonic = "DC.W";
            in.operands = hex(mem_.read16(start_), 4);
        }
        return in;
    }

    static FlowKind flowOf(const char* name) {
        if (!std::strcmp(name, "BRA") || !std::strcmp(name, "JMP")) return FlowKind::Jump;
        if (!std::strcmp(name, "BSR") || !std::strcmp(name, "JSR")) return FlowKind::Call;
        if (!std::strcmp(name, "RTS") || !std::strcmp(name, "RTE") || !std::strcmp(name, "RTR") ||
            !std::strcmp(name, "ILLEGAL"))
            return FlowKind::Return;
        if (name[0] == 'B' && std::strcmp(name, "BTST") && std::strcmp(name, "BCHG") && std::strcmp(name, "BCLR") &&
            std::strcmp(name, "BSET"))
            return FlowKind::CondJump;
        if (name[0] == 'D' && name[1] == 'B') return FlowKind::CondJump;
        return FlowKind::Normal;
    }

    std::string regName(char kind, unsigned n) const { return std::string(1, kind) + static_cast<char>('0' + n); }

    // ---- operands ---------------------------------------------------------------------------
    bool decodeIndexWord(Word e, std::string& out, long& disp) const {
        if (e & 0x0700U) return false;  // 68020 scale / full-extension bits
        const unsigned xn = (e >> 12) & 7U;
        out = std::string((e & 0x8000U) ? "A" : "D") + static_cast<char>('0' + xn) + ((e & 0x0800U) ? ".L" : ".W");
        disp = static_cast<std::int8_t>(e & 0xFFU);
        return true;
    }

    bool decodeEa(unsigned mode, unsigned reg, Size size, std::string& out) {
        switch (mode) {
            case 0: out = regName('D', reg); return true;
            case 1: out = regName('A', reg); return true;
            case 2: out = "(" + regName('A', reg) + ")"; return true;
            case 3: out = "(" + regName('A', reg) + ")+"; return true;
            case 4: out = "-(" + regName('A', reg) + ")"; return true;
            case 5: {
                const long d = static_cast<std::int16_t>(next());
                out = sgn(d) + "(" + regName('A', reg) + ")";
                return true;
            }
            case 6: {
                std::string idx;
                long d = 0;
                if (!decodeIndexWord(next(), idx, d)) return false;
                out = sgn(d) + "(" + regName('A', reg) + "," + idx + ")";
                return true;
            }
            default: break;
        }
        switch (reg) {
            case 0: {
                const Word v = next();
                const Address a = static_cast<Address>(static_cast<std::int32_t>(static_cast<std::int16_t>(v))) & kAddressMask;
                // A label stands for a value; `$8000.W` means $FFFF8000, so only 0..$7FFF may be replaced.
                const std::string s = (v & 0x8000U) ? std::string() : sym(a);
                out = (s.empty() ? hex(v, 4) : s) + ".W";
                noteRef(a);
                return true;
            }
            case 1: {
                const Address hi = next();
                const Address lo = next();
                const Address v = (hi << 16) | lo;
                // The upper byte is ignored by the bus but part of the encoding: keep it visible.
                const std::string s = (v & ~kAddressMask) ? std::string() : sym(v);
                out = (s.empty() ? hex(v, 8) : s) + ".L";
                noteRef(v & kAddressMask);
                return true;
            }
            case 2: {
                const Address base = pos_;
                const long d = static_cast<std::int16_t>(next());
                const Address t = static_cast<Address>(base + d) & kAddressMask;
                const std::string s = sym(t);
                out = (s.empty() ? hex(t, 6) : s) + "(PC)";
                noteRef(t);
                return true;
            }
            case 3: {
                const Address base = pos_;
                std::string idx;
                long d = 0;
                if (!decodeIndexWord(next(), idx, d)) return false;
                const Address t = static_cast<Address>(base + d) & kAddressMask;
                const std::string s = sym(t);
                out = (s.empty() ? hex(t, 6) : s) + "(PC," + idx + ")";
                return true;
            }
            case 4: {
                switch (size) {
                    case Size::B: {
                        const Word v = next();
                        if (v & 0xFF00U) return false;
                        out = "#" + hex(v, 2);
                        return true;
                    }
                    case Size::W: out = "#" + hex(next(), 4); return true;
                    case Size::L: {
                        const Address hi = next();
                        const Address lo = next();
                        out = "#" + hex((hi << 16) | lo, 8);
                        return true;
                    }
                }
                return false;
            }
            default: return false;
        }
    }

    void noteRef(Address a) {
        if (!ref_) ref_ = a;
        eaTarget_ = a;
    }

    std::string regList(Word mask, bool reversed) const {
        // bit i of `m` = register i: D0..D7 = 0..7, A0..A7 = 8..15
        unsigned m = 0;
        for (unsigned i = 0; i < 16; ++i) {
            const bool set = reversed ? (mask & (1U << (15 - i))) != 0 : (mask & (1U << i)) != 0;
            if (set) m |= 1U << i;
        }
        std::string out;
        for (unsigned bank = 0; bank < 2; ++bank) {
            const char letter = bank == 0 ? 'D' : 'A';
            unsigned i = 0;
            while (i < 8) {
                if (!(m & (1U << (bank * 8 + i)))) {
                    ++i;
                    continue;
                }
                unsigned j = i;
                while (j + 1 < 8 && (m & (1U << (bank * 8 + j + 1)))) ++j;
                if (!out.empty()) out += '/';
                out += regName(letter, i);
                if (j > i) out += "-" + regName(letter, j);
                i = j + 1;
            }
        }
        return out;
    }

    bool decodeForm(const Form& f, Word w, Size size, Instruction& /*in*/) {
        operands_.clear();
        branchSuffix_ = nullptr;
        target_.reset();
        ref_.reset();
        eaTarget_.reset();

        // The MOVEM mask is the first extension word, whichever order the operands are written in.
        Word moveMask = 0;
        bool haveMask = false;
        for (const Op o : f.op) {
            if (o == Op::RegList) {
                moveMask = next();
                haveMask = true;
            }
        }
        if (haveMask && moveMask == 0) return false;

        for (int i = 0; i < 2; ++i) {
            const Op o = f.op[i];
            if (o == Op::None) continue;
            std::string text;
            switch (o) {
                case Op::Ea:
                case Op::EaDst: {
                    unsigned mode, reg;
                    if (o == Op::Ea) {
                        mode = (w >> 3) & 7U;
                        reg = w & 7U;
                    } else {
                        mode = (w >> 6) & 7U;
                        reg = (w >> 9) & 7U;
                    }
                    const unsigned bit = detail::eaBitOf(mode, reg);
                    if (bit == 0 || !(f.set[i] & bit)) return false;
                    if (f.noAnByte && size == Size::B && bit == detail::kAn) return false;
                    if (!decodeEa(mode, reg, size, text)) return false;
                    break;
                }
                case Op::Dn9: text = regName('D', (w >> 9) & 7U); break;
                case Op::Dn0: text = regName('D', w & 7U); break;
                case Op::An9: text = regName('A', (w >> 9) & 7U); break;
                case Op::An0: text = regName('A', w & 7U); break;
                case Op::PostInc0: text = "(" + regName('A', w & 7U) + ")+"; break;
                case Op::PostInc9: text = "(" + regName('A', (w >> 9) & 7U) + ")+"; break;
                case Op::PreDec0: text = "-(" + regName('A', w & 7U) + ")"; break;
                case Op::PreDec9: text = "-(" + regName('A', (w >> 9) & 7U) + ")"; break;
                case Op::Imm: {
                    unsigned bit = detail::kImm;
                    (void)bit;
                    if (!decodeEa(7, 4, size, text)) return false;
                    break;
                }
                case Op::ImmQ3: {
                    const unsigned n = (w >> 9) & 7U;
                    text = "#" + std::to_string(n == 0 ? 8U : n);
                    break;
                }
                case Op::ImmMoveq: text = "#" + hex(w & 0xFFU, 2); break;
                case Op::ImmTrap: text = "#" + std::to_string(w & 15U); break;
                case Op::ImmWord: text = "#" + hex(next(), 4); break;
                case Op::ImmSWord: text = "#" + sgn(static_cast<std::int16_t>(next())); break;
                case Op::ImmBit: {
                    const Word e = next();
                    if (e & 0xFF00U) return false;
                    text = "#" + std::to_string(e);
                    break;
                }
                case Op::Ccr: text = "CCR"; break;
                case Op::Sr: text = "SR"; break;
                case Op::Usp: text = "USP"; break;
                case Op::Branch: {
                    const unsigned d8 = w & 0xFFU;
                    long disp;
                    if (d8 == 0) {
                        disp = static_cast<std::int16_t>(next());
                        branchSuffix_ = ".W";
                    } else {
                        disp = static_cast<std::int8_t>(d8);
                        branchSuffix_ = ".S";
                    }
                    const Address t = static_cast<Address>(start_ + 2 + disp) & kAddressMask;
                    if (t & 1U) return false;
                    target_ = t;
                    const std::string s = sym(t);
                    text = s.empty() ? hex(t, 6) : s;
                    break;
                }
                case Op::DbDisp: {
                    const long disp = static_cast<std::int16_t>(next());
                    const Address t = static_cast<Address>(start_ + 2 + disp) & kAddressMask;
                    if (t & 1U) return false;
                    target_ = t;
                    const std::string s = sym(t);
                    text = s.empty() ? hex(t, 6) : s;
                    break;
                }
                case Op::RegList: {
                    // Written reversed only for the register-to-memory form with -(An).
                    const bool predec = f.op[1] == Op::Ea && ((w >> 3) & 7U) == 4;
                    text = regList(moveMask, predec);
                    break;
                }
                case Op::D16An0: {
                    const long d = static_cast<std::int16_t>(next());
                    text = sgn(d) + "(" + regName('A', w & 7U) + ")";
                    break;
                }
                case Op::None: break;
            }
            if (!operands_.empty()) operands_ += ",";
            operands_ += text;
        }
        return true;
    }

    const Memory& mem_;
    const DecodeOptions& opt_;
    Address start_;
    Address pos_;
    std::string operands_;
    const char* branchSuffix_ = nullptr;
    std::optional<Address> target_;
    std::optional<Address> ref_;
    std::optional<Address> eaTarget_;
};

}  // namespace

Instruction decode(const Memory& memory, Address address, const DecodeOptions& options) {
    return Decoder(memory, address & kAddressMask, options).run();
}

}  // namespace m68k
