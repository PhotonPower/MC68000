#include "m68k/disassembler.hpp"

#include "m68k/analysis.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "m68k/expression.hpp"

namespace m68k {

Disassembler::Disassembler(const Memory& memory, Address start, Address end)
    : memory_(memory), start_(start & kAddressMask), end_(std::min<Address>(end, kAddressMask + 1U)) {
    if (end_ < start_) end_ = start_;
    state_.assign(end_ - start_, kData);
}

void Disassembler::addEntry(Address a) {
    a &= kAddressMask;
    entries_.insert(a);
    if (inRange(a)) labels_.insert(a);
}
void Disassembler::addSymbol(Address a, const std::string& name) {
    a &= kAddressMask;
    symbols_[a] = name;
    if (inRange(a)) labels_.insert(a);
}
void Disassembler::addComment(Address a, const std::string& text) {
    std::string& c = comments_[a & kAddressMask];
    c += c.empty() ? text : " / " + text;
}

std::size_t Disassembler::codeBytes() const {
    return static_cast<std::size_t>(std::count_if(state_.begin(), state_.end(), [](std::uint8_t s) { return s != kData; }));
}

bool Disassembler::mark(Address a, const Instruction& in) {
    const std::size_t len = in.length();
    for (std::size_t i = 0; i < len; ++i)
        if (state_[a - start_ + i] != kData) return false;
    state_[a - start_] = kStart;
    for (std::size_t i = 1; i < len; ++i) state_[a - start_ + i] = kCont;
    return true;
}

void Disassembler::noteTargets(Address from, const Instruction& in, std::vector<Address>* work) {
    if (in.target) {
        xrefs_[*in.target].insert(from);
        if (inRange(*in.target)) {
            labels_.insert(*in.target);
            if (work) work->push_back(*in.target);
        }
    }
    if (in.reference) {
        xrefs_[*in.reference].insert(from);
        if (inRange(*in.reference)) labels_.insert(*in.reference);
    }
}

void Disassembler::follow(Address a, std::vector<Address>& work) {
    while (inRange(a) && !(a & 1U)) {
        if (state_[a - start_] != kData) return;  // already analysed (or a clash with other code)
        const Instruction in = decode(memory_, a);
        if (!in.valid || a + in.length() > end_) return;
        if (!mark(a, in)) return;
        noteTargets(a, in, &work);
        if (in.flow == FlowKind::Jump || in.flow == FlowKind::Return) return;
        a += static_cast<Address>(in.length());
    }
}

void Disassembler::analyzeFlow() {
    std::vector<Address> work(entries_.rbegin(), entries_.rend());
    auto drain = [&] {
        while (!work.empty()) {
            const Address a = work.back();
            work.pop_back();
            follow(a, work);
        }
    };
    drain();
    // A jump through a table (JMP/JSR d(PC,Dn)) has no direct target. The code flow analysis recognises the table (docs/reverse-engineering.md): the cases
    // of a branch or offset table are code that nothing calls, and the entries of a branch table are code themselves.
    Analysis analysis(memory_, start_, end_);
    for (const Address e : entries_) analysis.addEntry(e);
    analysis.analyzeFlow();
    for (const JumpTable& table : analysis.jumpTables()) {
        if (!table.followed) continue;
        if (inRange(table.table)) labels_.insert(table.table);
        if (table.kind == JumpTable::Kind::Branch)
            for (const Address slot : table.slots)
                if (inRange(slot)) work.push_back(slot);
        for (const Address target : table.targets)
            if (inRange(target)) {
                labels_.insert(target);
                work.push_back(target);
            }
    }
    drain();
}

void Disassembler::analyzeLinear() {
    Address a = start_;
    if (a & 1U) ++a;
    while (a < end_) {
        const Instruction in = decode(memory_, a);
        if (a + in.length() > end_) break;
        if (!mark(a, in)) break;
        noteTargets(a, in, nullptr);
        a += static_cast<Address>(in.length());
    }
}

std::string Disassembler::labelName(Address a) const {
    if (auto it = symbols_.find(a); it != symbols_.end()) return it->second;
    return "L_" + hexDigits(a, 6);
}

std::string Disassembler::render(const DisassemblerOptions& opt) const {
    // Collect the lines first: code lines and data lines.
    struct Line {
        Address address;
        bool code;
        std::size_t length;
        Instruction inst;
        std::vector<Byte> data;
    };
    DecodeOptions dopt;
    dopt.style = opt.style;
    dopt.symbolizer = [this](Address a) { return labels_.count(a) ? labelName(a) : std::string(); };

    std::vector<Line> lines;
    std::set<Address> lineStarts;
    for (Address a = start_; a < end_;) {
        Line l{a, false, 0, {}, {}};
        if (state_[a - start_] == kStart) {
            l.code = true;
            l.inst = decode(memory_, a, dopt);
            l.length = l.inst.length();
        } else {
            std::size_t n = 0;
            while (n < 8 && a + n < end_) {
                if (n > 0 && (state_[a - start_ + n] != kData || labels_.count(a + static_cast<Address>(n)))) break;
                l.data.push_back(memory_.read8(a + static_cast<Address>(n)));
                ++n;
            }
            l.length = n;
        }
        lineStarts.insert(a);
        a += static_cast<Address>(l.length);
        lines.push_back(std::move(l));
    }

    std::ostringstream out;
    auto hex = [&](unsigned v, int d) { return formatHex(v, d, opt.style); };
    if (opt.asmSource) {
        out << "; MC68000 disassembly " << hex(start_, 6) << " - " << hex(end_ - (end_ > start_ ? 1 : 0), 6) << "\n";
        // Labels that fall inside an instruction or data line cannot sit on a line: define them by value.
        for (const Address a : labels_)
            if (!lineStarts.count(a)) out << labelName(a) << " EQU " << hex(a, 6) << "\n";
        out << "            ORG     " << hex(start_, 6) << "\n";
    } else {
        out << "; MC68000 disassembly  " << hex(start_, 6) << " - " << hex(end_ - (end_ > start_ ? 1 : 0), 6) << "\n";
    }

    auto padTo = [](std::string s, std::size_t w) {
        if (s.size() < w) s.append(w - s.size(), ' ');
        return s;
    };
    for (const Line& l : lines) {
        if (labels_.count(l.address)) {
            std::string label = labelName(l.address) + ":";
            if (!opt.asmSource && opt.xrefs) {
                if (auto it = xrefs_.find(l.address); it != xrefs_.end() && !it->second.empty()) {
                    std::string refs;
                    int n = 0;
                    for (const Address from : it->second) {
                        if (n++ == 8) {
                            refs += " ...";
                            break;
                        }
                        refs += (refs.empty() ? "" : " ") + hex(from, 6);
                    }
                    label = padTo(label, 24) + "; <- " + refs;
                }
            }
            out << label << "\n";
        }
        std::string text;
        if (l.code) {
            text = padTo(l.inst.mnemonic, 8) + " " + l.inst.operands;
            while (!text.empty() && text.back() == ' ') text.pop_back();
        } else {
            std::string items;
            for (const Byte b : l.data) items += (items.empty() ? "" : ",") + hex(b, 2);
            text = padTo("DC.B", 8) + " " + items;
        }
        std::string comment;
        if (auto it = comments_.find(l.address); it != comments_.end()) comment = it->second;
        if (opt.asmSource) {
            out << "            " << text;
        } else {
            out << hexDigits(l.address, 6) << "  ";
            std::string bytes;
            if (opt.showBytes) {
                const std::vector<Byte> raw = l.code ? l.inst.bytes : l.data;
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    bytes += hexDigits(raw[i], 2);
                    if (i % 2 == 1) bytes += ' ';
                }
            }
            out << padTo(bytes, opt.showBytes ? 22 : 0) << text;
        }
        if (!comment.empty()) out << "  ; " << comment;
        out << "\n";
    }
    return out.str();
}

std::vector<Diagnostic> loadCommentFile(const std::string& text, Disassembler& dis) {
    std::vector<Diagnostic> errors;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::size_t b = 0;
        while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
        if (b >= line.size() || line[b] == ';') continue;
        const std::size_t comma = line.find(',', b);
        if (comma == std::string::npos) {
            errors.push_back({n, "expected: address,\"text\""});
            continue;
        }
        const ExpressionEvaluator ev(nullptr, 0);
        const ExprResult r = ev.evaluate(line.substr(b, comma - b));
        if (!r.ok || r.undefined || r.value < 0 || r.value > static_cast<std::int64_t>(kAddressMask)) {
            errors.push_back({n, "bad address"});
            continue;
        }
        std::string t = line.substr(comma + 1);
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
            std::string u;
            for (std::size_t i = 1; i + 1 < t.size(); ++i) {
                if (t[i] == '"' && i + 2 < t.size() && t[i + 1] == '"') ++i;
                u += t[i];
            }
            t = u;
        }
        dis.addEntry(static_cast<Address>(r.value));
        dis.addComment(static_cast<Address>(r.value), t);
    }
    return errors;
}

}  // namespace m68k
