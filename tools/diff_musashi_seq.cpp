// m68k_diff_musashi_seq - differential test of the emulator core against Musashi on SEQUENCES of instructions.
//
//   m68k_diff_musashi_seq [programs] [seed] [only-program-number]   (with a program number: run just that one and print every step)
//
// tools/diff_musashi.cpp checks one instruction at a time from random states. This tool runs short random
// PROGRAMS (2-10 instructions in a row, ~15 % of them from the families whose result depends on earlier ones:
// ADDX/SUBX/NEGX/ABCD/SBCD/NBCD/ROXL/ROXR, MOVE to/from SR/CCR, LINK/UNLK, MOVEM, ...) in both emulators in lockstep and
// compares registers, SR, PC and every byte written after EACH instruction. What that finds beyond the single-instruction test:
// state that has to survive from one instruction to the next (the X flag, the stack pointers when the privilege level changes,
// exception frames that RTE has to take apart again, post-increment/pre-decrement side effects that a later instruction reads).
//
// The vector table points to small handlers: `RTE` for exceptions whose stacked PC is the next instruction (CHK, TRAPV, TRAP #n,
// division by zero) and `ADDQ.L #2,2(A7) ; RTE` for illegal instruction, privilege violation and line A/F (it skips the faulting
// opcode word), so that a program goes on after an exception. Programs in which this core raises an address error are dropped
// (Musashi is built without address-error emulation), as are programs that reach STOP and programs in which the PC gets bits above
// bit 23 (an RTE/RTS/JMP to such an address): Musashi keeps the upper byte of the PC and pushes it again later, this core masks the PC
// to 24 bits, and what the real MC68000 does there is not something this test can decide. Musashi must be built with
// M68K_EMULATE_TRACE=1 (the trace exception is part of what is compared). See tools/run_diff_musashi.sh.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "m68k/cpu.hpp"
#include "m68k/decoder.hpp"

extern "C" {
#include "m68k.h"
}

namespace {

std::vector<unsigned char> g_ref(0x1000000);                    // Musashi's memory
std::vector<std::pair<unsigned, unsigned char>> g_refWrites;    // address, previous value (in order of writing)
std::vector<unsigned> g_refTouched;                             // addresses written since the last clear

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
void m68k_write_memory_32_pd(unsigned int a, unsigned int v) {
    m68k_write_memory_16(a + 2, v);
    m68k_write_memory_16(a, v >> 16);
}
}

using namespace m68k;

namespace {

constexpr unsigned kWindow = 0x10000;
constexpr Address kPc = 0x1000;
constexpr Address kHandlers = 0x20000;
constexpr unsigned kHandlerSize = 8;

std::vector<unsigned char> g_base(kWindow);       // the image every program starts from (random bytes + vector table)
std::vector<unsigned char> g_handlerImage(256 * kHandlerSize);

unsigned char baseByte(unsigned a) {
    a &= 0xFFFFFF;
    if (a < kWindow) return g_base[a];
    if (a >= kHandlers && a < kHandlers + g_handlerImage.size()) return g_handlerImage[a - kHandlers];
    return 0;
}

Long randomValue(std::mt19937& rng) {
    static const Long special[] = {0, 1, 2, 7, 8, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
                                   0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFF80, 0xFFFF8000, 0x99, 0x0F, 0x10};
    const unsigned r = rng() % 8;
    if (r < 3) return special[rng() % (sizeof special / sizeof special[0])];
    if (r == 3) return rng() & 0xFF;
    if (r == 4) return rng() & 0xFFFF;
    return static_cast<Long>(rng());
}

Long randomPointer(std::mt19937& rng) { return (0x2000 + (rng() % 0xB000)) & ~1U; }

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// opcode words of the families whose behaviour depends on earlier instructions
bool preferred(const Instruction& in) {
    static const char* const names[] = {"ADDX", "SUBX", "NEGX", "ABCD", "SBCD", "NBCD", "ROXL", "ROXR", "LINK", "UNLK",
                                        "MOVEM", "RTE", "RTR", "TRAP", "CHK", "MOVE.W SR", "ANDI", "ORI", "EORI", "MOVEP", "TAS", "EXG", "SWAP"};
    for (const char* n : names)
        if (startsWith(in.mnemonic, n)) {
            if ((startsWith(in.mnemonic, "ANDI") || startsWith(in.mnemonic, "ORI") || startsWith(in.mnemonic, "EORI")) &&
                in.operands.find("SR") == std::string::npos && in.operands.find("CCR") == std::string::npos)
                return false;
            return true;
        }
    return startsWith(in.mnemonic, "MOVE") && (in.operands.find("SR") != std::string::npos || in.operands.find("CCR") != std::string::npos ||
                                               in.operands.find("USP") != std::string::npos);
}

struct Program {
    std::vector<Word> words;
    std::vector<std::string> text;
};

}  // namespace

