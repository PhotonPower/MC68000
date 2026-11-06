// Exhaustive consistency check of decoder and assembler: every one of the 65536 first opcode words,
// combined with several sets of extension words, is decoded; what the decoder accepts as an
// instruction must assemble (through the complete assembler) to exactly the same bytes.
// The decoder itself is checked against GNU objdump by tools/crosscheck_objdump.py.

#include <array>

#include "m68k/assembler.hpp"
#include "m68k/decoder.hpp"
#include "test_util.hpp"

using namespace m68k;

int main() {
    const std::array<std::array<Word, 4>, 6> patterns = {{
        {0x0010, 0x0020, 0x0030, 0x0040},
        {0x0100, 0x8000, 0x0042, 0x007E},
        {0x1004, 0x0002, 0xFFF0, 0x0004},
        {0x0001, 0x0002, 0x0003, 0x0004},
        {0xFFF0, 0x8812, 0x0004, 0x0002},
        {0x0000, 0x0000, 0x0000, 0x0000},
    }};
    constexpr Address kBase = 0x400000;  // far from 0 so that backward targets never wrap

    std::size_t valid = 0, failures = 0;
    for (const auto& pattern : patterns) {
        for (unsigned w = 0; w < 0x10000; ++w) {
            Memory mem;
            mem.write16(kBase, static_cast<Word>(w));
            for (std::size_t i = 0; i < 4; ++i) mem.write16(kBase + 2 + 2 * static_cast<Address>(i), pattern[i]);
            const Instruction in = decode(mem, kBase);
            if (!in.valid) continue;
            ++valid;
            AssemblerOptions opt;
            opt.origin = kBase;
            const AssemblyResult r = assemble("  " + in.text() + "\n", opt);
            const bool same = r.ok() && r.flatten() == in.bytes;
            CHECK(same);
            if (!same && ++failures <= 10) {
                std::cerr << "roundtrip failed for " << hexDigits(w, 4) << ": '" << in.text() << "' -> "
                          << (r.ok() ? "different bytes" : r.errors[0].message) << "\n";
            }
        }
    }
    CHECK(valid > 200000);
    std::cout << "roundtrip: " << valid << " instructions\n";
    return finish("roundtrip");
}
