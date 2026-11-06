#include "m68k/assembler.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

#include "m68k/encoder.hpp"
#include "m68k/expression.hpp"
#include "m68k/format.hpp"

namespace m68k {

namespace {

constexpr int kMaxPasses = 40;

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Removes a trailing ';' comment (outside quotes) and handles '*' comment lines.
std::string stripComment(const std::string& line) {
    if (!line.empty() && line[0] == '*') return "";
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == ';') {
            return line.substr(0, i);
        }
    }
    return line;
}

struct SourceLine {
    int number = 0;
    std::string text;      // original text (for the listing)
    std::string label;     // "" if none
    std::string mnemonic;  // upper case, without size
    char size = 0;
    std::string operands;
    bool hasStatement = false;
};

SourceLine parseLine(int number, const std::string& original) {
    SourceLine l;
    l.number = number;
    l.text = original;
    const std::string s = stripComment(original);
    std::size_t i = 0;
    auto skipWs = [&] {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    };
    auto token = [&] {
        const std::size_t b = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(b, i - b);
    };
    if (s.empty()) return l;
    if (!std::isspace(static_cast<unsigned char>(s[0]))) {
        l.label = token();
        if (!l.label.empty() && l.label.back() == ':') l.label.pop_back();
    }
    skipWs();
    if (i >= s.size()) return l;
    std::string mn = token();
    if (l.label.empty() && mn.size() > 1 && mn.back() == ':') {  // indented "label:"
        l.label = mn.substr(0, mn.size() - 1);
        skipWs();
        if (i >= s.size()) return l;
        mn = token();
    }
    skipWs();
    l.operands = trim(s.substr(i));
    // "name = expr" without spaces around '='
    if (mn == "=") {
        l.mnemonic = "EQU";
    } else {
        const std::size_t dot = mn.rfind('.');
        if (dot != std::string::npos && dot + 2 == mn.size() && dot > 0) {
            const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(mn[dot + 1])));
            if (c == 'B' || c == 'W' || c == 'L' || c == 'S') {
                l.size = c;
                mn.resize(dot);
            }
        }
        l.mnemonic = upper(mn);
    }
    l.hasStatement = true;
    return l;
}

struct Pass {
    Pass(const std::vector<SourceLine>& l, const AssemblerOptions& o, const std::map<std::string, std::int64_t>& p,
         std::vector<int>& h, bool hunkMode_)
        : lines(l), options(o), previous(p), hints(h), hunkMode(hunkMode_) {}

    const std::vector<SourceLine>& lines;
    const AssemblerOptions& options;
    const std::map<std::string, std::int64_t>& previous;
    std::vector<int>& hints;

    bool hunkMode;
    std::vector<HunkSection> secs;   // SECTION programs
    int cur = -1;                    // current section
    std::vector<std::string> xdefs;
    std::vector<std::pair<int, unsigned>> relocChecks;  // (line, target section) of every relocation, checked when all sections are known
    std::map<std::string, std::int64_t> symbols;
    std::vector<Segment> segments;
    std::vector<Diagnostic> errors;
    std::vector<ListingLine> listing;
    bool hintsChanged = false;
    Address pc = 0;
    std::string lastGlobal;

    void error(int line, const std::string& m) { errors.push_back({line, m}); }

    std::string scoped(const std::string& name) const { return !name.empty() && name[0] == '.' ? lastGlobal + name : name; }

    bool lookup(const std::string& raw, std::int64_t& v) const {
        const std::string name = scoped(raw);
        if (auto it = symbols.find(name); it != symbols.end()) {
            v = it->second;
            return true;
        }
        if (auto it = previous.find(name); it != previous.end()) {
            v = it->second;
            return true;
        }
        if (auto it = options.predefined.find(name); it != options.predefined.end()) {
            v = it->second;
            return true;
        }
        return false;
    }

    // Evaluates an expression; reports problems on `line`. `undefinedOk` allows unresolved forward references.
    bool eval(int line, const std::string& text, std::int64_t& out, bool& undefined) {
        const ExpressionEvaluator ev([this](const std::string& n, std::int64_t& v) { return lookup(n, v); }, here());
        const ExprResult r = ev.evaluate(text);
        if (!r.ok) {
            error(line, r.error);
            return false;
        }
        out = r.value;
        undefined = r.undefined;
        if (r.undefined) pendingUndefined = r.undefinedName;
        return true;
    }
    std::string pendingUndefined;

    // The value of the location counter: a plain address, or section-relative in SECTION programs.
    std::int64_t here() const { return hunkMode && cur >= 0 ? relocatable(static_cast<unsigned>(cur), pc) : static_cast<std::int64_t>(pc); }

