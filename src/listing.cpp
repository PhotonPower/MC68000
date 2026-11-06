#include "m68k/listing.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

#include "m68k/assembler.hpp"
#include "m68k/decoder.hpp"
#include "m68k/format.hpp"

namespace m68k {

namespace {

std::string pad(std::string s, std::size_t w) {
    if (s.size() < w) s.append(w - s.size(), ' ');
    return s;
}
std::string hexv(unsigned v, int digits) { return formatHex(v, digits, NumberStyle::Dollar); }

bool printable(Byte b) { return b >= 0x20 && b < 0x7F; }

// Comments must never start a new source line: newlines become spaces.
std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

}  // namespace

ListingResult renderListing(const Memory& view, const Analysis& analysis, const Annotations& ann, const ListingOptions& opt) {
    ListingResult result;
    const Address from = opt.from & kAddressMask;
    const Address to = std::min<Address>(opt.to, kAddressMask + 1U);
    if (to <= from) {
        result.problem = "empty range";
        return result;
    }
    auto inRange = [&](Address a) { return a >= from && a < to; };
    auto lineStartOk = [&](Address a) { return !analysis.isCode(a) || analysis.at(a) != nullptr; };

    // ---- which addresses get a label ----
    std::map<Address, std::string> names;      // line labels inside the range
    std::map<Address, std::string> equNames;   // names that cannot sit on a line: defined with EQU
    std::set<Address> placeholders;
    auto placeholder = [&](const char* prefix, Address a) { return std::string(prefix) + hexDigits(a, 6); };
    auto markPlaceholder = [&](Address a, const char* prefix) {
        if (!inRange(a) || !lineStartOk(a) || ann.nameAt(a)) return;
        if (names.count(a)) return;
        names[a] = placeholder(prefix, a);
        placeholders.insert(a);
    };
    for (const Address a : analysis.entries())
        if (analysis.at(a)) markPlaceholder(a, "sub_");
    for (const Address a : analysis.callTargets())
        if (analysis.at(a)) markPlaceholder(a, "sub_");
    for (const Address a : analysis.jumpTargets())
        if (analysis.at(a)) markPlaceholder(a, "loc_");
    for (const Address a : analysis.dataReferences()) markPlaceholder(a, analysis.at(a) ? "loc_" : "data_");
    // A long immediate that equals an address inside the range is very likely a pointer (MOVE.L #table,A0).
    // Giving it a label only adds a name and never changes bytes, so a coincidence costs nothing.
    for (const auto& [ia, in] : analysis.code()) {
        if (!inRange(ia)) continue;
        const std::size_t hash = in.operands.find("#$");
        if (hash == std::string::npos || in.operands.size() < hash + 10) continue;
        const std::string digits = in.operands.substr(hash + 2, 8);
        if (digits.find_first_not_of("0123456789ABCDEF") != std::string::npos) continue;
        const Long v = static_cast<Long>(std::stoul(digits, nullptr, 16));
        if (v != 0 && v == (v & kAddressMask) && in.mnemonic.size() > 2 && in.mnemonic.compare(in.mnemonic.size() - 2, 2, ".L") == 0)
            markPlaceholder(v, analysis.at(v) ? "loc_" : "data_");
    }
    for (const auto& [a, r] : ann.dataRegions()) {
        if (r.kind != DataKind::Pointers) continue;
        for (unsigned off = 0; off + 4 <= r.length; off += 4) {
            const Address p = a + off;
            if (!inRange(p + 3)) break;
            const Address t = view.read32(p) & kAddressMask;
            if (view.read32(p) == t) markPlaceholder(t, analysis.at(t) ? "loc_" : "data_");
        }
    }
    for (const auto& [a, n] : ann.labels()) {
        if (inRange(a) && lineStartOk(a)) {
            names[a] = n;
            placeholders.erase(a);
        } else {
            equNames[a] = n;
        }
    }

    // ---- symbolizer used inside operands ----
    std::map<Address, std::string> usedEqu;
    auto symbolize = [&](Address a) -> std::string {
        a &= kAddressMask;
        if (const auto it = names.find(a); it != names.end()) return it->second;
        if (const auto expr = ann.expressionFor(a)) {
            const std::size_t plus = expr->find('+');
            const std::string base = expr->substr(0, plus);
            if (const auto baseAddr = ann.addressOf(base)) {
                if (!names.count(*baseAddr)) usedEqu[*baseAddr] = base;
            }
            return *expr;
        }
        if (opt.platformSymbol) {
            const std::string p = opt.platformSymbol(a);
            if (!p.empty()) {
                usedEqu[a] = p;
                return p;
            }
        }
        return "";
    };
    DecodeOptions dopt;
    dopt.symbolizer = symbolize;

    // ---- body ----
    std::ostringstream body;
    auto emitComment = [&](const std::string& text) {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) body << "; " << line << "\n";
    };
    auto withComment = [&](std::string line, const std::string& comment) {
        if (comment.empty()) return line;
        return pad(line, 44) + "  ; " + oneLine(comment);
    };
    auto isBoundary = [&](Address b) {
        return b >= to || names.count(b) || analysis.at(b) || ann.dataAt(b) || ann.variableAt(b);
    };

