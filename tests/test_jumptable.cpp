// Jump table detection in the code flow analysis, its use in the listing, and the session commands.
#include <random>
#include <string>

#include "m68k/analysis.hpp"
#include "m68k/assembler.hpp"
#include "m68k/listing.hpp"
#include "m68k/annotations.hpp"
#include "m68k/session.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

struct Built {
    AssemblyResult asmResult;
    Memory mem;
    Address from = 0, to = 0;
    explicit Built(const std::string& source) {
        AssemblerOptions o;
        o.origin = 0x1000;
        asmResult = assemble(source, o);
        if (!asmResult.ok()) {
            for (const Diagnostic& d : asmResult.errors) std::fprintf(stderr, "  assembler error line %d: %s\n", d.line, d.message.c_str());
            std::fprintf(stderr, "%s\n", source.c_str());
            std::exit(2);
        }
        for (const Segment& s : asmResult.segments)
            for (std::size_t i = 0; i < s.bytes.size(); ++i) mem.write8(s.address + static_cast<Address>(i), s.bytes[i]);
        from = asmResult.lowest();
        to = from + static_cast<Address>(asmResult.flatten().size());
    }
    Address sym(const std::string& n) const { return static_cast<Address>(asmResult.symbols.at(n)); }
};

Analysis analyse(const Built& b, std::initializer_list<const char*> entries) {
    Analysis an(b.mem, b.from, b.to);
    for (const char* e : entries) an.addEntry(b.sym(e));
    an.analyzeFlow();
    return an;
}

const JumpTable* find(const Analysis& an, JumpTable::Kind kind) {
    for (const JumpTable& t : an.jumpTables())
        if (t.kind == kind) return &t;
    return nullptr;
}

Json call(Session& s, const std::string& line) { return Json::parse(s.handleLine(line)); }
bool ok(const Json& j) { return j.find("ok") && j.find("ok")->asBool(); }

