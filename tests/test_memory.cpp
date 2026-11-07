#include "m68k/memory.hpp"

#include <array>

#include "test_util.hpp"

using namespace m68k;

int main() {
    Memory mem;
    CHECK_EQ(mem.mappedPages(), std::size_t{0});
    CHECK_EQ(mem.read8(0x123456), Byte{0});     // untouched memory reads as 0
    CHECK_EQ(mem.mappedPages(), std::size_t{0});  // reading does not allocate

    // big-endian composition
    mem.write32(0x1000, 0x11223344);
    CHECK_EQ(mem.read8(0x1000), Byte{0x11});
    CHECK_EQ(mem.read8(0x1001), Byte{0x22});
    CHECK_EQ(mem.read8(0x1002), Byte{0x33});
    CHECK_EQ(mem.read8(0x1003), Byte{0x44});
    CHECK_EQ(mem.read16(0x1000), Word{0x1122});
    CHECK_EQ(mem.read16(0x1002), Word{0x3344});
    CHECK_EQ(mem.read32(0x1000), Long{0x11223344});
    mem.write16(0x2000, 0xABCD);
    CHECK_EQ(mem.read8(0x2000), Byte{0xAB});
    CHECK_EQ(mem.read8(0x2001), Byte{0xCD});

    // only 24 address bits are significant
    mem.write8(0xFF000010, 0x5A);
    CHECK_EQ(mem.read8(0x00000010), Byte{0x5A});
    CHECK_EQ(mem.read8(0x12000010), Byte{0x5A});

    // accesses wrap at 16 MiB
    mem.write32(0xFFFFFE, 0xDEADBEEF);
    CHECK_EQ(mem.read16(0xFFFFFE), Word{0xDEAD});
    CHECK_EQ(mem.read16(0x000000), Word{0xBEEF});

    // pages are allocated on write (0x1000/0x2000 -> page 0, 0xFFFFFE -> page 255)
    CHECK_EQ(mem.mappedPages(), std::size_t{2});

    // load / readBytes
    const std::array<Byte, 4> code{0x4E, 0x75, 0x4E, 0x71};  // RTS, NOP
    mem.load(0x4000, code);
    CHECK_EQ(mem.read16(0x4000), Word{0x4E75});
    CHECK_EQ(mem.read16(0x4002), Word{0x4E71});
    CHECK(mem.readBytes(0x4000, 4) == std::vector<Byte>(code.begin(), code.end()));

    // the memory is copyable (used for checkpoints / snapshots later)
    Memory copy = mem;
    copy.write8(0x4000, 0);
    CHECK_EQ(mem.read8(0x4000), Byte{0x4E});
    CHECK_EQ(copy.read8(0x4000), Byte{0});

    mem.clear();
    CHECK_EQ(mem.read32(0x1000), Long{0});
    CHECK_EQ(mem.mappedPages(), std::size_t{0});

    CHECK(Memory::isAligned(0x1000));
    CHECK(!Memory::isAligned(0x1001));

    // ---- which bytes were written -------------------------------------------------------------------------------------------------------------------
    {
        Memory m;
        m.write8(0x100, 1);                          // before tracking: not recorded
        CHECK(!m.tracking());
        CHECK(!m.written(0x100));
        m.trackWrites(true);
        CHECK(m.tracking());
        CHECK_EQ(m.writtenBytes(), std::size_t{0});
        m.write8(0x2000, 7);
        m.write16(0x2001, 0x1234);                    // 0x2001 and 0x2002
        m.write32(0x3000, 0xDEADBEEF);
        m.write8(0x3000, 0xAA);                       // the same byte again does not count twice
        CHECK(m.written(0x2000) && m.written(0x2001) && m.written(0x2002) && !m.written(0x2003));
        CHECK_EQ(m.writtenBytes(), std::size_t{7});
        m.write8(0x1000000 + 0x10, 5);                // the address wraps at 16 MiB
        CHECK(m.written(0x10));
        const auto ranges = m.writtenRanges();
        CHECK_EQ(ranges.size(), std::size_t{3});
        CHECK(ranges[0] == std::make_pair(Address{0x10}, Address{0x11}));
        CHECK(ranges[1] == std::make_pair(Address{0x2000}, Address{0x2003}));
        CHECK(ranges[2] == std::make_pair(Address{0x3000}, Address{0x3004}));
        CHECK_EQ(m.writtenRanges(3).size(), std::size_t{2});     // at least 3 consecutive bytes
        m.write8(0xFFFFFF, 1);                        // the last byte of the address space ends a range at 16 MiB
        CHECK(m.writtenRanges().back() == std::make_pair(Address{0xFFFFFF}, Address{0x1000000}));
        m.clearWritten();
        CHECK_EQ(m.writtenBytes(), std::size_t{0});
        CHECK(!m.written(0x2000));
        CHECK(m.read8(0x2000) == 7);                  // the memory itself is untouched
        m.write8(0x4000, 1);
        m.clear();                                    // clearing the memory forgets what was written, too
        CHECK_EQ(m.writtenBytes(), std::size_t{0});
        CHECK(m.tracking());
        m.trackWrites(false);
        CHECK(!m.tracking());
        m.write8(0x5000, 1);
        CHECK(!m.written(0x5000));
        CHECK(m.writtenRanges().empty());
    }

    return finish("memory");
}