    auto dcBytes = [&](Address a, std::size_t n) {
        std::string items;
        for (std::size_t i = 0; i < n; ++i) items += (i ? "," : "") + hexv(view.read8(a + static_cast<Address>(i)), 2);
        return "        DC.B     " + items;
    };

    std::set<Address> emittedLabels;
    auto emitHeader = [&](Address a) {
        if (const std::string* b = ann.blockCommentAt(a)) emitComment(*b);
        if (const RoutineDoc* r = ann.routineAt(a)) {
            body << "; " << std::string(60, '-') << "\n";
            body << "; " << (r->name.empty() ? "routine" : r->name) << (r->summary.empty() ? "" : ": " + oneLine(r->summary)) << "\n";
            if (!r->inputs.empty()) body << ";   in      : " << oneLine(r->inputs) << "\n";
            if (!r->outputs.empty()) body << ";   out     : " << oneLine(r->outputs) << "\n";
            if (!r->clobbers.empty()) body << ";   clobbers: " << oneLine(r->clobbers) << "\n";
            if (r->confidence == "low") body << ";   confidence: low - a guess, please verify\n";
            else if (r->confidence == "medium") body << ";   confidence: medium\n";
            body << "; " << std::string(60, '-') << "\n";
        }
        if (const auto it = names.find(a); it != names.end()) {
            body << it->second << ":\n";
            emittedLabels.insert(a);
        }
    };

    auto textLine = [&](Address a, std::size_t maxLen, std::size_t& used) -> std::string {
        // longest printable run, as a string literal (if it has no mixed quotes)
        std::size_t n = 0;
        bool dq = false, sq = false;
        while (n < maxLen && a + n < to && printable(view.read8(a + static_cast<Address>(n)))) {
            const Byte b = view.read8(a + static_cast<Address>(n));
            dq = dq || b == '"';
            sq = sq || b == '\'';
            ++n;
        }
        if (n == 0 || (dq && sq)) return "";
        std::string s;
        for (std::size_t i = 0; i < n; ++i) s += static_cast<char>(view.read8(a + static_cast<Address>(i)));
        const char q = dq ? '\'' : '"';
        used = n;
        return std::string(1, q) + s + q;
    };

