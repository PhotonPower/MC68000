// m68k_diff_musashi - differential test of the emulator core against Musashi (an independent,
// widely used MC68000 emulator).
//
//   m68k_diff_musashi [trials-per-opcode] [seed] [first-opcode] [last-opcode]
//
// For every opcode word (all 65536) and every trial, a random processor state and random extension
// words are set up in both emulators; one instruction is executed in each; then all registers, the
// status register and every byte written to memory are compared. Cases where this core raises an
// address error are skipped (Musashi is built without address-error emulation). See
// tools/run_diff_musashi.sh for building Musashi and running this tool.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <vector>

#include "m68k/cpu.hpp"
#include "m68k/decoder.hpp"

extern "C" {
#include "m68k.h"
}

namespace {

std::vector<unsigned char> g_ref(0x1000000);          // Musashi's memory
std::vector<std::pair<unsigned, unsigned char>> g_refWrites;  // address, previous value (for restoring)
std::vector<unsigned> g_refTouched;

}  // namespace

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return g_ref[a & 0xFFFFFF]; }
unsigned int m68k_read_memory_16(unsigned int a) { return (g_ref[a & 0xFFFFFF] << 8) | g_ref[(a + 1) & 0xFFFFFF]; }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    a &= 0xFFFFFF;
    g_refWrites.emplace_back(a, g_ref[a]);
    g_refTouched.push_back(a);
    g_ref[a] = static_cast<unsigned char>(v);
}
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    m68k_write_memory_8(a, v >> 8);
    m68k_write_memory_8(a + 1, v);
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v);
}
void m68k_write_memory_32_pd(unsigned int a, unsigned int v) {  // used for -(An) long writes (high word last)
    m68k_write_memory_16(a + 2, v);
    m68k_write_memory_16(a, v >> 16);
}
}

using namespace m68k;

namespace {

constexpr unsigned kWindow = 0x10000;
constexpr Address kPc = 0x1000;

struct State {
    std::array<Long, 8> d, a;
    Long usp, ssp;
    Word sr;
};

Long randomValue(std::mt19937& rng) {
    static const Long special[] = {0, 1, 2, 7, 8, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
                                   0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFF80, 0xFFFF8000, 0x99, 0x0F, 0x10};
    const unsigned r = rng() % 8;
    if (r < 3) return special[rng() % (sizeof special / sizeof special[0])];
    if (r == 3) return rng() & 0xFF;
    if (r == 4) return rng() & 0xFFFF;
    return static_cast<Long>(rng());
}

Long randomPointer(std::mt19937& rng) {
    // even addresses inside the random window (so that most effective addresses hit RAM)
    return (0x2000 + (rng() % 0xB000)) & ~1U;
}

}  // namespace

