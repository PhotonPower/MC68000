// Programs with SECTION directives: relocatable assembly, the hunk writer, and a round trip through the loader and the emulator.
#include <random>
#include <sstream>
#include <string>

#include "m68k/assembler.hpp"
#include "m68k/cpu.hpp"
#include "m68k/hunk.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

AssemblyResult asmOk(const std::string& src, bool exportAll = false) {
    AssemblerOptions o;
    o.exportAllSymbols = exportAll;
    AssemblyResult r = assemble(src, o);
    if (!r.ok()) {
        for (const Diagnostic& d : r.errors) std::fprintf(stderr, "  unexpected error line %d: %s\n", d.line, d.message.c_str());
    }
    return r;
}

// the message of the first error, or "" if the program assembles
std::string firstError(const std::string& src) {
    const AssemblyResult r = assemble(src);
    return r.errors.empty() ? "" : r.errors.front().message;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::uint32_t be32(const std::vector<Byte>& b, std::size_t at) {
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) | (std::uint32_t{b[at + 2]} << 8) | b[at + 3];
}

const char* kProgram =
    "        SECTION code,CODE\n"
    "        XDEF    start\n"
    "start:  MOVE.L  #table,A0\n"        // relocated long immediate (-> data)
    "        LEA     counter,A1\n"       // long absolute address (-> bss)
    "        MOVEQ   #3,D1\n"
    "        MOVEQ   #0,D0\n"
    "loop:   ADD.W   (A0)+,D0\n"
    "        DBRA    D1,loop\n"
    "        MOVE.W  D0,(A1)\n"
    "        MOVE.L  #ptrs,D2\n"
    "        MOVEA.L ptrs+4,A2\n"        // reads the second pointer through a relocated absolute address
    "        RTS\n"
    "        SECTION data,DATA\n"
    "table:  DC.W 10,20,30,40\n"
    "ptrs:   DC.L table,start\n"
    "        SECTION bss,BSS\n"
    "counter: DS.W 1\n";

}  // namespace

