#include <filesystem>
#include <cstdio>
#include <random>

#include "m68k/amiga.hpp"
#include "m68k/analysis.hpp"
#include "m68k/annotations.hpp"
#include "m68k/assembler.hpp"
#include "m68k/image_io.hpp"
#include "m68k/listing.hpp"
#include "m68k/session.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

Memory assembleAt(const std::string& src, Address origin, AssemblyResult* out = nullptr) {
    AssemblerOptions opt;
    opt.origin = origin;
    AssemblyResult r = assemble(src, opt);
    if (!r.ok()) {
        std::cerr << "test source does not assemble: " << r.errors[0].message << " (line " << r.errors[0].line << ")\n";
        std::exit(2);
    }
    Memory m;
    loadSegments(m, r.segments);
    if (out) *out = std::move(r);
    return m;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* kProgram =
    "        ORG $1000\n"
    "main:   LEA text(PC),A0\n"
    "        MOVE.W #$0F00,$DFF180.L\n"
    "        BSR show\n"
    "        BSR show\n"
    "        JSR -552(A6)\n"
    "        STOP #$2700\n"
    "show:   MOVE.B (A0)+,D0\n"
    "        BEQ.S end\n"
    "        BSR emit\n"
    "        BRA.S show\n"
    "end:    RTS\n"
    "emit:   MOVE.B D0,$2000.W\n"
    "        RTS\n"
    "text:   DC.B \"Hello, Amiga!\",0\n"
    "        EVEN\n"
    "table:  DC.L main,show\n"
    "count:  DC.W 7\n"
    "        DS.B 20\n"
    "tail:   DC.B 1,2,3\n";

Json call(Session& s, const std::string& line) { return Json::parse(s.handleLine(line)); }
bool ok(const Json& j) { return j.find("ok") && j.find("ok")->asBool(); }
std::string str(const Json& j, const char* k) {
    const Json* v = j.find(k);
    return v && v->isString() ? v->asString() : "<missing>";
}

}  // namespace