    void emit(const std::vector<Byte>& bytes) {
        if (hunkMode) {
            HunkSection& sec = secs[static_cast<std::size_t>(cur)];
            sec.bytes.insert(sec.bytes.end(), bytes.begin(), bytes.end());
            pc += static_cast<Address>(bytes.size());
            return;
        }
        if (segments.empty() || segments.back().address + segments.back().bytes.size() != pc) {
            segments.push_back({pc, {}});
        }
        segments.back().bytes.insert(segments.back().bytes.end(), bytes.begin(), bytes.end());
        pc += static_cast<Address>(bytes.size());
    }

    void define(int line, const std::string& rawName, std::int64_t value) {
        const std::string name = scoped(rawName);
        if (symbols.count(name)) {
            error(line, "symbol '" + rawName + "' defined twice");
            return;
        }
        symbols[name] = value;
    }

    // ---- data directives -----------------------------------------------------------------------
    void dataBytes(const SourceLine& l, std::vector<Byte>& out) {
        const int unit = l.size == 'L' ? 4 : l.size == 'W' ? 2 : 1;
        for (const std::string& item : splitOperands(l.operands)) {
            if (item.empty()) {
                error(l.number, "empty item in " + l.mnemonic);
                continue;
            }
            // "text" is always a string (also one character or empty); 'AB' is a string, 'A' a character constant
            if (unit == 1 && item.size() >= 2 && item.back() == item[0] && (item[0] == '"' || (item[0] == '\'' && item.size() > 3))) {
                for (std::size_t k = 1; k + 1 < item.size(); ++k) out.push_back(static_cast<Byte>(item[k]));
                continue;
            }
            std::int64_t v = 0;
            bool undef = false;
            pendingUndefined.clear();
            if (!eval(l.number, item, v, undef)) continue;
            if (undef) pendingUndefinedList.push_back({l.number, pendingUndefined});
            if (hunkMode && !undef && sectionTag(v) != 0) {
                if (!isSectionTag(sectionTag(v))) {
                    error(l.number, "'" + item + "' mixes addresses of different SECTIONs");
                    continue;
                }
                if (unit != 4) {
                    error(l.number, "'" + item + "' is an address inside a SECTION: only DC.L can hold it (it is relocated by the loader)");
                    continue;
                }
                relocChecks.push_back({l.number, tagSection(sectionTag(v))});
                secs[static_cast<std::size_t>(cur)].relocs.push_back({pc + static_cast<Address>(out.size()), tagSection(sectionTag(v))});
                v = sectionOffset(v);
            }
            const std::int64_t lo = unit == 1 ? -128 : unit == 2 ? -32768 : -(std::int64_t{1} << 31);
            const std::int64_t hi = unit == 1 ? 255 : unit == 2 ? 65535 : 0xFFFFFFFFLL;
            if (!undef && (v < lo || v > hi)) error(l.number, "value out of range for " + l.mnemonic + "." + l.size);
            for (int b = unit - 1; b >= 0; --b) out.push_back(static_cast<Byte>((v >> (8 * b)) & 0xFF));
        }
    }
    std::vector<Diagnostic> pendingUndefinedList;

    // SECTION name[,CODE|DATA|BSS][_C|_F]   (also the shorthands CODE, DATA, BSS)
    void section(const SourceLine& l) {
        std::vector<std::string> ops = splitOperands(l.operands);
        std::string name, type;
        if (l.mnemonic == "SECTION") {
            if (ops.empty() || ops[0].empty()) return error(l.number, "SECTION needs a name");
            name = ops[0];
            type = ops.size() > 1 ? upperCase(ops[1]) : "CODE";
            if (ops.size() > 2) return error(l.number, "too many operands for SECTION");
        } else {
            type = l.mnemonic;
            name = ops.empty() || ops[0].empty() ? lowerCase(type.substr(0, type.find('_'))) : ops[0];
        }
        if (name.size() >= 2 && name.front() == name.back() && (name.front() == '"' || name.front() == '\'')) name = name.substr(1, name.size() - 2);
        HunkSection::Kind kind;
        bool chip = false, fast = false;
        std::string base = type;
        if (const std::size_t us = type.find('_'); us != std::string::npos) {
            base = type.substr(0, us);
            const std::string mem = type.substr(us + 1);
            if (mem == "C") chip = true;
            else if (mem == "F") fast = true;
            else if (mem != "P") return error(l.number, "unknown memory type '_" + mem + "' (use _C, _F or _P)");
        }
        if (base == "CODE") kind = HunkSection::Kind::Code;
        else if (base == "DATA") kind = HunkSection::Kind::Data;
        else if (base == "BSS") kind = HunkSection::Kind::Bss;
        else return error(l.number, "unknown section type '" + type + "' (CODE, DATA or BSS, optionally with _C or _F)");
        for (std::size_t i = 0; i < secs.size(); ++i) {
            if (secs[i].name != name) continue;
            if (secs[i].kind != kind) return error(l.number, "section '" + name + "' was declared with another type");
            cur = static_cast<int>(i);
            pc = secs[i].size();
            return;
        }
        HunkSection s;
        s.name = name;
        s.kind = kind;
        s.chip = chip;
        s.fast = fast;
        secs.push_back(std::move(s));
        cur = static_cast<int>(secs.size()) - 1;
        pc = 0;
    }

