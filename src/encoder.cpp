#include "m68k/encoder.hpp"

#include <algorithm>
#include <cctype>
#include <map>

#include "forms.hpp"

namespace m68k {

using detail::Form;
using detail::Op;
using detail::SizeKind;

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// ---- operand parsing ---------------------------------------------------------------------------

enum class K { Dn, An, Ind, PostInc, PreDec, D16An, D8AnXn, AbsW, AbsL, AbsAuto, D16Pc, D8PcXn, Imm, Ccr, Sr, Usp, RegList };

struct Parsed {
    K kind = K::AbsAuto;
    int reg = 0;
    int xreg = 0;
    bool xAddr = false;
    bool xLong = false;
    std::string expr;
    unsigned mask = 0;  // register list: bit 0-7 = D0-D7, bit 8-15 = A0-A7
};

// Register name: D0-D7, A0-A7, SP (= A7). Returns 0..7 and sets `addr`, or -1.
int parseReg(const std::string& s, bool& addr) {
    const std::string u = upper(trim(s));
    if (u == "SP") {
        addr = true;
        return 7;
    }
    if (u.size() == 2 && (u[0] == 'D' || u[0] == 'A') && u[1] >= '0' && u[1] <= '7') {
        addr = u[0] == 'A';
        return u[1] - '0';
    }
    return -1;
}

// Index register "D3", "A1.L", "D0.W" (default .W).
bool parseIndex(const std::string& s, Parsed& p) {
    std::string u = upper(trim(s));
    bool isLong = false;
    if (u.size() > 2 && u[u.size() - 2] == '.') {
        if (u.back() == 'L') isLong = true;
        else if (u.back() != 'W') return false;
        u.resize(u.size() - 2);
    }
    bool addr = false;
    const int r = parseReg(u, addr);
    if (r < 0 || u == "SP") return false;
    p.xreg = r;
    p.xAddr = addr;
    p.xLong = isLong;
    return true;
}

bool parseRegList(const std::string& s, unsigned& mask) {
    mask = 0;
    std::size_t i = 0;
    const std::string u = upper(s);
    if (u.empty()) return false;
    while (i < u.size()) {
        const std::size_t slash = u.find('/', i);
        const std::string part = trim(u.substr(i, slash == std::string::npos ? std::string::npos : slash - i));
        const std::size_t dash = part.find('-');
        bool a1 = false, a2 = false;
        if (dash == std::string::npos) {
            const int r = parseReg(part, a1);
            if (r < 0) return false;
            mask |= 1U << (r + (a1 ? 8 : 0));
        } else {
            const int r1 = parseReg(part.substr(0, dash), a1);
            const int r2 = parseReg(part.substr(dash + 1), a2);
            if (r1 < 0 || r2 < 0 || a1 != a2 || r2 < r1) return false;
            for (int r = r1; r <= r2; ++r) mask |= 1U << (r + (a1 ? 8 : 0));
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return mask != 0;
}

// Index of the '(' that matches the final ')' of s, or npos.
std::size_t matchingOpen(const std::string& s) {
    if (s.empty() || s.back() != ')') return std::string::npos;
    int depth = 0;
    for (std::size_t i = s.size(); i-- > 0;) {
        if (s[i] == ')') ++depth;
        else if (s[i] == '(' && --depth == 0) return i;
    }
    return std::string::npos;
}

bool parseOperand(const std::string& text, Parsed& p, std::string& err) {
    std::string s = trim(text);
    if (s.empty()) {
        err = "empty operand";
        return false;
    }
    const std::string u = upper(s);
    if (u == "CCR") { p.kind = K::Ccr; return true; }
    if (u == "SR") { p.kind = K::Sr; return true; }
    if (u == "USP") { p.kind = K::Usp; return true; }
    if (s[0] == '#') {
        p.kind = K::Imm;
        p.expr = trim(s.substr(1));
        return true;
    }
    bool addr = false;
    if (const int r = parseReg(s, addr); r >= 0) {
        p.kind = addr ? K::An : K::Dn;
        p.reg = r;
        return true;
    }
    if (s.find('(') == std::string::npos && (s.find('/') != std::string::npos || s.find('-') != std::string::npos)) {
        if (parseRegList(s, p.mask)) {
            p.kind = K::RegList;
            return true;
        }
    }
    // -(An), (An)+
    if (s.size() > 3 && s[0] == '-' && s[1] == '(' && s.back() == ')') {
        const int r = parseReg(s.substr(2, s.size() - 3), addr);
        if (r >= 0 && addr) {
            p.kind = K::PreDec;
            p.reg = r;
            return true;
        }
    }
    if (s.size() > 4 && s[0] == '(' && s.compare(s.size() - 2, 2, ")+") == 0) {
        const int r = parseReg(s.substr(1, s.size() - 3), addr);
        if (r >= 0 && addr) {
            p.kind = K::PostInc;
            p.reg = r;
            return true;
        }
    }
    // optional .W / .L on absolute addresses
    char absSize = 0;
    std::string body = s;
    if (body.size() > 2 && body[body.size() - 2] == '.') {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(body.back())));
        if (c == 'W' || c == 'L') {
            absSize = c;
            body.resize(body.size() - 2);
        }
    }
    // d(An), d(An,Xn), d(PC), d(PC,Xn), (d,An), (An,Xn), (An), (expr)
    if (const std::size_t open = matchingOpen(body); open != std::string::npos) {
        const std::string prefix = trim(body.substr(0, open));
        const std::string inside = body.substr(open + 1, body.size() - open - 2);
        std::vector<std::string> parts = splitOperands(inside);
        std::string disp = prefix;
        bool shapeOk = !parts.empty();
        if (shapeOk && prefix.empty()) {
            // "(d,An)": first part is the displacement unless it is a register or PC
            bool a = false;
            const std::string f = trim(parts[0]);
            if (parseReg(f, a) < 0 && upper(f) != "PC") {
                if (parts.size() >= 2) {
                    disp = f;
                    parts.erase(parts.begin());
                } else {
                    shapeOk = false;  // "(expr)": parenthesised absolute
                }
            }
        }
        if (shapeOk && parts.size() <= 2 && !parts.empty()) {
            const std::string base = trim(parts[0]);
            bool baseAddr = false;
            const int br = parseReg(base, baseAddr);
            const bool isPc = upper(base) == "PC";
            if ((br >= 0 && baseAddr) || isPc) {
                Parsed q;
                q.expr = disp.empty() ? "0" : disp;
                if (parts.size() == 2) {
                    if (!parseIndex(parts[1], q)) {
                        err = "bad index register '" + trim(parts[1]) + "'";
                        return false;
                    }
                }
                if (absSize) {
                    err = "size suffix not allowed here";
                    return false;
                }
                if (isPc) q.kind = parts.size() == 2 ? K::D8PcXn : K::D16Pc;
                else {
                    q.kind = parts.size() == 2 ? K::D8AnXn : (disp.empty() && prefix.empty() ? K::Ind : K::D16An);
                    q.reg = br;
                }
                p = q;
                return true;
            }
        }
    }
    p.kind = absSize == 'W' ? K::AbsW : absSize == 'L' ? K::AbsL : K::AbsAuto;
    p.expr = body;
    // "(expr).W" -> drop the redundant parentheses
    if (const std::size_t open = matchingOpen(body); open == 0) p.expr = body.substr(1, body.size() - 2);
    return true;
}

// ---- mnemonic aliases ----------------------------------------------------------------------------

std::string canonicalName(const std::string& n) {
    static const std::map<std::string, std::string> alias = {
        {"DBF", "DBRA"}, {"BHS", "BCC"}, {"BLO", "BCS"}, {"SHS", "SCC"}, {"SLO", "SCS"},
        {"DBHS", "DBCC"}, {"DBLO", "DBCS"}};
    const auto it = alias.find(n);
    return it == alias.end() ? n : it->second;
}

// Generic names that may select a different form depending on the operands.
std::vector<std::string> alternatives(const std::string& n) {
    static const std::map<std::string, std::vector<std::string>> alt = {
        {"MOVE", {"MOVEA"}},        {"ADD", {"ADDA", "ADDI"}}, {"SUB", {"SUBA", "SUBI"}},
        {"CMP", {"CMPA", "CMPI"}},  {"AND", {"ANDI"}},         {"OR", {"ORI"}},
        {"EOR", {"EORI"}},
    };
    const auto it = alt.find(n);
    return it == alt.end() ? std::vector<std::string>{} : it->second;
}

// Displacement from `base` to `target` on a 24 bit address bus: the PC wraps at 16 MiB, so the
// difference is taken modulo 2^24 and sign-extended.
std::int64_t pcDisp(std::int64_t target, std::int64_t base) {
    std::int64_t d = (target - base) & 0xFFFFFF;
    if (d & 0x800000) d -= 0x1000000;
    return d;
}

bool fitsSigned(std::int64_t v, int bits) { return v >= -(std::int64_t{1} << (bits - 1)) && v < (std::int64_t{1} << (bits - 1)); }

// ---- encoding of one form ------------------------------------------------------------------------

struct Ctx {
    const EncodeRequest& req;
    const ValueFn& val;
    std::vector<Word> ext;
    int level = 0;
    std::string error;

