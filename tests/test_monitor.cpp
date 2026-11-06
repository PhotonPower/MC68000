#include "m68k/monitor.hpp"

#include <cstdio>
#include <random>

#include "m68k/image_io.hpp"
#include "m68k/assembler.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
bool isError(const std::string& text) { return text.rfind("? ", 0) == 0; }

// Types several lines and returns everything the monitor printed.
std::string type(Monitor& m, const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) out += m.execute(l);
    return out;
}

std::string bytesAt(Monitor& m, Address a, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += hexDigits(m.memory().read8(a + static_cast<Address>(i)), 2);
    return s;
}

struct HunkBuilder {
    std::vector<Byte> out;
    void u32(std::uint32_t v) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<Byte>(v >> (8 * i)));
    }
};

}  // namespace

int main() {
    // ---- numbers: hexadecimal by default -------------------------------------------------------------------
    {
        Monitor m;
        CHECK_EQ(m.execute("e 1000 12 AB \"Hi\" 0"), std::string("ok\n"));
        CHECK_EQ(bytesAt(m, 0x1000, 6), std::string("12AB48690000").substr(0, 10) + "00");
        m.execute("e 1000 100.");           // decimal
        CHECK_EQ(bytesAt(m, 0x1000, 1), std::string("64"));
        m.execute("e 1000 $FF");
        CHECK_EQ(bytesAt(m, 0x1000, 1), std::string("FF"));
        m.execute("e 1000 %1010");
        CHECK_EQ(bytesAt(m, 0x1000, 1), std::string("0A"));
        m.execute("e 1000 0x20");
        CHECK_EQ(bytesAt(m, 0x1000, 1), std::string("20"));
        m.execute("e 1000 1+2*3");          // expressions, hexadecimal
        CHECK_EQ(bytesAt(m, 0x1000, 1), std::string("07"));
        m.execute("e 1000 1B");
        CHECK_EQ(bytesAt(m, 0x1000, 1), std::string("1B"));
        m.execute("sym foo 1004");
        m.execute("e foo 7");
        m.execute("e foo+1 8");
        CHECK_EQ(bytesAt(m, 0x1004, 2), std::string("0708"));
        m.execute("sym dead 5");            // a known symbol wins over the hex number
        m.execute("e 1000 dead");
        CHECK_EQ(bytesAt(m, 0x1000, 1), std::string("05"));
        CHECK(isError(m.execute("e 1000 100")));       // 0x100 does not fit a byte
        CHECK(isError(m.execute("e 1000 nosuch")));
        CHECK(isError(m.execute("e 1000 1+")));
        CHECK(isError(m.execute("e 1000 12.A")));
        m.execute("e.w 1000 DEAD BEEF");
        CHECK_EQ(bytesAt(m, 0x1000, 4), std::string("DEADBEEF"));
        m.execute("e.l 1000 CAFEBABE");
        CHECK_EQ(bytesAt(m, 0x1000, 4), std::string("CAFEBABE"));
        CHECK(isError(m.execute("e.w 1001 1234")));    // odd address for word data
        m.execute("r d0=1234 a1=2000");
        m.execute("e.w 3000 @d0");                      // registers in expressions
        CHECK_EQ(bytesAt(m, 0x3000, 2), std::string("1234"));
        m.execute("e 3002 @a1+5");                      // 2005 does not fit a byte
        CHECK(isError(m.execute("e 3002 @a1")));
        CHECK(isError(m.execute("e 3002 @q9")));
        CHECK(isError(m.execute("frobnicate")));
        CHECK(isError(m.execute("e")));
    }

    // ---- dump, fill, compare, copy, search -----------------------------------------------------------------------
    {
        Monitor m;
        m.execute("e 2000 \"Hello, 68000!\" 0");
        const std::string dump = m.execute("m 2000 10");
        CHECK(has(dump, "002000  48 65 6C 6C 6F 2C 20 36 38 30 30 30 21 00"));
        CHECK(has(dump, "|Hello, 68000!...|"));
        CHECK(has(m.execute("m.w 2000 8"), "4865 6C6C 6F2C 2036"));
        CHECK(has(m.execute("m.l 2000 8"), "48656C6C 6F2C2036"));
        CHECK(has(m.execute("m"), "002008"));          // continues after the last dump
        m.execute("f 3000 300F AA 55");
        CHECK_EQ(bytesAt(m, 0x3000, 4), std::string("AA55AA55"));
        CHECK_EQ(m.execute("c 3000 3007 3008"), std::string("identical\n"));
        m.execute("e 3008 11");
        CHECK(has(m.execute("c 3000 3007 3008"), "003000: AA  003008: 11"));
        CHECK(isError(m.execute("f 3010 3000 1")));
        m.execute("e 4000 1 2 3 4 5 6 7 8");
        m.execute("t 4000 4007 4002");                // overlapping copy must not smear
        CHECK_EQ(bytesAt(m, 0x4000, 10), std::string("01020102030405060708"));
        CHECK(has(m.execute("h 4000 40FF 05 06"), "004006"));
        CHECK(has(m.execute("h 2000 20FF \"68000\""), "002007"));
        CHECK_EQ(m.execute("h 2000 20FF \"nothere\""), std::string("not found\n"));
    }

    // ---- assemble mode ----------------------------------------------------------------------------------------------
    {
        Monitor m;
        std::string out = type(m, {"a 1000", "start: MOVEQ #5,D0", "MOVEQ #0,D1", "loop: ADD.W D0,D1", "SUBQ.W #1,D0",
                                   "BNE.S loop", "MOVE.W D1,$2000.W", "DC.W $1234", "RTS", ""});
        CHECK(has(out, "assemble mode"));
        CHECK(has(out, "  001000  7005"));
        CHECK(has(out, "  001008  66FA"));
        CHECK(has(out, "  00100E  1234"));
        CHECK_EQ(bytesAt(m, 0x1000, 2), std::string("7005"));
        CHECK_EQ(m.prompt(), std::string("-"));
        out = m.execute("d start 3");
        CHECK(has(out, "start:\n"));
        CHECK(has(out, "MOVEQ #$05,D0"));
        out = m.execute("d 1004 3");
        CHECK(has(out, "loop:\n"));
        CHECK(has(out, "BNE.S loop"));

        m.execute("a 3000");
        CHECK_EQ(m.prompt(), std::string("003000:"));
        CHECK(isError(m.execute("BRA later")));                 // forward references are not possible
        CHECK(has(m.execute("BRA later"), "forward references"));
        CHECK(isError(m.execute("NOPE")));                      // a lone word that is no instruction
        CHECK(isError(m.execute("MOVE.W D0")));                 // wrong operands
        CHECK(has(m.execute("lab:"), "lab = 003000"));
        m.execute("NOP");
        m.execute("BRA lab");
        CHECK_EQ(bytesAt(m, 0x3000, 6), std::string("4E71") + "60FC" + "0000");
        m.execute("");
        CHECK_EQ(m.prompt(), std::string("-"));
        m.execute("a 3001");
        CHECK(isError(m.execute("NOP")));                       // instructions need an even address
        m.execute("");
        // the assembler syntax is decimal in assemble mode, hexadecimal outside
        m.execute("a 3100");
        m.execute("MOVE.W #10,D0");
        m.execute("MOVE.W #$10,D1");
        m.execute("");
        CHECK_EQ(bytesAt(m, 0x3100, 8), std::string("303C000A") + "323C0010");
    }

    // ---- registers and running --------------------------------------------------------------------------------------------
    {
        Monitor m;
        m.execute("sym twice 1014");   // no forward references while typing: the address is fixed first
        type(m, {"a 1000", "MOVEQ #5,D0", "MOVEQ #0,D1", "loop: ADD.W D0,D1", "SUBQ.W #1,D0", "BNE.S loop", "MOVE.W D1,$2000.W",
                 "BSR.S twice", "STOP #$2700", "twice: ADD.W D1,D1", "RTS", ""});
        CHECK_EQ(bytesAt(m, 0x1010, 4), std::string("4E722700"));
        CHECK_EQ(bytesAt(m, 0x1014, 4), std::string("D241") + "4E75");

        std::string out = m.execute("r pc=1000");
        CHECK(has(out, "PC=001000"));
        CHECK(has(out, "D0=00000000"));
        out = m.execute("n");
        CHECK(has(out, "stopped: stepped  [1 instructions]"));
        CHECK(has(out, "D0=00000005"));
        CHECK(has(out, "> 001002"));
        out = m.execute("n 3");                                  // 2..20 steps: every instruction is shown
        CHECK(has(out, "MOVEQ #$00,D1"));
        CHECK(has(out, "ADD.W D0,D1"));
        CHECK(has(out, "[3 instructions]"));

        m.execute("b twice");
        out = m.execute("g 1000");
        CHECK(has(out, "stopped: breakpoint at 001014"));
        CHECK(has(out, "D1=0000000F"));
        CHECK(has(out, "twice:\n> 001014"));
        out = m.execute("u");
        CHECK(has(out, "stopped: returned"));
        CHECK(has(out, "D1=0000001E"));
        CHECK(has(m.execute("m 2000 2"), "00 0F"));
        CHECK(has(m.execute("b"), "break 001014  (twice)"));
        m.execute("bc");
        CHECK(has(m.execute("b"), "no breakpoints"));

        // step over, run until an address, STOP
        m.execute("r pc=100E sp=8000");
        out = m.execute("o");
        CHECK(has(out, "PC=001010"));
        out = m.execute("g");
        CHECK(has(out, "STOP instruction"));
        out = m.execute("ru 1000");                              // the processor is stopped: continues only after r pc=
        m.execute("r pc=1000");
        out = m.execute("ru 100A");
        CHECK(has(out, "stopped: reached 00100A"));

        // watchpoints
        m.execute("r pc=1000");
        m.execute("w 2000 2 w");
        CHECK(has(m.execute("b"), "watch 002000"));
        out = m.execute("g");
        CHECK(has(out, "stopped: watchpoint at 002000"));
        m.execute("bc");

        // limits stop endless loops
        type(m, {"a 5000", "spin: BRA.S spin", ""});
        // `spin` is defined by the line itself, so BRA.S spin works
        m.execute("limit 100");
        m.execute("r pc=5000");
        out = m.execute("g");
        CHECK(has(out, "instruction limit (256)"));              // 100 hexadecimal
        CHECK(has(m.execute("limit"), "limit 256"));
        CHECK(isError(m.execute("limit 0")));
        CHECK(isError(m.execute("r bogus=1")));
        CHECK(isError(m.execute("r d0")));

        // exceptions are reported
        m.execute("limit 100000");
        m.execute("r pc=6000 sp=8000");
        m.execute("e 6000 4A FC");                               // ILLEGAL
        m.execute("e.l 10 7000");                                // vector 4 -> $7000
        m.execute("e.w 7000 4E71");
        out = m.execute("n");
        CHECK(has(out, "exceptions: 1 (last: vector 4)"));
        CHECK(has(out, "PC=007000"));

        // interrupts
        m.execute("e.l 6C 7100");                                // level 3 autovector (27)
        m.execute("e.w 7100 4E71");
        m.execute("r pc=7000 sr=2000");
        m.execute("int 3");
        out = m.execute("n");
        CHECK(has(out, "PC=007100"));
        CHECK(isError(m.execute("int 9")));
        CHECK(isError(m.execute("int")));
    }

    // ---- symbols ----------------------------------------------------------------------------------------------------------------
    {
        Monitor m;
        CHECK_EQ(m.execute("sym"), std::string("no symbols\n"));
        m.execute("sym b_one 20");
        m.execute("sym a_two 3000");
        const std::string list = m.execute("sym");
        CHECK(has(list, "a_two"));
        CHECK(has(list, "= $00003000"));
        CHECK(list.find("a_two") < list.find("b_one"));
        CHECK_EQ(m.execute("sym -a_two"), std::string("ok\n"));
        CHECK(isError(m.execute("sym -a_two")));
        CHECK(isError(m.execute("sym 1abc 5")));
        CHECK(isError(m.execute("sym x")));
        // symbol files
        m.execute("sym keep 1234");
        CHECK_EQ(m.execute("symsave m68k_mon_syms.tmp"), std::string("ok\n"));
        Monitor n;
        CHECK(has(n.execute("symload m68k_mon_syms.tmp"), "ok: 2 symbols"));
        CHECK(has(n.execute("sym"), "keep"));
        std::remove("m68k_mon_syms.tmp");
        CHECK(isError(n.execute("symload m68k_mon_does_not_exist.tmp")));
    }

    // ---- files -----------------------------------------------------------------------------------------------------------------------
    {
        Monitor m;
        writeTextFile("m68k_mon_prog.s", "        ORG $1000\nstart:  MOVEQ #7,D0\n        RTS\nmsg:    DC.B \"ok\",0\n");
        CHECK(has(m.execute("asm m68k_mon_prog.s"), "ok: 7 bytes at 001000, 2 symbols"));
        CHECK(has(m.execute("d start 2"), "MOVEQ #$07,D0"));
        CHECK(has(m.execute("m msg 3"), "6F 6B 00"));
        writeTextFile("m68k_mon_bad.s", "        NOPE\n        MOVE.W D0\n");
        const std::string bad = m.execute("asm m68k_mon_bad.s");
        CHECK(has(bad, "m68k_mon_bad.s:1:"));
        CHECK(has(bad, "m68k_mon_bad.s:2:"));
        std::remove("m68k_mon_bad.s");
        std::remove("m68k_mon_prog.s");

        writeBinaryFile("m68k_mon_raw.bin", {0x4E, 0x75, 0x4E, 0x71});
        CHECK(isError(m.execute("l m68k_mon_raw.bin")));       // raw needs an address
        CHECK(has(m.execute("l m68k_mon_raw.bin 4000"), "ok: 4 bytes at 004000"));
        CHECK_EQ(bytesAt(m, 0x4000, 4), std::string("4E754E71"));
        CHECK(has(m.execute("sv m68k_mon_save.bin 4000 4003"), "ok: 4 bytes"));
        CHECK(readBinaryFile("m68k_mon_save.bin") == (std::vector<Byte>{0x4E, 0x75, 0x4E, 0x71}));
        CHECK(isError(m.execute("sv m68k_mon_save.bin 4003 4000")));
        std::remove("m68k_mon_raw.bin");
        std::remove("m68k_mon_save.bin");

        writeTextFile("m68k_mon.s19", formatSRecords({{0x5000, {0x70, 0x01, 0x4E, 0x75}}}, 0x5000));
        CHECK(has(m.execute("l m68k_mon.s19"), "PC=005000"));
        CHECK_EQ(m.cpu().registers().pc, Address{0x5000});
        std::remove("m68k_mon.s19");

        HunkBuilder h;
        h.u32(0x3F3); h.u32(0); h.u32(1); h.u32(0); h.u32(0); h.u32(2);
        h.u32(0x3E9); h.u32(2);
        for (const unsigned b : {0x70U, 0x2AU, 0x4EU, 0x75U, 0x4EU, 0x71U, 0x4EU, 0x71U}) h.out.push_back(static_cast<Byte>(b));
        h.u32(0x3F0); h.u32(1); h.u32(0x6D61696E); h.u32(0); h.u32(0);
        h.u32(0x3F2);
        writeBinaryFile("m68k_mon.hunk", h.out);
        const std::string loaded = m.execute("l m68k_mon.hunk");
        CHECK(has(loaded, "CODE hunk at 020000, 8 bytes"));
        CHECK(has(loaded, "PC=020000, 1 symbols"));
        m.execute("r pc=main sp=8000");
        CHECK(has(m.execute("n"), "D0=0000002A"));
        std::remove("m68k_mon.hunk");

        // checkpoints and snapshots
        m.execute("e 7000 11");
        m.execute("ck one");
        m.execute("e 7000 22");
        CHECK(has(m.execute("rb one"), "ok"));
        CHECK_EQ(bytesAt(m, 0x7000, 1), std::string("11"));
        CHECK(isError(m.execute("rb nothere")));
        m.execute("r d3=BEEF");
        CHECK_EQ(m.execute("savestate m68k_mon.snap"), std::string("ok\n"));
        m.execute("reset mem");
        CHECK_EQ(bytesAt(m, 0x7000, 1), std::string("00"));
        CHECK(has(m.execute("loadstate m68k_mon.snap"), "D3=0000BEEF"));
        CHECK_EQ(bytesAt(m, 0x7000, 1), std::string("11"));
        std::remove("m68k_mon.snap");

        // file commands can be switched off
        MonitorOptions opt;
        opt.allowFiles = false;
        Monitor locked(opt);
        for (const char* c : {"asm x.s", "l x.bin 0", "sv x.bin 0 1", "symsave x", "symload x", "savestate x", "loadstate x", "rom x"})
            CHECK(has(locked.execute(c), "disabled"));
        CHECK(!isError(locked.execute("e 1000 1")));
    }

    // ---- the Amiga memory map -----------------------------------------------------------------------------------------------------------
    {
        Monitor m;
        CHECK(has(m.execute("machine"), "generic"));
        CHECK(has(m.execute("io"), "no device map"));
        CHECK_EQ(m.execute("machine amiga500"), std::string("ok\n"));
        CHECK(has(m.execute("machine"), "amiga500 (512 KiB chip RAM)"));
        CHECK(has(m.execute("r"), "A7=0007FFF0"));
        type(m, {"a 1000", "MOVE.W #$0F00,$DFF180.L", "MOVE.L #1,$300000.L", "STOP #$2700", ""});
        m.execute("r pc=1000");
        m.execute("g");
        const std::string io = m.execute("io");
        CHECK(has(io, "DFF180 <- 0F00  (custom COLOR00)  by the instruction at 001000"));
        CHECK(has(io, "(unmapped)"));
        CHECK_EQ(m.execute("io"), std::string("no writes\n"));
        CHECK(has(m.execute("m dff180 2"), "0F 00"));
        CHECK(isError(m.execute("e F80000 1")));                // ROM
        CHECK(isError(m.execute("a 400000")) == false);
        m.execute("");
        CHECK(isError(m.execute("rom nothere.rom")));
        CHECK(isError(m.execute("machine c64")));
        CHECK(isError(m.execute("machine amiga500 8")));
        CHECK_EQ(m.execute("machine generic"), std::string("ok\n"));
        CHECK(!isError(m.execute("e 300000 1")));
    }

    {
        Monitor m;  // oversized ranges are refused
        CHECK(isError(m.execute("f 0 FFFFFF 1")));
        CHECK(isError(m.execute("h 0 FFFFFF 1")));
        CHECK(isError(m.execute("t 0 FFFFF 2000000")) == false);  // exactly 1 MiB is fine
        CHECK(isError(m.execute("t 0 100000 200000")));
        CHECK(isError(m.execute("sv x 0 FFFFFF")));
    }

    // ---- help, quitting, empty lines ---------------------------------------------------------------------------------------------------------
    {
        Monitor m;
        CHECK(has(m.execute("?"), "hexadecimal"));
        CHECK(has(m.execute("help"), "assemble"));
        CHECK_EQ(m.execute(""), std::string(""));
        CHECK_EQ(m.execute("   "), std::string(""));
        CHECK(!m.finished());
        m.execute("q");
        CHECK(m.finished());
        CHECK(has(Monitor::banner(), "monitor"));
    }

    // ---- garbage never crashes it -----------------------------------------------------------------------------------------------------------------
    {
        std::mt19937 rng(4711);
        const char* words[] = {"d", "m", "m.w", "e", "e.l", "f", "c", "t", "h", "a", "r", "n", "o", "u", "g", "ru", "b", "bc", "w", "limit",
                               "sym", "int", "reset", "machine", "ck", "rb", "io", "l", "asm", "sv", "symload", "symsave", "rom"};
        const char* args[] = {"0", "1000", "FFFFFF", "FFFFFFFF", "-1", "100.", "$", "%", "\"", "\"x\"", "@", "@pc", "@zz", "a=b", "pc=", "d0=1",
                              "+", "1+", "(", ")", "nosuch", "20", "3000", "zzzz", "0x", "'", "dead", "1000000000000000000000"};
        MonitorOptions fuzzOptions;
        fuzzOptions.maxInstructions = 500;
        for (int i = 0; i < 5000; ++i) {
            Monitor m(fuzzOptions);
            std::string line = words[rng() % 33];
            const int n = static_cast<int>(rng() % 4);
            for (int k = 0; k < n; ++k) line += std::string(" ") + args[rng() % 28];
            if (rng() % 7 == 0) line = "a 1000";
            const std::string out = m.execute(line);
            (void)out;
            m.execute(std::string(args[rng() % 28]));
            m.execute("");
        }
        CHECK(true);
    }

    return finish("monitor");
}
