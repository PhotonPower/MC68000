#include "m68k/disassembler.hpp"

#include <random>
#include <sstream>

#include "m68k/assembler.hpp"
#include "m68k/image_io.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

Memory assembleToMemory(const std::string& src, Address origin, std::vector<Byte>* bytes = nullptr) {
    AssemblerOptions opt;
    opt.origin = origin;
    const AssemblyResult r = assemble(src, opt);
    if (!r.ok()) {
        std::cerr << "test source does not assemble: " << r.errors[0].message << "\n";
        std::exit(2);
    }
    Memory mem;
    loadSegments(mem, r.segments);
    if (bytes) *bytes = r.flatten();
    return mem;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* kProgram =
    "start:  LEA     msg(PC),A0\n"
    "loop:   MOVE.B  (A0)+,D0\n"
    "        BEQ.S   done\n"
    "        BSR     putc\n"
    "        BRA.S   loop\n"
    "done:   RTS\n"
    "putc:   MOVE.B  D0,$00FF0000.L\n"
    "        RTS\n"
    "msg:    DC.B    \"Hi\",0\n"
    "        EVEN\n"
    "unused: MOVEQ   #1,D0\n"
    "        RTS\n";

}  // namespace

int main() {
    // ---- flow analysis separates code from data ---------------------------------------------------
    {
        std::vector<Byte> bytes;
        Memory mem = assembleToMemory(kProgram, 0x1000, &bytes);
        Disassembler dis(mem, 0x1000, 0x1000 + static_cast<Address>(bytes.size()));
        dis.addEntry(0x1000);
        dis.analyzeFlow();
        const std::string text = dis.render();
        CHECK(contains(text, "L_001000:"));
        CHECK(contains(text, "LEA      L_001016(PC),A0"));    // msg: data, referenced through PC
        CHECK(contains(text, "BEQ.S    L_00100C"));
        CHECK(contains(text, "BSR.W    L_00100E") || contains(text, "BSR.S    L_00100E"));
        CHECK(contains(text, "DC.B     $48,$69,$00"));       // the string is data, not code
        CHECK(contains(text, "<- "));                          // cross references
        // `unused` is not reachable from the entry point: shown as data
        CHECK(!contains(text, "MOVEQ"));
        // with a second entry point it becomes code
        Disassembler dis2(mem, 0x1000, 0x1000 + static_cast<Address>(bytes.size()));
        dis2.addEntry(0x1000);
        dis2.addEntry(0x101A);
        dis2.analyzeFlow();
        CHECK(contains(dis2.render(), "MOVEQ    #$01,D0"));
        CHECK(dis2.codeBytes() > dis.codeBytes());
    }

    // ---- user symbols, comments and number styles ----------------------------------------------------
    {
        Memory mem = assembleToMemory(kProgram, 0x1000);
        Disassembler dis(mem, 0x1000, 0x1030);
        dis.addEntry(0x1000);
        dis.addSymbol(0x100E, "print_char");
        dis.addComment(0x1000, "entry");
        dis.analyzeFlow();
        DisassemblerOptions opt;
        opt.style = NumberStyle::CPrefix;
        const std::string text = dis.render(opt);
        CHECK(contains(text, "print_char:"));
        CHECK(contains(text, "BSR.W    print_char") || contains(text, "BSR.S    print_char"));
        CHECK(contains(text, "; entry"));
        CHECK(contains(text, "0x00FF0000.L"));
    }

    // ---- comment files ------------------------------------------------------------------------------------
    {
        Memory mem = assembleToMemory(kProgram, 0x1000);
        Disassembler dis(mem, 0x1000, 0x1030);
        const std::vector<Diagnostic> errs = loadCommentFile(
            "; comment file\n"
            "$1000,\"entry: print the message\"\n"
            "0x1004,fetch a character\n"
            "4110,\"say \"\"hi\"\"\"\n"
            "nonsense\n"
            "zz,\"bad address\"\n",
            dis);
        CHECK_EQ(errs.size(), std::size_t{2});
        CHECK_EQ(errs[0].line, 5);
        CHECK_EQ(errs[1].line, 6);
        dis.analyzeFlow();
        const std::string text = dis.render();
        CHECK(contains(text, "; entry: print the message"));
        CHECK(contains(text, "; fetch a character"));
        CHECK(contains(text, "; say \"hi\""));  // 4110 = $100E
    }

    // ---- listing options ------------------------------------------------------------------------------------
    {
        Memory mem = assembleToMemory(kProgram, 0x1000);
        Disassembler dis(mem, 0x1000, 0x1030);
        dis.addEntry(0x1000);
        dis.analyzeFlow();
        DisassemblerOptions opt;
        opt.showBytes = false;
        opt.xrefs = false;
        const std::string text = dis.render(opt);
        CHECK(!contains(text, "41FA"));
        CHECK(!contains(text, "<-"));
        CHECK(contains(text, "001000  LEA"));
        CHECK(contains(dis.render(), "001000  41FA 0014"));
    }

    // ---- linear mode: invalid words become DC.W, a trailing odd byte becomes DC.B --------------------
    {
        Memory mem;
        mem.write16(0x1000, 0x4E71);
        mem.write16(0x1002, 0xA123);
        mem.write16(0x1004, 0x4E75);
        mem.write8(0x1006, 0x99);
        Disassembler dis(mem, 0x1000, 0x1007);
        dis.analyzeLinear();
        const std::string text = dis.render();
        CHECK(contains(text, "NOP"));
        CHECK(contains(text, "DC.W     $A123"));
        CHECK(contains(text, "RTS"));
        CHECK(contains(text, "DC.B     $99"));
    }

    // ---- an instruction that crosses the end of the range is data -------------------------------------------
    {
        Memory mem;
        mem.write16(0x1000, 0x303C);  // MOVE.W #imm,D0 needs 4 bytes
        mem.write16(0x1002, 0x1234);
        Disassembler dis(mem, 0x1000, 0x1002);
        dis.addEntry(0x1000);
        dis.analyzeFlow();
        CHECK_EQ(dis.codeBytes(), std::size_t{0});
        CHECK(contains(dis.render(), "DC.B     $30,$3C"));
    }

    // ---- code overlapping already analysed code is not accepted -----------------------------------------------
    {
        Memory mem;
        mem.write16(0x1000, 0x303C);  // MOVE.W #$4E75,D0   (the immediate looks like RTS)
        mem.write16(0x1002, 0x4E75);
        mem.write16(0x1004, 0x4E75);
        Disassembler dis(mem, 0x1000, 0x1006);
        dis.addEntry(0x1000);
        dis.addEntry(0x1002);  // would start in the middle of the first instruction
        dis.analyzeFlow();
        CHECK_EQ(dis.codeBytes(), std::size_t{6});  // MOVE.W (4 bytes) + RTS at 1004; no code starts at 1002
        const std::string text = dis.render();
        CHECK(text.find("RTS") != std::string::npos && text.find("RTS") == text.rfind("RTS"));
    }

    // ---- the assembler accepts what --asm writes: byte-exact roundtrip ------------------------------------
    {
        // linear mode over random bytes (mostly invalid words, some instructions)
        std::mt19937 rng(2024);
        for (int round = 0; round < 300; ++round) {
            Memory mem;
            const Address start = 0x2000;
            const Address end = start + 512 + static_cast<Address>(rng() % 8);
            for (Address a = start; a < end; ++a) {
                Byte b = static_cast<Byte>(rng());
                // bias towards real instructions: a third of the words come from a small set of opcodes
                mem.write8(a, b);
            }
            const Word common[] = {0x4E75, 0x4E71, 0x6000, 0x6100, 0x6700, 0x4EB9, 0x4EF9, 0x41FA, 0x4879, 0x33FC, 0x51C8, 0x2079};
            for (Address a = start; a + 1 < end; a += 2)
                if (rng() % 3 == 0) mem.write16(a, common[rng() % 12]);
            Disassembler dis(mem, start, end);
            dis.analyzeLinear();
            DisassemblerOptions opt;
            opt.asmSource = true;
            const std::string src = dis.render(opt);
            AssemblyResult r = assemble(src);
            if (!r.ok()) {
                std::cerr << "re-assembly failed: line " << r.errors[0].line << ": " << r.errors[0].message << "\n";
                CHECK(false);
                continue;
            }
            const std::vector<Byte> got = r.flatten();
            const std::vector<Byte> want = mem.readBytes(start, end - start);
            CHECK(got == want);
            if (got != want) {
                std::size_t i = 0;
                while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
                std::cerr << "round " << round << ": first difference at offset " << i << " (sizes " << got.size() << "/"
                          << want.size() << ")\n";
                writeTextFile("m68k_roundtrip_failure.s", src);
                for (std::size_t k = (i >= 6 ? i - 6 : 0); k < i + 8 && k < got.size(); ++k)
                    std::cerr << "   +" << k << ": got " << hexDigits(got[k], 2) << " want " << hexDigits(want[k], 2) << "\n";
                break;
            }
            CHECK_EQ(r.lowest(), start);
        }
        // flow mode over random bytes with random entry points
        for (int round = 0; round < 150; ++round) {
            Memory mem;
            const Address start = 0x4000;
            const Address end = start + 600 + static_cast<Address>(rng() % 5);
            for (Address a = start; a < end; ++a) mem.write8(a, static_cast<Byte>(rng()));
            const Word common[] = {0x4E75, 0x4E71, 0x6000, 0x6100, 0x6700, 0x4EB9, 0x4EF9, 0x41FA, 0x4879, 0x33FC, 0x51C8, 0x2079, 0x6600};
            for (Address a = start; a + 1 < end; a += 2)
                if (rng() % 3 == 0) mem.write16(a, common[rng() % 13]);
            Disassembler dis(mem, start, end);
            for (int e = 0; e < 4; ++e) dis.addEntry(start + 2 * static_cast<Address>(rng() % 300));
            dis.analyzeFlow();
            DisassemblerOptions opt;
            opt.asmSource = true;
            const AssemblyResult r = assemble(dis.render(opt));
            CHECK(r.ok());
            if (!r.ok()) {
                std::cerr << "flow re-assembly failed: " << r.errors[0].message << "\n";
                continue;
            }
            CHECK(r.flatten() == mem.readBytes(start, end - start));
        }
        // flow mode over a real program: code is re-assembled, data stays DC.B
        std::vector<Byte> bytes;
        Memory mem = assembleToMemory(kProgram, 0x1000, &bytes);
        Disassembler dis(mem, 0x1000, 0x1000 + static_cast<Address>(bytes.size()));
        dis.addEntry(0x1000);
        dis.analyzeFlow();
        DisassemblerOptions opt;
        opt.asmSource = true;
        const AssemblyResult r = assemble(dis.render(opt));
        CHECK(r.ok());
        CHECK(r.flatten() == bytes);
    }

    // labels in the middle of an instruction are defined by value so that the source still assembles
    {
        Memory mem;
        mem.write16(0x1000, 0x303C);  // MOVE.W #imm,D0
        mem.write16(0x1002, 0x1234);
        mem.write16(0x1004, 0x4EB9);  // JSR $00001002.L  -> label in the middle of the first instruction
        mem.write16(0x1006, 0x0000);
        mem.write16(0x1008, 0x1002);
        Disassembler dis(mem, 0x1000, 0x100A);
        dis.addEntry(0x1000);
        dis.analyzeFlow();
        DisassemblerOptions opt;
        opt.asmSource = true;
        const std::string src = dis.render(opt);
        CHECK(contains(src, "L_001002 EQU"));
        const AssemblyResult r = assemble(src);
        CHECK(r.ok());
        CHECK(r.flatten() == mem.readBytes(0x1000, 10));
    }

    return finish("disassembler");
}