int main(int argc, char** argv) {
    const unsigned trials = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 4;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 1;
    const unsigned first = argc > 3 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 0)) : 0;
    const unsigned last = argc > 4 ? static_cast<unsigned>(std::strtoul(argv[4], nullptr, 0)) : 0xFFFF;
    std::mt19937 rng(seed);

    // base image: random bytes in the window, vector table at 0
    std::vector<unsigned char> base(kWindow);
    for (auto& b : base) b = static_cast<unsigned char>(rng());
    for (unsigned v = 0; v < 256; ++v) {
        const unsigned handler = 0x20000 + v * 2;
        base[v * 4] = 0;
        base[v * 4 + 1] = static_cast<unsigned char>(handler >> 16);
        base[v * 4 + 2] = static_cast<unsigned char>(handler >> 8);
        base[v * 4 + 3] = static_cast<unsigned char>(handler);
    }
    std::memcpy(g_ref.data(), base.data(), kWindow);

    Memory mem;
    for (unsigned i = 0; i < kWindow; ++i) mem.write8(i, base[i]);
    Cpu cpu(mem);
    std::vector<std::pair<Address, Size>> ourWrites;
    cpu.accessHook = [&](Address a, Size s, Long, bool write) {
        if (write) ourWrites.emplace_back(a, s);
    };

    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_pulse_reset();

    // warm-up: the first instruction Musashi runs after a reset does not behave like the others
    g_ref[kPc] = 0x4E;
    g_ref[kPc + 1] = 0x71;
    m68k_set_reg(M68K_REG_PC, kPc);
    m68k_execute(1);
    std::uint64_t cases = 0, skipped = 0, failures = 0;
    std::map<std::string, unsigned> failuresByKind;
    for (unsigned op = first; op <= last; ++op) {
        // 68040 MOVE16 ($F620-$F62F) is emulated by Musashi even for the 68000; not MC68000 instructions
        if ((op & 0xFFF0) == 0xF620) continue;
        for (unsigned t = 0; t < trials; ++t) {
            // ---- random state ----
            State st;
            for (auto& d : st.d) d = randomValue(rng);
            for (auto& a : st.a) a = randomPointer(rng);
            if (rng() % 8 == 0) st.a[rng() % 7] = randomValue(rng);  // wild pointers too
            st.usp = randomPointer(rng) + 0x100;
            st.ssp = randomPointer(rng) + 0x100;
            st.sr = static_cast<Word>(((rng() % 4 == 0) ? 0 : 0x2000) | ((rng() % 8) << 8) | (rng() % 32));
            Word words[6];
            words[0] = static_cast<Word>(op);
            for (int i = 1; i < 6; ++i) {
                // extension words: mostly small values so that displacements stay inside the window
                const unsigned r = rng() % 6;
                words[i] = r < 2 ? static_cast<Word>(rng() % 0x40) * 2 : r == 2 ? static_cast<Word>(rng()) : r == 3 ? static_cast<Word>(0xFFF0 + (rng() & 0xE)) : static_cast<Word>(rng() % 0x1000);
                // keep index words free of the 68020 scale bits (the real chip ignores them; the table treats them as data)
            }

            // ---- our emulator ----
            Registers& r = cpu.registers();
            r.sr = 0;
            cpu.setSr(st.sr);
            r.d = st.d;
            r.a = st.a;
            r.a[7] = (st.sr & 0x2000) ? st.ssp : st.usp;
            r.inactiveSp = (st.sr & 0x2000) ? st.usp : st.ssp;
            r.pc = kPc;
            for (int i = 0; i < 6; ++i) mem.write16(kPc + 2 * i, words[i]);
            ourWrites.clear();
            const StepResult sr = cpu.step();
            if (sr.exception == kVecAddressError || sr.reason == StopReason::Halted || sr.reason == StopReason::Stopped) {
                // restore memory and skip
                ++skipped;
                for (const auto& w : ourWrites)
                    for (unsigned k = 0; k < static_cast<unsigned>(w.second); ++k)
                        if (((w.first + k) & 0xFFFFFF) < kWindow) mem.write8(w.first + k, base[(w.first + k) & 0xFFFFFF]);
                    else mem.write8(w.first + k, 0);
                for (int i = 0; i < 6; ++i) mem.write16(kPc + 2 * i, static_cast<Word>((base[kPc + 2 * i] << 8) | base[kPc + 2 * i + 1]));
                if (cpu.halted() || cpu.stopped()) cpu.reset();
                continue;
            }

            // ---- Musashi ----
            for (int i = 0; i < 6; ++i) {
                g_ref[kPc + 2 * i] = static_cast<unsigned char>(words[i] >> 8);
                g_ref[kPc + 2 * i + 1] = static_cast<unsigned char>(words[i]);
            }
            g_refWrites.clear();
            g_refTouched.clear();
            m68k_set_reg(M68K_REG_SR, st.sr);
            m68k_set_reg(M68K_REG_USP, st.usp);
            m68k_set_reg(M68K_REG_ISP, st.ssp);
            for (int i = 0; i < 8; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_D0 + i), st.d[i]);
            for (int i = 0; i < 7; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_A0 + i), st.a[i]);
            m68k_set_reg(M68K_REG_PC, kPc);
            m68k_execute(1);

            // ---- compare ----
            ++cases;
            char what[256] = "";
            auto diff = [&](const char* name, unsigned long long a, unsigned long long b) {
                if (a != b && !what[0]) std::snprintf(what, sizeof what, "%s: ours %llX, musashi %llX", name, a, b);
            };
            for (int i = 0; i < 8; ++i) {
                char n[8];
                std::snprintf(n, sizeof n, "D%d", i);
                diff(n, r.d[static_cast<size_t>(i)], m68k_get_reg(nullptr, static_cast<m68k_register_t>(M68K_REG_D0 + i)));
            }
            for (int i = 0; i < 7; ++i) {
                char n[8];
                std::snprintf(n, sizeof n, "A%d", i);
                diff(n, r.a[static_cast<size_t>(i)], m68k_get_reg(nullptr, static_cast<m68k_register_t>(M68K_REG_A0 + i)));
            }
            diff("A7", r.a[7], m68k_get_reg(nullptr, M68K_REG_SP));
            diff("PC", r.pc, m68k_get_reg(nullptr, M68K_REG_PC) & 0xFFFFFF);
            diff("SR", r.sr, m68k_get_reg(nullptr, M68K_REG_SR) & kSrValidBits);
            diff("USP", r.usp(), m68k_get_reg(nullptr, M68K_REG_USP));
            diff("SSP", r.ssp(), m68k_get_reg(nullptr, M68K_REG_ISP));
            // memory: every byte written by either side
            std::map<Address, int> touched;
            for (const auto& w : ourWrites)
                for (unsigned k = 0; k < static_cast<unsigned>(w.second); ++k) touched[(w.first + k) & 0xFFFFFF] = 1;
            for (const unsigned a : g_refTouched) touched[a] = 1;
            for (const auto& [a, _] : touched) {
                if (mem.read8(a) != g_ref[a] && !what[0])
                    std::snprintf(what, sizeof what, "memory[%06X]: ours %02X, musashi %02X", a, mem.read8(a), g_ref[a]);
            }
            if (what[0]) {
                ++failures;
                std::string kind = what;
                kind = kind.substr(0, kind.find(':'));
                // the instruction text is taken from a clean memory image
                Memory clean;
                for (int i = 0; i < 6; ++i) clean.write16(kPc + 2 * i, words[i]);
                const Instruction in = decode(clean, kPc);
                kind += " " + in.mnemonic.substr(0, in.mnemonic.find('.'));
                if (failuresByKind[kind]++ < 2) {
                    std::printf("MISMATCH opcode %04X (%s) exception=%u  %s\n", op, in.text().c_str(), sr.exception, what);
                    std::printf("   start: SR=%04X D=", st.sr);
                    for (auto d : st.d) std::printf("%X ", d);
                    std::printf(" A=");
                    for (auto a : st.a) std::printf("%X ", a);
                    std::printf(" ext=%04X %04X %04X %04X %04X\n", words[1], words[2], words[3], words[4], words[5]);
                }
            }

            // ---- restore both memories to the base image ----
            for (const auto& w : g_refWrites) g_ref[w.first] = w.second;
            for (const auto& w : ourWrites)
                for (unsigned k = 0; k < static_cast<unsigned>(w.second); ++k) {
                    const Address a = (w.first + k) & 0xFFFFFF;
                    mem.write8(a, a < kWindow ? base[a] : 0);
                }
            for (int i = 0; i < 6; ++i) {
                const unsigned v = (base[kPc + 2 * i] << 8) | base[kPc + 2 * i + 1];
                mem.write16(kPc + 2 * i, static_cast<Word>(v));
                g_ref[kPc + 2 * i] = base[kPc + 2 * i];
                g_ref[kPc + 2 * i + 1] = base[kPc + 2 * i + 1];
            }
        }
    }
    std::printf("%llu cases compared, %llu skipped (address errors / STOP), %llu mismatches\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(skipped),
                static_cast<unsigned long long>(failures));
    for (const auto& [k, n] : failuresByKind) std::printf("   %-16s %u\n", k.c_str(), n);
    return failures ? 1 : 0;
}
