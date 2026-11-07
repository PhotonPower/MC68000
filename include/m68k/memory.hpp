#pragma once

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

#include "m68k/types.hpp"

namespace m68k {

/// Sparse 16 MiB MC68000 address space, big-endian.
///
/// - Only the lower 24 address bits are significant (`address & kAddressMask`).
/// - Memory is allocated in 64 KiB pages on the first write; reading an untouched page yields 0.
/// - Multi-byte accesses compose bytes, most significant byte first. `Memory` does *not* raise
///   address errors for odd word/long accesses - that is the CPU's job (see `isAligned`), so
///   that tools such as the disassembler can read freely.
class Memory {
public:
    static constexpr std::size_t kPageSize = 0x10000;
    static constexpr std::size_t kPageCount = kAddressSpace / kPageSize;

    Memory() : pages_(kPageCount) {}

    [[nodiscard]] Byte read8(Address address) const {
        address &= kAddressMask;
        const auto& page = pages_[address / kPageSize];
        return page.empty() ? Byte{0} : page[address % kPageSize];
    }
    [[nodiscard]] Word read16(Address address) const {
        return static_cast<Word>((read8(address) << 8) | read8(address + 1));
    }
    [[nodiscard]] Long read32(Address address) const {
        return (static_cast<Long>(read16(address)) << 16) | read16(address + 2);
    }

    void write8(Address address, Byte value) {
        address &= kAddressMask;
        auto& page = pages_[address / kPageSize];
        if (page.empty()) page.assign(kPageSize, 0);
        page[address % kPageSize] = value;
        if (!written_.empty()) markWritten(address);
    }
    void write16(Address address, Word value) {
        write8(address, static_cast<Byte>(value >> 8));
        write8(address + 1, static_cast<Byte>(value));
    }
    void write32(Address address, Long value) {
        write16(address, static_cast<Word>(value >> 16));
        write16(address + 2, static_cast<Word>(value));
    }

    /// Copies `bytes` to `address`; the address wraps at 16 MiB like the hardware does.
    void load(Address address, std::span<const Byte> bytes) {
        for (Byte b : bytes) write8(address++, b);
    }

    /// Reads `count` bytes starting at `address`.
    [[nodiscard]] std::vector<Byte> readBytes(Address address, std::size_t count) const {
        std::vector<Byte> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) out.push_back(read8(address++));
        return out;
    }

    void clear() {
        pages_.assign(kPageCount, {});
        if (!written_.empty()) clearWritten();
    }

    // ---- which bytes were written (to find the code a self-extracting program has unpacked) ----------------------------------------------------------
    /// Starts (or stops) remembering every byte that is written from now on: one bit per byte, 2 MiB while it is on. Starting clears the record.
    void trackWrites(bool on) {
        if (on) written_.assign(kAddressSpace / 8, 0), writtenBytes_ = 0;
        else written_.clear(), writtenBytes_ = 0;
    }
    [[nodiscard]] bool tracking() const { return !written_.empty(); }
    void clearWritten() {
        std::fill(written_.begin(), written_.end(), Byte{0});
        writtenBytes_ = 0;
    }
    [[nodiscard]] bool written(Address address) const {
        address &= kAddressMask;
        return !written_.empty() && (written_[address >> 3] >> (address & 7U)) & 1U;
    }
    /// Number of different bytes written since tracking started.
    [[nodiscard]] std::size_t writtenBytes() const { return writtenBytes_; }
    /// The run of consecutive written bytes that contains `address` as [first, end), looking at most `cap` bytes up and down (so a run of `cap` bytes or
    /// more is cut off at that size: enough to tell that it is large). Empty ({a, a}) if the byte was not written.
    [[nodiscard]] std::pair<Address, Address> writtenRunAround(Address address, std::size_t cap) const {
        address &= kAddressMask;
        if (!written(address)) return {address, address};
        Address first = address, end = address + 1;
        while (address - first < cap && first > 0 && written(first - 1)) --first;
        while (end - address <= cap && end < kAddressSpace && written(end)) ++end;
        return {first, end};
    }
    /// The written bytes as ranges [first, end) of at least `minLength` consecutive bytes, in address order.
    [[nodiscard]] std::vector<std::pair<Address, Address>> writtenRanges(std::size_t minLength = 1) const {
        std::vector<std::pair<Address, Address>> out;
        if (written_.empty()) return out;
        Address start = 0;
        bool inside = false;
        for (std::uint32_t a = 0; a <= kAddressSpace; ++a) {
            const bool w = a < kAddressSpace && ((written_[a >> 3] >> (a & 7U)) & 1U);
            if (w && !inside) start = a, inside = true;
            if (!w && inside) {
                if (a - start >= minLength) out.emplace_back(start, a);
                inside = false;
            }
            if (a < kAddressSpace && !w && (a & 7U) == 0 && written_[a >> 3] == 0) a += 7;   // skip a whole empty byte of the bitmap
        }
        return out;
    }

    /// Number of 64 KiB pages that have been written to.
    [[nodiscard]] std::size_t mappedPages() const {
        std::size_t n = 0;
        for (const auto& p : pages_) n += p.empty() ? 0 : 1;
        return n;
    }

    /// Word and long accesses must be at even addresses on a real 68000.
    [[nodiscard]] static constexpr bool isAligned(Address address) { return (address & 1U) == 0; }

private:
    void markWritten(Address address) {
        Byte& b = written_[address >> 3];
        const Byte bit = static_cast<Byte>(1U << (address & 7U));
        if (!(b & bit)) {
            b |= bit;
            ++writtenBytes_;
        }
    }

    std::vector<std::vector<Byte>> pages_;
    std::vector<Byte> written_;       // empty = not tracking
    std::size_t writtenBytes_ = 0;
};

}  // namespace m68k
