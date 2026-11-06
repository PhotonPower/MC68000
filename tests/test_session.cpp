#include "m68k/session.hpp"

#include <cstdio>

#include "m68k/amiga.hpp"
#include "m68k/assembler.hpp"
#include "m68k/image_io.hpp"
#include "m68k/snapshot.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

Json call(Session& s, const std::string& line) {
    const std::string reply = s.handleLine(line);
    return Json::parse(reply);
}
bool ok(const Json& j) {
    const Json* o = j.find("ok");
    return o && o->asBool();
}
std::string str(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isString() ? v->asString() : "<missing>";
}
std::string regOf(const Json& j, const char* reg) {
    const Json* regs = j.find("regs");
    return regs ? str(*regs, reg) : "<no regs>";
}
std::string error(const Json& j) { return str(j, "error"); }

const char* kSum =
    "        ORG $1000\\n"
    "start:  MOVEQ #0,D0\\n"
    "        MOVE.W #9,D1\\n"
    "loop:   ADD.W D1,D0\\n"
    "        MOVE.W D0,result\\n"
    "        DBRA D1,loop\\n"
    "        STOP #$2700\\n"
    "result: DC.W 0\\n";

std::string asmLine(const std::string& src, const std::string& extra = "") {
    return "{\"cmd\":\"asm\",\"source\":\"" + src + "\"" + extra + "}";
}

struct HunkBuilder {
    std::vector<Byte> out;
    void u32(std::uint32_t v) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<Byte>(v >> (8 * i)));
    }
};

}  // namespace