int main() {
    // ---- the assembled sections -------------------------------------------------------------------------------------------
    {
        const AssemblyResult r = asmOk(kProgram);
        CHECK(r.ok());
        CHECK(r.hunk);
        CHECK_EQ(r.sections.size(), std::size_t{3});
        CHECK_EQ(r.sections[0].name, std::string("code"));
        CHECK(r.sections[0].kind == HunkSection::Kind::Code);
        CHECK(r.sections[1].kind == HunkSection::Kind::Data);
        CHECK(r.sections[2].kind == HunkSection::Kind::Bss);
        CHECK_EQ(r.sections[2].bssSize, Address{2});
        CHECK(r.sections[2].bytes.empty());
        CHECK_EQ(r.sections[0].size(), Address{static_cast<Address>(r.sections[0].bytes.size())});
        CHECK_EQ(r.sections[1].bytes.size(), std::size_t{16});
        // relocations: #table (offset 2, -> data), LEA counter (8, -> bss), #ptrs (0x1A, -> data), ptrs+4 (0x20, -> data)
        CHECK_EQ(r.sections[0].relocs.size(), std::size_t{4});
        CHECK_EQ(r.sections[0].relocs[0].offset, Address{2});
        CHECK_EQ(r.sections[0].relocs[0].target, 1U);
        CHECK_EQ(r.sections[0].relocs[1].offset, Address{8});
        CHECK_EQ(r.sections[0].relocs[1].target, 2U);
        CHECK_EQ(r.sections[0].relocs[2].target, 1U);
        CHECK_EQ(r.sections[0].relocs[3].target, 1U);
        // the data hunk holds two pointers: one into code, one into data
        CHECK_EQ(r.sections[1].relocs.size(), std::size_t{2});
        CHECK_EQ(r.sections[1].relocs[0].offset, Address{8});
        CHECK_EQ(r.sections[1].relocs[0].target, 1U);
        CHECK_EQ(r.sections[1].relocs[1].offset, Address{12});
        CHECK_EQ(r.sections[1].relocs[1].target, 0U);
        // symbols: offsets inside their sections
        CHECK_EQ(r.symbols.at("ptrs"), std::int64_t{8});
        CHECK_EQ(r.symbolSection.at("ptrs"), 1);
        CHECK_EQ(r.symbolSection.at("counter"), 2);
        CHECK_EQ(r.symbolSection.at("start"), 0);
        // only XDEF symbols are exported unless asked otherwise
        CHECK_EQ(r.sections[0].symbols.size(), std::size_t{1});
        CHECK_EQ(r.sections[0].symbols[0].first, std::string("start"));
        CHECK(r.sections[1].symbols.empty());
        const AssemblyResult all = asmOk(kProgram, true);
        CHECK_EQ(all.sections[1].symbols.size(), std::size_t{2});   // table, ptrs
        CHECK_EQ(all.sections[0].symbols.size(), std::size_t{2});   // start, loop
        // the listing shows the section
        CHECK(has(r.formatListing(), "S1:000008"));
        CHECK(has(r.formatSymbols(), "; section 2 (bss)"));
    }

    // ---- the hunk file: exact bytes of a small program ---------------------------------------------------------------------------
    {
        const AssemblyResult r = asmOk(
            "        SECTION code,CODE_C\n"
            "        XDEF start\n"
            "start:  MOVE.L #msg,D0\n"
            "        RTS\n"
            "        SECTION data,DATA\n"
            "msg:    DC.B \"Hi\",0\n");
        CHECK(r.ok());
        const std::vector<Byte> f = writeHunkFile(r.sections);
        const std::vector<std::uint32_t> expect = {
            0x3F3, 0, 2, 0, 1,            // header: no libraries, 2 hunks, first 0, last 1
            0x40000002, 1,                // sizes in longs: code 2 longs (chip memory flag), data 1 long
            0x3E9, 2,                     // HUNK_CODE, 2 longs
            0x203C0000, 0x00004E75,       // MOVE.L #0,D0 ; RTS   (the immediate is relocated: offset 2)
            0x3EC, 1, 1, 2, 0,            // HUNK_RELOC32: 1 offset to hunk 1: offset 2; end
            0x3F0, 2, 0x73746172, 0x74000000, 0, 0,   // HUNK_SYMBOL: "start" (2 longs) = 0; end
            0x3F2,                        // HUNK_END
            0x3EA, 1, 0x48690000,         // HUNK_DATA: "Hi\0" padded
            0x3F2};
        CHECK_EQ(f.size(), expect.size() * 4);
        for (std::size_t i = 0; i < expect.size() && i * 4 < f.size(); ++i) CHECK_EQ(be32(f, i * 4), expect[i]);
        CHECK(looksLikeHunkFile(f));
        CHECK_EQ(writeHunkFile(r.sections, false).size(), f.size() - 24);   // without the symbol block (6 longs)
        bool threw = false;
        try { (void)writeHunkFile({}); } catch (const IoError&) { threw = true; }
        CHECK(threw);
    }

    // ---- round trip: assemble -> hunk file -> loader -> emulator ----------------------------------------------------------------------------
    {
        const AssemblyResult r = asmOk(kProgram);
        const std::vector<Byte> file = writeHunkFile(r.sections);
        for (const Address base : {Address{0x20000}, Address{0x31234 & ~3U}, Address{0x100}}) {
            const HunkImage img = loadHunkFile(file, base);
            CHECK_EQ(img.hunks.size(), std::size_t{3});
            const Address code = img.hunks[0].address, data = img.hunks[1].address, bss = img.hunks[2].address;
            CHECK_EQ(code, base);
            CHECK_EQ(img.entry, code);
            CHECK_EQ(img.symbols.size(), std::size_t{1});
            CHECK_EQ(img.symbols[0].address, code);
            Memory mem;
            for (const Segment& s : img.segments)
                for (std::size_t i = 0; i < s.bytes.size(); ++i) mem.write8(s.address + static_cast<Address>(i), s.bytes[i]);
            // the relocated fields hold the real addresses
            CHECK_EQ(mem.read32(code + 2), data);              // #table
            CHECK_EQ(mem.read32(code + 8), bss);               // counter
            CHECK_EQ(mem.read32(code + 0x1A), data + 8);       // #ptrs
            CHECK_EQ(mem.read32(data + 8), data);              // DC.L table
            CHECK_EQ(mem.read32(data + 12), code);             // DC.L start
            Cpu cpu(mem);
            cpu.registers().a[7] = 0x7F000;
            cpu.registers().sr = 0x2700;
            const RunResult run = cpu.call(code, 0xF00000, 1000);
            CHECK(run.reason == StopReason::Returned || run.reason == StopReason::Reached);
            CHECK_EQ(cpu.registers().d[0], Long{100});         // 10 + 20 + 30 + 40, read through the relocated pointer
            CHECK_EQ(mem.read16(bss), Word{100});              // stored through the relocated absolute address
            CHECK_EQ(cpu.registers().d[2], data + 8);
            CHECK_EQ(cpu.registers().a[2], code);              // the pointer to `start`, relocated in the data hunk
        }
    }

    // ---- expressions ------------------------------------------------------------------------------------------------------------------------
    {
        const AssemblyResult r = asmOk(
            "        SECTION code,CODE\n"
            "a:      NOP\n"
            "b:      NOP\n"
            "size    EQU b-a\n"                        // inside one section: a plain number
            "        MOVE.L #size,D0\n"
            "        MOVE.L #t+2,D1\n"                 // section-relative with an offset
            "        MOVE.L #end-t,D2\n"
            "        SECTION data,DATA\n"
            "t:      DC.W 1,2,3\n"
            "end:    EVEN\n");
        CHECK(r.ok());
        CHECK_EQ(r.symbolSection.at("size"), -1);
        CHECK_EQ(r.symbols.at("size"), std::int64_t{2});
        const std::vector<Byte>& c = r.sections[0].bytes;
        CHECK_EQ(be32(c, 6), std::uint32_t{2});            // size
        CHECK_EQ(be32(c, 12), std::uint32_t{2});           // t+2: the offset (relocation adds the base)
        CHECK_EQ(be32(c, 18), std::uint32_t{6});           // end-t
        CHECK_EQ(r.sections[0].relocs.size(), std::size_t{1});   // only `#t+2`
    }

    // ---- sections can be continued, BSS, memory types ----------------------------------------------------------------------------------------------
    {
        const AssemblyResult r = asmOk(
            "        SECTION code,CODE\n"
            "        NOP\n"
            "        SECTION data,DATA_F\n"
            "d1:     DC.B 1\n"
            "        SECTION code,CODE\n"            // continue
            "        RTS\n"
            "        BSS bss1\n"                     // shorthand directive with a name
            "v1:     DS.L 2\n"
            "        EVEN\n"
            "v2:     DS.B 3\n");
        CHECK(r.ok());
        CHECK_EQ(r.sections.size(), std::size_t{3});
        CHECK_EQ(r.sections[0].bytes.size(), std::size_t{4});   // NOP + RTS
        CHECK(r.sections[1].fast);
        CHECK(!r.sections[1].chip);
        CHECK_EQ(r.sections[2].name, std::string("bss1"));
        CHECK_EQ(r.sections[2].bssSize, Address{11});
        CHECK_EQ(r.symbols.at("v2"), std::int64_t{8});
        const std::vector<Byte> f = writeHunkFile(r.sections);
        CHECK_EQ(be32(f, 20), std::uint32_t{1});                // CODE: NOP + RTS = 1 long
        CHECK_EQ(be32(f, 24), std::uint32_t{0x80000001});       // DATA_F: fast memory flag, 1 long
        CHECK_EQ(be32(f, 28), std::uint32_t{3});                // BSS: 11 bytes = 3 longs
    }

    // ---- what is refused (and why) ------------------------------------------------------------------------------------------------------------------
    {
        const std::string head = "        SECTION code,CODE\n";
        CHECK(has(firstError(head + "a: LEA b(PC),A0\n        SECTION data,DATA\nb: DC.W 1\n"), "only work inside one SECTION"));
        CHECK(has(firstError(head + "a: BRA b\n        SECTION data,DATA\nb: DC.W 1\n"), "only work inside one SECTION"));
        CHECK_EQ(firstError(head + "a: LEA b(PC),A0\nb: DC.W 1\n"), std::string(""));              // inside one section: fine
        CHECK(has(firstError(head + "        MOVE.W #t,D0\n        SECTION data,DATA\nt: DC.W 1\n"), "only a long immediate"));
        CHECK(has(firstError(head + "        MOVE.W t.W,D0\n        SECTION data,DATA\nt: DC.W 1\n"), "needs a long absolute address"));
        CHECK(has(firstError(head + "        MOVE.W t(A0),D0\n        SECTION data,DATA\nt: DC.W 1\n"), "address inside a SECTION"));
        CHECK(has(firstError(head + "        SECTION data,DATA\nt: DC.W t\n"), "only DC.L can hold it"));
        CHECK(has(firstError(head + "        SECTION data,DATA\nt: DC.B t\n"), "only DC.L can hold it"));
        CHECK(has(firstError(head + "        MOVE.L #b-a,D0\n        SECTION data,DATA\na: DC.W 1\n        SECTION code,CODE\nb: NOP\n"), "mixes addresses"));
        CHECK(has(firstError(head + "a: NOP\n        SECTION data,DATA\nb: DC.L a+b\n"), "mixes addresses"));
        CHECK(has(firstError(head + "        ORG $1000\n"), "ORG cannot be used"));
        CHECK(has(firstError("        NOP\n        SECTION code,CODE\n"), "before the first SECTION"));
        CHECK(has(firstError("lbl:    SECTION code,CODE\n"), "label on a SECTION line"));
        CHECK(has(firstError("        SECTION code,CODE\n        SECTION code,DATA\n"), "another type"));
        CHECK(has(firstError("        SECTION x,STUFF\n"), "unknown section type"));
        CHECK(has(firstError("        SECTION x,CODE_Q\n"), "unknown memory type"));
        CHECK(has(firstError("        SECTION\n"), "needs a name"));
        CHECK(has(firstError("        SECTION b,BSS\n        DC.W 1\n"), "BSS section can only contain DS"));
        CHECK(has(firstError("        SECTION b,BSS\n        NOP\n"), "BSS section can only contain DS"));
        CHECK(has(firstError(head + "        XDEF nosuch\n"), "not defined"));
        CHECK(has(firstError(head + "five EQU 5\n        XDEF five\n"), "plain number"));
        CHECK(has(firstError(head + "        XREF printf\n"), "needs a linker"));
        CHECK(has(firstError("        XDEF a\na: NOP\n"), "XDEF only makes sense"));
        // an absolute program is unaffected by all of this
        CHECK(!assemble("        ORG $1000\nx: MOVE.L #x,D0\n        MOVE.L #$FFFFFFFF,D1\n").hunk);
        CHECK(assemble("        ORG $1000\nx: MOVE.L #x,D0\n        MOVE.L #$FFFFFFFF,D1\n").ok());
    }

    // ---- the relocation check that does not rely on my own reasoning: random programs assembled twice -----------------------------------------------
    // (a) as a SECTION program, written as a hunk file and loaded at `base`; (b) as an absolute program with every section at the address the
    // loader gives it. The two memory images must be identical.
    {
        std::mt19937 rng(20261106);
        const Address base = 0x20000;
        int checked = 0, withRelocs = 0;
        for (int trial = 0; trial < 400; ++trial) {
            const int nLabels = 3 + static_cast<int>(rng() % 6);
            std::vector<int> labelSection(static_cast<std::size_t>(nLabels));
            for (int& sct : labelSection) sct = static_cast<int>(rng() % 3);  // 0 code, 1 data, 2 bss
            std::vector<std::vector<std::string>> body(3);
            auto anyLabel = [&] { return "L" + std::to_string(rng() % static_cast<unsigned>(nLabels)); };
            auto sameSection = [&](int sct) -> std::string {
                std::vector<int> candidates;
                for (int k = 0; k < nLabels; ++k)
                    if (labelSection[static_cast<std::size_t>(k)] == sct) candidates.push_back(k);
                return candidates.empty() ? "" : "L" + std::to_string(candidates[rng() % candidates.size()]);
            };
            const int nCode = 4 + static_cast<int>(rng() % 12);
            for (int i = 0; i < nCode; ++i) {
                const unsigned d = rng() % 8;
                const std::string reg = std::to_string(rng() % 8);
                const std::string off = rng() % 3 ? "" : "+" + std::to_string(rng() % 6);
                switch (rng() % 10) {
                    case 0: body[0].push_back("        MOVE.L #" + anyLabel() + off + ",D" + std::to_string(d)); break;
                    case 1: body[0].push_back("        LEA " + anyLabel() + off + ".L,A" + std::to_string(d % 7)); break;
                    case 2: body[0].push_back("        MOVE.L " + anyLabel() + off + ".L,D" + reg); break;
                    case 3: body[0].push_back("        MOVE.W D" + reg + "," + anyLabel() + off + ".L"); break;
                    case 4: body[0].push_back("        JSR " + anyLabel() + ".L"); break;
                    case 5: {
                        const std::string l = sameSection(0);
                        body[0].push_back(l.empty() ? "        NOP" : "        LEA " + l + "(PC),A" + std::to_string(d % 7));
                        break;
                    }
                    case 6: {
                        const std::string l = sameSection(0);
                        body[0].push_back(l.empty() ? "        NOP" : "        BRA " + l);
                        break;
                    }
                    case 7: {
                        const std::string l = sameSection(0);
                        body[0].push_back(l.empty() ? "        NOP" : "        DBRA D" + reg + "," + l);
                        break;
                    }
                    case 8: body[0].push_back("        MOVE.L #" + anyLabel() + "-" + anyLabel() + ",D0"); break;  // mostly refused: handled below
                    default: body[0].push_back("        MOVEQ #" + std::to_string(rng() % 100) + ",D" + reg); break;
                }
            }
            const int nData = 2 + static_cast<int>(rng() % 8);
            for (int i = 0; i < nData; ++i) {
                switch (rng() % 5) {
                    case 0: body[1].push_back("        DC.L " + anyLabel() + (rng() % 2 ? "+" + std::to_string(rng() % 9) : "")); break;
                    case 1: body[1].push_back("        DC.W " + std::to_string(rng() % 60000)); break;
                    case 2: body[1].push_back("        DC.B " + std::to_string(rng() % 250) + "," + std::to_string(rng() % 250)); break;
                    case 3: body[1].push_back("        DS.B " + std::to_string(1 + rng() % 5)); break;
                    default: body[1].push_back("        DC.L " + std::to_string(rng()) ); break;
                }
            }
            const int nBss = 1 + static_cast<int>(rng() % 4);
            for (int i = 0; i < nBss; ++i) body[2].push_back("        DS." + std::string(1, "BWL"[rng() % 3]) + " " + std::to_string(1 + rng() % 6));
            // put the labels at random lines of their sections (also at the end)
            for (int k = 0; k < nLabels; ++k) {
                std::vector<std::string>& b = body[static_cast<std::size_t>(labelSection[static_cast<std::size_t>(k)])];
                b.insert(b.begin() + static_cast<std::ptrdiff_t>(rng() % (b.size() + 1)), "L" + std::to_string(k) + ":");
            }
            // BSS and data labels would be at odd addresses for code references: harmless (data only). The code section must end evenly (it does:
            // only instructions), and `DS.B` in data may leave the data section odd, which only matters for the next section's base (aligned by the loader).
            const char* names[] = {"code,CODE", "data,DATA", "bss,BSS"};
            std::string hunkSrc, absSrc;
            for (int sct = 0; sct < 3; ++sct) {
                hunkSrc += std::string("        SECTION ") + names[sct] + "\n";
                for (const std::string& line : body[static_cast<std::size_t>(sct)]) hunkSrc += line + "\n";
            }
            const AssemblyResult hr = assemble(hunkSrc);
            if (!hr.ok()) {
                // the only acceptable failures: a difference/sum of addresses from different sections (case 8)
                for (const Diagnostic& d : hr.errors) CHECK(has(d.message, "mixes addresses") || has(d.message, "address inside a SECTION"));
                continue;
            }
            const Address codeSize = (hr.sections[0].size() + 3U) & ~3U, dataSize = (hr.sections[1].size() + 3U) & ~3U;
            const Address addrs[3] = {base, base + codeSize, base + codeSize + dataSize};
            for (int sct = 0; sct < 3; ++sct) {
                std::ostringstream org;
                org << "        ORG $" << std::hex << addrs[sct] << "\n";
                absSrc += org.str();
                for (const std::string& line : body[static_cast<std::size_t>(sct)]) absSrc += line + "\n";
            }
            const AssemblyResult ar = assemble(absSrc);
            CHECK(ar.ok());
            if (!ar.ok()) {
                for (const Diagnostic& d : ar.errors) std::fprintf(stderr, "  abs error line %d: %s\n", d.line, d.message.c_str());
                std::fprintf(stderr, "%s\n", absSrc.c_str());
                continue;
            }
            const HunkImage img = loadHunkFile(writeHunkFile(hr.sections), base);
            Memory viaHunk, viaAbs;
            for (const Segment& sg : img.segments)
                for (std::size_t i = 0; i < sg.bytes.size(); ++i) viaHunk.write8(sg.address + static_cast<Address>(i), sg.bytes[i]);
            for (const Segment& sg : ar.segments)
                for (std::size_t i = 0; i < sg.bytes.size(); ++i) viaAbs.write8(sg.address + static_cast<Address>(i), sg.bytes[i]);
            const Address end = addrs[2] + hr.sections[2].size();
            bool same = true;
            for (Address a = base; a < end && same; ++a) same = viaHunk.read8(a) == viaAbs.read8(a);
            CHECK(same);
            if (!same) std::fprintf(stderr, "  mismatch in trial %d\n%s\n", trial, hunkSrc.c_str());
            // the symbol values agree with the absolute assembly
            for (const auto& [name, off] : hr.symbols) {
                const int sct = hr.symbolSection.at(name);
                if (sct < 0) continue;
                CHECK_EQ(static_cast<std::int64_t>(addrs[sct]) + off, ar.symbols.at(name));
            }
            ++checked;
            for (const HunkSection& hs : hr.sections) if (!hs.relocs.empty()) { ++withRelocs; break; }
        }
        CHECK(checked > 200);
        CHECK(withRelocs > 150);
    }

    return finish("hunk_write");
}