    Address a = from;
    bool cut = false;
    while (a < to) {
        if (opt.maxChars && static_cast<std::size_t>(body.tellp()) > opt.maxChars) {
            cut = true;
            break;
        }
        emitHeader(a);

        if (const DataRegion* r = ann.dataAt(a)) {
            const Address end = std::min<Address>(a + r->length, to);
            if (!r->comment.empty()) emitComment(r->comment);
            Address p = a;
            while (p < end) {
                switch (r->kind) {
                    case DataKind::Bytes: {
                        std::size_t n = std::min<std::size_t>(8, end - p);
                        for (std::size_t i = 1; i < n; ++i)
                            if (names.count(p + static_cast<Address>(i))) {
                                n = i;
                                break;
                            }
                        body << dcBytes(p, n) << "\n";
                        p += static_cast<Address>(n);
                        break;
                    }
                    case DataKind::Words:
                    case DataKind::Longs:
                    case DataKind::Pointers: {
                        const unsigned unit = r->kind == DataKind::Words ? 2 : 4;
                        const std::size_t perLine = unit == 2 ? 8 : 4;
                        std::string items;
                        std::size_t count = 0;
                        while (count < perLine && p + unit <= end) {
                            if (count) items += ",";
                            const Long v = unit == 2 ? view.read16(p) : view.read32(p);
                            std::string text = hexv(v, static_cast<int>(unit) * 2);
                            if (r->kind == DataKind::Pointers && v == (v & kAddressMask)) {
                                const std::string s = symbolize(v);
                                if (!s.empty()) text = s;
                            }
                            items += text;
                            p += unit;
                            ++count;
                            // a label in the middle of the table ends the line
                            if (p < end && names.count(p)) break;
                        }
                        if (count == 0) {  // a trailing odd byte
                            body << dcBytes(p, end - p) << "\n";
                            p = end;
                        } else {
                            body << (unit == 2 ? "        DC.W     " : "        DC.L     ") << items << "\n";
                        }
                        break;
                    }
                    case DataKind::Text: {
                        std::size_t used = 0;
                        std::size_t room = std::min<std::size_t>(60, end - p);
                        for (std::size_t i = 1; i < room; ++i)
                            if (names.count(p + static_cast<Address>(i))) {
                                room = i;
                                break;
                            }
                        std::string lit = textLine(p, room, used);
                        if (used < 2) lit = "";  // one character is written as a number
                        if (lit.empty()) {
                            body << dcBytes(p, 1) << "\n";
                            p += 1;
                        } else {
                            body << "        DC.B     " << lit << "\n";
                            p += static_cast<Address>(used);
                        }
                        break;
                    }
                }
                if (p < end && names.count(p)) emitHeader(p);
            }
            a = end;
            continue;
        }

        if (const VariableDoc* v = ann.variableAt(a)) {
            const Address end = std::min<Address>(a + v->size, to);
            const unsigned size = end - a;
            std::string line;
            if (size == 2 && !(a & 1U)) line = "        DC.W     " + hexv(view.read16(a), 4);
            else if (size == 4 && !(a & 1U)) line = "        DC.L     " + hexv(view.read32(a), 8);
            else if (size == 1) line = "        DC.B     " + hexv(view.read8(a), 2);
            else line = "";
            if (!line.empty()) {
                body << withComment(line, v->comment) << "\n";
            } else {
                if (!v->comment.empty()) emitComment(v->comment);
                for (Address p = a; p < end; p += 8) body << dcBytes(p, std::min<std::size_t>(8, end - p)) << "\n";
            }
            a = end;
            continue;
        }

        if (analysis.at(a)) {
            const Instruction in = decode(view, a, dopt);
            std::string text = in.operands.empty() ? in.mnemonic : pad(in.mnemonic, 8) + " " + in.operands;
            std::string comment;
            if (opt.addresses) comment += hexv(a, 6);
            if (const std::string* c = ann.commentAt(a)) comment += (comment.empty() ? "" : " ") + *c;
            // an immediate that equals a known address is probably a pointer: say so (comment only)
            const std::size_t hash = in.operands.find("#$");
            if (hash != std::string::npos && in.operands.size() >= hash + 10) {
                const std::string digits = in.operands.substr(hash + 2, 8);
                if (digits.find_first_not_of("0123456789ABCDEF") == std::string::npos) {
                    const Long value = static_cast<Long>(std::stoul(digits, nullptr, 16));
                    if (value == (value & kAddressMask) && value != 0) {
                        const std::string s = symbolize(value);
                        if (!s.empty()) comment += (comment.empty() ? "" : "  ") + std::string("[#imm = ") + s + "]";
                    }
                }
            }
            body << withComment("        " + text, comment) << "\n";
            ++result.instructions;
            if (ann.commentAt(a)) ++result.userComments;
            a += static_cast<Address>(in.length());
            continue;
        }

        // bytes that are neither code nor annotated data: text, zero runs, or plain bytes
        Address end = a + 1;
        while (end < to && !isBoundary(end) && end - a < 8) ++end;
        // a run of 16 or more zero bytes becomes DS.B: end this line where such a run starts
        for (Address i = a + 1; i < end; ++i) {
            bool run = i + 16 <= to;
            for (Address k = i; run && k < i + 16; ++k) run = view.read8(k) == 0 && (k == i || !isBoundary(k)) && !analysis.at(k);
            if (run) {
                end = i;
                break;
            }
        }
        std::size_t used = 0;
        const std::string lit = textLine(a, 60, used);
        if (used >= 4) {
            // the run must not swallow a label or code that starts inside it
            std::size_t n = used;
            for (std::size_t i = 1; i < n; ++i)
                if (isBoundary(a + static_cast<Address>(i))) {
                    n = i;
                    break;
                }
            if (n >= 4) {
                std::size_t u2 = 0;
                std::string text = textLine(a, n, u2);
                std::string line = "        DC.B     " + text;
                Address next = a + static_cast<Address>(u2);
                if (next < to && view.read8(next) == 0 && !isBoundary(next)) {
                    line += ",0";
                    ++next;
                }
                body << line << "\n";
                a = next;
                continue;
            }
        }
        std::size_t zeros = 0;
        while (a + zeros < to && view.read8(a + static_cast<Address>(zeros)) == 0 && (zeros == 0 || !isBoundary(a + static_cast<Address>(zeros))) &&
               !analysis.at(a + static_cast<Address>(zeros)))
            ++zeros;
        if (zeros >= 16) {
            body << "        DS.B     " << zeros << "\n";
            a += static_cast<Address>(zeros);
            continue;
        }
        body << dcBytes(a, end - a) << "\n";
        a = end;
    }

