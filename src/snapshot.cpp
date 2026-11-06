#include "m68k/snapshot.hpp"

#include <cstring>

namespace m68k {

namespace {

constexpr char kMagic[8] = {'M', '6', '8', 'K', 'S', 'N', 'P', '1'};

class Writer {
public:
    std::vector<Byte> out;
    void u8(unsigned v) { out.push_back(static_cast<Byte>(v)); }
    void u32(std::uint64_t v) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<Byte>(v >> (8 * i)));
    }
    void u64(std::uint64_t v) {
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<Byte>(v >> (8 * i)));
    }
};

class Reader {
public:
    explicit Reader(const std::vector<Byte>& b) : b_(b) {}
    void need(std::size_t n) const {
        if (n > b_.size() - pos_) throw IoError("snapshot: unexpected end of data");
    }
    unsigned u8() {
        need(1);
        return b_[pos_++];
    }
    std::uint32_t u32() {
        need(4);
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | b_[pos_++];
        return v;
    }
    std::uint64_t u64() {
        const std::uint64_t hi = u32();
        return (hi << 32) | u32();
    }
    const Byte* take(std::size_t n) {
        need(n);
        const Byte* p = b_.data() + pos_;
        pos_ += n;
        return p;
    }
    bool atEnd() const { return pos_ == b_.size(); }

private:
    const std::vector<Byte>& b_;
    std::size_t pos_ = 0;
};

}  // namespace

Snapshot capture(const Cpu& cpu) {
    Snapshot s;
    s.cpu = cpu.state();
    s.memory = cpu.memory();
    return s;
}

void restore(Cpu& cpu, const Snapshot& s) {
    cpu.memory() = s.memory;
    cpu.setState(s.cpu);
}

std::vector<Byte> serialize(const Snapshot& s) {
    Writer w;
    for (const char c : kMagic) w.u8(static_cast<unsigned char>(c));
    const CpuState& c = s.cpu;
    for (const Long v : c.regs.d) w.u32(v);
    for (const Long v : c.regs.a) w.u32(v);
    w.u32(c.regs.inactiveSp);
    w.u32(c.regs.pc);
    w.u32(c.regs.sr);
    w.u8(c.halted ? 1 : 0);
    w.u8(c.stopped ? 1 : 0);
    w.u64(c.instructions);
    w.u64(c.exceptions);
    w.u32(c.lastException);
    w.u32(static_cast<std::uint32_t>(c.depth));
    w.u32(c.irqLevel);
    w.u32(static_cast<std::uint32_t>(c.irqVector));
    // pages that contain data
    std::vector<std::size_t> pages;
    for (std::size_t p = 0; p < Memory::kPageCount; ++p) {
        const std::vector<Byte> bytes = s.memory.readBytes(static_cast<Address>(p * Memory::kPageSize), Memory::kPageSize);
        for (const Byte b : bytes) {
            if (b) {
                pages.push_back(p);
                break;
            }
        }
    }
    w.u32(pages.size());
    for (const std::size_t p : pages) {
        w.u32(p);
        for (const Byte b : s.memory.readBytes(static_cast<Address>(p * Memory::kPageSize), Memory::kPageSize)) w.u8(b);
    }
    return w.out;
}

Snapshot deserializeSnapshot(const std::vector<Byte>& bytes) {
    Reader r(bytes);
    if (std::memcmp(r.take(8), kMagic, 8) != 0) throw IoError("snapshot: not a snapshot file (bad magic)");
    Snapshot s;
    CpuState& c = s.cpu;
    for (Long& v : c.regs.d) v = r.u32();
    for (Long& v : c.regs.a) v = r.u32();
    c.regs.inactiveSp = r.u32();
    c.regs.pc = r.u32() & kAddressMask;
    c.regs.sr = static_cast<Word>(r.u32() & kSrValidBits);
    c.halted = r.u8() != 0;
    c.stopped = r.u8() != 0;
    c.instructions = r.u64();
    c.exceptions = r.u64();
    c.lastException = r.u32();
    c.depth = static_cast<int>(r.u32());
    c.irqLevel = r.u32() & 7U;
    c.irqVector = static_cast<int>(r.u32());
    const std::uint32_t pages = r.u32();
    if (pages > Memory::kPageCount) throw IoError("snapshot: too many memory pages");
    for (std::uint32_t i = 0; i < pages; ++i) {
        const std::uint32_t p = r.u32();
        if (p >= Memory::kPageCount) throw IoError("snapshot: bad page number");
        const Byte* data = r.take(Memory::kPageSize);
        s.memory.load(static_cast<Address>(p * Memory::kPageSize), std::span<const Byte>(data, Memory::kPageSize));
    }
    if (!r.atEnd()) throw IoError("snapshot: trailing data");
    return s;
}

std::vector<MemoryChange> diffMemory(const Memory& before, const Memory& after, std::size_t gap, std::size_t maxRanges,
                                     std::size_t* total) {
    std::vector<MemoryChange> out;
    std::size_t changed = 0;
    MemoryChange cur;
    bool open = false;
    std::size_t lastDiff = 0;
    auto flush = [&] {
        if (open) {
            if (out.size() < maxRanges) out.push_back(std::move(cur));
            cur = MemoryChange{};
            open = false;
        }
    };
    for (std::size_t p = 0; p < Memory::kPageCount; ++p) {
        const Address base = static_cast<Address>(p * Memory::kPageSize);
        const std::vector<Byte> a = before.readBytes(base, Memory::kPageSize);
        const std::vector<Byte> b = after.readBytes(base, Memory::kPageSize);
        if (a == b) {
            if (open && base + Memory::kPageSize - lastDiff > gap + 1) flush();
            continue;
        }
        for (std::size_t i = 0; i < Memory::kPageSize; ++i) {
            const std::size_t addr = base + i;
            if (a[i] == b[i]) continue;
            ++changed;
            if (open && addr - lastDiff > gap + 1) flush();
            if (!open) {
                cur.address = static_cast<Address>(addr);
                open = true;
            } else {
                // fill the unchanged bytes between two changes
                for (std::size_t k = lastDiff + 1; k < addr; ++k) {
                    cur.before.push_back(before.read8(static_cast<Address>(k)));
                    cur.after.push_back(after.read8(static_cast<Address>(k)));
                }
            }
            cur.before.push_back(a[i]);
            cur.after.push_back(b[i]);
            lastDiff = addr;
        }
    }
    flush();
    if (total) *total = changed;
    return out;
}

}  // namespace m68k