    static std::string upperCase(std::string t) {
        for (char& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return t;
    }
    static std::string lowerCase(std::string t) {
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    }

    void run() {
        pc = options.origin;
        if (hunkMode) pc = 0;
        for (std::size_t idx = 0; idx < lines.size(); ++idx) {
            const SourceLine& l = lines[idx];
            Address startPc = pc;
            std::vector<Byte> produced;
            bool stop = false;
            const bool isSectionDirective =
                l.hasStatement && hunkMode &&
                (l.mnemonic == "SECTION" || l.mnemonic == "CODE" || l.mnemonic == "DATA" || l.mnemonic == "BSS" || l.mnemonic == "CODE_C" ||
                 l.mnemonic == "DATA_C" || l.mnemonic == "BSS_C" || l.mnemonic == "CODE_F" || l.mnemonic == "DATA_F" || l.mnemonic == "BSS_F");
            if (isSectionDirective) {
                section(l);
                if (!l.label.empty()) error(l.number, "a label on a SECTION line is not supported");
                listing.push_back({l.number, cur, pc, {}, l.text});
                continue;
            }
            const bool needsSection = hunkMode && cur < 0 && l.hasStatement && l.mnemonic != "EQU" && l.mnemonic != "XDEF" && l.mnemonic != "END";
            if (needsSection || (hunkMode && cur < 0 && !l.label.empty() && !(l.hasStatement && l.mnemonic == "EQU"))) {
                error(l.number, "code, data or a label before the first SECTION");
                continue;
            }
            const bool inBss = hunkMode && cur >= 0 && secs[static_cast<std::size_t>(cur)].kind == HunkSection::Kind::Bss;

            if (!l.label.empty() && !(l.hasStatement && (l.mnemonic == "EQU"))) {
                if (!(l.label[0] == '.')) lastGlobal = l.label;
                define(l.number, l.label, here());
            }
            if (l.hasStatement) {
                const std::string& m = l.mnemonic;
                if (m == "XDEF") {
                    if (!hunkMode) {
                        error(l.number, "XDEF only makes sense in a program with SECTIONs");
                    } else {
                        for (const std::string& n : splitOperands(l.operands)) xdefs.push_back(n);
                    }
                } else if (m == "XREF") {
                    error(l.number, "XREF (external references) needs a linker; this assembler produces finished programs");
                } else if (inBss && m != "DS" && m != "EQU" && m != "EVEN" && m != "END") {
                    error(l.number, "a BSS section can only contain DS (and labels); use a DATA section for initialised data");
                } else if (m == "EQU") {
                    if (l.label.empty()) {
                        error(l.number, "EQU needs a label");
                    } else {
                        std::int64_t v = 0;
                        bool undef = false;
                        pendingUndefined.clear();
                        if (eval(l.number, l.operands, v, undef)) {
                            if (undef) {
                                pendingUndefinedList.push_back({l.number, pendingUndefined});
                            } else {
                                if (l.label[0] != '.') lastGlobal = l.label;
                                define(l.number, l.label, v);
                            }
                        }
                    }
                } else if (m == "ORG" && hunkMode) {
                    error(l.number, "ORG cannot be used in a program with SECTIONs (the loader decides the address)");
                } else if (m == "ORG") {
                    std::int64_t v = 0;
                    bool undef = false;
                    pendingUndefined.clear();
                    if (eval(l.number, l.operands, v, undef)) {
                        if (undef) error(l.number, "ORG needs a defined value");
                        else if (v < 0 || v > static_cast<std::int64_t>(kAddressMask)) error(l.number, "ORG address out of range");
                        else pc = static_cast<Address>(v);
                    }
                } else if (m == "EVEN") {
                    if (pc & 1U) {
                        if (inBss) {
                            ++secs[static_cast<std::size_t>(cur)].bssSize;
                            ++pc;
                        } else {
                            produced.push_back(0);
                        }
                    }
                } else if (m == "END") {
                    stop = true;
                } else if (m == "DC") {
                    if (l.size == 0 || l.size == 'S') error(l.number, "DC needs a size (.B, .W or .L)");
                    else dataBytes(l, produced);
                } else if (m == "DS") {
                    std::int64_t n = 0;
                    bool undef = false;
                    pendingUndefined.clear();
                    if (l.size == 0 || l.size == 'S') {
                        error(l.number, "DS needs a size (.B, .W or .L)");
                    } else if (eval(l.number, l.operands, n, undef)) {
                        if (undef) error(l.number, "DS needs a defined count");
                        else if (n < 0 || n > 0x1000000) error(l.number, "bad DS count");
                        else if (inBss) {
                            const Address bytes = static_cast<Address>(n) * (l.size == 'L' ? 4 : l.size == 'W' ? 2 : 1);
                            secs[static_cast<std::size_t>(cur)].bssSize += bytes;
                            pc += bytes;
                        } else produced.assign(static_cast<std::size_t>(n) * (l.size == 'L' ? 4 : l.size == 'W' ? 2 : 1), 0);
                    }
                } else if (isMnemonic(m)) {
                    if (pc & 1U) error(l.number, "instruction at odd address");
                    EncodeRequest rq;
                    rq.mnemonic = m;
                    rq.size = l.size;
                    rq.operands = splitOperands(l.operands);
                    rq.pc = pc;
                    rq.section = hunkMode ? tagOfSection(static_cast<unsigned>(cur)) : 0;
                    rq.hint = hints[idx];
                    std::string undefinedName;
                    const EncodeResult r = encodeInstruction(rq, [&](const std::string& e) {
                        EncodeValue v;
                        const ExpressionEvaluator ev([this](const std::string& n, std::int64_t& val) { return lookup(n, val); }, here());
                        const ExprResult x = ev.evaluate(e);
                        if (!x.ok) {
                            v.error = x.error;
                            return v;
                        }
                        v.value = x.value;
                        v.undefined = x.undefined;
                        if (x.undefined && undefinedName.empty()) undefinedName = x.undefinedName;
                        return v;
                    });
                    if (!r.ok) {
                        error(l.number, r.error);
                    } else {
                        if (!undefinedName.empty()) pendingUndefinedList.push_back({l.number, undefinedName});
                        if (r.level != hints[idx]) {
                            hints[idx] = r.level;
                            hintsChanged = true;
                        }
                        produced = r.bytes;
                        for (const Relocation& rel : r.relocs) {
                            relocChecks.push_back({l.number, rel.section});
                            secs[static_cast<std::size_t>(cur)].relocs.push_back({pc + rel.offset, rel.section});
                        }
                    }
                } else {
                    error(l.number, "unknown instruction or directive '" + l.mnemonic + "'");
                }
            }
            if (!produced.empty()) emit(produced);
            if (l.hasStatement || !l.label.empty()) {
                listing.push_back({l.number, hunkMode ? cur : -1, startPc, produced, l.text});
            } else if (!trim(l.text).empty()) {
                listing.push_back({l.number, hunkMode ? cur : -1, pc, {}, l.text});
            }
            if (stop) break;
        }
        for (const auto& [line, target] : relocChecks)
            if (target >= secs.size()) error(line, "address arithmetic mixes sections in an unsupported way");
    }
};

}  // namespace