    // ---- assemble the pieces ----
    std::ostringstream head;
    if (!ann.title().empty()) head << "; " << ann.title() << "\n";
    head << "; MC68000 listing " << hexv(from, 6) << " - " << hexv(to - 1, 6) << (cut ? " (cut)" : "") << "\n";
    std::map<Address, std::string> equ = equNames;
    for (const auto& [addr, n] : usedEqu) equ[addr] = n;
    // a label that ended up inside a variable or a data line could not be written on a line of its own:
    // it is defined by value instead, so every name that is used also exists
    if (!cut)
        for (const auto& [addr, n] : names)
            if (!emittedLabels.count(addr)) equ[addr] = n;
    std::string source;
    {
        std::ostringstream s;
        for (const auto& [addr, n] : equ) {
            if (emittedLabels.count(addr) && names[addr] == n) continue;  // defined by a line label
            s << n << " EQU " << hexv(addr, 6) << "\n";
        }
        s << "        ORG      " << hexv(from, 6) << "\n";
        s << body.str();
        source = s.str();
    }
    for (const Address p : placeholders) {
        (void)p;
        ++result.unnamed;
    }
    result.truncated = cut;

    if (opt.verify && !cut) {
        const AssemblyResult r = assemble(source);
        if (!r.ok()) {
            result.problem = "the listing does not assemble: line " + std::to_string(r.errors[0].line) + ": " + r.errors[0].message;
        } else {
            const std::vector<Byte> got = r.flatten();
            std::vector<Byte> want;
            want.reserve(to - from);
            for (Address x = from; x < to; ++x) want.push_back(view.read8(x));
            if (r.lowest() != from) result.problem = "the listing starts at the wrong address";
            else if (got != want) result.problem = "the assembled bytes differ from the memory";
            else result.verified = true;
        }
    }
    head << "; " << (result.verified ? "VERIFIED: assembled again, identical bytes" : cut ? "not verified (cut)" : opt.verify ? "NOT VERIFIED: " + result.problem : "not verified (not requested)") << "\n";
    result.text = head.str() + source;
    return result;
}

}  // namespace m68k