    Address extAddress() const { return req.pc + 2 + 2 * static_cast<Address>(ext.size()); }
    bool eval(const std::string& e, EncodeValue& out) {
        out = val(e);
        if (!out.error.empty()) {
            error = out.error;
            return false;
        }
        return true;
    }
};

unsigned eaBitForKind(K k) {
    using namespace detail;
    switch (k) {
        case K::Dn: return kDn;
        case K::An: return kAn;
        case K::Ind: return kInd;
        case K::PostInc: return kPostInc;
        case K::PreDec: return kPreDec;
        case K::D16An: return kD16An;
        case K::D8AnXn: return kD8AnXn;
        case K::AbsW: return kAbsW;
        case K::AbsL: return kAbsL;
        case K::AbsAuto: return kAbsW | kAbsL;
        case K::D16Pc: return kD16Pc;
        case K::D8PcXn: return kD8PcXn;
        case K::Imm: return kImm;
        default: return 0;
    }
}

// Encodes an effective address. Sets mode/reg and appends extension words.
bool encodeEa(Ctx& c, const Parsed& p, Size size, unsigned allowed, bool noAnByte, unsigned& mode, unsigned& reg) {
    using namespace detail;
    const unsigned bits = eaBitForKind(p.kind);
    if (bits == 0 || !(allowed & bits)) {
        c.error = "addressing mode not allowed here";
        return false;
    }
    if (noAnByte && size == Size::B && p.kind == K::An) {
        c.error = "address register not allowed with byte size";
        return false;
    }
    EncodeValue v;
    switch (p.kind) {
        case K::Dn: mode = 0; reg = static_cast<unsigned>(p.reg); return true;
        case K::An: mode = 1; reg = static_cast<unsigned>(p.reg); return true;
        case K::Ind: mode = 2; reg = static_cast<unsigned>(p.reg); return true;
        case K::PostInc: mode = 3; reg = static_cast<unsigned>(p.reg); return true;
        case K::PreDec: mode = 4; reg = static_cast<unsigned>(p.reg); return true;
        case K::D16An:
            if (!c.eval(p.expr, v)) return false;
            if (!v.undefined && !fitsSigned(v.value, 16)) {
                c.error = "displacement out of 16 bit range";
                return false;
            }
            c.ext.push_back(static_cast<Word>(v.value & 0xFFFF));
            mode = 5;
            reg = static_cast<unsigned>(p.reg);
            return true;
        case K::D8AnXn:
        case K::D8PcXn: {
            if (!c.eval(p.expr, v)) return false;
            std::int64_t d = v.value;
            if (p.kind == K::D8PcXn) d = pcDisp(v.value, static_cast<std::int64_t>(c.extAddress()));
            if (!v.undefined && !fitsSigned(d, 8)) {
                c.error = "displacement out of 8 bit range";
                return false;
            }
            if (v.undefined) d = 0;
            c.ext.push_back(static_cast<Word>((p.xAddr ? 0x8000 : 0) | (p.xreg << 12) | (p.xLong ? 0x0800 : 0) | (d & 0xFF)));
            if (p.kind == K::D8AnXn) {
                mode = 6;
                reg = static_cast<unsigned>(p.reg);
            } else {
                mode = 7;
                reg = 3;
            }
            return true;
        }
        case K::D16Pc: {
            if (!c.eval(p.expr, v)) return false;
            std::int64_t d = pcDisp(v.value, static_cast<std::int64_t>(c.extAddress()));
            if (v.undefined) d = 0;
            if (!v.undefined && !fitsSigned(d, 16)) {
                c.error = "PC-relative displacement out of 16 bit range";
                return false;
            }
            c.ext.push_back(static_cast<Word>(d & 0xFFFF));
            mode = 7;
            reg = 2;
            return true;
        }
        case K::AbsW:
        case K::AbsL:
        case K::AbsAuto: {
            if (!c.eval(p.expr, v)) return false;
            bool useLong = p.kind == K::AbsL;
            if (p.kind == K::AbsAuto) {
                if (!(allowed & kAbsW)) useLong = true;
                else if (!(allowed & kAbsL)) useLong = false;
                else if (c.req.hint >= 1 || (!v.undefined && !fitsSigned(v.value, 16))) useLong = true;
                if (useLong) c.level = 1;
            }
            if (useLong) {
                if (!v.undefined && (v.value < -(std::int64_t{1} << 31) || v.value > 0xFFFFFFFFLL)) {
                    c.error = "address out of 32 bit range";
                    return false;
                }
                c.ext.push_back(static_cast<Word>((v.value >> 16) & 0xFFFF));
                c.ext.push_back(static_cast<Word>(v.value & 0xFFFF));
                mode = 7;
                reg = 1;
            } else {
                if (!v.undefined && (v.value < -0x8000 || v.value > 0xFFFF)) {
                    c.error = "address out of 16 bit range for .W";
                    return false;
                }
                c.ext.push_back(static_cast<Word>(v.value & 0xFFFF));
                mode = 7;
                reg = 0;
            }
            return true;
        }
        case K::Imm: {
            if (!c.eval(p.expr, v)) return false;
            const std::int64_t x = v.value;
            if (size == Size::B) {
                if (!v.undefined && (x < -128 || x > 255)) {
                    c.error = "immediate out of byte range";
                    return false;
                }
                c.ext.push_back(static_cast<Word>(x & 0xFF));
            } else if (size == Size::W) {
                if (!v.undefined && (x < -32768 || x > 65535)) {
                    c.error = "immediate out of word range";
                    return false;
                }
                c.ext.push_back(static_cast<Word>(x & 0xFFFF));
            } else {
                if (!v.undefined && (x < -(std::int64_t{1} << 31) || x > 0xFFFFFFFFLL)) {
                    c.error = "immediate out of long range";
                    return false;
                }
                c.ext.push_back(static_cast<Word>((x >> 16) & 0xFFFF));
                c.ext.push_back(static_cast<Word>(x & 0xFFFF));
            }
            mode = 7;
            reg = 4;
            return true;
        }
        default:
            c.error = "bad operand";
            return false;
    }
}

EncodeResult tryForm(const Form& f, const EncodeRequest& req, const std::vector<Parsed>& ops, const ValueFn& val) {
    EncodeResult r;
    Ctx c{req, val, {}, 0, {}};
    auto fail = [&](const std::string& m) {
        r.ok = false;
        r.error = c.error.empty() ? m : c.error;
        return r;
    };
    const int nops = (f.op[0] != Op::None ? 1 : 0) + (f.op[1] != Op::None ? 1 : 0);
    if (static_cast<int>(ops.size()) != nops) return fail("wrong number of operands");

    // ---- size ----
    const char us = req.size;
    const bool branch = f.op[0] == Op::Branch;
    Size size = Size::W;
    auto fromChar = [](char ch) { return ch == 'B' ? Size::B : ch == 'W' ? Size::W : Size::L; };
    switch (f.sk) {
        case SizeKind::None:
            if (branch) {
                if (us != 0 && us != 'S' && us != 'B' && us != 'W') return fail("bad size for a branch");
            } else if (f.op[1] == Op::DbDisp) {
                if (us != 0 && us != 'W') return fail("bad size");
            } else if (us != 0) {
                return fail("this instruction has no size");
            }
            break;
        case SizeKind::Any:
            if (us != 0 && us != 'B' && us != 'L') return fail("size must be .B or .L");
            break;
        case SizeKind::B76:
        case SizeKind::Move:
            if (us != 0 && us != 'B' && us != 'W' && us != 'L') return fail("bad size");
            size = us == 0 ? Size::W : fromChar(us);
            break;
        case SizeKind::W8:
        case SizeKind::W6:
            if (us != 0 && us != 'W' && us != 'L') return fail("size must be .W or .L");
            size = us == 0 ? Size::W : fromChar(us);
            break;
        case SizeKind::Fixed: {
            const char want = f.fixed == Size::B ? 'B' : f.fixed == Size::W ? 'W' : 'L';
            if (us != 0 && us != want) return fail(std::string("size must be .") + want);
            size = f.fixed;
            break;
        }
    }

    std::uint16_t op = f.value;
    switch (f.sk) {
        case SizeKind::B76: op = static_cast<std::uint16_t>(op | ((size == Size::B ? 0U : size == Size::W ? 1U : 2U) << 6)); break;
        case SizeKind::Move: op = static_cast<std::uint16_t>(op | ((size == Size::B ? 1U : size == Size::W ? 3U : 2U) << 12)); break;
        case SizeKind::W8: if (size == Size::L) op |= 0x0100; break;
        case SizeKind::W6: if (size == Size::L) op |= 0x0040; break;
        default: break;
    }

    // ---- MOVEM mask comes first ----
    for (int i = 0; i < 2; ++i) {
        if (f.op[i] != Op::RegList) continue;
        const Parsed& p = ops[static_cast<std::size_t>(i)];
        unsigned mask = 0;
        if (p.kind == K::RegList) mask = p.mask;
        else if (p.kind == K::Dn) mask = 1U << p.reg;
        else if (p.kind == K::An) mask = 1U << (p.reg + 8);
        else return fail("register list expected");
        const Parsed& other = ops[static_cast<std::size_t>(1 - i)];
        if (other.kind == K::PreDec) {  // -(An): bit 0 = A7 ... bit 15 = D0
            unsigned rev = 0;
            for (unsigned b = 0; b < 16; ++b)
                if (mask & (1U << b)) rev |= 1U << (15 - b);
            mask = rev;
        }
        c.ext.push_back(static_cast<Word>(mask));
    }

    // ---- operands ----
    EncodeValue v;
    for (int i = 0; i < 2; ++i) {
        const Op o = f.op[i];
        if (o == Op::None || o == Op::RegList) continue;
        const Parsed& p = ops[static_cast<std::size_t>(i)];
        switch (o) {
            case Op::Ea:
            case Op::EaDst: {
                unsigned mode = 0, reg = 0;
                if (!encodeEa(c, p, size, f.set[i], f.noAnByte, mode, reg)) return fail("bad operand");
                if (o == Op::Ea) op = static_cast<std::uint16_t>(op | (mode << 3) | reg);
                else op = static_cast<std::uint16_t>(op | (reg << 9) | (mode << 6));
                break;
            }
            case Op::Dn9: if (p.kind != K::Dn) return fail("data register expected"); op = static_cast<std::uint16_t>(op | (p.reg << 9)); break;
            case Op::Dn0: if (p.kind != K::Dn) return fail("data register expected"); op = static_cast<std::uint16_t>(op | p.reg); break;
            case Op::An9: if (p.kind != K::An) return fail("address register expected"); op = static_cast<std::uint16_t>(op | (p.reg << 9)); break;
            case Op::An0: if (p.kind != K::An) return fail("address register expected"); op = static_cast<std::uint16_t>(op | p.reg); break;
            case Op::PostInc0: if (p.kind != K::PostInc) return fail("(An)+ expected"); op = static_cast<std::uint16_t>(op | p.reg); break;
            case Op::PostInc9: if (p.kind != K::PostInc) return fail("(An)+ expected"); op = static_cast<std::uint16_t>(op | (p.reg << 9)); break;
            case Op::PreDec0: if (p.kind != K::PreDec) return fail("-(An) expected"); op = static_cast<std::uint16_t>(op | p.reg); break;
            case Op::PreDec9: if (p.kind != K::PreDec) return fail("-(An) expected"); op = static_cast<std::uint16_t>(op | (p.reg << 9)); break;
            case Op::Imm: {
                if (p.kind != K::Imm) return fail("immediate operand expected");
                unsigned mode = 0, reg = 0;
                if (!encodeEa(c, p, size, detail::kImm, false, mode, reg)) return fail("bad immediate");
                break;
            }
            case Op::ImmQ3:
                if (p.kind != K::Imm) return fail("immediate operand expected");
                if (!c.eval(p.expr, v)) return fail("bad value");
                if (!v.undefined && (v.value < 1 || v.value > 8)) return fail("quick value must be 1..8");
                op = static_cast<std::uint16_t>(op | ((static_cast<unsigned>(v.value) & 7U) << 9));
                break;
            case Op::ImmMoveq:
                if (p.kind != K::Imm) return fail("immediate operand expected");
                if (!c.eval(p.expr, v)) return fail("bad value");
                if (!v.undefined && (v.value < -128 || v.value > 255)) return fail("MOVEQ value out of range");
                op = static_cast<std::uint16_t>(op | (static_cast<unsigned>(v.value) & 0xFFU));
                break;
            case Op::ImmTrap:
                if (p.kind != K::Imm) return fail("immediate operand expected");
                if (!c.eval(p.expr, v)) return fail("bad value");
                if (!v.undefined && (v.value < 0 || v.value > 15)) return fail("trap vector must be 0..15");
                op = static_cast<std::uint16_t>(op | (static_cast<unsigned>(v.value) & 15U));
                break;
            case Op::ImmWord:
            case Op::ImmSWord:
                if (p.kind != K::Imm) return fail("immediate operand expected");
                if (!c.eval(p.expr, v)) return fail("bad value");
                if (!v.undefined && (v.value < -32768 || v.value > 65535)) return fail("immediate out of word range");
                c.ext.push_back(static_cast<Word>(v.value & 0xFFFF));
                break;
            case Op::ImmBit:
                if (p.kind != K::Imm) return fail("immediate operand expected");
                if (!c.eval(p.expr, v)) return fail("bad value");
                if (!v.undefined && (v.value < 0 || v.value > 255)) return fail("bit number must be 0..255");
                c.ext.push_back(static_cast<Word>(v.value & 0xFF));
                break;
            case Op::Ccr: if (p.kind != K::Ccr) return fail("CCR expected"); break;
            case Op::Sr: if (p.kind != K::Sr) return fail("SR expected"); break;
            case Op::Usp: if (p.kind != K::Usp) return fail("USP expected"); break;
            case Op::Branch: {
                if (p.kind != K::AbsAuto) return fail("branch target expected");
                if (!c.eval(p.expr, v)) return fail("bad target");
                const std::int64_t disp = pcDisp(v.value, static_cast<std::int64_t>(req.pc + 2));
                bool shortForm;
                if (us == 'S' || us == 'B') shortForm = true;
                else if (us == 'W') shortForm = false;
                else if (v.undefined) shortForm = req.hint == 0;
                else shortForm = req.hint == 0 && disp >= -128 && disp <= 127 && disp != 0;
                if (!v.undefined) {
                    if (v.value & 1) return fail("branch target at odd address");
                    if (shortForm && (disp < -128 || disp > 127 || disp == 0)) return fail("branch too far for .S");
                    if (!shortForm && !fitsSigned(disp, 16)) return fail("branch target out of 16 bit range");
                }
                if (!(us == 'S' || us == 'B' || us == 'W') && !shortForm) c.level = 1;
                if (shortForm) op = static_cast<std::uint16_t>(op | (static_cast<unsigned>(v.undefined ? 2 : disp) & 0xFFU));
                else c.ext.push_back(static_cast<Word>((v.undefined ? 0 : disp) & 0xFFFF));
                break;
            }
            case Op::DbDisp: {
                if (p.kind != K::AbsAuto) return fail("branch target expected");
                if (!c.eval(p.expr, v)) return fail("bad target");
                const std::int64_t disp = pcDisp(v.value, static_cast<std::int64_t>(c.extAddress()));
                if (!v.undefined) {
                    if (v.value & 1) return fail("branch target at odd address");
                    if (!fitsSigned(disp, 16)) return fail("branch target out of 16 bit range");
                }
                c.ext.push_back(static_cast<Word>((v.undefined ? 0 : disp) & 0xFFFF));
                break;
            }
            case Op::D16An0: {
                if (p.kind != K::D16An) return fail("d16(An) expected");
                if (!c.eval(p.expr, v)) return fail("bad displacement");
                if (!v.undefined && !fitsSigned(v.value, 16)) return fail("displacement out of 16 bit range");
                c.ext.push_back(static_cast<Word>(v.value & 0xFFFF));
                op = static_cast<std::uint16_t>(op | p.reg);
                break;
            }
            default:
                return fail("unsupported operand");
        }
    }

    r.ok = true;
    r.level = std::max(c.level, req.hint);
    r.bytes.push_back(static_cast<Byte>(op >> 8));
    r.bytes.push_back(static_cast<Byte>(op));
    for (const Word w : c.ext) {
        r.bytes.push_back(static_cast<Byte>(w >> 8));
        r.bytes.push_back(static_cast<Byte>(w));
    }
    return r;
}

}  // namespace