int main(int argc, char** argv) {
    const unsigned programs = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 2000;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 1;
    const long only = argc > 3 ? std::atol(argv[3]) : -1;
    std::mt19937 rng(seed);

    // base image: random bytes in the window, a proper vector table, handlers
    for (auto& b : g_base) b = static_cast<unsigned char>(rng());
    for (unsigned v = 0; v < 256; ++v) {
        const unsigned handler = kHandlers + v * kHandlerSize;
        g_base[v * 4] = 0;
        g_base[v * 4 + 1] = static_cast<unsigned char>(handler >> 16);
        g_base[v * 4 + 2] = static_cast<unsigned char>(handler >> 8);
        g_base[v * 4 + 3] = static_cast<unsigned char>(handler);
        unsigned char* h = &g_handlerImage[v * kHandlerSize];
        const bool skipOpcode = v == 4 || v == 8 || v == 10 || v == 11;
        const unsigned char code[8] = {0x4E, 0x73, 0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71};
        const unsigned char skip[8] = {0x54, 0xAF, 0x00, 0x02, 0x4E, 0x73, 0x4E, 0x71};  // ADDQ.L #2,2(A7) ; RTE
        std::memcpy(h, skipOpcode ? skip : code, 8);
    }
    std::memcpy(g_ref.data(), g_base.data(), kWindow);
    std::memcpy(g_ref.data() + kHandlers, g_handlerImage.data(), g_handlerImage.size());

    Memory mem;
    for (unsigned i = 0; i < kWindow; ++i) mem.write8(i, g_base[i]);
    for (unsigned i = 0; i < g_handlerImage.size(); ++i) mem.write8(kHandlers + i, g_handlerImage[i]);
    Cpu cpu(mem);
    std::vector<std::pair<Address, Size>> ourWrites;
    cpu.accessHook = [&](Address a, Size s, Long, bool write) {
        if (write) ourWrites.emplace_back(a, s);
    };

    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
    m68k_pulse_reset();
    g_ref[kPc] = 0x4E;
    g_ref[kPc + 1] = 0x71;
    m68k_set_reg(M68K_REG_PC, kPc);
    m68k_execute(1);  // warm-up: the first instruction after a reset does not behave like the others

    std::uint64_t compared = 0, abandoned = 0, exceptions = 0, failures = 0, completed = 0, highPc = 0;
    std::map<std::string, unsigned> failuresByKind;

    for (unsigned prog = 0; prog < programs; ++prog) {
        // ---- the program ----
        const unsigned count = 2 + rng() % 9;
        std::vector<Word> words;
        std::vector<std::string> text;
        Memory scratch;
        for (unsigned i = 0; i < count && words.size() < 48; ++i) {
            Word op = 0;
            Instruction in;
            for (int tries = 0; tries < 400; ++tries) {
                op = static_cast<Word>(rng());
                if (op == 0x4E72 || (op & 0xFFF0) == 0xF620) continue;  // STOP; 68040 MOVE16
                const bool wantFamily = rng() % 100 < 15;
                scratch.write16(0x100, op);
                for (int k = 1; k < 6; ++k) scratch.write16(0x100 + 2 * k, 0x0004);
                in = decode(scratch, 0x100);
                if (!wantFamily || preferred(in)) break;
            }
            std::array<Word, 5> ext;
            for (Word& e : ext) {
                const unsigned r = rng() % 6;
                e = r < 2 ? static_cast<Word>(rng() % 0x40) * 2 : r == 2 ? static_cast<Word>(rng()) : r == 3 ? static_cast<Word>(0xFFF0 + (rng() & 0xE)) : static_cast<Word>(rng() % 0x1000);
            }
            if ((op & 0xF000) == 0x6000) {                                 // BRA/BSR/Bcc: small even forward displacement
                const unsigned low = op & 0xFF;
                if (low == 0) ext[0] = static_cast<Word>(2 + 2 * (rng() % 8));
                else if (low != 0xFF) op = static_cast<Word>((op & 0xFF00) | (2 + 2 * (rng() % 7)));
            }
            if ((op & 0xF0F8) == 0x50C8) ext[0] = static_cast<Word>(static_cast<int>(2 * (rng() % 16)) - 8);   // DBcc
            for (int k = 0; k < 5; ++k) scratch.write16(0x100 + 2 * (k + 1), ext[static_cast<size_t>(k)]);
            scratch.write16(0x100, op);
            in = decode(scratch, 0x100);
            const std::size_t len = in.length() / 2;
            words.push_back(op);
            for (std::size_t k = 1; k < len; ++k) words.push_back(ext[k - 1]);
            text.push_back(in.text());
        }

        // ---- the start state ----
        std::array<Long, 8> d, a;
        for (auto& x : d) x = randomValue(rng);
        for (auto& x : a) x = randomPointer(rng);
        if (rng() % 8 == 0) a[rng() % 7] = randomValue(rng);
        const Long usp = randomPointer(rng) + 0x100, ssp = randomPointer(rng) + 0x100;
        const Word sr = static_cast<Word>(((rng() % 4 == 0) ? 0 : 0x2000) | ((rng() % 8) << 8) | (rng() % 32));

        if (only >= 0 && static_cast<long>(prog) != only) continue;
        Registers& r = cpu.registers();
        r.sr = 0;
        cpu.setSr(sr);
        r.d = d;
        r.a = a;
        r.a[7] = (sr & 0x2000) ? ssp : usp;
        r.inactiveSp = (sr & 0x2000) ? usp : ssp;
        r.pc = kPc;
        for (std::size_t i = 0; i < words.size(); ++i) {
            mem.write16(kPc + 2 * static_cast<Address>(i), words[i]);
            g_ref[kPc + 2 * i] = static_cast<unsigned char>(words[i] >> 8);
            g_ref[kPc + 2 * i + 1] = static_cast<unsigned char>(words[i]);
        }
        ourWrites.clear();
        g_refWrites.clear();
        g_refTouched.clear();
        m68k_set_reg(M68K_REG_SR, sr);
        m68k_set_reg(M68K_REG_USP, usp);
        m68k_set_reg(M68K_REG_ISP, ssp);
        for (int i = 0; i < 8; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_D0 + i), d[static_cast<size_t>(i)]);
        for (int i = 0; i < 7; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_A0 + i), a[static_cast<size_t>(i)]);
        m68k_set_reg(M68K_REG_PC, kPc);

        // ---- lockstep: `count` instructions, compare after every one ----
        bool dropped = false, failed = false;
        std::vector<Address> pcs;
        for (unsigned step = 0; step < count && !failed && !dropped; ++step) {
            const std::size_t ourFrom = ourWrites.size();
            const std::size_t refFrom = g_refTouched.size();
            const Address pcBefore = r.pc;
            pcs.push_back(pcBefore);
            const StepResult res = cpu.step();
            if (res.exception == kVecAddressError || res.reason == StopReason::Halted || res.reason == StopReason::Stopped) {
                dropped = true;
                break;
            }
            m68k_execute(1);
            ++compared;
            if (only >= 0)
                std::printf("step %u at %06X %-28s ours: PC=%06X SR=%04X A7=%06X exc=%u | musashi: PC=%06X SR=%04X A7=%06X\n", step + 1, pcBefore,
                            decode(mem, pcBefore).text().c_str(), r.pc, r.sr, r.a[7], res.exception,
                            m68k_get_reg(nullptr, M68K_REG_PC) & 0xFFFFFF, m68k_get_reg(nullptr, M68K_REG_SR) & kSrValidBits,
                            m68k_get_reg(nullptr, M68K_REG_SP));
            if (res.exception != 0) ++exceptions;

            char what[256] = "";
            auto diff = [&](const char* name, unsigned long long x, unsigned long long y) {
                if (x != y && !what[0]) std::snprintf(what, sizeof what, "%s: ours %llX, musashi %llX", name, x, y);
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
            std::map<Address, int> touched;
            for (std::size_t i = ourFrom; i < ourWrites.size(); ++i)
                for (unsigned k = 0; k < static_cast<unsigned>(ourWrites[i].second); ++k) touched[(ourWrites[i].first + k) & 0xFFFFFF] = 1;
            for (std::size_t i = refFrom; i < g_refTouched.size(); ++i) touched[g_refTouched[i]] = 1;
            for (const auto& [addr, _] : touched)
                if (mem.read8(addr) != g_ref[addr] && !what[0])
                    std::snprintf(what, sizeof what, "memory[%06X]: ours %02X, musashi %02X", addr, mem.read8(addr), g_ref[addr]);

            if (!what[0] && (m68k_get_reg(nullptr, M68K_REG_PC) >> 24) != 0) {  // see the comment at the top
                ++highPc;
                dropped = true;
                break;
            }
            if (what[0]) {
                failed = true;
                ++failures;
                std::string kind = what;
                kind = kind.substr(0, kind.find(':'));
                const std::string& t = text[std::min<std::size_t>(step, text.size() - 1)];
                if (step < pcs.size()) {
                    // the instruction that was executed (it may not be one of the generated ones after a jump)
                    Memory clean;
                    for (unsigned k = 0; k < 6; ++k) clean.write16(pcBefore + 2 * k, mem.read16(pcBefore + 2 * k));
                    kind += " " + decode(clean, pcBefore).mnemonic.substr(0, decode(clean, pcBefore).mnemonic.find('.'));
                }
                (void)t;
                if (failuresByKind[kind]++ < 2) {
                    std::printf("MISMATCH in program %u after instruction %u (at %06X: %s)  %s\n", prog, step + 1, pcBefore,
                                decode(mem, pcBefore).text().c_str(), what);
                    std::printf("   program:");
                    for (const std::string& line : text) std::printf("  [%s]", line.c_str());
                    std::printf("\n   start: SR=%04X D=", sr);
                    for (auto x : d) std::printf("%X ", x);
                    std::printf(" A=");
                    for (auto x : a) std::printf("%X ", x);
                    std::printf(" USP=%X SSP=%X\n", usp, ssp);
                }
            }
        }
        if (dropped) ++abandoned;
        else if (!failed) ++completed;

        // ---- restore both memories ----
        for (auto it = g_refWrites.rbegin(); it != g_refWrites.rend(); ++it) g_ref[it->first] = it->second;
        for (const auto& w : ourWrites)
            for (unsigned k = 0; k < static_cast<unsigned>(w.second); ++k) {
                const Address ad = (w.first + k) & 0xFFFFFF;
                mem.write8(ad, baseByte(ad));
            }
        for (unsigned i = 0; i < 64; ++i) {
            mem.write8(kPc + i, baseByte(kPc + i));
            g_ref[kPc + i] = baseByte(kPc + i);
        }
        if (cpu.halted() || cpu.stopped()) cpu.reset();
    }
    std::printf("%u programs: %llu instructions compared, %llu programs completed, %llu dropped (address errors / STOP / PC above 24 bits: %llu), %llu exceptions taken, %llu mismatches\n",
                programs, static_cast<unsigned long long>(compared), static_cast<unsigned long long>(completed),
                static_cast<unsigned long long>(abandoned), static_cast<unsigned long long>(highPc), static_cast<unsigned long long>(exceptions),
                static_cast<unsigned long long>(failures));
    for (const auto& [k, n] : failuresByKind) std::printf("   %-24s %u\n", k.c_str(), n);
    return failures ? 1 : 0;
}