Address AssemblyResult::lowest() const {
    Address lo = 0xFFFFFFFF;
    for (const Segment& s : segments)
        if (!s.bytes.empty()) lo = std::min(lo, s.address);
    return lo == 0xFFFFFFFF ? 0 : lo;
}

std::vector<Byte> AssemblyResult::flatten() const {
    std::vector<Byte> out;
    const Address lo = lowest();
    for (const Segment& s : segments) {
        if (s.bytes.empty()) continue;
        const std::size_t off = s.address - lo;
        if (out.size() < off + s.bytes.size()) out.resize(off + s.bytes.size(), 0);
        std::copy(s.bytes.begin(), s.bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(off));
    }
    return out;
}

std::string AssemblyResult::formatListing() const {
    std::ostringstream out;
    for (const ListingLine& l : listing) {
        std::string bytes;
        for (std::size_t i = 0; i < l.bytes.size() && i < 8; ++i) {
            bytes += hexDigits(l.bytes[i], 2);
            if (i % 2 == 1) bytes += ' ';
        }
        if (l.bytes.size() > 8) bytes += "..";
        char buf[64];
        if (l.section >= 0) std::snprintf(buf, sizeof buf, "%5d  S%d:%06X  %-21s ", l.line, l.section, l.address & kAddressMask, bytes.c_str());
        else std::snprintf(buf, sizeof buf, "%5d  %06X  %-21s ", l.line, l.address & kAddressMask, bytes.c_str());
        out << buf << l.source << '\n';
    }
    return out.str();
}

