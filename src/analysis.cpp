#include "m68k/analysis.hpp"

#include <algorithm>
#include <cstdlib>

#include "m68k/decoder.hpp"

namespace m68k {

Analysis::Analysis(const Memory& view, Address from, Address to)
    : mem_(view), from_(from & kAddressMask), to_(std::min<Address>(to, kAddressMask + 1U)) {
    if (to_ < from_) to_ = from_;
    state_.assign(to_ - from_, 0);
}

void Analysis::addEntry(Address a) { entries_.insert(a & kAddressMask); }

const Instruction* Analysis::at(Address a) const {
    const auto it = code_.find(a & kAddressMask);
    return it == code_.end() ? nullptr : &it->second;
}

bool Analysis::isCode(Address a) const {
    a &= kAddressMask;
    return inRange(a) && (state_[a - from_] == 1 || state_[a - from_] == 2);
}

bool Analysis::isTableData(Address a) const {
    a &= kAddressMask;
    return inRange(a) && state_[a - from_] == 3;
}

std::size_t Analysis::codeBytes() const {
    return static_cast<std::size_t>(std::count_if(state_.begin(), state_.end(), [](std::uint8_t s) { return s == 1 || s == 2; }));
}

bool Analysis::place(Address a, const Instruction& in) {
    const std::size_t len = in.length();
    for (std::size_t i = 0; i < len; ++i) {
        if (state_[a - from_ + i] != 0) return false;
        if (isData_ && isData_(static_cast<Address>(a + i))) return false;
    }
    state_[a - from_] = 1;
    for (std::size_t i = 1; i < len; ++i) state_[a - from_ + i] = 2;
    code_[a] = in;
    return true;
}

void Analysis::note(Address from, const Instruction& in, std::vector<Address>* work) {
    if (in.target) {
        if (in.flow == FlowKind::Call) {
            callTargets_.insert(*in.target);
            callers_[*in.target].insert(from);
        } else {
            jumpTargets_.insert(*in.target);
        }
        if (work && inRange(*in.target)) work->push_back(*in.target);
    }
    if (in.reference) refs_.insert(*in.reference);
}

void Analysis::follow(Address a, std::vector<Address>& work) {
    while (inRange(a) && !(a & 1U)) {
        if (state_[a - from_] != 0) return;
        if (isData_ && isData_(a)) return;
        const Instruction in = decode(mem_, a);
        if (!in.valid || a + in.length() > to_) return;
        if (!place(a, in)) return;
        note(a, in, &work);
        detectJumpTable(a, in, work);
        if (in.flow == FlowKind::Jump || in.flow == FlowKind::Return) return;
        a += static_cast<Address>(in.length());
    }
}

void Analysis::analyzeFlow() {
    std::vector<Address> work(entries_.rbegin(), entries_.rend());
    while (!work.empty()) {
        const Address a = work.back();
        work.pop_back();
        follow(a, work);
    }
    scanPointerTables();
}

namespace {
std::int32_t sext8(unsigned v) { return static_cast<std::int8_t>(v & 0xFF); }
std::int32_t sext16(unsigned v) { return static_cast<std::int16_t>(v & 0xFFFF); }
}  // namespace

void Analysis::addPointerTable(Address table, std::size_t bytes) {
    table &= kAddressMask;
    JumpTable jt;
    jt.kind = JumpTable::Kind::Pointers;
    jt.table = table;
    jt.bytes = static_cast<unsigned>(bytes);
    for (std::size_t off = 0; off + 4 <= bytes; off += 4) {
        const Address slot = table + static_cast<Address>(off);
        if (!inRange(slot) || !inRange(slot + 3)) break;
        const Long v = mem_.read32(slot);
        const Address t = v & kAddressMask;
        jt.slots.push_back(slot);
        jt.targets.push_back(t);
        if (v == t && inRange(t) && !(t & 1U)) {
            entries_.insert(t);
            jumpTargets_.insert(t);
        }
    }
    if (!jt.slots.empty()) tables_.push_back(std::move(jt));
}

// Jump tables behind JMP/JSR (d8,PC,Xn). Both patterns are tied to the jump instruction itself, so false positives need a very specific
// byte pattern; a table needs at least two entries.
void Analysis::detectJumpTable(Address a, const Instruction& in, std::vector<Address>& work) {
    if (in.bytes.size() != 4) return;
    const unsigned op = (static_cast<unsigned>(in.bytes[0]) << 8) | in.bytes[1];
    if (op != 0x4EFB && op != 0x4EBB) return;  // JMP / JSR (d8,PC,Xn)
    const unsigned ext = (static_cast<unsigned>(in.bytes[2]) << 8) | in.bytes[3];
    const Address base = static_cast<Address>(a + 2 + sext8(ext)) & kAddressMask;

    // (A) a branch table: the entries are BRA / JMP instructions
    {
        JumpTable jt;
        jt.kind = JumpTable::Kind::Branch;
        jt.jump = a;
        jt.table = base;
        Address p = base;
        while (inRange(p) && !(p & 1U) && jt.slots.size() < 1024) {
            if (state_[p - from_] == 2 || state_[p - from_] == 3) break;   // inside an instruction or table data (an entry that is code already is fine)
            if (isData_ && isData_(p)) break;
            const Instruction e = decode(mem_, p);
            const bool jumpEntry = e.valid && e.flow == FlowKind::Jump && e.target &&
                                   (e.mnemonic.rfind("BRA", 0) == 0 || (e.mnemonic == "JMP" && e.length() == 6));
            if (!jumpEntry || p + e.length() > to_) break;
            jt.slots.push_back(p);
            jt.targets.push_back(*e.target);
            p += static_cast<Address>(e.length());
        }
        if (jt.slots.size() >= 2) {
            jt.bytes = p - base;
            for (const Address slot : jt.slots) work.push_back(slot);
            for (const Address t : jt.targets) jumpTargets_.insert(t);
            refs_.insert(base);
            tables_.push_back(std::move(jt));
            return;
        }
    }

    // (B) an offset table: MOVE.W tab(PC,Dn),Dm ; JMP base(PC,Dm)   (at most one instruction in between)
    const unsigned jumpIndex = (ext >> 12) & 0xF;   // data register number if bit 15 is clear
    if (ext & 0x8000U) return;
    Address next = a;
    auto it = code_.find(a);
    for (int back = 0; back < 2 && it != code_.begin(); ++back) {
        --it;
        const Instruction& pv = it->second;
        if (it->first + pv.length() != next) break;
        next = it->first;
        if (pv.bytes.size() != 4) continue;
        const unsigned pop = (static_cast<unsigned>(pv.bytes[0]) << 8) | pv.bytes[1];
        if ((pop & 0xF1FF) != 0x303B || ((pop >> 9) & 7U) != jumpIndex) continue;   // MOVE.W (d8,PC,Xn),Dm
        const unsigned pext = (static_cast<unsigned>(pv.bytes[2]) << 8) | pv.bytes[3];
        const Address table = static_cast<Address>(it->first + 2 + sext8(pext)) & kAddressMask;
        JumpTable jt;
        jt.kind = JumpTable::Kind::Offset;
        jt.jump = a;
        jt.table = table;
        jt.base = base;
        Address minTarget = 0xFFFFFFFF;
        for (Address slot = table; jt.slots.size() < 512; slot += 2) {
            if (!inRange(slot) || !inRange(slot + 1) || state_[slot - from_] != 0 || state_[slot + 1 - from_] != 0) break;
            if (slot >= minTarget) break;   // the table ends where the first target begins
            if (isData_ && (isData_(slot) || isData_(slot + 1))) break;
            const Address target = static_cast<Address>(base + sext16(mem_.read16(slot))) & kAddressMask;
            if (!inRange(target) || (target & 1U) || (isData_ && isData_(target))) break;
            if (!decode(mem_, target).valid) break;
            jt.slots.push_back(slot);
            jt.targets.push_back(target);
            minTarget = std::min(minTarget, target);
        }
        if (jt.slots.size() < 2) continue;
        jt.bytes = static_cast<unsigned>(jt.slots.size() * 2);
        for (unsigned i = 0; i < jt.bytes; ++i) state_[table - from_ + i] = 3;
        for (const Address t : jt.targets) {
            work.push_back(t);
            jumpTargets_.insert(t);
        }
        refs_.insert(table);
        tables_.push_back(std::move(jt));
        return;
    }
}

// Rows of longword addresses of code that data references point at (LEA tab(PC),A0 ...): reported as suggestions, not followed,
// because a row of numbers that look like addresses can also be plain data.
void Analysis::scanPointerTables() {
    std::set<Address> covered;
    for (const JumpTable& t : tables_)
        for (Address b = t.table; b < t.table + t.bytes; ++b) covered.insert(b);
    for (const Address r : refs_) {
        if (!inRange(r) || (r & 1U) || state_[r - from_] != 0 || covered.count(r)) continue;
        JumpTable jt;
        jt.kind = JumpTable::Kind::Pointers;
        jt.table = r;
        jt.followed = false;
        for (Address p = r; p + 4 <= to_ && jt.slots.size() < 256; p += 4) {
            if (state_[p - from_] != 0 || state_[p + 3 - from_] != 0 || covered.count(p)) break;
            if (p != r && refs_.count(p)) break;   // another table starts here
            const Long v = mem_.read32(p);
            const Address t = v & kAddressMask;
            if (v != t || !inRange(t) || (t & 1U) || (isData_ && isData_(t))) break;
            const Instruction first = decode(mem_, t);
            if (!first.valid || (first.mnemonic == "ORI.B" && first.operands == "#$00,D0")) break;   // not zero words
            jt.slots.push_back(p);
            jt.targets.push_back(t);
        }
        if (jt.slots.size() < 3) continue;
        jt.bytes = static_cast<unsigned>(jt.slots.size() * 4);
        tables_.push_back(std::move(jt));
    }
}

void Analysis::analyzeLinear(Address from, Address to) {
    Address a = std::max(from & kAddressMask, from_);
    if (a & 1U) ++a;
    const Address end = std::min(to, to_);
    while (a < end) {
        const Instruction in = decode(mem_, a);
        if (!in.valid || a + in.length() > end || !place(a, in)) {
            a += 2;
            continue;
        }
        note(a, in, nullptr);
        a += static_cast<Address>(in.length());
    }
}

namespace {

// "-$228(A6)" -> -552
bool libraryOffset(const Instruction& in, int& offset) {
    const std::string& s = in.operands;
    if (in.mnemonic != "JSR" || s.size() < 8 || s.compare(s.size() - 4, 4, "(A6)") != 0 || s.compare(0, 2, "-$") != 0) return false;
    char* end = nullptr;
    const std::string hexPart = s.substr(2, s.size() - 6);
    const long v = std::strtol(hexPart.c_str(), &end, 16);
    if (!end || *end != '\0' || v <= 0 || v > 0x8000) return false;
    offset = -static_cast<int>(v);
    return true;
}

}  // namespace

std::vector<RoutineInfo> Analysis::routines() const {
    std::set<Address> entries;
    for (const Address a : entries_)
        if (at(a)) entries.insert(a);
    for (const Address a : callTargets_)
        if (at(a)) entries.insert(a);

    std::vector<RoutineInfo> out;
    for (const Address entry : entries) {
        RoutineInfo r;
        r.entry = entry;
        r.start = entry;
        r.end = entry;
        std::set<Address> seen;
        std::vector<Address> stack{entry};
        std::set<Address> callees, external;
        while (!stack.empty()) {
            const Address a = stack.back();
            stack.pop_back();
            if (!seen.insert(a).second) continue;
            const Instruction* in = at(a);
            if (!in) continue;
            ++r.instructions;
            r.bytes += static_cast<unsigned>(in->length());
            r.start = std::min(r.start, a);
            r.end = std::max(r.end, static_cast<Address>(a + in->length()));
            int off = 0;
            if (in->flow == FlowKind::Call) {
                if (in->target) {
                    if (at(*in->target)) callees.insert(*in->target);
                    else external.insert(*in->target);
                }
            }
            if (libraryOffset(*in, off)) r.libraryCalls.push_back({a, off});
            const Address next = static_cast<Address>(a + in->length());
            switch (in->flow) {
                case FlowKind::Normal:
                case FlowKind::Call: stack.push_back(next); break;
                case FlowKind::CondJump:
                    stack.push_back(next);
                    if (in->target) stack.push_back(*in->target);
                    break;
                case FlowKind::Jump:
                    if (in->target) stack.push_back(*in->target);
                    break;
                case FlowKind::Return: break;
            }
        }
        r.callees.assign(callees.begin(), callees.end());
        r.externalCalls.assign(external.begin(), external.end());
        if (const auto it = callers_.find(entry); it != callers_.end()) r.callers.assign(it->second.begin(), it->second.end());
        out.push_back(std::move(r));
    }

    // depth over the call graph (cycles are cut by treating a routine under evaluation as depth 0)
    std::map<Address, int> depth;
    std::map<Address, const RoutineInfo*> byEntry;
    for (const RoutineInfo& r : out) byEntry[r.entry] = &r;
    std::set<Address> active;
    std::function<int(Address)> eval = [&](Address e) -> int {
        if (const auto it = depth.find(e); it != depth.end()) return it->second;
        if (!active.insert(e).second) return 0;
        int d = 0;
        for (const Address c : byEntry[e]->callees)
            if (byEntry.count(c) && c != e) d = std::max(d, 1 + eval(c));
        active.erase(e);
        depth[e] = d;
        return d;
    };
    for (RoutineInfo& r : out) r.depth = eval(r.entry);
    return out;
}

}  // namespace m68k
