#pragma once

#include <span>
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

    void clear() { pages_.assign(kPageCount, {}); }

    /// Number of 64 KiB pages that have been written to.
    [[nodiscard]] std::size_t mappedPages() const {
        std::size_t n = 0;
        for (const auto& p : pages_) n += p.empty() ? 0 : 1;
        return n;
    }

    /// Word and long accesses must be at even addresses on a real 68000.
    [[nodiscard]] static constexpr bool isAligned(Address address) { return (address & 1U) == 0; }

private:
    std::vector<std::vector<Byte>> pages_;
};

}  // namespace m68k
