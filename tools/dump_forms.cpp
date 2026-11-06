// m68k_dump_forms - prints the decoder's view of all 65536 first opcode words.
//
//   m68k_dump_forms [pattern]
//
// Every word is decoded with a fixed set of following extension words (selected by `pattern`).
// Output, one line per word:  <opcode hex> <valid 0|1> <length in bytes> <mnemonic> <operands>
// tools/crosscheck_objdump.py compares this with GNU objdump (an independent decoder).

#include <cstdlib>
#include <iostream>

#include "m68k/decoder.hpp"

namespace {
// Four extension words per pattern. Pattern 0 contains only "canonical" words (see decode()).
const m68k::Word kPatterns[][4] = {
    {0x0010, 0x0020, 0x0030, 0x0040},
    {0x0102, 0x1234, 0x5678, 0x9ABC},
    {0xFFF0, 0x8812, 0x0004, 0x0002},
    {0x0001, 0x0002, 0x0003, 0x0004},
};
}  // namespace

int main(int argc, char** argv) {
    const int pattern = argc > 1 ? std::atoi(argv[1]) : 0;
    if (pattern < 0 || pattern >= 4) return 2;
    // Word w sits at kBase + 32 * w, exactly where tools/crosscheck_objdump.py puts it.
    constexpr m68k::Address kBase = 0x400000;
    for (unsigned w = 0; w < 0x10000; ++w) {
        m68k::Memory mem;
        const m68k::Address at = kBase + 32 * w;
        mem.write16(at, static_cast<m68k::Word>(w));
        for (int i = 0; i < 4; ++i) mem.write16(at + 2 + 2 * static_cast<unsigned>(i), kPatterns[pattern][i]);
        const m68k::Instruction in = m68k::decode(mem, at);
        std::cout << m68k::hexDigits(w, 4) << ' ' << (in.valid ? 1 : 0) << ' ' << in.length() << ' ' << in.mnemonic
                  << ' ' << (in.operands.empty() ? "-" : in.operands) << '\n';
    }
    return 0;
}