std::string AssemblyResult::formatSymbols() const {
    std::vector<std::pair<std::int64_t, std::string>> v;
    for (const auto& [name, value] : symbols) v.emplace_back(value, name);
    std::sort(v.begin(), v.end());
    std::ostringstream out;
    for (const auto& [value, name] : v) {
        out << name << " = " << formatHex(static_cast<unsigned>(value), 8, NumberStyle::Dollar);
        if (hunk) {
            const auto it = symbolSection.find(name);
            if (it != symbolSection.end() && it->second >= 0) out << "   ; section " << it->second << " (" << sections[static_cast<std::size_t>(it->second)].name << ")";
        }
        out << '\n';
    }
    return out.str();
}

AssemblyResult assemble(const std::string& source, const AssemblerOptions& options) {
    std::vector<SourceLine> lines;
    {
        std::istringstream in(source);
        std::string text;
        int n = 0;
        while (std::getline(in, text)) {
            if (!text.empty() && text.back() == '\r') text.pop_back();
            lines.push_back(parseLine(++n, text));
        }
    }

    std::vector<int> hints(lines.size(), 0);
    std::map<std::string, std::int64_t> previous;
    AssemblyResult result;
    // A program with a SECTION directive is assembled relocatably.
    bool hunkMode = false;
    for (const SourceLine& l : lines)
        if (l.hasStatement && l.mnemonic == "SECTION") hunkMode = true;
    result.hunk = hunkMode;

    for (int passNo = 0; passNo < kMaxPasses; ++passNo) {
        Pass pass(lines, options, previous, hints, hunkMode);
        pass.run();
        const bool stable = !pass.hintsChanged && pass.symbols == previous;
        if (stable || passNo == kMaxPasses - 1) {
            result.segments = std::move(pass.segments);
            result.errors = std::move(pass.errors);
            result.listing = std::move(pass.listing);
            if (hunkMode) {
                // symbols: offsets inside their sections
                std::set<std::string> exported(pass.xdefs.begin(), pass.xdefs.end());
                for (const auto& [name, value] : pass.symbols) {
                    const int tag = sectionTag(value);
                    if (isSectionTag(tag) && tagSection(tag) < pass.secs.size()) {
                        result.symbols[name] = sectionOffset(value);
                        result.symbolSection[name] = static_cast<int>(tagSection(tag));
                    } else {
                        result.symbols[name] = value;
                        result.symbolSection[name] = -1;
                    }
                }
                for (const std::string& n : pass.xdefs) {
                    if (!pass.symbols.count(n)) result.errors.push_back({0, "XDEF symbol '" + n + "' is not defined"});
                    else if (result.symbolSection[n] < 0) result.errors.push_back({0, "XDEF symbol '" + n + "' is a plain number, not an address"});
                }
                for (const auto& [name, value] : result.symbols) {
                    const int sec = result.symbolSection[name];
                    if (sec < 0 || name.find('.') != std::string::npos) continue;  // numbers and local labels
                    if (options.exportAllSymbols || exported.count(name))
                        pass.secs[static_cast<std::size_t>(sec)].symbols.emplace_back(name, static_cast<Address>(value));
                }
                for (HunkSection& sec : pass.secs) {
                    std::sort(sec.relocs.begin(), sec.relocs.end(), [](const HunkReloc& a, const HunkReloc& b) { return a.offset < b.offset; });
                    std::sort(sec.symbols.begin(), sec.symbols.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
                }
                result.sections = std::move(pass.secs);
            } else {
                result.symbols = std::move(pass.symbols);
            }
            // Forward references that stayed undefined are errors only in the final pass.
            for (const Diagnostic& d : pass.pendingUndefinedList)
                result.errors.push_back({d.line, "undefined symbol '" + d.message + "'"});
            if (!stable) result.errors.push_back({0, "assembly did not converge"});
            break;
        }
        previous = std::move(pass.symbols);
    }
    std::stable_sort(result.errors.begin(), result.errors.end(),
                     [](const Diagnostic& a, const Diagnostic& b) { return a.line < b.line; });
    return result;
}

}  // namespace m68k
