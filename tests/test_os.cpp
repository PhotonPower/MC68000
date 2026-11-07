#include <cstdio>

#include "m68k/amiga_os.hpp"
#include "m68k/assembler.hpp"
#include "m68k/session.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

Json call(Session& s, const std::string& line) { return Json::parse(s.handleLine(line)); }
bool ok(const Json& j) { return j.find("ok") && j.find("ok")->asBool(); }
std::string str(const Json& j, const char* k) {
    const Json* v = j.find(k);
    return v && v->isString() ? v->asString() : "<missing>";
}
std::string regOf(const Json& j, const char* r) { return str(*j.find("regs"), r); }

std::string escape(const std::string& t) {
    std::string out;
    for (const char c : t) {
        if (c == '\n') out += "\\n";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

Json load(Session& s, const std::string& source) {
    return call(s, "{\"cmd\":\"asm\",\"source\":\"" + escape(source) + "\",\"entry\":\"start\"}");
}

const char* kHello =
    "        ORG $20000\n"
    "start:  MOVEA.L 4.W,A6\n"
    "        LEA dosname(PC),A1\n"
    "        MOVEQ #0,D0\n"
    "        JSR -552(A6)\n"                // OpenLibrary
    "        MOVE.L D0,D7\n"
    "        BEQ.S fail\n"
    "        MOVEA.L D0,A6\n"
    "        JSR -60(A6)\n"                 // Output
    "        MOVE.L D0,D1\n"
    "        LEA msg(PC),A0\n"
    "        MOVE.L A0,D2\n"
    "        MOVEQ #6,D3\n"
    "        JSR -48(A6)\n"                 // Write
    "        MOVEA.L 4.W,A6\n"
    "        MOVE.L #100,D0\n"
    "        MOVE.L #$10001,D1\n"           // MEMF_PUBLIC | MEMF_CLEAR
    "        JSR -198(A6)\n"                // AllocMem
    "        MOVE.L D0,D6\n"
    "        MOVEA.L D0,A1\n"
    "        MOVE.L #100,D0\n"
    "        JSR -210(A6)\n"                // FreeMem
    "        MOVEQ #7,D0\n"
    "        RTS\n"
    "fail:   MOVEQ #-1,D0\n"
    "        RTS\n"
    "dosname: DC.B \"dos.library\",0\n"
    "msg:    DC.B \"Hello\",10\n";

}  // namespace

int main() {
    // ---- a program that talks to the OS --------------------------------------------------------------------------
    {
        Session s;
        CHECK(ok(call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}")));
        CHECK(ok(load(s, kHello)));
        // ExecBase at address 4, a jump table below it
        const Json eb = call(s, "{\"cmd\":\"read\",\"addr\":4,\"len\":4}");
        CHECK_EQ(str(eb, "hex"), std::string("0007AE00"));
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0x7AE00-552\",\"len\":2}"), "hex"), std::string("4EF9"));   // JMP
        Json r = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r, "stop"), std::string("exit"));
        CHECK(str(r, "message").find("D0 = 7") != std::string::npos);
        CHECK_EQ(regOf(r, "D0"), std::string("0x00000007"));
        CHECK_EQ(regOf(r, "D6"), std::string("0x00040000"));   // AllocMem: the first block of the heap
        CHECK(regOf(r, "D7") != "0x00000000");                  // a base for dos.library
        CHECK_EQ(str(call(s, "{\"cmd\":\"console\"}"), "text"), std::string("Hello\n"));
        CHECK_EQ(str(call(s, "{\"cmd\":\"console\"}"), "text"), std::string(""));   // reading clears it
        r = call(s, "{\"cmd\":\"libcalls\"}");
        const std::vector<Json>& calls = r.find("calls")->items();
        CHECK_EQ(calls.size(), std::size_t{5});
        CHECK_EQ(str(calls[0], "function"), std::string("OpenLibrary"));
        CHECK_EQ(str(calls[0], "note"), std::string("dos.library"));
        // the call site is the JSR instruction, the caller its return address (the instruction behind it)
        CHECK_EQ(str(calls[0], "call_site"), std::string("0x02000A"));
        CHECK_EQ(str(calls[0], "caller"), std::string("0x02000E"));
        CHECK_EQ(str(calls[1], "call_site"), std::string("0x020014"));              // JSR -60(A6) (Output): MOVE.L, BEQ.S and MOVEA.L (2 bytes each) lie between the two calls
        CHECK_EQ(calls[0].find("offset")->asInt(), std::int64_t{-552});
        CHECK(calls[0].find("emulated")->asBool());
        CHECK_EQ(str(calls[1], "function"), std::string("Output"));
        CHECK_EQ(str(calls[2], "function"), std::string("Write"));
        CHECK_EQ(str(calls[2], "library"), std::string("dos.library"));
        CHECK_EQ(str(*calls[3].find("args"), "D0"), std::string("0x00000064"));
        CHECK_EQ(str(calls[3], "result"), std::string("0x00040000"));
        CHECK_EQ(str(calls[4], "function"), std::string("FreeMem"));
        CHECK_EQ(call(s, "{\"cmd\":\"libcalls\"}").find("calls")->items().size(), std::size_t{0});
        // the memory the program got was zero filled and is free again
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"0x40000\",\"len\":4}"), "hex"), std::string("00000000"));
    }

    // ---- memory allocation ------------------------------------------------------------------------------------------
    {
        Session s;
        call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        // A6 = ExecBase; allocate, allocate, free the first, allocate again (first fit), AllocVec/FreeVec, out of memory, size 0
        const std::string prog =
            "        ORG $20000\n"
            "start:  MOVEA.L 4.W,A6\n"
            "        MOVEQ #100,D0\n        MOVEQ #0,D1\n        JSR -198(A6)\n        MOVE.L D0,D2\n"        // block 1
            "        MOVEQ #100,D0\n        MOVEQ #0,D1\n        JSR -198(A6)\n        MOVE.L D0,D3\n"        // block 2
            "        MOVEA.L D2,A1\n        MOVEQ #100,D0\n        JSR -210(A6)\n"                              // free block 1
            "        MOVEQ #50,D0\n        MOVEQ #0,D1\n        JSR -198(A6)\n        MOVE.L D0,D4\n"         // reuses the hole
            "        MOVEQ #32,D0\n        MOVEQ #0,D1\n        JSR -684(A6)\n        MOVE.L D0,D5\n"         // AllocVec
            "        MOVEA.L D5,A1\n        JSR -690(A6)\n"                                                    // FreeVec
            "        MOVE.L #$7FFFFFFF,D0\n        MOVEQ #0,D1\n        JSR -198(A6)\n        MOVE.L D0,D6\n"  // too much
            "        MOVEQ #0,D0\n        MOVEQ #0,D1\n        JSR -198(A6)\n        MOVE.L D0,D7\n"         // size 0
            "        MOVEA.L #$12345,A1\n        MOVEQ #4,D0\n        JSR -210(A6)\n"                          // FreeMem of nonsense
            "        RTS\n";
        CHECK(ok(load(s, prog)));
        const Json r = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r, "stop"), std::string("exit"));
        CHECK_EQ(regOf(r, "D2"), std::string("0x00040000"));
        CHECK_EQ(regOf(r, "D3"), std::string("0x00040068"));   // 100 bytes round up to 104 = $68
        CHECK_EQ(regOf(r, "D4"), std::string("0x00040000"));   // the hole is used again
        CHECK(regOf(r, "D5") != "0x00000000");
        CHECK_EQ(regOf(r, "D5"), std::string("0x00040040"));   // AllocVec: first fit in the hole behind block 4, returns block + 8
        CHECK_EQ(regOf(r, "D6"), std::string("0x00000000"));   // out of memory
        CHECK_EQ(regOf(r, "D7"), std::string("0x00000000"));   // zero bytes
        const Json log = call(s, "{\"cmd\":\"libcalls\"}");
        const std::vector<Json>& calls = log.find("calls")->items();
        CHECK_EQ(str(calls.back(), "note"), std::string("not an allocated block"));
        bool sawOom = false;
        for (const Json& c : calls) sawOom = sawOom || str(c, "note") == "out of memory";
        CHECK(sawOom);
    }

    // ---- calls that are not emulated are logged, never silently wrong -------------------------------------------------------
    {
        Session s;
        call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        const std::string prog =
            "        ORG $20000\n"
            "start:  MOVEA.L 4.W,A6\n"
            "        LEA gfx(PC),A1\n        MOVEQ #33,D0\n        JSR -552(A6)\n        MOVEA.L D0,A6\n"
            "        MOVEQ #5,D1\n        MOVEQ #9,D2\n        JSR -222(A6)\n"            // unknown graphics.library function
            "        MOVE.L D0,D5\n"
            "        MOVEA.L 4.W,A6\n        JSR -318(A6)\n"                             // exec Wait: known name, not emulated
            "        MOVEQ #1,D0\n        MOVEA.L 4.W,A6\n        MOVEQ #0,D1\n        SUBA.L A1,A1\n        JSR -294(A6)\n"  // FindTask(0)
            "        MOVE.L D0,D4\n"
            "        RTS\n"
            "gfx:    DC.B \"graphics.library\",0\n";
        CHECK(ok(load(s, prog)));
        const Json r = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r, "stop"), std::string("exit"));
        CHECK_EQ(regOf(r, "D5"), std::string("0x00000000"));
        const std::vector<Json> calls = call(s, "{\"cmd\":\"libcalls\"}").find("calls")->items();
        CHECK_EQ(calls.size(), std::size_t{4});
        CHECK_EQ(str(calls[1], "library"), std::string("graphics.library"));
        CHECK(calls[1].find("function") == nullptr);          // no name for an unknown function
        CHECK(!calls[1].find("emulated")->asBool());
        CHECK_EQ(calls[1].find("offset")->asInt(), std::int64_t{-222});
        CHECK_EQ(str(*calls[1].find("args"), "D1"), std::string("0x00000005"));
        CHECK_EQ(str(calls[2], "function"), std::string("Wait"));
        CHECK(!calls[2].find("emulated")->asBool());          // known name, but not emulated
        CHECK_EQ(str(calls[3], "function"), std::string("FindTask"));
        // ThisTask: a process node
        const std::string task = regOf(r, "D4");
        CHECK(task != "0x00000000");
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"" + task + "+8\",\"len\":1}"), "hex"), std::string("0D"));
    }

    // ---- Exit, files that do not exist, many libraries ---------------------------------------------------------------------------
    {
        Session s;
        call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        const std::string prog =
            "        ORG $20000\n"
            "start:  MOVEA.L 4.W,A6\n        LEA dosname(PC),A1\n        MOVEQ #0,D0\n        JSR -552(A6)\n        MOVEA.L D0,A6\n"
            "        LEA fname(PC),A0\n        MOVE.L A0,D1\n        MOVE.L #1005,D2\n        JSR -30(A6)\n        MOVE.L D0,D6\n"  // Open
            "        JSR -132(A6)\n        MOVE.L D0,D5\n"                                                                           // IoErr
            "        MOVEQ #3,D1\n        JSR -144(A6)\n"                                                                           // Exit(3)
            "        MOVEQ #99,D4\n        RTS\n"
            "dosname: DC.B \"dos.library\",0\nfname: DC.B \"S:Startup\",0\n";
        CHECK(ok(load(s, prog)));
        const Json r = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r, "stop"), std::string("exit"));
        CHECK(str(r, "message").find("Exit(3)") != std::string::npos);
        CHECK_EQ(regOf(r, "D6"), std::string("0x00000000"));
        CHECK_EQ(regOf(r, "D5"), std::string("0x000000CD"));   // ERROR_OBJECT_NOT_FOUND = 205
        CHECK_EQ(regOf(r, "D4"), std::string("0x00000000"));   // never reached
        // stepping continues to say "exit" rather than running into the rest
        CHECK_EQ(str(call(s, "{\"cmd\":\"run\"}"), "stop"), std::string("exit"));

        // OpenLibrary: the same name gives the same base; the seventh library is refused
        Session t;
        call(t, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        std::string src = "        ORG $20000\nstart:  MOVEA.L 4.W,A6\n";
        for (int i = 0; i < 8; ++i)
            src += "        LEA n" + std::to_string(i % 7) + "(PC),A1\n        MOVEQ #0,D0\n        JSR -552(A6)\n";
        src += "        MOVEQ #0,D0\n        RTS\n";
        for (int i = 0; i < 7; ++i) src += "n" + std::to_string(i) + ":      DC.B \"lib" + std::to_string(i) + ".library\",0\n";
        CHECK(ok(load(t, src)));
        call(t, "{\"cmd\":\"run\"}");
        const std::vector<Json> calls = call(t, "{\"cmd\":\"libcalls\"}").find("calls")->items();
        CHECK_EQ(calls.size(), std::size_t{8});
        CHECK_EQ(str(calls[0], "result"), str(calls[7], "result"));   // "lib0.library" twice: the same base
        CHECK_EQ(str(calls[6], "result"), std::string("0x00000000"));  // exec + 6 = 7 libraries; "lib6" is the 7th name: refused
    }

    // ---- exec lists, CopyMem, AvailMem ------------------------------------------------------------------------------------------------------
    {
        Session s;
        call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        auto be = [](unsigned v) {
            char b[9];
            std::snprintf(b, sizeof b, "%08X", v);
            return std::string(b);
        };
        auto write = [&](unsigned addr, const std::string& hex) { call(s, "{\"cmd\":\"write\",\"addr\":" + std::to_string(addr) + ",\"hex\":\"" + hex + "\"}"); };
        const unsigned list = 0x30000;
        write(list, be(list + 4) + be(0) + be(list));                       // NewList: lh_Head = &lh_Tail, lh_Tail = 0, lh_TailPred = &lh_Head
        // nodes n0..n6 at $30100 + 0x20*i; ln_Pri at +9, ln_Name at +10 (a pointer to the strings at $30300 + 16*i)
        const int pri[7] = {0, 0, 0, 5, -2, 5, 3};
        const char* names[7] = {"zero", "one", "two", "three", "four", "five", "six"};
        for (int i = 0; i < 7; ++i) {
            const unsigned n = 0x30100 + 0x20 * static_cast<unsigned>(i);
            write(n + 9, std::string(1, "0123456789ABCDEF"[(static_cast<unsigned>(pri[i]) >> 4) & 15]) + "0123456789ABCDEF"[static_cast<unsigned>(pri[i]) & 15]);
            write(n + 10, be(0x30300 + 16 * static_cast<unsigned>(i)));
            std::string hex;
            for (const char* c = names[i]; *c; ++c) hex += be(static_cast<unsigned char>(*c)).substr(6);
            write(0x30300 + 16 * static_cast<unsigned>(i), hex + "00");
        }
        auto node = [](int i) { return 0x30100 + 0x20 * i; };
        // the program: every call loads its registers from absolute values
        std::string prog = "        ORG $20000\nstart:  MOVEA.L 4.W,A6\n";
        auto callFn = [&](int offset, std::initializer_list<std::pair<const char*, unsigned>> regs, const char* keep = nullptr) {
            for (const auto& [reg, value] : regs) prog += std::string("        MOVE.L #") + std::to_string(value) + "," + reg + "\n";
            prog += "        JSR " + std::to_string(offset) + "(A6)\n";
            if (keep) prog += std::string("        MOVE.L D0,") + keep + "\n";
        };
        callFn(-246, {{"A0", list}, {"A1", static_cast<unsigned>(node(1))}});                 // AddTail one
        callFn(-246, {{"A0", list}, {"A1", static_cast<unsigned>(node(2))}});                 // AddTail two
        callFn(-240, {{"A0", list}, {"A1", static_cast<unsigned>(node(0))}});                 // AddHead zero        -> zero one two
        callFn(-258, {{"A0", list}}, "D1");                                                    // RemHead             -> zero; list: one two
        callFn(-252, {{"A1", static_cast<unsigned>(node(1))}}, "D2");                          // Remove one          -> list: two
        callFn(-270, {{"A0", list}, {"A1", static_cast<unsigned>(node(3))}});                 // Enqueue three (5)   -> three two
        callFn(-270, {{"A0", list}, {"A1", static_cast<unsigned>(node(4))}});                 // Enqueue four (-2)   -> three two four
        callFn(-270, {{"A0", list}, {"A1", static_cast<unsigned>(node(5))}});                 // Enqueue five (5)    -> three five two four
        callFn(-276, {{"A0", list}, {"A1", 0x30300 + 16 * 2}}, "D3");                          // FindName "two"      -> node two
        callFn(-276, {{"A0", list}, {"A1", 0x30300 + 16 * 0}}, "D4");                          // FindName "zero"     -> 0 (it was removed)
        callFn(-264, {{"A0", list}}, "D5");                                                    // RemTail             -> four; list: three five two
        callFn(-234, {{"A0", list}, {"A1", static_cast<unsigned>(node(6))}, {"A2", static_cast<unsigned>(node(3))}});   // Insert six behind three
        callFn(-258, {{"A0", 0x30400}}, "D6");                                                 // RemHead of an empty list -> 0
        prog += "        RTS\n";
        write(0x30400, be(0x30404) + be(0) + be(0x30400));                                    // an empty list
        CHECK(ok(load(s, prog)));
        const Json r = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r, "stop"), std::string("exit"));
        auto u32 = [&](unsigned addr) {
            const Json j = call(s, "{\"cmd\":\"read\",\"addr\":" + std::to_string(addr) + ",\"len\":4}");
            return static_cast<unsigned>(std::stoul(str(j, "hex"), nullptr, 16));
        };
        CHECK_EQ(regOf(r, "D1"), "0x" + be(static_cast<unsigned>(node(0))));    // RemHead
        CHECK_EQ(regOf(r, "D2"), "0x" + be(static_cast<unsigned>(node(0))));    // Remove is void in the autodocs: D0 stays as RemHead left it
        CHECK_EQ(regOf(r, "D3"), "0x" + be(static_cast<unsigned>(node(2))));    // FindName "two"
        CHECK_EQ(regOf(r, "D4"), std::string("0x00000000"));                    // "zero" is not in the list
        CHECK_EQ(regOf(r, "D5"), "0x" + be(static_cast<unsigned>(node(4))));    // RemTail
        CHECK_EQ(regOf(r, "D6"), std::string("0x00000000"));                    // RemHead of an empty list
        // final order: three, six, five, two - walk the list forwards and backwards
        const int want[4] = {3, 6, 5, 2};
        unsigned cur = u32(list);
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(cur, static_cast<unsigned>(node(want[i])));
            CHECK_EQ(u32(cur + 4), i == 0 ? list : static_cast<unsigned>(node(want[i - 1])));    // ln_Pred
            cur = u32(cur);
        }
        CHECK_EQ(cur, list + 4);                                                  // the tail marker
        CHECK_EQ(u32(list + 8), static_cast<unsigned>(node(2)));                  // lh_TailPred

        // CopyMem copies overlapping blocks correctly; AvailMem sees allocations
        Session t;
        call(t, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        call(t, "{\"cmd\":\"write\",\"addr\":196608,\"hex\":\"0102030405060708\"}");
        const std::string p2 =
            "        ORG $20000\nstart:  MOVEA.L 4.W,A6\n"
            "        MOVEQ #0,D1\n        JSR -216(A6)\n        MOVE.L D0,D6\n"                       // AvailMem: all free
            "        MOVE.L #1000,D0\n        MOVEQ #0,D1\n        JSR -198(A6)\n"                // AllocMem 1000 -> 1000 rounds to 1000 (125 * 8)
            "        MOVEQ #0,D1\n        JSR -216(A6)\n        MOVE.L D0,D7\n"                       // AvailMem after
            "        MOVE.L #$20000,D1\n        JSR -216(A6)\n        MOVE.L D0,D5\n"             // MEMF_LARGEST
            "        MOVE.L #$30000,A0\n        MOVE.L #$30002,A1\n        MOVEQ #6,D0\n        JSR -624(A6)\n"   // CopyMem, overlapping, forwards
            "        MOVE.L #$30003,A0\n        MOVE.L #$30001,A1\n        MOVEQ #3,D0\n        JSR -630(A6)\n"   // CopyMemQuick, overlapping, backwards
            "        RTS\n";
        CHECK(ok(load(t, p2)));
        const Json r2 = call(t, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r2, "stop"), std::string("exit"));
        CHECK_EQ(std::stoul(regOf(r2, "D6"), nullptr, 16) - std::stoul(regOf(r2, "D7"), nullptr, 16), 1000UL);
        CHECK_EQ(std::stoul(regOf(r2, "D5"), nullptr, 16), std::stoul(regOf(r2, "D7"), nullptr, 16));    // one free block: the largest one is all of it
        CHECK_EQ(str(call(t, "{\"cmd\":\"read\",\"addr\":196608,\"len\":8}"), "hex"), std::string("0102030403040506"));   // 01..08, copy 6 bytes from +0 to +2: 01 02 01 02 03 04 05 06; then 3 bytes from +3 to +1: 01 02 03 04 03 04 05 06
    }

    // ---- MEMF_FAST on a machine without fast memory, minimum versions (checked against the NDK, exec autodoc / exec_lib.fd) -------------------------------------
    {
        const std::string prog =
            "        ORG $20000\nstart:  MOVEA.L 4.W,A6\n"
            "        MOVEQ #100,D0\n        MOVEQ #4,D1\n        JSR -198(A6)\n        MOVE.L D0,D2\n"                       // AllocMem(100, MEMF_FAST)            -> 0
            "        MOVEQ #100,D0\n        MOVE.L #$10002,D1\n        JSR -198(A6)\n        MOVE.L D0,D3\n"               // AllocMem(100, CHIP|CLEAR)           -> a block
            "        MOVEQ #100,D0\n        MOVEQ #4,D1\n        JSR -684(A6)\n        MOVE.L D0,D4\n"                       // AllocVec(100, MEMF_FAST)            -> 0
            "        MOVEQ #100,D0\n        MOVEQ #1,D1\n        JSR -684(A6)\n        MOVE.L D0,D5\n"                       // AllocVec(100, MEMF_PUBLIC)          -> a block
            "        MOVEQ #4,D1\n        JSR -216(A6)\n        MOVE.L D0,D6\n"                                                // AvailMem(MEMF_FAST)                 -> 0
            "        MOVEQ #0,D1\n        JSR -216(A6)\n        MOVE.L D0,D7\n"                                                // AvailMem(any)                       -> free bytes
            "        MOVEA.L D5,A1\n        JSR -690(A6)\n        RTS\n";                                                     // FreeVec
        Session s;
        call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        CHECK(ok(load(s, prog)));
        const Json r = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(r, "stop"), std::string("exit"));
        CHECK_EQ(regOf(r, "D2"), std::string("0x00000000"));
        CHECK(regOf(r, "D3") != "0x00000000");
        CHECK_EQ(regOf(r, "D4"), std::string("0x00000000"));
        CHECK(regOf(r, "D5") != "0x00000000");
        CHECK_EQ(regOf(r, "D6"), std::string("0x00000000"));
        CHECK(regOf(r, "D7") != "0x00000000");
        const std::vector<Json> calls = call(s, "{\"cmd\":\"libcalls\"}").find("calls")->items();
        CHECK_EQ(str(calls[0], "note"), std::string("MEMF_FAST, but the machine has no fast memory"));
        CHECK_EQ(calls[0].find("min_version")->asInt(), std::int64_t{33});            // AllocMem is in Kickstart 1.2
        CHECK_EQ(str(calls[2], "function"), std::string("AllocVec"));
        CHECK_EQ(calls[2].find("min_version")->asInt(), std::int64_t{36});            // AllocVec is not
        CHECK_EQ(calls.back().find("min_version")->asInt(), std::int64_t{36});        // FreeVec
        CHECK_EQ(str(calls.back(), "function"), std::string("FreeVec"));
        // with fast memory in the machine the request succeeds (the heap is chip RAM in this emulation, see docs/amiga500.md)
        Session t;
        call(t, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"fast_kb\":512,\"os\":true}");
        CHECK(ok(load(t, prog)));
        const Json r2 = call(t, "{\"cmd\":\"run\"}");
        CHECK(regOf(r2, "D2") != "0x00000000");
        CHECK(regOf(r2, "D6") != "0x00000000");
    }

    // ---- the video beam: a program that waits for a raster line comes through ------------------------------------------------------------------------------
    {
        Session s;
        call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\"}");
        CHECK(ok(load(s, "        ORG $20000\nstart:  MOVE.W $DFF006,D1\n        MOVE.W $DFF006,D2\n        MOVE.W $DFF006,D3\n        MOVE.W $DFF004,D4\n"
                         "wait:   CMP.B #$80,$DFF006\n        BNE.S wait\n        MOVEQ #1,D7\n        STOP #$2700\n")));
        CHECK(ok(call(s, "{\"cmd\":\"checkpoint\",\"name\":\"c1\"}")));
        const Json r = call(s, "{\"cmd\":\"step\",\"n\":4}");
        CHECK_EQ(regOf(r, "D1"), std::string("0x00000000"));          // line 0, colour clock 0
        CHECK_EQ(regOf(r, "D2"), std::string("0x00000020"));          // each read of VHPOSR moves the beam on by 32 colour clocks
        CHECK_EQ(regOf(r, "D3"), std::string("0x00000040"));
        CHECK_EQ(regOf(r, "D4"), std::string("0x00000000"));          // VPOSR: line 0 is in the first 256, a short frame
        const Json done = call(s, "{\"cmd\":\"run\",\"max_instructions\":20000}");
        CHECK_EQ(str(done, "stop"), std::string("stopped"));           // the loop for line $80 ended: STOP was reached
        CHECK_EQ(regOf(done, "D7"), std::string("0x00000001"));
        // a rollback puts the beam back (a run must not depend on how often something was read before)
        CHECK(ok(call(s, "{\"cmd\":\"rollback\",\"name\":\"c1\"}")));
        const Json again = call(s, "{\"cmd\":\"step\",\"n\":2}");
        CHECK_EQ(regOf(again, "D1"), std::string("0x00000000"));
        CHECK_EQ(regOf(again, "D2"), std::string("0x00000020"));
        // beyond line 255 VPOSR has the ninth bit; after 312 lines the frame is over and the long-frame flag toggles. A read of VHPOSR moves the beam
        // 32 clocks on, a line has 228: n reads make floor(32 n / 228) lines.
        for (const auto& [reads, expected] : {std::pair<int, const char*>{2000, "0x00000001"}, {2300, "0x00008000"}}) {
            Session t;
            call(t, "{\"cmd\":\"machine\",\"type\":\"amiga500\"}");
            CHECK(ok(load(t, "        ORG $20000\nstart:  MOVE.W #" + std::to_string(reads - 1) + ",D5\nl:      MOVE.W $DFF006,D1\n        DBRA D5,l\n        MOVE.W $DFF004,D2\n        STOP #$2700\n")));
            const Json r2 = call(t, "{\"cmd\":\"run\",\"max_instructions\":20000}");
            CHECK_EQ(regOf(r2, "D2"), std::string(expected));
        }
    }

    // ---- self-extracting programs: run until the program jumps into memory it has written -------------------------------------------------------------
    {
        // writes 70 NOPs and an RTS to $3000 and jumps there
        const std::string prog =
            "        ORG $20000\nstart:  LEA $30000,A0\n        MOVEQ #69,D0\n.l:     MOVE.W #$4E71,(A0)+\n        DBRA D0,.l\n        MOVE.W #$4E75,(A0)+\n        JMP $30000\n";
        SessionOptions opt;
        opt.allowFiles = true;
        Session s(opt);
        call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        CHECK(ok(load(s, prog)));
        const Json hit = call(s, "{\"cmd\":\"run_until_unpacked\",\"min_written\":100}");
        CHECK(ok(hit));
        CHECK_EQ(str(hit, "stop"), std::string("unpacked"));
        CHECK_EQ(str(hit, "pc"), std::string("0x030000"));
        CHECK_EQ(str(*hit.find("region"), "start"), std::string("0x030000"));
        CHECK_EQ(hit.find("region")->find("size")->asInt(), std::int64_t{142});          // 70 words and the RTS
        CHECK(hit.find("written_bytes")->asInt() >= 142);
        const Json w = call(s, "{\"cmd\":\"written\",\"min_length\":16}");
        CHECK_EQ(w.find("ranges")->items().size(), std::size_t{1});
        CHECK_EQ(str(w.find("ranges")->items()[0], "start"), std::string("0x030000"));
        // too little written for the threshold: the program runs on (it returns to the system through the RTS)
        Session t;
        call(t, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        CHECK(ok(load(t, prog)));
        const Json no = call(t, "{\"cmd\":\"run_until_unpacked\",\"min_written\":1000}");
        CHECK_EQ(str(no, "stop"), std::string("exit"));
        // an ignored range does not count, and `continue` ignores the stage that was seen last
        Session u;
        call(u, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        CHECK(ok(load(u, prog)));
        const Json ign = call(u, "{\"cmd\":\"run_until_unpacked\",\"min_written\":100,\"ignore\":[[\"0x30000\",\"0x30100\"]]}");
        CHECK_EQ(str(ign, "stop"), std::string("exit"));
        CHECK(!ok(call(u, "{\"cmd\":\"run_until_unpacked\",\"ignore\":[[1]]}")));
        CHECK(!ok(call(u, "{\"cmd\":\"run_until_unpacked\",\"min_written\":0}")));
        CHECK(ok(load(s, prog)));
        CHECK_EQ(str(call(s, "{\"cmd\":\"run_until_unpacked\",\"min_written\":100}"), "stop"), std::string("unpacked"));
        const Json next = call(s, "{\"cmd\":\"run_until_unpacked\",\"min_written\":100,\"continue\":true}");
        CHECK_EQ(str(next, "stop"), std::string("exit"));                                  // the NOPs and the RTS are the stage that was seen: it does not stop again
        // save_memory: the unpacked bytes, as a file
        CHECK(ok(load(s, prog)));
        CHECK_EQ(str(call(s, "{\"cmd\":\"run_until_unpacked\",\"min_written\":100}"), "stop"), std::string("unpacked"));
        CHECK(ok(call(s, "{\"cmd\":\"save_memory\",\"addr\":\"0x30000\",\"len\":142,\"file\":\"m68k_unpack_test.tmp\"}")));
        const std::vector<Byte> saved = readBinaryFile("m68k_unpack_test.tmp");
        std::remove("m68k_unpack_test.tmp");
        CHECK_EQ(saved.size(), std::size_t{142});
        CHECK_EQ(saved[0], Byte{0x4E});
        CHECK_EQ(saved[1], Byte{0x71});
        CHECK_EQ(saved[140], Byte{0x4E});
        CHECK_EQ(saved[141], Byte{0x75});
        CHECK(!ok(call(s, "{\"cmd\":\"save_memory\",\"addr\":\"0x30000\",\"len\":0,\"file\":\"x\"}")));
        // without --allow-files no file is written
        Session plain;
        call(plain, "{\"cmd\":\"machine\",\"type\":\"amiga500\"}");
        CHECK(!ok(call(plain, "{\"cmd\":\"save_memory\",\"addr\":\"0x30000\",\"len\":4,\"file\":\"m68k_must_not_exist.tmp\"}")));
        // the calls of the emulated system write library jump tables: that is not "code that the program wrote"
        Session lib;
        call(lib, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        CHECK(ok(load(lib, "        ORG $20000\nstart:  MOVEA.L 4.W,A6\n        LEA dos(PC),A1\n        MOVEQ #0,D0\n        JSR -552(A6)\n        MOVEA.L D0,A6\n        JSR -60(A6)\n"
                           "        MOVEQ #0,D0\n        RTS\ndos:    DC.B \"dos.library\",0\n")));
        CHECK_EQ(str(call(lib, "{\"cmd\":\"run_until_unpacked\",\"min_written\":1}"), "stop"), std::string("exit"));
        // the first instruction of a run is not checked (a run goes on from a stop)
        CHECK(ok(call(s, "{\"cmd\":\"track_writes\",\"on\":false}")));
        CHECK(!call(s, "{\"cmd\":\"written\"}").find("tracking")->asBool());
    }

    // ---- state: checkpoints cover the OS, the stop is a protocol answer, the options ----------------------------------------------------
    {
        Session s;
        call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"os\":true}");
        CHECK(ok(load(s, kHello)));
        CHECK(ok(call(s, "{\"cmd\":\"checkpoint\",\"name\":\"start\"}")));
        call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(str(call(s, "{\"cmd\":\"console\",\"clear\":false}"), "text"), std::string("Hello\n"));
        CHECK(ok(call(s, "{\"cmd\":\"rollback\",\"name\":\"start\"}")));
        CHECK_EQ(str(call(s, "{\"cmd\":\"console\"}"), "text"), std::string(""));        // the console was rolled back too
        const Json again = call(s, "{\"cmd\":\"run\"}");
        CHECK_EQ(regOf(again, "D6"), std::string("0x00040000"));                          // the allocator too: the same block
        // run_until_return / step_over work across emulated calls
        CHECK(ok(call(s, "{\"cmd\":\"rollback\",\"name\":\"start\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"step\",\"n\":3}")));                                // MOVEA, LEA, MOVEQ
        const Json over = call(s, "{\"cmd\":\"step_over\"}");                             // JSR -552(A6)
        CHECK_EQ(str(over, "stop"), std::string("address"));
        CHECK(regOf(over, "D0") != "0x00000000");
        // without the os option nothing is installed
        Session plain;
        call(plain, "{\"cmd\":\"machine\",\"type\":\"amiga500\"}");
        CHECK(call(plain, "{\"cmd\":\"libcalls\"}").find("note") != nullptr);
        CHECK_EQ(str(*call(plain, "{\"cmd\":\"regs\"}").find("regs"), "A7"), std::string("0x0007FFF0"));
        CHECK(!ok(call(plain, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"chip_kb\":256,\"os\":true}")));   // too little chip RAM
        CHECK(ok(call(plain, "{\"cmd\":\"machine\",\"type\":\"amiga500\",\"chip_kb\":1024,\"os\":true}")));
        CHECK_EQ(str(*call(plain, "{\"cmd\":\"regs\"}").find("regs"), "A7"), std::string("0x000F9FF0"));   // below the OS area at the top
        CHECK(ok(call(plain, "{\"cmd\":\"machine\",\"type\":\"generic\"}")));
        CHECK(call(plain, "{\"cmd\":\"libcalls\"}").find("note") != nullptr);
    }

    return finish("os");
}
