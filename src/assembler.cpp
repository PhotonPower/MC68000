#include "m68k/assembler.hpp"

#include <algorithm>
#include <cctype>
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
         std::vector<int>& h)
        : lines(l), options(o), previous(p), hints(h) {}

    const std::vector<SourceLine>& lines;
    const AssemblerOptions& options;
    const std::map<std::string, std::int64_t>& previous;
    std::vector<int>& hints;

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
        const ExpressionEvaluator ev([this](const std::string& n, std::int64_t& v) { return lookup(n, v); }, pc);
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

    void emit(const std::vector<Byte>& bytes) {
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
            if ((item[0] == '"' || item[0] == '\'') && item.size() >= 2 && item.back() == item[0] && unit == 1 &&
                item.size() > 3) {
                for (std::size_t k = 1; k + 1 < item.size(); ++k) out.push_back(static_cast<Byte>(item[k]));
                continue;
            }
            std::int64_t v = 0;
            bool undef = false;
            pendingUndefined.clear();
            if (!eval(l.number, item, v, undef)) continue;
            if (undef) pendingUndefinedList.push_back({l.number, pendingUndefined});
            const std::int64_t lo = unit == 1 ? -128 : unit == 2 ? -32768 : -(std::int64_t{1} << 31);
            const std::int64_t hi = unit == 1 ? 255 : unit == 2 ? 65535 : 0xFFFFFFFFLL;
            if (!undef && (v < lo || v > hi)) error(l.number, "value out of range for " + l.mnemonic + "." + l.size);
            for (int b = unit - 1; b >= 0; --b) out.push_back(static_cast<Byte>((v >> (8 * b)) & 0xFF));
        }
    }
    std::vector<Diagnostic> pendingUndefinedList;

    void run() {
        pc = options.origin;
        for (std::size_t idx = 0; idx < lines.size(); ++idx) {
            const SourceLine& l = lines[idx];
            const Address startPc = pc;
            std::vector<Byte> produced;
            bool stop = false;

            if (!l.label.empty() && !(l.hasStatement && (l.mnemonic == "EQU"))) {
                if (!(l.label[0] == '.')) lastGlobal = l.label;
                define(l.number, l.label, pc);
            }
            if (l.hasStatement) {
                const std::string& m = l.mnemonic;
                if (m == "EQU") {
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
                    if (pc & 1U) produced.push_back(0);
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
                        else produced.assign(static_cast<std::size_t>(n) * (l.size == 'L' ? 4 : l.size == 'W' ? 2 : 1), 0);
                    }
                } else if (isMnemonic(m)) {
                    if (pc & 1U) error(l.number, "instruction at odd address");
                    EncodeRequest rq;
                    rq.mnemonic = m;
                    rq.size = l.size;
                    rq.operands = splitOperands(l.operands);
                    rq.pc = pc;
                    rq.hint = hints[idx];
                    std::string undefinedName;
                    const EncodeResult r = encodeInstruction(rq, [&](const std::string& e) {
                        EncodeValue v;
                        const ExpressionEvaluator ev([this](const std::string& n, std::int64_t& val) { return lookup(n, val); }, pc);
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
                    }
                } else {
                    error(l.number, "unknown instruction or directive '" + l.mnemonic + "'");
                }
            }
            if (!produced.empty()) emit(produced);
            if (l.hasStatement || !l.label.empty()) {
                listing.push_back({l.number, startPc, produced, l.text});
            } else if (!trim(l.text).empty()) {
                listing.push_back({l.number, pc, {}, l.text});
            }
            if (stop) break;
        }
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
        std::snprintf(buf, sizeof buf, "%5d  %06X  %-21s ", l.line, l.address & kAddressMask, bytes.c_str());
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
        out << name << " = " << formatHex(static_cast<unsigned>(value), 8, NumberStyle::Dollar) << '\n';
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

    for (int passNo = 0; passNo < kMaxPasses; ++passNo) {
        Pass pass(lines, options, previous, hints);
        pass.run();
        const bool stable = !pass.hintsChanged && pass.symbols == previous;
        if (stable || passNo == kMaxPasses - 1) {
            result.segments = std::move(pass.segments);
            result.errors = std::move(pass.errors);
            result.symbols = std::move(pass.symbols);
            result.listing = std::move(pass.listing);
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