std::string escape(const std::string& t) {
    std::string out;
    for (const char c : t) {
        if (c == '\n') out += "\\n";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* kProgram =
    "        ORG $1000\n"
    "start:  MOVE.W  D1,D0\n"
    "        ASL.W   #2,D0\n"
    "        JMP     btab(PC,D0.W)\n"
    "btab:   BRA.W   c0\n"
    "        BRA.W   c1\n"
    "        BRA.W   c2\n"
    "c0:     MOVEQ   #0,D2\n"
    "        RTS\n"
    "c1:     MOVEQ   #1,D2\n"
    "        RTS\n"
    "c2:     MOVEQ   #2,D2\n"
    "        RTS\n"
    "dispatch: ADD.W D0,D0\n"
    "        MOVE.W  otab(PC,D0.W),D0\n"
    "        JMP     base(PC,D0.W)\n"
    "base:   NOP\n"
    "otab:   DC.W    o0-base,o1-base,o2-base,o3-base\n"
    "o0:     MOVEQ   #10,D3\n"
    "        RTS\n"
    "o1:     MOVEQ   #11,D3\n"
    "        RTS\n"
    "o2:     MOVEQ   #12,D3\n"
    "        RTS\n"
    "o3:     MOVEQ   #13,D3\n"
    "        RTS\n"
    "callit: LEA     ptab(PC),A0\n"
    "        MOVEA.L (A0,D0.W),A1\n"
    "        JSR     (A1)\n"
    "        RTS\n"
    "ptab:   DC.L    p0,p1,p2\n"
    "p0:     MOVEQ   #20,D4\n"
    "        RTS\n"
    "p1:     MOVEQ   #21,D4\n"
    "        RTS\n"
    "p2:     MOVEQ   #22,D4\n"
    "        RTS\n";

}  // namespace

int main() {
    // ---- the three kinds ----------------------------------------------------------------------------------------------------------------
    {
        const Built b(kProgram);
        const Analysis an = analyse(b, {"start", "dispatch", "callit"});
        CHECK_EQ(an.jumpTables().size(), std::size_t{3});

        const JumpTable* br = find(an, JumpTable::Kind::Branch);
        CHECK(br != nullptr);
        if (br) {
            CHECK_EQ(br->jump, b.sym("start") + 4);
            CHECK_EQ(br->table, b.sym("btab"));
            CHECK_EQ(br->slots.size(), std::size_t{3});
            CHECK_EQ(br->targets[0], b.sym("c0"));
            CHECK_EQ(br->targets[2], b.sym("c2"));
            CHECK_EQ(br->bytes, 12U);
            CHECK(br->followed);
        }
        CHECK(an.at(b.sym("c0")) != nullptr);          // the targets are code although nothing calls them
        CHECK(an.at(b.sym("c2")) != nullptr);

        const JumpTable* off = find(an, JumpTable::Kind::Offset);
        CHECK(off != nullptr);
        if (off) {
            CHECK_EQ(off->table, b.sym("otab"));
            CHECK_EQ(off->base, b.sym("base"));
            CHECK_EQ(off->slots.size(), std::size_t{4});
            CHECK_EQ(off->targets[1], b.sym("o1"));
            CHECK_EQ(off->targets[3], b.sym("o3"));
            CHECK_EQ(off->bytes, 8U);
        }
        CHECK(an.at(b.sym("o3")) != nullptr);
        CHECK(an.isTableData(b.sym("otab")));            // the offsets are data, not code
        CHECK(!an.isCode(b.sym("otab")));
        CHECK(an.at(b.sym("otab")) == nullptr);

        const JumpTable* ptr = find(an, JumpTable::Kind::Pointers);
        CHECK(ptr != nullptr);
        if (ptr) {
            CHECK(!ptr->followed);                       // only a suggestion
            CHECK_EQ(ptr->table, b.sym("ptab"));
            CHECK_EQ(ptr->slots.size(), std::size_t{3});
        }
        CHECK(an.at(b.sym("p0")) == nullptr);            // not followed: the handlers are not code (yet)
    }

    // ---- an annotated pointer region is followed ------------------------------------------------------------------------------------------------
    {
        const Built b(kProgram);
        Analysis an(b.mem, b.from, b.to);
        an.addEntry(b.sym("callit"));
        an.addPointerTable(b.sym("ptab"), 12);
        an.analyzeFlow();
        CHECK(an.at(b.sym("p0")) != nullptr);
        CHECK(an.at(b.sym("p2")) != nullptr);
        CHECK(an.routines().size() >= 4);                 // callit and the three handlers
    }

    // ---- the listing: verified, with the tables shown ------------------------------------------------------------------------------------------
    {
        const Built b(kProgram);
        const Analysis an = analyse(b, {"start", "dispatch", "callit"});
        const Annotations ann;
        ListingOptions lo;
        lo.from = b.from;
        lo.to = b.to;
        const ListingResult r = renderListing(b.mem, an, ann, lo);
        CHECK(r.verified);
        CHECK(has(r.text, "jump table of the jump at $001004: 3 branch entries"));
        CHECK(has(r.text, "jump table of the jump at $001026: target = $00102A + signed word"));
        CHECK(has(r.text, "DC.W     $000A                        ; -> loc_001034"));
        CHECK(has(r.text, "DC.W     $0016                        ; -> loc_001040"));
        CHECK(has(r.text, "loc_001034:\n        MOVEQ    #$0A,D3"));
    }

    // ---- the session: analyze, jump_tables, ann_data, analyze again -------------------------------------------------------------------------------
    {
        Session s;
        CHECK(ok(call(s, "{\"cmd\":\"asm\",\"source\":\"" + escape(kProgram) + "\"}")));
        const Json a1 = call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\",\"dispatch\",\"callit\"]}");
        CHECK(ok(a1));
        CHECK_EQ(a1.find("jump_tables")->asInt(), std::int64_t{3});
        CHECK_EQ(a1.find("routines")->asInt(), std::int64_t{3});
        const Json jt = call(s, "{\"cmd\":\"jump_tables\"}");
        CHECK(ok(jt));
        const std::vector<Json>& tables = jt.find("tables")->items();
        CHECK_EQ(tables.size(), std::size_t{3});
        bool sawSuggestion = false;
        for (const Json& t : tables) {
            const std::string kind = t.find("kind")->asString();
            if (kind == "pointers") {
                sawSuggestion = true;
                CHECK(!t.find("followed")->asBool());
                CHECK(t.find("hint") != nullptr);
                CHECK_EQ(t.find("entry_count")->asInt(), std::int64_t{3});
            } else {
                CHECK(t.find("followed")->asBool());
                CHECK(t.find("jump") != nullptr);
            }
            if (kind == "offset") CHECK_EQ(t.find("base")->asString(), std::string("0x00102A"));
        }
        CHECK(sawSuggestion);
        CHECK_EQ(call(s, "{\"cmd\":\"jump_tables\",\"max_entries\":1}").find("tables")->items()[0].find("entries")->items().size(), std::size_t{1});
        // follow the suggestion: mark the region, analyse again
        CHECK(ok(call(s, "{\"cmd\":\"ann_data\",\"addr\":\"ptab\",\"length\":12,\"kind\":\"pointers\",\"name\":\"handlers\"}")));
        const Json a2 = call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\",\"dispatch\",\"callit\"]}");
        CHECK_EQ(a2.find("routines")->asInt(), std::int64_t{6});
        const Json lst = call(s, "{\"cmd\":\"listing\",\"verify\":true}");
        CHECK(lst.find("verified")->asBool());
        CHECK(has(lst.find("listing")->asString(), "DC.L     sub_00105C,sub_001060,sub_001064"));
    }

    // ---- random programs: the right table, the right entries, a verified listing -----------------------------------------------------------------
    {
        std::mt19937 rng(1234);
        int branchTables = 0, offsetTables = 0;
        for (int trial = 0; trial < 300; ++trial) {
            const unsigned n = 2 + static_cast<unsigned>(rng() % 7);
            const bool branch = rng() % 2;
            const std::string idx = "D" + std::to_string(1 + rng() % 7);
            std::string src = "        ORG $1000\nstart:  MOVEQ #0,D0\n";
            for (unsigned k = static_cast<unsigned>(rng() % 4); k > 0; --k) src += "        NOP\n";
            if (branch) {
                src += "        JMP tab(PC," + idx + ".W)\ntab:\n";
                for (unsigned i = 0; i < n; ++i) src += "        BRA h" + std::to_string(i) + "\n";
            } else {
                src += "        MOVE.W tab(PC," + idx + ".W)," + idx + "\n        JMP base(PC," + idx + ".W)\nbase:\n";
                if (rng() % 2) src += "        NOP\n";
                src += "tab:\n";
                for (unsigned i = 0; i < n; ++i) src += "        DC.W h" + std::to_string(i) + "-base\n";
            }
            for (unsigned i = 0; i < n; ++i) src += "h" + std::to_string(i) + ":\n        MOVEQ #" + std::to_string(i) + ",D2\n        RTS\n";
            const Built b(src);
            const Analysis an = analyse(b, {"start"});
            const JumpTable* t = find(an, branch ? JumpTable::Kind::Branch : JumpTable::Kind::Offset);
            CHECK(t != nullptr);
            if (!t) continue;
            CHECK_EQ(an.jumpTables().size(), std::size_t{1});
            CHECK_EQ(t->slots.size(), static_cast<std::size_t>(n));
            CHECK_EQ(t->table, b.sym("tab"));
            for (unsigned i = 0; i < n; ++i) {
                CHECK_EQ(t->targets[i], b.sym("h" + std::to_string(i)));
                CHECK(an.at(b.sym("h" + std::to_string(i))) != nullptr);
            }
            (branch ? branchTables : offsetTables)++;
            const Annotations ann;
            ListingOptions lo;
            lo.from = b.from;
            lo.to = b.to;
            const ListingResult r = renderListing(b.mem, an, ann, lo);
            CHECK(r.verified);
            if (!r.verified) std::fprintf(stderr, "%s\n--\n%s\n", src.c_str(), r.text.c_str());
        }
        CHECK(branchTables > 80);
        CHECK(offsetTables > 80);
    }

    // ---- what is not a table ------------------------------------------------------------------------------------------------------------------------
    {
        // a single entry is not a table; neither is an indexed jump into ordinary code
        const Built one(
            "        ORG $1000\nstart:  MOVE.W tab(PC,D0.W),D0\n        JMP base(PC,D0.W)\nbase:\ntab:    DC.W h0-base\n        DC.W $FFFF\nh0:     RTS\n");
        const Analysis a1 = analyse(one, {"start"});
        CHECK(find(a1, JumpTable::Kind::Offset) == nullptr);
        const Built code("        ORG $1000\nstart:  JMP nx(PC,D0.W)\nnx:     MOVEQ #1,D0\n        RTS\n");
        const Analysis a2 = analyse(code, {"start"});
        CHECK(a2.jumpTables().empty());
        // a table whose entries lead out of the analysed range ends at the first bad entry
        const Built far(
            "        ORG $1000\nstart:  MOVE.W tab(PC,D0.W),D0\n        JMP base(PC,D0.W)\nbase:\ntab:    DC.W h0-base,h1-base,$7000,$7002\n"
            "h0:     RTS\nh1:     RTS\n");
        const Analysis a3 = analyse(far, {"start"});
        const JumpTable* t = find(a3, JumpTable::Kind::Offset);
        CHECK(t != nullptr);
        if (t) CHECK_EQ(t->slots.size(), std::size_t{2});
        // zero words are not a pointer table
        const Built zeros("        ORG $1000\nstart:  LEA tab(PC),A0\n        RTS\ntab:    DC.L 0,0,0\n");
        CHECK(find(analyse(zeros, {"start"}), JumpTable::Kind::Pointers) == nullptr);
    }

    // ---- random bytes with indexed jumps: no crash, the listing always verifies ------------------------------------------------------------------------
    {
        std::mt19937 rng(4321);
        for (int trial = 0; trial < 400; ++trial) {
            Memory mem;
            const unsigned size = 64 + static_cast<unsigned>(rng() % 96) * 2;
            std::vector<Byte> bytes(size);
            for (Byte& x : bytes) x = static_cast<Byte>(rng());
            for (int k = 0; k < 4; ++k) {   // JMP/JSR (d8,PC,Xn) and MOVE.W (d8,PC,Xn),Dm at random even places
                const unsigned at = static_cast<unsigned>(rng() % (size / 2 - 2)) * 2;
                const Word words[] = {static_cast<Word>(rng() % 2 ? 0x4EFB : 0x4EBB), static_cast<Word>(rng() % 0x800), static_cast<Word>(0x303B | ((rng() % 8) << 9)), static_cast<Word>(rng() % 0x800)};
                const unsigned w = rng() % 2 ? 0U : 2U;
                bytes[at] = static_cast<Byte>(words[w] >> 8);
                bytes[at + 1] = static_cast<Byte>(words[w]);
                bytes[at + 2] = static_cast<Byte>(words[w + 1] >> 8);
                bytes[at + 3] = static_cast<Byte>(words[w + 1]);
            }
            for (unsigned i = 0; i < size; ++i) mem.write8(0x1000 + i, bytes[i]);
            Analysis an(mem, 0x1000, 0x1000 + size);
            an.addEntry(0x1000);
            an.analyzeFlow();
            an.analyzeLinear(0x1000, 0x1000 + size);
            const Annotations ann;
            ListingOptions lo;
            lo.from = 0x1000;
            lo.to = 0x1000 + size;
            const ListingResult r = renderListing(mem, an, ann, lo);
            CHECK(r.verified);
            if (!r.verified) std::fprintf(stderr, "trial %d: %s\n", trial, r.problem.c_str());
        }
    }

    return finish("jumptable");
}