int main() {
    // ---- JSON ---------------------------------------------------------------------------------------------
    {
        const Json j = Json::parse("{\"a\":1,\"b\":[true,null,\"x\\n\\u0041\"],\"c\":{\"d\":-2.5}}");
        CHECK(j.isObject());
        CHECK_EQ(j.find("a")->asInt(), std::int64_t{1});
        CHECK_EQ(j.find("b")->items().size(), std::size_t{3});
        CHECK_EQ(j.find("b")->items()[2].asString(), std::string("x\nA"));
        CHECK(j.find("c")->find("d")->asDouble() == -2.5);
        CHECK_EQ(j.dump(), std::string("{\"a\":1,\"b\":[true,null,\"x\\nA\"],\"c\":{\"d\":-2.5}}"));
        bool threw = false;
        try { (void)Json::parse("{\"a\":"); } catch (const JsonError&) { threw = true; }
        CHECK(threw);
    }

    // ---- assemble, run, inspect ---------------------------------------------------------------------------
    {
        Session s;
        Json r = call(s, asmLine(kSum, ",\"entry\":\"start\""));
        CHECK(ok(r));
        CHECK_EQ(str(r, "start"), std::string("0x001000"));
        CHECK_EQ(str(*r.find("symbols"), "result"), std::string("0x00001014"));
        r = call(s, "{\"cmd\":\"run\"}");
        CHECK(ok(r));
        CHECK_EQ(str(r, "stop"), std::string("stopped"));
        CHECK_EQ(r.find("instructions")->asInt(), std::int64_t{32});
        CHECK_EQ(regOf(r, "D0"), std::string("0x0000002D"));
        CHECK_EQ(regOf(r, "PC"), std::string("0x001014"));
        r = call(s, "{\"cmd\":\"read\",\"addr\":\"result\",\"len\":2}");
        CHECK_EQ(str(r, "hex"), std::string("002D"));
        r = call(s, "{\"cmd\":\"regs\"}");
        CHECK_EQ(str(*r.find("regs"), "flags").size(), std::size_t{5});
        CHECK(r.find("regs")->find("S")->asBool());
        CHECK_EQ(r.find("regs")->find("IPL")->asInt(), std::int64_t{7});
        // disassembly uses the labels
        r = call(s, "{\"cmd\":\"dis\",\"addr\":\"start\",\"count\":4}");
        CHECK(ok(r));
        CHECK_EQ(str(r.find("lines")->items()[0], "label"), std::string("start"));
        CHECK_EQ(str(r.find("lines")->items()[3], "text"), std::string("MOVE.W D0,result.W"));
    }

    // ---- arguments: numbers, expressions, registers, symbols --------------------------------------------------
    {
        Session s;
        CHECK(ok(call(s, asmLine("        ORG $2000\\ntbl:    DC.B 1,2,3,4\\n"))));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"tbl+1\",\"len\":2}"), "hex"), std::string("0203"));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":8192,\"len\":1}"), "hex"), std::string("01"));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"$2002\",\"len\":1}"), "hex"), std::string("03"));
        CHECK(ok(call(s, "{\"cmd\":\"set\",\"regs\":{\"A0\":\"tbl\",\"D1\":\"0x10\",\"A2\":\"A0+2\"}}")));
        Json r = call(s, "{\"cmd\":\"regs\"}");
        CHECK_EQ(str(*r.find("regs"), "A2"), std::string("0x00002002"));
        CHECK(!ok(call(s, "{\"cmd\":\"read\",\"addr\":\"nosuchsymbol\"}")));
        CHECK(error(call(s, "{\"cmd\":\"read\",\"addr\":\"nosuchsymbol\"}")).find("unknown symbol") != std::string::npos);
        CHECK(!ok(call(s, "{\"cmd\":\"set\",\"regs\":{\"Q9\":1}}")));
        CHECK(ok(call(s, "{\"cmd\":\"write\",\"addr\":\"tbl\",\"hex\":\"AA BB\"}")));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"tbl\",\"len\":3}"), "hex"), std::string("AABB03"));
        CHECK(ok(call(s, "{\"cmd\":\"write\",\"addr\":\"tbl\",\"bytes\":[1,2,\"3\"]}")));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"tbl\",\"len\":3}"), "hex"), std::string("010203"));
        CHECK(!ok(call(s, "{\"cmd\":\"write\",\"addr\":0,\"hex\":\"ABC\"}")));
        CHECK(!ok(call(s, "{\"cmd\":\"read\",\"addr\":0,\"len\":100000}")));
    }

    // ---- errors never crash the server ---------------------------------------------------------------------------
    {
        Session s;
        CHECK(!ok(call(s, "not json")));
        CHECK(!ok(call(s, "[1,2]")));
        CHECK(!ok(call(s, "{}")));
        CHECK(!ok(call(s, "{\"cmd\":\"fly\"}")));
        CHECK(!ok(call(s, "{\"cmd\":5}")));
        CHECK(!ok(call(s, "{\"cmd\":\"read\"}")));
        // a failed asm reports every error with its source line and changes nothing
        const Json bad = call(s, asmLine("        ORG $3000\\n        NOP\\n        BOGUS D0\\n        MOVE.W #1,D0,D1\\n"));
        CHECK(!ok(bad));
        CHECK_EQ(bad.find("errors")->items().size(), std::size_t{2});
        CHECK_EQ(str(bad.find("errors")->items()[0], "source"), std::string("        BOGUS D0"));
        CHECK_EQ(bad.find("errors")->items()[0].find("line")->asInt(), std::int64_t{3});
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0x3000\",\"len\":2}"), "hex"), std::string("0000"));
        // limits stop endless loops
        CHECK(ok(call(s, asmLine("        ORG $1000\\nspin:   BRA.S spin\\n", ",\"entry\":\"spin\""))));
        const Json lim = call(s, "{\"cmd\":\"run\",\"max_instructions\":500}");
        CHECK_EQ(str(lim, "stop"), std::string("instruction_limit"));
        CHECK_EQ(lim.find("instructions")->asInt(), std::int64_t{500});
        CHECK(ok(call(s, "{\"cmd\":\"help\"}")));
    }

    // ---- breakpoints, watchpoints, stepping, calls ---------------------------------------------------------------------
    {
        Session s;
        const char* prog =
            "        ORG $1000\\n"
            "main:   MOVEQ #0,D0\\n"
            "        BSR inc\\n"
            "        BSR inc\\n"
            "        MOVE.W D0,$2000.W\\n"
            "        STOP #$2700\\n"
            "inc:    ADDQ.W #1,D0\\n"
            "        RTS\\n";
        CHECK(ok(call(s, asmLine(prog, ",\"entry\":\"main\""))));
        CHECK(ok(call(s, "{\"cmd\":\"break\",\"addr\":\"inc\"}")));
        Json r = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r, "stop"), std::string("breakpoint"));
        CHECK_EQ(str(r, "pc"), std::string("0x00100E"));
        r = call(s, "{\"cmd\":\"breaks\"}");
        CHECK_EQ(r.find("breakpoints")->items().size(), std::size_t{1});
        CHECK(ok(call(s, "{\"cmd\":\"unbreak\",\"addr\":\"inc\"}")));
        r = call(s, "{\"cmd\":\"run_until_return\"}");
        CHECK_EQ(str(r, "stop"), std::string("returned"));
        CHECK_EQ(regOf(r, "D0"), std::string("0x00000001"));
        // step_over runs the whole subroutine
        r = call(s, "{\"cmd\":\"step_over\"}");
        CHECK_EQ(regOf(r, "D0"), std::string("0x00000002"));
        // watchpoint on the store
        CHECK(ok(call(s, "{\"cmd\":\"watch\",\"addr\":\"0x2000\",\"len\":2,\"kind\":\"write\"}")));
        r = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r, "stop"), std::string("watchpoint"));
        CHECK_EQ(str(*r.find("watch"), "addr"), std::string("0x002000"));
        // run until an address, single steps with a trace
        CHECK(ok(call(s, "{\"cmd\":\"set\",\"regs\":{\"PC\":\"main\"}}")));
        r = call(s, "{\"cmd\":\"run\",\"until\":\"inc\"}");
        CHECK_EQ(str(r, "stop"), std::string("address"));
        r = call(s, "{\"cmd\":\"step\",\"n\":2,\"trace\":true}");
        CHECK_EQ(r.find("trace")->items().size(), std::size_t{2});
        CHECK_EQ(str(r.find("trace")->items()[0], "text"), std::string("ADDQ.W #1,D0"));
        CHECK_EQ(str(r.find("trace")->items()[1], "text"), std::string("RTS"));
        // call: a routine used like a function
        r = call(s, "{\"cmd\":\"call\",\"addr\":\"inc\",\"regs\":{\"D0\":41}}");
        CHECK(r.find("returned")->asBool());
        CHECK_EQ(str(r, "stop"), std::string("returned"));
        CHECK_EQ(regOf(r, "D0"), std::string("0x0000002A"));
        // a faulting routine is reported, not a crash
        CHECK(ok(call(s, "{\"cmd\":\"clear_breaks\"}")));
    }
    {
        Session s;  // exceptions are counted and reported
        CHECK(ok(call(s, asmLine("        ORG $1000\\n        MOVEQ #1,D0\\n        TRAP #5\\n        NOP\\n", ",\"entry\":4096"))));
        CHECK(ok(call(s, "{\"cmd\":\"write\",\"addr\":\"0x94\",\"hex\":\"00003000\"}")));  // vector 37 -> $3000
        CHECK(ok(call(s, "{\"cmd\":\"write\",\"addr\":\"0x3000\",\"hex\":\"4E73\"}")));    // RTE
        const Json r = call(s, "{\"cmd\":\"step\",\"n\":2}");
        CHECK_EQ(r.find("exceptions")->asInt(), std::int64_t{1});
        CHECK_EQ(r.find("last_exception")->asInt(), std::int64_t{37});
        CHECK_EQ(str(r, "pc"), std::string("0x003000"));
    }

    // ---- checkpoints: roll back and run again, bit-identical ------------------------------------------------------------
    {
        Session s;
        CHECK(ok(call(s, asmLine(kSum, ",\"entry\":\"start\""))));
        CHECK(ok(call(s, "{\"cmd\":\"checkpoint\",\"name\":\"fresh\"}")));
        const Json first = call(s, "{\"cmd\":\"run\"}");
        const Json mem1 = call(s, "{\"cmd\":\"read\",\"addr\":\"0x1000\",\"len\":32}");
        const Json diff = call(s, "{\"cmd\":\"diff\",\"name\":\"fresh\"}");
        CHECK_EQ(diff.find("changed_bytes")->asInt(), std::int64_t{1});
        CHECK_EQ(str(diff.find("ranges")->items()[0], "addr"), std::string("0x001015"));
        CHECK(ok(call(s, "{\"cmd\":\"rollback\",\"name\":\"fresh\"}")));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"result\",\"len\":2}"), "hex"), std::string("0000"));
        const Json second = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(first.dump(), second.dump());                       // same registers, same counters
        CHECK_EQ(mem1.dump(), call(s, "{\"cmd\":\"read\",\"addr\":\"0x1000\",\"len\":32}").dump());
        CHECK_EQ(call(s, "{\"cmd\":\"checkpoints\"}").find("checkpoints")->items().size(), std::size_t{1});
        CHECK(ok(call(s, "{\"cmd\":\"drop\",\"name\":\"fresh\"}")));
        CHECK(!ok(call(s, "{\"cmd\":\"rollback\",\"name\":\"fresh\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"reset\",\"memory\":true}")));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0x1000\",\"len\":2}"), "hex"), std::string("0000"));
    }

    // ---- state ------------------------------------------------------------------------------------------------------
    {
        Session s;
        CHECK(ok(call(s, "{\"cmd\":\"set_state\",\"state\":{\"SR\":\"0x0004\",\"D3\":7,\"USP\":\"0x6000\",\"SSP\":\"0x7000\",\"PC\":\"0x1234\"}}")));
        const Json r = call(s, "{\"cmd\":\"state\"}");
        const Json& st = *r.find("state");
        CHECK_EQ(str(st, "D3"), std::string("0x00000007"));
        CHECK_EQ(str(st, "PC"), std::string("0x001234"));
        CHECK_EQ(str(st, "A7"), std::string("0x00006000"));  // user mode: A7 is the USP
        CHECK_EQ(str(st, "SSP"), std::string("0x00007000"));
        CHECK(!st.find("S")->asBool());
        CHECK_EQ(str(st, "flags"), std::string("..Z.."));
        // interrupts through the protocol
        CHECK(ok(call(s, "{\"cmd\":\"interrupt\",\"level\":3}")));
        CHECK(!ok(call(s, "{\"cmd\":\"interrupt\",\"level\":9}")));
    }

    // ---- file access is off unless the operator allows it ------------------------------------------------------------------
    {
        Session s;
        CHECK(!ok(call(s, "{\"cmd\":\"load_file\",\"file\":\"x.bin\",\"addr\":0}")));
        CHECK(error(call(s, "{\"cmd\":\"load_file\",\"file\":\"x.bin\",\"addr\":0}")).find("--allow-files") != std::string::npos);
        CHECK(!ok(call(s, "{\"cmd\":\"save_state\",\"file\":\"x.snap\"}")));
        CHECK(!ok(call(s, "{\"cmd\":\"load_rom\",\"file\":\"x.rom\"}")));
    }
    {
        SessionOptions opt;
        opt.allowFiles = true;
        Session s(opt);
        writeBinaryFile("m68k_session_raw.tmp", {0x4E, 0x75, 0x4E, 0x71});
        Json r = call(s, "{\"cmd\":\"load_file\",\"file\":\"m68k_session_raw.tmp\",\"addr\":\"0x4000\"}");
        CHECK(ok(r));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0x4000\",\"len\":4}"), "hex"), std::string("4E754E71"));
        CHECK(!ok(call(s, "{\"cmd\":\"load_file\",\"file\":\"m68k_session_raw.tmp\"}")));  // raw needs an address
        std::remove("m68k_session_raw.tmp");
        CHECK(!ok(call(s, "{\"cmd\":\"load_file\",\"file\":\"does_not_exist.bin\",\"addr\":0}")));

        // S-records set the PC from the termination record
        writeTextFile("m68k_session.s19", formatSRecords({{0x5000, {0x70, 0x07, 0x4E, 0x75}}}, 0x5000));
        r = call(s, "{\"cmd\":\"load_file\",\"file\":\"m68k_session.s19\"}");
        CHECK(ok(r));
        CHECK_EQ(regOf(r, "PC"), std::string("0x005000"));
        std::remove("m68k_session.s19");

        // an AmigaDOS program: relocated, symbols become usable names, ready to run
        HunkBuilder h;
        h.u32(0x3F3); h.u32(0); h.u32(1); h.u32(0); h.u32(0); h.u32(2);
        h.u32(0x3E9); h.u32(2);
        for (const unsigned b : {0x70U, 0x2AU, 0x4EU, 0x75U, 0x4EU, 0x71U, 0x4EU, 0x71U}) h.out.push_back(static_cast<Byte>(b));  // MOVEQ #42,D0 ; RTS
        h.u32(0x3F0); h.u32(1); h.u32(0x6D61696E); h.u32(0); h.u32(0);                              // symbol "main" at offset 0
        h.u32(0x3F2);
        writeBinaryFile("m68k_session.hunk", h.out);
        r = call(s, "{\"cmd\":\"load_file\",\"file\":\"m68k_session.hunk\"}");
        CHECK(ok(r));
        CHECK_EQ(str(r, "entry"), std::string("0x020000"));
        CHECK_EQ(str(*r.find("symbols"), "main"), std::string("0x00020000"));
        CHECK_EQ(regOf(r, "PC"), std::string("0x020000"));
        r = call(s, "{\"cmd\":\"call\",\"addr\":\"main\"}");
        CHECK(r.find("returned")->asBool());
        CHECK_EQ(regOf(r, "D0"), std::string("0x0000002A"));
        std::remove("m68k_session.hunk");

        // snapshot files
        CHECK(ok(call(s, "{\"cmd\":\"save_state\",\"file\":\"m68k_session.snap\"}")));
        const Json before = call(s, "{\"cmd\":\"regs\"}");
        CHECK(ok(call(s, "{\"cmd\":\"reset\",\"memory\":true}")));
        CHECK(ok(call(s, "{\"cmd\":\"load_state\",\"file\":\"m68k_session.snap\"}")));
        CHECK_EQ(before.find("regs")->dump(), call(s, "{\"cmd\":\"regs\"}").find("regs")->dump());
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0x020000\",\"len\":4}"), "hex"), std::string("702A4E75"));
        std::remove("m68k_session.snap");
    }

    // ---- the Amiga 500 machine -----------------------------------------------------------------------------------------------
    {
        Session s;
        CHECK(ok(call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\"}")));
        // a program writes custom registers, CIA registers and a wild address
        CHECK(ok(call(s, asmLine(
            "        ORG $1000\\n"
            "start:  MOVE.W #$0F00,$DFF180.L\\n"
            "        MOVE.W #$8200,$DFF096.L\\n"
            "        MOVE.B #$03,$BFE201.L\\n"
            "        MOVE.L #$DEADBEEF,$300000.L\\n"
            "        MOVE.W $DFF180.L,D0\\n"
            "        MOVE.L #$12345678,$2000.W\\n"
            "        STOP #$2700\\n", ",\"entry\":\"start\""))));
        Json r = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r, "stop"), std::string("stopped"));
        CHECK_EQ(regOf(r, "D0"), std::string("0x00000F00"));  // custom registers remember what was written
        r = call(s, "{\"cmd\":\"io\"}");
        const std::vector<Json>& w = r.find("output")->items();
        CHECK_EQ(w.size(), std::size_t{4});
        CHECK_EQ(str(w[0], "what"), std::string("COLOR00"));
        CHECK_EQ(str(w[0], "value"), std::string("0x0F00"));
        CHECK_EQ(str(w[0], "pc"), std::string("0x001000"));
        CHECK_EQ(str(w[1], "what"), std::string("DMACON"));
        CHECK_EQ(str(w[2], "kind"), std::string("cia"));
        CHECK_EQ(str(w[3], "kind"), std::string("unmapped"));
        CHECK_EQ(call(s, "{\"cmd\":\"io\"}").find("output")->items().size(), std::size_t{0});  // the log is cleared by reading it
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0x2000\",\"len\":4}"), "hex"), std::string("12345678"));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0x300000\",\"len\":4}"), "hex"), std::string("00000000"));
        // programs and the debugger cannot write ROM or unmapped space
        CHECK(!ok(call(s, "{\"cmd\":\"write\",\"addr\":\"0xF80000\",\"hex\":\"00\"}")));
        CHECK(!ok(call(s, asmLine("        ORG $F80000\\n        NOP\\n"))));
        // the stack starts at the top of chip RAM
        CHECK_EQ(str(*call(s, "{\"cmd\":\"regs\"}").find("regs"), "A7"), std::string("0x0007FFF0"));
        // checkpoints cover the device registers too
        CHECK(ok(call(s, "{\"cmd\":\"checkpoint\",\"name\":\"a\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"write\",\"addr\":\"0xDFF180\",\"hex\":\"0123\"}")));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0xDFF180\",\"len\":2}"), "hex"), std::string("0123"));
        CHECK(ok(call(s, "{\"cmd\":\"rollback\",\"name\":\"a\"}")));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0xDFF180\",\"len\":2}"), "hex"), std::string("0F00"));
        // other configurations
        CHECK(ok(call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"chip_kb\":1024,\"fast_kb\":512}")));
        CHECK(ok(call(s, "{\"cmd\":\"write\",\"addr\":\"0x300000\",\"hex\":\"11\"}")) == false);  // fast RAM is at $200000
        CHECK(ok(call(s, "{\"cmd\":\"write\",\"addr\":\"0x200000\",\"hex\":\"11\"}")));
        CHECK(!ok(call(s, "{\"cmd\":\"machine\",\"type\":\"c64\"}")));
        CHECK(!ok(call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"chip_kb\":8}")));
        CHECK(ok(call(s, "{\"cmd\":\"machine\",\"type\":\"generic\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"write\",\"addr\":\"0x300000\",\"hex\":\"11\"}")));  // generic: everything is RAM
    }

    // ---- ROM overlay, driven by a program like the Kickstart does it ------------------------------------------------------------------
    {
        AssemblerOptions opt;
        opt.origin = 0xF80008;
        const AssemblyResult prog = assemble(
            "        MOVE.L 4.W,D6\n"                    // overlay on: address 4 is the ROM's reset PC
            "        MOVE.B #1,$BFE201.L\n"              // CIA-A port A bit 0 becomes an output ...
            "        MOVE.B #0,$BFE001.L\n"              // ... and is driven low: the overlay goes off
            "        MOVE.L #$11223344,4.W\n"            // address 4 is chip RAM now
            "        MOVE.L 4.W,D7\n"
            "        STOP #$2700\n",
            opt);
        CHECK(prog.ok());
        std::vector<Byte> rom(0x40000, 0);
        const std::vector<Byte> head = {0x00, 0x07, 0xF0, 0x00, 0x00, 0xF8, 0x00, 0x08};  // SSP $7F000, PC $F80008
        std::copy(head.begin(), head.end(), rom.begin());
        const std::vector<Byte> code = prog.flatten();
        std::copy(code.begin(), code.end(), rom.begin() + 8);

        Memory mem;
        Cpu cpu(mem);
        Amiga500 amiga(mem);
        amiga.install(cpu);
        amiga.loadRom(rom);
        amiga.reset(cpu);
        CHECK(amiga.overlay());
        CHECK_EQ(cpu.registers().a[7], Long{0x7F000});
        CHECK_EQ(cpu.registers().pc, Address{0xF80008});
        const RunResult r = cpu.run(100);
        CHECK(r.reason == StopReason::Stopped);
        CHECK_EQ(cpu.registers().d[6], Long{0x00F80008});
        CHECK_EQ(cpu.registers().d[7], Long{0x11223344});
        CHECK(!amiga.overlay());
        CHECK_EQ(amiga.readByte(0x7), Byte{0x44});
        CHECK_EQ(amiga.log().size(), std::size_t{2});
        // the ROM is mirrored and read-only
        CHECK_EQ(amiga.readByte(0xF80000), amiga.readByte(0xFC0000));
        CHECK(amiga.classify(0xF90000) == Amiga500::Region::Rom);
        Long v = 0;
        CHECK(amiga.write(0xF80000, Size::B, 0xFF));
        CHECK(amiga.read(0xF80000, Size::B, v));
        CHECK_EQ(v, Long{0});  // the write was dropped (and logged)
        // region map
        CHECK(amiga.classify(0x0) == Amiga500::Region::ChipRam);
        CHECK(amiga.classify(0x7FFFF) == Amiga500::Region::ChipRam);
        CHECK(amiga.classify(0x80000) == Amiga500::Region::Unmapped);
        CHECK(amiga.classify(0xDFF09A) == Amiga500::Region::Custom);
        CHECK(amiga.classify(0xBFE001) == Amiga500::Region::Cia);
        CHECK(amiga.classify(0xC00000) == Amiga500::Region::Unmapped);  // no slow RAM configured
        CHECK_EQ(Amiga500::customRegisterName(0xDFF09A), std::string("INTENA"));
        CHECK_EQ(Amiga500::customRegisterName(0xDFF19E), std::string("COLOR15"));
        CHECK_EQ(Amiga500::customRegisterName(0xDFF09B), std::string("INTENA"));  // either byte of the word
        CHECK_EQ(Amiga500::customRegisterName(0x1000), std::string(""));
    }

    // ---- snapshots and memory differences ------------------------------------------------------------------------------------------------
    {
        Memory mem;
        Cpu cpu(mem);
        cpu.registers().d[3] = 0xCAFE;
        cpu.registers().pc = 0x1234;
        mem.write32(0x1000, 0xDEADBEEF);
        mem.write8(0xABCDEF, 0x42);
        Snapshot snap = capture(cpu);
        const std::vector<Byte> bytes = serialize(snap);
        const Snapshot back = deserializeSnapshot(bytes);
        CHECK_EQ(back.cpu.regs.d[3], Long{0xCAFE});
        CHECK_EQ(back.cpu.regs.pc, Address{0x1234});
        CHECK_EQ(back.memory.read32(0x1000), Long{0xDEADBEEF});
        CHECK_EQ(back.memory.read8(0xABCDEF), Byte{0x42});
        CHECK_EQ(back.memory.mappedPages(), std::size_t{2});  // only pages with data are stored
        mem.write8(0x1000, 0);
        restore(cpu, back);
        CHECK_EQ(mem.read32(0x1000), Long{0xDEADBEEF});
        // damaged files are rejected
        std::vector<Byte> bad = bytes;
        bad[0] = 'X';
        bool threw = false;
        try { (void)deserializeSnapshot(bad); } catch (const IoError&) { threw = true; }
        CHECK(threw);
        for (std::size_t n = 0; n < 200 && n < bytes.size(); ++n) {
            const std::vector<Byte> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
            threw = false;
            try { (void)deserializeSnapshot(cut); } catch (const IoError&) { threw = true; }
            CHECK(threw);
        }
        std::vector<Byte> extra = bytes;
        extra.push_back(0);
        threw = false;
        try { (void)deserializeSnapshot(extra); } catch (const IoError&) { threw = true; }
        CHECK(threw);

        // memory differences: nearby changes are merged, far ones are not
        Memory a, b;
        b.write8(0x100, 1);
        b.write8(0x104, 2);     // gap of 3: merged with the first
        b.write8(0x200, 3);
        b.write8(0x20000, 4);   // another page
        std::size_t total = 0;
        const std::vector<MemoryChange> d = diffMemory(a, b, 8, 1000, &total);
        CHECK_EQ(total, std::size_t{4});
        CHECK_EQ(d.size(), std::size_t{3});
        CHECK_EQ(d[0].address, Address{0x100});
        CHECK_EQ(d[0].after.size(), std::size_t{5});
        CHECK_EQ(d[0].after[4], Byte{2});
        CHECK_EQ(d[0].before[4], Byte{0});
        CHECK_EQ(d[2].address, Address{0x20000});
        CHECK_EQ(diffMemory(a, b, 8, 1).size(), std::size_t{1});  // range limit
        CHECK(diffMemory(a, a).empty());
    }

    return finish("session");
}