int main() {
    // ---- annotation names ---------------------------------------------------------------------------------
    {
        Annotations a;
        CHECK(!a.setLabel(0x1000, "init_screen"));
        CHECK(a.nameAt(0x1000).value() == "init_screen");
        CHECK(a.addressOf("INIT_SCREEN").value() == 0x1000);  // names are not case sensitive
        for (const char* bad : {"", "1abc", "a-b", "a b", "d0", "A7", "sp", "move", "MOVEM", "org", "dc", "dmacon", "COLOR05",
                                "sub_001000", "LOC_00ABCD", "data_000000"}) {
            const auto err = a.setLabel(0x2000, bad);
            CHECK(err.has_value());
            if (!err) std::cerr << "name accepted: '" << bad << "'\n";
        }
        CHECK(a.setLabel(0x2000, std::string(65, 'x')).has_value());
        CHECK(a.setLabel(0x2000, "INIT_screen").has_value());       // used by 0x1000
        CHECK(a.setLabel(0x2000, "sub_1").has_value() == false);    // short placeholder-like names are fine
        CHECK(!a.setLabel(0x1000, "renamed"));                      // renaming frees the old name
        CHECK(!a.setLabel(0x3000, "init_screen"));
        CHECK(!a.nameAt(0x1000).value().empty());

        RoutineDoc d;
        d.name = "draw";
        d.summary = "draws";
        d.confidence = "maybe";
        CHECK(a.setRoutine(0x4000, d).has_value());
        d.confidence = "low";
        CHECK(!a.setRoutine(0x4000, d));
        CHECK(a.routineAt(0x4000)->confidence == "low");
        d.name = "";
        d.summary = "only the doc changes";
        CHECK(!a.setRoutine(0x4000, d));
        CHECK(a.routineAt(0x4000)->name == "draw");  // the existing name is kept

        VariableDoc v;
        v.name = "score";
        v.address = 0x5000;
        v.size = 4;
        CHECK(!a.setVariable(v));
        CHECK(a.expressionFor(0x5002).value() == "score+2");
        CHECK(a.expressionFor(0x5004) == std::nullopt);
        v.size = 0;
        v.name = "x1";
        CHECK(a.setVariable(v).has_value());

        DataRegion r;
        r.address = 0x6001;
        r.length = 4;
        r.kind = DataKind::Words;
        CHECK(a.setData(r).has_value());           // odd start
        r.address = 0x6000;
        r.length = 3;
        CHECK(a.setData(r).has_value());           // odd length
        r.length = 4;
        r.name = "tbl";
        CHECK(!a.setData(r));
        r.address = 0x6002;
        r.name = "tbl2";
        CHECK(a.setData(r).has_value());           // overlaps
        r.address = 0x6004;
        CHECK(!a.setData(r));
        CHECK(a.dataCovering(0x6003) != nullptr && a.dataCovering(0x6003)->address == 0x6000);
        CHECK(a.dataCovering(0x6008) == nullptr);
        CHECK(a.expressionFor(0x6006).value() == "tbl2+2");

        // removing a name removes what hangs on it
        CHECK(a.removeLabel(0x5000));
        CHECK(a.variableAt(0x5000) == nullptr);
        CHECK(!a.addressOf("score").has_value());
        CHECK(a.removeLabel(0x6000));
        CHECK(a.dataAt(0x6000) == nullptr);
        CHECK(!a.removeLabel(0x7777));
    }

    // ---- annotations as a project file -----------------------------------------------------------------------------
    {
        Annotations a;
        a.setTitle("demo");
        a.setLabel(0x1000, "start");
        RoutineDoc d;
        d.name = "show";
        d.summary = "print";
        d.inputs = "A0";
        d.confidence = "high";
        a.setRoutine(0x1010, d);
        VariableDoc v;
        v.name = "count";
        v.address = 0x2000;
        v.size = 2;
        v.comment = "a \"quoted\"\nmulti-line comment";
        a.setVariable(v);
        DataRegion r;
        r.address = 0x3000;
        r.length = 8;
        r.kind = DataKind::Pointers;
        r.name = "vectors";
        a.setData(r);
        a.setComment(0x1004, "hello");
        a.setBlockComment(0x1010, "block");
        const std::string text = a.toJson().dump();
        const Annotations b = Annotations::fromJson(Json::parse(text));
        CHECK_EQ(b.toJson().dump(), text);
        CHECK_EQ(b.title(), std::string("demo"));
        CHECK_EQ(b.variableAt(0x2000)->comment, std::string("a \"quoted\"\nmulti-line comment"));
        // broken files
        auto bad = [](const std::string& t) {
            try {
                (void)Annotations::fromJson(Json::parse(t));
            } catch (const JsonError&) {
                return true;
            }
            return false;
        };
        CHECK(bad("[]"));
        CHECK(bad("{\"labels\":[{\"addr\":\"0x10\",\"name\":\"a\"},{\"addr\":\"0x20\",\"name\":\"A\"}]}"));  // duplicate name
        CHECK(bad("{\"labels\":[{\"name\":\"a\"}]}"));                                                       // missing address
        CHECK(bad("{\"data\":[{\"addr\":\"0x10\",\"length\":4,\"kind\":\"nibbles\"}]}"));
        CHECK(bad("{\"labels\":[{\"addr\":\"0x10\",\"name\":\"d0\"}]}"));
    }

    // ---- flow analysis ---------------------------------------------------------------------------------------------------
    {
        AssemblyResult asmResult;
        Memory mem = assembleAt(kProgram, 0x1000, &asmResult);
        const Address end = asmResult.lowest() + static_cast<Address>(asmResult.flatten().size());
        Analysis an(mem, 0x1000, end);
        an.addEntry(0x1000);
        an.analyzeFlow();
        CHECK(an.at(0x1000) != nullptr);
        CHECK(an.at(0x1001) == nullptr);
        CHECK(an.isCode(0x1001));                                    // inside an instruction
        const Address showAddr = static_cast<Address>(asmResult.symbols.at("show"));
        const Address emit = static_cast<Address>(asmResult.symbols.at("emit"));
        const Address text = static_cast<Address>(asmResult.symbols.at("text"));
        CHECK(an.at(showAddr) != nullptr);
        CHECK(!an.isCode(text));                                     // the string is data
        CHECK(an.callTargets().count(showAddr) == 1);
        CHECK(an.jumpTargets().count(static_cast<Address>(asmResult.symbols.at("end"))) == 1);
        CHECK(an.dataReferences().count(text) == 1);

        std::vector<RoutineInfo> rs = an.routines();
        CHECK_EQ(rs.size(), std::size_t{3});  // main (entry), showAddr, emit
        const RoutineInfo* rMain = nullptr;
        const RoutineInfo* rShow = nullptr;
        const RoutineInfo* rEmit = nullptr;
        for (const RoutineInfo& r : rs) {
            if (r.entry == 0x1000) rMain = &r;
            if (r.entry == showAddr) rShow = &r;
            if (r.entry == emit) rEmit = &r;
        }
        CHECK(rMain && rShow && rEmit);
        CHECK_EQ(rEmit->depth, 0);
        CHECK_EQ(rShow->depth, 1);
        CHECK_EQ(rMain->depth, 2);
        CHECK_EQ(rShow->callers.size(), std::size_t{2});
        CHECK_EQ(rShow->callees.size(), std::size_t{1});
        CHECK_EQ(rShow->callees[0], emit);
        CHECK_EQ(rEmit->callers.size(), std::size_t{1});
        // the extent of `showAddr` includes the code its branches reach (the RTS at `end`) but not the callee
        CHECK_EQ(rShow->start, showAddr);
        CHECK_EQ(rShow->end, emit);
        CHECK_EQ(rShow->instructions, 5u);
        // the library call through A6
        CHECK_EQ(rMain->libraryCalls.size(), std::size_t{1});
        CHECK_EQ(rMain->libraryCalls[0].offset, -552);
        CHECK(rMain->externalCalls.empty());

        // declared data is a barrier: code analysis does not decode into it
        Analysis guarded(mem, 0x1000, end);
        guarded.setDataBarrier([&](Address a) { return a >= showAddr && a < emit; });
        guarded.addEntry(0x1000);
        guarded.analyzeFlow();
        CHECK(guarded.at(showAddr) == nullptr);
        CHECK(guarded.at(emit) == nullptr);                          // not reachable any more
        CHECK(guarded.at(0x1000) != nullptr);

        // linear analysis finds code that no flow reaches
        Analysis lin(mem, 0x1000, end);
        lin.analyzeLinear(0x1000, emit + 6);
        CHECK(lin.at(emit) != nullptr);
    }
    {
        Memory mem = assembleAt("        ORG $1000\nrec:    SUBQ.W #1,D0\n        BEQ.S out\n        BSR rec\nout:    RTS\n"
                                "ping:   BSR pong\n        RTS\npong:   BSR ping\n        RTS\n        JSR $F80000\n",
                                0x1000);
        Analysis an(mem, 0x1000, 0x1030);
        an.addEntry(0x1000);
        an.addEntry(static_cast<Address>(0x1000 + 8));  // ping
        an.analyzeFlow();
        for (const RoutineInfo& r : an.routines()) CHECK(r.depth >= 0 && r.depth < 10);  // cycles do not hang
        const std::vector<RoutineInfo> rs = an.routines();
        CHECK(rs.size() >= 3);
    }
    {
        // calls to code outside the analysed range are reported
        Memory mem = assembleAt("        ORG $1000\n        JSR $F80010\n        BSR.S sub\n        RTS\nsub:    RTS\n", 0x1000);
        Analysis an(mem, 0x1000, 0x1010);
        an.addEntry(0x1000);
        an.analyzeFlow();
        const std::vector<RoutineInfo> routines = an.routines();
        const RoutineInfo& r = routines.front();
        CHECK_EQ(r.externalCalls.size(), std::size_t{1});
        CHECK_EQ(r.externalCalls[0], Address{0xF80010});
    }

    // ---- listings ------------------------------------------------------------------------------------------------------------
    {
        AssemblyResult asmResult;
        Memory mem = assembleAt(kProgram, 0x1000, &asmResult);
        const Address end = 0x1000 + static_cast<Address>(asmResult.flatten().size());
        Analysis an(mem, 0x1000, end);
        an.addEntry(0x1000);
        an.analyzeFlow();
        ListingOptions lo;
        lo.from = 0x1000;
        lo.to = end;
        lo.platformSymbol = [](Address a) { return Amiga500::customRegisterName(a); };

        ListingResult r = renderListing(mem, an, Annotations{}, lo);
        CHECK(r.verified);
        CHECK(has(r.text, "VERIFIED"));
        CHECK(has(r.text, "COLOR00 EQU $DFF180"));
        CHECK(has(r.text, "MOVE.W   #$0F00,COLOR00.L"));
        CHECK(has(r.text, "sub_"));
        CHECK(has(r.text, "loc_"));
        CHECK(has(r.text, "data_"));
        CHECK(has(r.text, "DC.B     \"Hello, Amiga!\",0"));
        CHECK(has(r.text, "DS.B     20"));   // a long run of zeros
        CHECK(r.unnamed >= 4);

        // with annotations: names, comments, headers, variables, data regions
        Annotations ann;
        ann.setTitle("hello demo");
        RoutineDoc d;
        d.name = "print_string";
        d.summary = "prints the text at A0";
        d.inputs = "A0 = text";
        d.confidence = "low";
        ann.setRoutine(static_cast<Address>(asmResult.symbols.at("show")), d);
        ann.setLabel(0x1000, "entry");
        ann.setLabel(static_cast<Address>(asmResult.symbols.at("end")), "done");
        ann.setComment(0x1000, "A0 = message");
        ann.setBlockComment(static_cast<Address>(asmResult.symbols.at("emit")), "write one character\nto the port");
        VariableDoc v;
        v.name = "line_count";
        v.address = static_cast<Address>(asmResult.symbols.at("count"));
        v.size = 2;
        v.comment = "lines printed";
        ann.setVariable(v);
        DataRegion tbl;
        tbl.address = static_cast<Address>(asmResult.symbols.at("table"));
        tbl.length = 8;
        tbl.kind = DataKind::Pointers;
        tbl.name = "handlers";
        ann.setData(tbl);
        lo.addresses = true;
        r = renderListing(mem, an, ann, lo);
        CHECK(r.verified);
        CHECK(has(r.text, "; hello demo"));
        CHECK(has(r.text, "print_string:"));
        CHECK(has(r.text, "; print_string: prints the text at A0"));
        CHECK(has(r.text, ";   confidence: low - a guess, please verify"));
        CHECK(has(r.text, "BSR.S    print_string"));
        CHECK(has(r.text, "entry:"));
        CHECK(has(r.text, "done:"));
        CHECK(has(r.text, "; $001000 A0 = message"));
        CHECK(has(r.text, "; write one character\n; to the port\n"));
        CHECK(has(r.text, "line_count:"));
        CHECK(has(r.text, "DC.W     $0007"));
        CHECK(has(r.text, "lines printed"));
        CHECK(has(r.text, "DC.L     entry,print_string"));   // pointers showAddr names
        CHECK(has(r.text, "handlers:"));

        // a name inside an instruction cannot sit on a line: it is defined with EQU and still verifies
        Annotations odd;
        odd.setLabel(0x1001, "mid");
        r = renderListing(mem, an, odd, lo);
        CHECK(r.verified);
        CHECK(has(r.text, "mid EQU $001001"));

        // a variable inside the string, referenced by name+offset
        Annotations ref;
        ref.setLabel(static_cast<Address>(asmResult.symbols.at("text")) + 7, "amiga_word");
        r = renderListing(mem, an, ref, lo);
        CHECK(r.verified);
        CHECK(has(r.text, "amiga_word:"));   // the data run is split at the label

        // truncation: cut at a line boundary and not verified
        lo.maxChars = 400;
        r = renderListing(mem, an, ann, lo);
        CHECK(r.truncated);
        CHECK(!r.verified);
        lo.maxChars = 0;
        lo.verify = false;
        r = renderListing(mem, an, ann, lo);
        CHECK(!r.verified);
        CHECK(has(r.text, "not verified (not requested)"));
    }
    {
        // every data kind
        Memory mem;
        const std::vector<Byte> bytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,           // bytes
                                         0x12, 0x34, 0x56, 0x78,                                               // words
                                         0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x10, 0x00,                       // longs / pointers
                                         'T', 'e', 'x', 't', ' ', '"', 'q', '"', 0x00, 0x7F, 'e', 'n', 'd', 'x'};  // text with quotes
        mem.load(0x1000, bytes);
        Analysis an(mem, 0x1000, 0x1000 + static_cast<Address>(bytes.size()));
        Annotations ann;
        DataRegion r;
        r.address = 0x1000; r.length = 10; r.kind = DataKind::Bytes; r.name = "b";
        ann.setData(r);
        r.address = 0x100A; r.length = 4; r.kind = DataKind::Words; r.name = "w";
        ann.setData(r);
        r.address = 0x100E; r.length = 4; r.kind = DataKind::Longs; r.name = "l";
        ann.setData(r);
        r.address = 0x1012; r.length = 4; r.kind = DataKind::Pointers; r.name = "p";
        ann.setData(r);
        r.address = 0x1016; r.length = 14; r.kind = DataKind::Text; r.name = "t";
        ann.setData(r);
        ListingOptions lo;
        lo.from = 0x1000;
        lo.to = 0x1000 + static_cast<Address>(bytes.size());
        const ListingResult res = renderListing(mem, an, ann, lo);
        CHECK(res.verified);
        CHECK(has(res.text, "DC.W     $1234,$5678"));
        CHECK(has(res.text, "DC.L     $DEADBEEF"));
        CHECK(has(res.text, "DC.L     p"[0] == 'D' ? "p:" : "p:"));
        CHECK(has(res.text, "DC.B     'Text \"q\"'"));
        CHECK(!res.text.empty());
    }
    {
        // a marked constant is a number, not a pointer; an address inside a variable is written as name+offset
        Memory mem;
        const std::vector<Byte> bytes = {0x20, 0x7C, 0x00, 0x00, 0x10, 0x20,   // $1000 MOVEA.L #$1020,A0 (a real pointer)
                                         0x20, 0x3C, 0x00, 0x00, 0x10, 0x06,   // $1006 MOVE.L  #$1006,D0 (a constant that equals an address)
                                         0x10, 0x39, 0x00, 0x00, 0x10, 0x32,   // $100C MOVE.B  $1032.L,D0 (inside the variable)
                                         0x4E, 0x75,                           // $1012 RTS
                                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // $1014..$101F
                                         1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,   // $1020 data
                                         1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};  // $1030 the variable
        mem.load(0x1000, bytes);
        const Address end = 0x1000 + static_cast<Address>(bytes.size());
        Analysis an(mem, 0x1000, end);
        an.addEntry(0x1000);
        an.analyzeFlow();
        ListingOptions lo;
        lo.from = 0x1000;
        lo.to = end;

        ListingResult res = renderListing(mem, an, Annotations{}, lo);
        CHECK(res.verified);
        CHECK(has(res.text, "[#imm = data_001020]"));   // the pointer is recognised
        CHECK(has(res.text, "[#imm = loc_001006]"));    // and so is the coincidence
        CHECK(has(res.text, "loc_001006:"));
        CHECK(has(res.text, "data_001032"));             // no variable known: a placeholder

        Annotations ann;
        ann.setConstant(0x1006, true);
        VariableDoc v;
        v.name = "var";
        v.address = 0x1030;
        v.size = 16;
        CHECK(!ann.setVariable(v).has_value());
        res = renderListing(mem, an, ann, lo);
        CHECK(res.verified);
        CHECK(has(res.text, "[#imm = data_001020]"));    // pointers keep their note
        CHECK(!has(res.text, "[#imm = loc_001006]"));
        CHECK(!has(res.text, "loc_001006"));             // the constant creates no label
        CHECK(has(res.text, "var+2"));
        CHECK(!has(res.text, "data_001032"));            // the variable names the address

        // annotations survive a JSON round trip, and removing a comment/label at the address also removes the mark
        const Annotations back = Annotations::fromJson(Json::parse(ann.toJson().dump()));
        CHECK(back.isConstant(0x1006));
        CHECK(!back.isConstant(0x1000));
        CHECK_EQ(back.constants().size(), std::size_t{1});
        Annotations gone = back;
        CHECK(gone.removeComment(0x1006));
        CHECK(!gone.isConstant(0x1006));

        // large offsets are written in hexadecimal, small ones stay bare
        VariableDoc big;
        big.name = "big";
        big.address = 0x2000;
        big.size = 100;
        Annotations a2;
        CHECK(!a2.setVariable(big).has_value());
        CHECK_EQ(a2.expressionFor(0x2026).value(), std::string("big+$26"));
        CHECK_EQ(a2.expressionFor(0x2003).value(), std::string("big+3"));
    }
    {
        // comments on data: ann_comment accepts any address, so a comment at a data address must appear in the listing,
        // and a comment or block comment in the middle of a data line must start a new line there (it used to be dropped)
        Memory mem;
        const std::vector<Byte> bytes = {0x4E, 0x75,                                       // $1000 RTS
                                         1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,             // $1002 bytes (annotated)
                                         0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,    // $100E words (annotated)
                                         'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 0,         // $1016 text (annotated)
                                         0,                                                 // $101F padding
                                         0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8};   // $1020 plain bytes (not annotated)
        mem.load(0x1000, bytes);
        const Address end = 0x1000 + static_cast<Address>(bytes.size());
        Analysis an(mem, 0x1000, end);
        an.addEntry(0x1000);
        an.analyzeFlow();
        Annotations ann;
        DataRegion r;
        r.address = 0x1002; r.length = 12; r.kind = DataKind::Bytes; r.name = "tab";
        CHECK(!ann.setData(r).has_value());
        r.address = 0x100E; r.length = 8; r.kind = DataKind::Words; r.name = "wtab";
        CHECK(!ann.setData(r).has_value());
        r.address = 0x1016; r.length = 9; r.kind = DataKind::Text; r.name = "msg";
        CHECK(!ann.setData(r).has_value());
        ann.setComment(0x1002, "first entry");        // start of a region
        ann.setComment(0x1005, "fourth entry");       // middle of a DC.B line
        ann.setBlockComment(0x100A, "second half");   // block comment in the middle of the region
        ann.setComment(0x1012, "third word");         // middle of a DC.W line
        ann.setComment(0x101A, "from E on");          // middle of a string
        ann.setComment(0x1023, "plain byte");         // bytes nobody annotated
        ListingOptions lo;
        lo.from = 0x1000;
        lo.to = end;
        const ListingResult res = renderListing(mem, an, ann, lo);
        CHECK(res.verified);
        CHECK(has(res.text, "tab:\n        DC.B     $01,$02,$03                  ; first entry\n"));
        CHECK(has(res.text, "\n        DC.B     $04,$05,$06,$07,$08          ; fourth entry\n; second half\n        DC.B     $09,$0A,$0B,$0C\n"));
        CHECK(has(res.text, "\n        DC.W     $9ABC,$DEF0                  ; third word\n"));
        CHECK(has(res.text, "\n        DC.B     \"ABCD\"\n        DC.B     \"EFGH\",0                     ; from E on\n"));
        CHECK(has(res.text, "\n        DC.B     $F4,$F5,$F6,$F7,$F8          ; plain byte\n"));
    }
    {
        // random programs and random annotations: the listing must always be verified
        std::mt19937 rng(777);
        const Word common[] = {0x4E75, 0x4E71, 0x6000, 0x6100, 0x6700, 0x6600, 0x4EB9, 0x4EF9, 0x41FA, 0x4879, 0x33FC, 0x51C8,
                               0x2079, 0x303C, 0x13FC, 0x4EB8, 0x4E56, 0x4E5E, 0x48E7, 0x4CDF};
        for (int round = 0; round < 150; ++round) {
            Memory mem;
            const Address start = 0x4000;
            const Address end = start + 400 + static_cast<Address>(rng() % 17);
            for (Address a = start; a < end; ++a) mem.write8(a, static_cast<Byte>(rng() % 4 == 0 ? rng() : (rng() % 3 == 0 ? 0 : 0x30 + rng() % 70)));
            for (Address a = start; a + 1 < end; a += 2)
                if (rng() % 3 == 0) mem.write16(a, common[rng() % 20]);
            Analysis an(mem, start, end);
            for (int e = 0; e < 4; ++e) an.addEntry(start + 2 * static_cast<Address>(rng() % 180));
            an.analyzeFlow();
            Annotations ann;
            const int items = static_cast<int>(rng() % 12);
            for (int i = 0; i < items; ++i) {
                const Address at = start + static_cast<Address>(rng() % (end - start));
                const std::string name = "n" + std::to_string(round) + "_" + std::to_string(i);
                switch (rng() % 6) {
                    case 0: ann.setLabel(at, name); break;
                    case 1: {
                        RoutineDoc d;
                        d.name = name;
                        d.summary = "s; \"q\" 'x'\nline2";
                        d.confidence = "low";
                        ann.setRoutine(at, d);
                        break;
                    }
                    case 2: {
                        VariableDoc v;
                        v.name = name;
                        v.address = at;
                        v.size = 1 + static_cast<unsigned>(rng() % 6);
                        v.comment = "c;x";
                        ann.setVariable(v);
                        break;
                    }
                    case 3: {
                        DataRegion r;
                        r.address = at & ~1U;
                        r.length = 2 * (1 + static_cast<unsigned>(rng() % 6));
                        r.kind = static_cast<DataKind>(rng() % 3 == 0 ? 3 : rng() % 5);
                        if (r.kind == DataKind::Longs || r.kind == DataKind::Pointers) r.length = 4 * (1 + static_cast<unsigned>(rng() % 3));
                        r.name = (rng() % 2) ? name : "";
                        ann.setData(r);
                        break;
                    }
                    case 4: ann.setComment(at, "note\nnext; line"); break;
                    default: ann.setBlockComment(at, "block"); break;
                }
            }
            ListingOptions lo;
            lo.from = start;
            lo.to = end;
            lo.addresses = rng() % 2;
            const ListingResult res = renderListing(mem, an, ann, lo);
            CHECK(res.verified);
            if (!res.verified) {
                std::cerr << "round " << round << ": " << res.problem << "\n";
                writeTextFile((std::filesystem::temp_directory_path() / "m68k_listing_failure.s").string(), res.text);
                break;
            }
        }
    }

    // ---- the protocol: analyze, routines, listing, annotations -------------------------------------------------------------------
    {
        Session s;
        CHECK(ok(call(s, "{\"cmd\":\"machine\",\"type\":\"amiga500\"}")));
        CHECK(!ok(call(s, "{\"cmd\":\"analyze\"}")));      // nothing loaded yet
        CHECK(!ok(call(s, "{\"cmd\":\"routines\"}")));     // no analysis yet
        CHECK(!ok(call(s, "{\"cmd\":\"listing\"}")));

        std::string src;
        {
            std::string t = kProgram;
            for (const char c : t) {
                if (c == '\n') src += "\\n";
                else if (c == '"') src += "\\\"";
                else src += c;
            }
        }
        CHECK(ok(call(s, "{\"cmd\":\"asm\",\"source\":\"" + src + "\",\"entry\":\"main\"}")));
        Json r = call(s, "{\"cmd\":\"analyze\",\"entries\":[\"main\"]}");
        CHECK(ok(r));
        CHECK_EQ(r.find("routines")->asInt(), std::int64_t{3});
        CHECK(r.find("code_bytes")->asInt() > 0);

        r = call(s, "{\"cmd\":\"routines\"}");
        const std::vector<Json>& list = r.find("routines")->items();
        CHECK_EQ(list.size(), std::size_t{3});
        CHECK_EQ(list[0].find("depth")->asInt(), std::int64_t{0});   // bottom-up: the leaf first
        CHECK_EQ(list[2].find("depth")->asInt(), std::int64_t{2});
        CHECK_EQ(str(list[2], "name"), std::string("sub_001000"));
        CHECK_EQ(list[2].find("library_calls")->items().size(), std::size_t{1});
        CHECK_EQ(list[2].find("library_calls")->items()[0].find("offset")->asInt(), std::int64_t{-552});
        CHECK_EQ(str(call(s, "{\"cmd\":\"routines\",\"order\":\"address\"}").find("routines")->items()[0], "addr"), std::string("0x001000"));
        CHECK(!ok(call(s, "{\"cmd\":\"routines\",\"order\":\"sideways\"}")));

        // the model's work: names, documentation, comments
        CHECK(ok(call(s, "{\"cmd\":\"ann_routine\",\"addr\":\"show\",\"name\":\"print_string\",\"summary\":\"print\",\"inputs\":\"A0\",\"confidence\":\"low\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_label\",\"addr\":\"emit\",\"name\":\"put_char\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_variable\",\"name\":\"line_count\",\"addr\":\"count\",\"size\":2,\"comment\":\"lines\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_comment\",\"addr\":\"main\",\"text\":\"A0 = text\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_title\",\"title\":\"demo\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_constant\",\"addr\":\"main\"}")));
        Json cl = call(s, "{\"cmd\":\"ann_list\"}");
        CHECK_EQ(cl.find("annotations")->find("constants")->items().size(), std::size_t{1});
        Json badc = call(s, "{\"cmd\":\"ann_constant\",\"addr\":\"main+2\"}");   // inside LEA
        CHECK(!ok(badc));
        CHECK(str(badc, "error").find("first byte") != std::string::npos);
        CHECK(ok(call(s, "{\"cmd\":\"ann_constant\",\"addr\":\"main\",\"remove\":true}")));
        CHECK_EQ(call(s, "{\"cmd\":\"ann_list\"}").find("annotations")->find("constants")->items().size(), std::size_t{0});
        // mistakes are rejected with an explanation
        Json bad = call(s, "{\"cmd\":\"ann_label\",\"addr\":\"emit\",\"name\":\"D3\"}");
        CHECK(!ok(bad));
        CHECK(str(bad, "error").find("reserved") != std::string::npos);
        bad = call(s, "{\"cmd\":\"ann_label\",\"addr\":\"main\",\"name\":\"put_char\"}");
        CHECK(str(bad, "error").find("already used") != std::string::npos);
        bad = call(s, "{\"cmd\":\"ann_comment\",\"addr\":\"main+2\",\"text\":\"x\"}");   // inside LEA
        CHECK(!ok(bad));
        CHECK(str(bad, "error").find("middle of an instruction") != std::string::npos);
        bad = call(s, "{\"cmd\":\"ann_data\",\"addr\":\"show\",\"length\":8}");
        CHECK(!ok(bad));
        CHECK(str(bad, "error").find("contains code") != std::string::npos);
        CHECK(!ok(call(s, "{\"cmd\":\"ann_data\",\"addr\":\"text\",\"length\":4,\"kind\":\"nibbles\"}")));
        // names work as symbols in later commands
        CHECK_EQ(str(call(s, "{\"cmd\":\"read\",\"addr\":\"line_count\",\"len\":2}"), "hex"), std::string("0007"));
        CHECK(str(call(s, "{\"cmd\":\"dis\",\"addr\":\"main\",\"count\":4}").find("lines")->items()[3], "text").find("print_string") != std::string::npos);

        r = call(s, "{\"cmd\":\"routines\",\"undocumented_only\":true}");
        CHECK_EQ(r.find("routines")->items().size(), std::size_t{2});

        r = call(s, "{\"cmd\":\"listing\"}");
        CHECK(ok(r));
        CHECK(r.find("verified")->asBool());
        const std::string listing = str(r, "listing");
        CHECK(has(listing, "print_string:"));
        CHECK(has(listing, "put_char:"));
        CHECK(has(listing, "COLOR00 EQU $DFF180"));
        CHECK(has(listing, "; demo"));
        r = call(s, "{\"cmd\":\"listing\",\"max_chars\":300}");
        CHECK(r.find("truncated")->asBool());
        CHECK(!r.find("verified")->asBool());
        CHECK(!ok(call(s, "{\"cmd\":\"listing\",\"from\":0,\"to\":16}")));   // outside the analysed range

        // declared data keeps analysis out of it, even when analysing again
        CHECK(ok(call(s, "{\"cmd\":\"ann_data\",\"addr\":\"table\",\"length\":8,\"kind\":\"pointers\",\"name\":\"handlers\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"analyze\",\"entries\":[\"main\"]}")));
        CHECK(has(str(call(s, "{\"cmd\":\"listing\"}"), "listing"), "handlers:"));

        // listing every annotation
        r = call(s, "{\"cmd\":\"ann_list\"}");
        CHECK_EQ(r.find("annotations")->find("routines")->items().size(), std::size_t{1});
        CHECK(call(s, "{\"cmd\":\"ann_remove\",\"addr\":\"emit\"}").find("removed")->asBool());
        CHECK(!call(s, "{\"cmd\":\"ann_remove\",\"addr\":\"0x7000\"}").find("removed")->asBool());
        // file access for projects needs the operator's permission
        CHECK(!ok(call(s, "{\"cmd\":\"ann_save\",\"file\":\"x.json\"}")));
        // a new machine forgets everything
        CHECK(ok(call(s, "{\"cmd\":\"machine\",\"type\":\"generic\"}")));
        CHECK(!ok(call(s, "{\"cmd\":\"routines\"}")));
        CHECK(call(s, "{\"cmd\":\"ann_list\"}").find("annotations")->find("labels")->items().empty());
    }
    {
        SessionOptions opt;
        opt.allowFiles = true;
        Session s(opt);
        CHECK(ok(call(s, "{\"cmd\":\"asm\",\"source\":\"        ORG $1000\\nmain: NOP\\n      RTS\\n\",\"entry\":\"main\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_label\",\"addr\":\"main\",\"name\":\"start_here\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_comment\",\"addr\":\"main\",\"text\":\"first\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_save\",\"file\":\"m68k_ann.tmp\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_remove\",\"addr\":\"main\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"ann_load\",\"file\":\"m68k_ann.tmp\"}")));
        CHECK_EQ(call(s, "{\"cmd\":\"ann_list\"}").find("annotations")->find("labels")->items().size(), std::size_t{1});
        writeTextFile("m68k_ann.tmp", "{\"labels\":[{\"addr\":\"0x10\",\"name\":\"d0\"}]}");
        const Json bad = call(s, "{\"cmd\":\"ann_load\",\"file\":\"m68k_ann.tmp\"}");
        CHECK(!ok(bad));
        CHECK(str(bad, "error").find("reserved") != std::string::npos);
        std::remove("m68k_ann.tmp");
    }

    return finish("reverse");
}
