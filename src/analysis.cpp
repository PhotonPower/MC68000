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
    return inRange(a) && state_[a - from_] != 0;
}

std::size_t Analysis::codeBytes() const {
    return static_cast<std::size_t>(std::count_if(state_.begin(), state_.end(), [](std::uint8_t s) { return s != 0; }));
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