std::vector<std::string> splitOperands(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    for (const char ch : text) {
        if (quote) {
            cur += ch;
            if (ch == quote) quote = 0;
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
            cur += ch;
        } else if (ch == '(') {
            ++depth;
            cur += ch;
        } else if (ch == ')') {
            --depth;
            cur += ch;
        } else if (ch == ',' && depth == 0) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!trim(cur).empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}

bool isMnemonic(const std::string& name) {
    const std::string n = canonicalName(upper(name));
    for (const Form& f : detail::forms())
        if (n == f.name) return true;
    return false;
}

EncodeResult encodeInstruction(const EncodeRequest& request, const ValueFn& value) {
    EncodeResult fail;
    const std::string name = canonicalName(upper(request.mnemonic));

    std::vector<Parsed> ops;
    for (const std::string& text : request.operands) {
        Parsed p;
        std::string err;
        if (!parseOperand(text, p, err)) {
            fail.error = err;
            return fail;
        }
        ops.push_back(p);
    }

    std::vector<const Form*> candidates;
    for (const Form& f : detail::forms())
        if (name == f.name) candidates.push_back(&f);
    for (const std::string& alt : alternatives(name))
        for (const Form& f : detail::forms())
            if (alt == f.name) candidates.push_back(&f);
    if (candidates.empty()) {
        fail.error = "unknown instruction '" + request.mnemonic + "'";
        return fail;
    }

    // If no form fits, report the most informative error: a value problem in a form whose operand
    // kinds matched ("quick value must be 1..8") beats "data register expected" from another form.
    std::string bestError;
    int bestScore = -1;
    for (const Form* f : candidates) {
        EncodeResult r = tryForm(*f, request, ops, value);
        if (r.ok) return r;
        const bool valueProblem = r.error.find("range") != std::string::npos || r.error.find("..") != std::string::npos ||
                                  r.error.find("too far") != std::string::npos || r.error.find("odd address") != std::string::npos;
        const int score = valueProblem ? 2 : r.error.find("number of operands") != std::string::npos ? 0 : 1;
        if (score > bestScore) {
            bestScore = score;
            bestError = r.error;
        }
    }
    fail.error = bestError.empty() ? "invalid operands" : bestError;
    return fail;
}

}  // namespace m68k
