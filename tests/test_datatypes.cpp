// Data type detection: strings, zero fill, tables of string pointers; the session command and the listing.
#include <random>
#include <string>

#include "m68k/analysis.hpp"
#include "m68k/annotations.hpp"
#include "m68k/assembler.hpp"
#include "m68k/listing.hpp"
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

Analysis analyse(const Built& b, const char* entry = "start") {
    Analysis an(b.mem, b.from, b.to);
    an.addEntry(b.sym(entry));
    an.analyzeFlow();
    return an;
}

const DataGuess* at(const std::vector<DataGuess>& g, Address a) {
    for (const DataGuess& x : g)
        if (x.start == a) return &x;
    return nullptr;
}

Json call(Session& s, const std::string& line) { return Json::parse(s.handleLine(line)); }
bool ok(const Json& j) { return j.find("ok") && j.find("ok")->asBool(); }
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string escape(const std::string& t) {
    std::string out;
    for (const char c : t) {
        if (c == '\n') out += "\\n";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

const char* kProgram =
    "        ORG $1000\n"
    "start:  LEA     msg(PC),A0\n"
    "        LEA     names(PC),A1\n"
    "        MOVEQ   #0,D0\n"
    "        RTS\n"
    "msg:    DC.B    \"Hello, Amiga world\",0\n"
    "        EVEN\n"
    "lib:    DC.B    \"dos.library\",0\n"
    "        EVEN\n"
    "n0:     DC.B    \"Alice\",0\n"
    "n1:     DC.B    \"Bob\",0\n"
    "        EVEN\n"
    "names:  DC.L    n0,n1,msg\n"
    "zeros:  DS.B    32\n"
    "plain:  DC.B    1,2,3,4,5,6,7,8\n";

}  // namespace

int main() {
    // ---- the example --------------------------------------------------------------------------------------------------------------------------------
    {
        const Built b(kProgram);
        const Analysis an = analyse(b);
        const std::vector<DataGuess> g = an.guessData();
        CHECK_EQ(g.size(), std::size_t{6});
        const DataGuess* msg = at(g, b.sym("msg"));
        CHECK(msg != nullptr);
        if (msg) {
            CHECK(msg->kind == DataGuess::Kind::Text);
            CHECK_EQ(msg->length, 20U);                         // 18 characters, the NUL and the NUL that pads to an even length
            CHECK_EQ(msg->preview, std::string("Hello, Amiga world"));
            CHECK(has(msg->reason, "referenced"));
        }
        const DataGuess* lib = at(g, b.sym("lib"));
        CHECK(lib != nullptr && lib->kind == DataGuess::Kind::Text && lib->length == 12);
        const DataGuess* bob = at(g, b.sym("n1"));
        CHECK(bob != nullptr && bob->kind == DataGuess::Kind::Text && bob->length == 4);   // too short on its own: found through the table
        if (bob) CHECK(has(bob->reason, "table of string pointers"));
        const DataGuess* tab = at(g, b.sym("names"));
        CHECK(tab != nullptr && tab->kind == DataGuess::Kind::Pointers && tab->length == 12);
        const DataGuess* z = at(g, b.sym("zeros"));
        CHECK(z != nullptr && z->kind == DataGuess::Kind::Zeros && z->length == 32);
        CHECK(at(g, b.sym("plain")) == nullptr);                // 1..8 is nothing we can name
        // annotated bytes are left alone
        const std::vector<DataGuess> g2 = an.guessData([&](Address a) { return a >= b.sym("msg") && a < b.sym("lib"); });
        CHECK(at(g2, b.sym("msg")) == nullptr);
        CHECK(at(g2, b.sym("lib")) != nullptr);
    }

    // ---- what is not text ------------------------------------------------------------------------------------------------------------------------------
    {
        // a row of RTS instructions looks like "NuNuNuNu" but is not text; punctuation and short runs are not text either
        const Built b(
            "        ORG $1000\nstart:  RTS\nrow:    DC.B \"NuNuNuNu\"\npunct:  DC.B \"!@#$%^&*()\",0\nshort:  DC.B \"ab\",0\n"
            "four:   DC.B \"abcd\",0\nlong:   DC.B \"just a long string without a terminator\"\n");
        const std::vector<DataGuess> g = analyse(b).guessData();
        CHECK(at(g, b.sym("row")) == nullptr);
        CHECK(at(g, b.sym("punct")) == nullptr);
        CHECK(at(g, b.sym("short")) == nullptr);
        CHECK(at(g, b.sym("four")) != nullptr);                  // NUL-terminated
        const DataGuess* l = at(g, b.sym("long"));
        CHECK(l != nullptr && l->length == 39U);                  // long enough without a terminator
        // the same row is text if an instruction refers to it
        const Built c("        ORG $1000\nstart:  LEA row(PC),A0\n        RTS\nrow:    DC.B \"NuNuNuNu\"\n");
        CHECK(at(analyse(c).guessData(), c.sym("row")) != nullptr);
        // code is never data
        const Built d("        ORG $1000\nstart:  MOVEQ #65,D0\n        MOVEQ #66,D1\n        RTS\n");
        CHECK(analyse(d).guessData().empty());
        // a table of three short strings
        const Built e(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        RTS\ntab:    DC.L a,b,c\na:      DC.B \"No\",0\nb:      DC.B \"OK\",0\nc:      DC.B \"A\",0\n");
        const std::vector<DataGuess> ge = analyse(e).guessData();
        const DataGuess* t = at(ge, e.sym("tab"));
        CHECK(t != nullptr && t->kind == DataGuess::Kind::Pointers);
        CHECK(at(ge, e.sym("a")) != nullptr);
        CHECK(at(ge, e.sym("c")) != nullptr);
        // two short strings are not enough evidence
        const Built f(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        RTS\ntab:    DC.L a,b\na:      DC.B \"No\",0\nb:      DC.B \"OK\",0\n");
        CHECK(at(analyse(f).guessData(), f.sym("tab")) == nullptr);
    }

    // ---- tables of numbers: found through the way the code walks through them ---------------------------------------------------------------------------------
    {
        // words: LEA tab,A0 then ADD.W (A0)+,D0 in a loop
        const Built w(
            "        ORG $1000\nstart:  LEA wtab(PC),A0\n        MOVEQ #3,D1\n        MOVEQ #0,D0\nloop:   ADD.W (A0)+,D0\n        DBRA D1,loop\n        RTS\n"
            "wtab:   DC.W 3,1,4,1,5\n");
        const std::vector<DataGuess> gw = analyse(w).guessData();
        const DataGuess* t = at(gw, w.sym("wtab"));
        CHECK(t != nullptr && t->kind == DataGuess::Kind::Words && t->length == 10);
        if (t) CHECK(has(t->reason, "ADD.W (A0)+,D0"));
        // longs through an index register, the address given as an immediate value
        const Built l(
            "        ORG $1000\nstart:  MOVE.L #ltab,A1\n        MOVE.L (A1,D2.W),D0\n        RTS\nltab:   DC.L 100000,200000,300000\n");
        const std::vector<DataGuess> gl = analyse(l).guessData();
        const DataGuess* tl = at(gl, l.sym("ltab"));
        CHECK(tl != nullptr && tl->kind == DataGuess::Kind::Longs && tl->length == 12);
        // no guess: mixed sizes (a structure), fixed displacements only, a single entry, an empty buffer, bytes, a table nobody walks through
        const Built mixed(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        MOVE.W (A0)+,D0\n        MOVE.L (A0)+,D1\n        RTS\ntab:    DC.W 1,2,3,4,5,6\n");
        CHECK(analyse(mixed).guessData().empty());
        const Built disp(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        MOVE.W 2(A0),D0\n        MOVE.W 4(A0),D1\n        RTS\ntab:    DC.W 1,2,3,4,5,6\n");
        CHECK(analyse(disp).guessData().empty());
        const Built single(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        MOVE.W (A0)+,D0\n        RTS\ntab:    DC.W 7\n");
        CHECK(analyse(single).guessData().empty());
        const Built zero(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        MOVE.W (A0)+,D0\n        RTS\ntab:    DC.W 0,0,0,0\n");
        CHECK(analyse(zero).guessData().empty());
        const Built bytes(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        MOVE.B (A0)+,D0\n        RTS\ntab:    DC.B 1,2,3,4\n");
        CHECK(analyse(bytes).guessData().empty());
        const Built unused("        ORG $1000\nstart:  RTS\ntab:    DC.W 1,2,3,4,5,6\n");
        CHECK(analyse(unused).guessData().empty());
        // the register is reloaded or a call comes in between: the evidence stops
        const Built reload(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        LEA other(PC),A0\n        MOVE.W (A0)+,D0\n        RTS\ntab:    DC.W 1,2,3,4\nother:  DC.W 9,9\n");
        const std::vector<DataGuess> gr = analyse(reload).guessData();
        CHECK(at(gr, reload.sym("tab")) == nullptr);
        CHECK(at(gr, reload.sym("other")) != nullptr);
        const Built withCall(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        BSR sub\n        MOVE.W (A0)+,D0\n        RTS\nsub:    RTS\ntab:    DC.W 1,2,3,4\n");
        CHECK(analyse(withCall).guessData().empty());
        // the table ends in front of the next referenced address
        const Built ends(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        MOVE.W (A0)+,D0\n        LEA next(PC),A1\n        RTS\ntab:    DC.W 1,2,3,4\nnext:   DC.W 9,9,9\n");
        const std::vector<DataGuess> ge2 = analyse(ends).guessData();
        const DataGuess* te = at(ge2, ends.sym("tab"));
        CHECK(te != nullptr && te->length == 8);
        // a table that is annotated already is left alone
        const Analysis an = analyse(w);
        const std::vector<DataGuess> gc = an.guessData([&](Address a) { return a == w.sym("wtab"); });
        CHECK(at(gc, w.sym("wtab")) == nullptr);
        // a copy loop that moves text with longword accesses does not make the text a table of numbers; random numbers that look like letters do not make a table text
        const Built copy(
            "        ORG $1000\nstart:  LEA msg(PC),A0\n        LEA buf(PC),A1\n        MOVEQ #5,D1\nloop:   MOVE.L (A0)+,(A1)+\n        DBRA D1,loop\n        RTS\n"
            "msg:    DC.B \"Copy this whole sentence\",0\n        EVEN\nbuf:    DC.L 0,0,0,0,0,0\n");
        const std::vector<DataGuess> gcp = analyse(copy).guessData();
        const DataGuess* tc = at(gcp, copy.sym("msg"));
        CHECK(tc != nullptr && tc->kind == DataGuess::Kind::Text);
        // as an annotation the table is shown as words
        Session s;
        CHECK(ok(call(s, "{\"cmd\":\"asm\",\"source\":\"" + escape(
            "        ORG $1000\nstart:  LEA wtab(PC),A0\n        MOVEQ #3,D1\n        MOVEQ #0,D0\nloop:   ADD.W (A0)+,D0\n        DBRA D1,loop\n        RTS\nwtab:   DC.W 3,1,4,1,5,9\n") + "\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\"]}")));
        const Json r = call(s, "{\"cmd\":\"data_regions\",\"apply\":true}");
        CHECK_EQ(r.find("regions")->items()[0].find("kind")->asString(), std::string("words"));
        CHECK(ok(call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\"]}")));
        const Json lst = call(s, "{\"cmd\":\"listing\",\"verify\":true}");
        CHECK(lst.find("verified")->asBool());
        CHECK(has(lst.find("listing")->asString(), "DC.W     $0003,$0001,$0004,$0001,$0005,$0009"));
    }

    // ---- scalar variables: the size the code accesses them with ------------------------------------------------------------------------------------------------
    {
        const Built b(
            "        ORG $1000\nstart:  MOVE.W D0,wvar\n        MOVE.W wvar,D1\n        MOVE.L D0,lvar\n        MOVE.B D0,bvar\n        RTS\n"
            "wvar:   DC.W 0\nlvar:   DC.L 0\nbvar:   DC.B 0\n");
        const std::vector<DataGuess> g = analyse(b).guessData();
        const DataGuess* w = at(g, b.sym("wvar"));
        CHECK(w != nullptr && w->kind == DataGuess::Kind::Words && w->length == 2);
        if (w) CHECK(has(w->reason, "word variable"));
        const DataGuess* l = at(g, b.sym("lvar"));
        CHECK(l != nullptr && l->kind == DataGuess::Kind::Longs && l->length == 4);
        CHECK(at(g, b.sym("bvar")) == nullptr);                 // a byte says nothing the listing does not show already
        // no guess: mixed sizes, an address that is taken, a byte-wise neighbour in the middle, outside the program
        const Built mixed("        ORG $1000\nstart:  MOVE.W D0,v\n        MOVE.L D0,v\n        RTS\nv:      DC.L 0\n");
        CHECK(analyse(mixed).guessData().empty());
        const Built taken("        ORG $1000\nstart:  LEA v,A0\n        MOVE.W D0,v\n        RTS\nv:      DC.W 0\n");
        CHECK(analyse(taken).guessData().empty());
        const Built inside("        ORG $1000\nstart:  MOVE.L D0,v\n        MOVE.W v+2,D1\n        RTS\nv:      DC.L 0\n");
        const std::vector<DataGuess> gi = analyse(inside).guessData();
        CHECK(at(gi, inside.sym("v")) == nullptr);
        const Built outside("        ORG $1000\nstart:  MOVE.W D0,$2000.W\n        MOVE.L D0,$100000\n        RTS\n");
        CHECK(analyse(outside).guessData().empty());
        // a direct access into the middle of a table is a boundary like any other referenced address (otherwise the table would swallow the variables behind it,
        // in a hunk program the next hunk): the table is cut short and gets no guess. Known limit, see docs/reverse-engineering.md.
        const Built array(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        MOVE.W (A0)+,D0\n        MOVE.W tab+2,D1\n        RTS\ntab:    DC.W 1,2,3,4\n");
        const std::vector<DataGuess> ga = analyse(array).guessData();
        CHECK(at(ga, array.sym("tab")) == nullptr);
        // an array that is only walked through keeps its kind and its full length
        const Built walked(
            "        ORG $1000\nstart:  LEA tab(PC),A0\n        MOVE.W (A0)+,D0\n        MOVE.W D0,total\n        RTS\ntab:    DC.W 1,2,3,4\ntotal:  DC.W 0\n");
        const std::vector<DataGuess> gw2 = analyse(walked).guessData();
        const DataGuess* tw = at(gw2, walked.sym("tab"));
        CHECK(tw != nullptr && tw->kind == DataGuess::Kind::Words && tw->length == 8);   // ends in front of `total`, which is referenced itself
        const DataGuess* tot = at(gw2, walked.sym("total"));
        CHECK(tot != nullptr && tot->kind == DataGuess::Kind::Words && tot->length == 2);
        // as annotations they are shown with their size
        Session s;
        CHECK(ok(call(s, "{\"cmd\":\"asm\",\"source\":\"" + escape(
            "        ORG $1000\nstart:  MOVE.W D0,wvar\n        MOVE.L D0,lvar\n        RTS\nwvar:   DC.W 0\nlvar:   DC.L 0\n") + "\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\"]}")));
        CHECK_EQ(call(s, "{\"cmd\":\"data_regions\",\"apply\":true}").find("applied")->asInt(), std::int64_t{2});
        CHECK(ok(call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\"]}")));
        const Json lst = call(s, "{\"cmd\":\"listing\",\"verify\":true}");
        CHECK(lst.find("verified")->asBool());
        CHECK(has(lst.find("listing")->asString(), "DC.W     $0000\n"));
        CHECK(has(lst.find("listing")->asString(), "DC.L     $00000000\n"));
    }
    {
        // random programs: every variable gets exactly its size, the listing verifies
        std::mt19937 rng(8080);
        int words = 0, longs = 0;
        for (int trial = 0; trial < 200; ++trial) {
            const unsigned n = 2 + static_cast<unsigned>(rng() % 6);
            std::vector<bool> isLong(n);
            std::string code = "        ORG $1000\nstart:\n", data;
            for (unsigned i = 0; i < n; ++i) {
                isLong[i] = rng() % 2;
                const std::string name = "v" + std::to_string(i);
                const char* sz = isLong[i] ? "L" : "W";
                for (unsigned k = 1 + static_cast<unsigned>(rng() % 3); k > 0; --k)
                    code += rng() % 2 ? std::string("        MOVE.") + sz + " D" + std::to_string(rng() % 8) + "," + name + "\n"
                                      : std::string("        MOVE.") + sz + " " + name + ",D" + std::to_string(rng() % 8) + "\n";
                data += name + ": DC." + sz + " " + std::to_string(rng() % 1000) + "\n";
                if (rng() % 3 == 0) data += "        DC.B 7\n        EVEN\n";
            }
            const Built b(code + "        RTS\n" + data);
            const Analysis an = analyse(b);
            const std::vector<DataGuess> g = an.guessData();
            Annotations ann;
            for (unsigned i = 0; i < n; ++i) {
                const DataGuess* x = at(g, b.sym("v" + std::to_string(i)));
                CHECK(x != nullptr);
                if (!x) continue;
                CHECK(x->kind == (isLong[i] ? DataGuess::Kind::Longs : DataGuess::Kind::Words));
                CHECK_EQ(x->length, isLong[i] ? 4U : 2U);
                (isLong[i] ? longs : words)++;
            }
            for (const DataGuess& x : g) {
                DataRegion region;
                region.address = x.start;
                region.length = x.length;
                region.kind = x.kind == DataGuess::Kind::Text ? DataKind::Text : x.kind == DataGuess::Kind::Words ? DataKind::Words : x.kind == DataGuess::Kind::Longs ? DataKind::Longs : DataKind::Pointers;
                if (x.kind != DataGuess::Kind::Zeros) (void)ann.setData(region);
            }
            Analysis an2(b.mem, b.from, b.to);
            an2.addEntry(b.sym("start"));
            an2.setDataBarrier([&ann](Address a) { return ann.dataCovering(a) != nullptr; });
            an2.analyzeFlow();
            ListingOptions lo;
            lo.from = b.from;
            lo.to = b.to;
            const ListingResult r = renderListing(b.mem, an2, ann, lo);
            CHECK(r.verified);
            if (!r.verified) std::fprintf(stderr, "%s\n--\n%s\n", (code + data).c_str(), r.text.c_str());
        }
        CHECK(words > 100);
        CHECK(longs > 100);
    }

    // ---- the session: data_regions, apply, listing --------------------------------------------------------------------------------------------------
    {
        Session s;
        CHECK(ok(call(s, "{\"cmd\":\"asm\",\"source\":\"" + escape(kProgram) + "\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\"]}")));
        const Json r = call(s, "{\"cmd\":\"data_regions\"}");
        CHECK(ok(r));
        const std::vector<Json>& regions = r.find("regions")->items();
        CHECK_EQ(regions.size(), std::size_t{6});
        CHECK_EQ(regions[0].find("kind")->asString(), std::string("text"));
        CHECK_EQ(regions[0].find("preview")->asString(), std::string("Hello, Amiga world"));
        CHECK(regions[0].find("applied") == nullptr);            // without apply nothing is changed
        const Json none = call(s, "{\"cmd\":\"ann_list\"}");
        CHECK(has(none.dump(), "\"data\":[]"));

        const Json applied = call(s, "{\"cmd\":\"data_regions\",\"apply\":true}");
        CHECK_EQ(applied.find("applied")->asInt(), std::int64_t{5});       // zero fill needs no annotation (the listing writes DS.B)
        // a second call finds nothing new: everything is annotated now
        const Json again = call(s, "{\"cmd\":\"data_regions\",\"apply\":true}");
        CHECK_EQ(again.find("applied")->asInt(), std::int64_t{0});
        CHECK(ok(call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\"]}")));
        const Json lst = call(s, "{\"cmd\":\"listing\",\"verify\":true}");
        CHECK(lst.find("verified")->asBool());
        const std::string text = lst.find("listing")->asString();
        CHECK(has(text, "DC.B     \"Hello, Amiga world\",0\n        EVEN\n"));
        CHECK(has(text, "DC.B     \"dos.library\",0\n"));
        CHECK(has(text, "DC.B     \"Bob\",0\n"));
        CHECK(has(text, "DC.L     data_00102C,data_001032,data_00100C"));
        CHECK(has(text, "DS.B     32"));
    }

    // ---- control characters stay in the string statement -------------------------------------------------------------------------------------------------
    {
        Session s;
        const std::string src =
            "        ORG $1000\nstart:  LEA msg(PC),A0\n        RTS\n"
            "msg:    DC.B \"Line one\",10,\"Line two\",13,10,0\n"
            "tab:    DC.B \"a\tb c d\",0\n"
            "odd:    DC.B \"x\",1,2,\"yz\",0\n        EVEN\n";
        CHECK(ok(call(s, "{\"cmd\":\"asm\",\"source\":\"" + escape(src) + "\"}")));
        CHECK(ok(call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\"]}")));
        CHECK(ok(call(s, "{\"cmd\":\"data_regions\",\"apply\":true}")));
        CHECK(ok(call(s, "{\"cmd\":\"analyze\",\"entries\":[\"start\"]}")));
        const Json lst = call(s, "{\"cmd\":\"listing\",\"verify\":true}");
        CHECK(lst.find("verified")->asBool());
        const std::string text = lst.find("listing")->asString();
        CHECK(has(text, "DC.B     \"Line one\",10,\"Line two\",13,10,0"));
        CHECK(has(text, "DC.B     $61,9,\"b c d\",0"));      // a single character is written as a number
    }

    // ---- random strings: the listing always verifies, the strings are found -----------------------------------------------------------------------------------
    {
        std::mt19937 rng(2024);
        int found = 0, total = 0;
        for (int trial = 0; trial < 300; ++trial) {
            const unsigned n = 1 + static_cast<unsigned>(rng() % 5);
            std::string src = "        ORG $1000\nstart:\n";
            const bool allTerminated = rng() % 2 == 0;   // strings without a NUL run into the next one: only count the strings of clean programs
            for (unsigned i = 0; i < n; ++i) src += "        LEA s" + std::to_string(i) + "(PC),A" + std::to_string(i % 7) + "\n";
            src += "        RTS\n";
            std::vector<std::string> strings;
            for (unsigned i = 0; i < n; ++i) {
                std::string str;
                const unsigned len = 4 + static_cast<unsigned>(rng() % 16);
                for (unsigned k = 0; k < len; ++k) str += "abcdefghijklmnopqrstuvwxyz ABCDEFG.,:"[rng() % 37];
                strings.push_back(str);
                const bool nul = allTerminated || rng() % 4 != 0;
                src += "s" + std::to_string(i) + ": DC.B \"" + str + "\"" + (nul ? ",0" : "") + "\n";
                if (rng() % 2) src += "        EVEN\n";
            }
            src += "tail:   DC.B 1,2,3\n";
            const Built b(src);
            const Analysis an = analyse(b);
            Annotations ann;
            for (const DataGuess& g : an.guessData())
                if (g.kind != DataGuess::Kind::Zeros) {
                    DataRegion region;
                    region.address = g.start;
                    region.length = g.length;
                    region.kind = g.kind == DataGuess::Kind::Text ? DataKind::Text : g.kind == DataGuess::Kind::Words ? DataKind::Words : g.kind == DataGuess::Kind::Longs ? DataKind::Longs : DataKind::Pointers;
                    (void)ann.setData(region);
                }
            const std::vector<DataGuess> guessed = an.guessData();
            for (unsigned i = 0; i < n && allTerminated; ++i) {
                ++total;
                const DataGuess* g = at(guessed, b.sym("s" + std::to_string(i)));
                if (g && g->kind == DataGuess::Kind::Text && g->preview.substr(0, 20) == strings[i].substr(0, 20)) ++found;
                else std::fprintf(stderr, "  missed string %u: \"%s\" (guess: %s, kind %d, length %u, preview length %zu, string length %zu)\n", i, strings[i].c_str(), g ? g->preview.c_str() : "none", g ? static_cast<int>(g->kind) : -1, g ? g->length : 0, g ? g->preview.size() : 0, strings[i].size());
            }
            Analysis an2(b.mem, b.from, b.to);
            an2.addEntry(b.sym("start"));
            an2.setDataBarrier([&ann](Address a) { return ann.dataCovering(a) != nullptr; });
            an2.analyzeFlow();
            ListingOptions lo;
            lo.from = b.from;
            lo.to = b.to;
            const ListingResult r = renderListing(b.mem, an2, ann, lo);
            CHECK(r.verified);
            if (!r.verified) std::fprintf(stderr, "%s\n--\n%s\n", src.c_str(), r.text.c_str());
        }
        CHECK(total > 200);
        CHECK(found == total);
    }

    // ---- random number tables: kind and extent are exact, the listing verifies --------------------------------------------------------------------------------
    {
        std::mt19937 rng(555);
        int words = 0, longs = 0;
        for (int trial = 0; trial < 200; ++trial) {
            const unsigned n = 2 + static_cast<unsigned>(rng() % 9);
            const bool isLong = rng() % 2;
            const bool indexed = rng() % 2;
            std::string src = "        ORG $1000\nstart:  LEA tab(PC),A0\n        LEA tail(PC),A1\n        MOVEQ #3,D1\n        MOVEQ #0,D0\n";
            src += indexed ? std::string("loop:   ADD.") + (isLong ? "L" : "W") + " (A0,D2.W),D0\n        ADDQ.W #2,D2\n        DBRA D1,loop\n"
                           : std::string("loop:   ADD.") + (isLong ? "L" : "W") + " (A0)+,D0\n        DBRA D1,loop\n";
            src += "        RTS\ntab:    DC." + std::string(isLong ? "L" : "W") + " ";
            for (unsigned i = 0; i < n; ++i) src += (i ? "," : "") + std::to_string(1 + rng() % (isLong ? 100000 : 30000));
            src += "\ntail:   DC.B 1,2,3\n";
            const Built b(src);
            const Analysis an = analyse(b);
            const std::vector<DataGuess> g = an.guessData();
            const DataGuess* t = at(g, b.sym("tab"));
            CHECK(t != nullptr);
            if (!t) continue;
            CHECK(t->kind == (isLong ? DataGuess::Kind::Longs : DataGuess::Kind::Words));
            CHECK_EQ(t->length, n * (isLong ? 4U : 2U));
            (isLong ? longs : words)++;
            Annotations ann;
            for (const DataGuess& x : g) {
                DataRegion region;
                region.address = x.start;
                region.length = x.length;
                region.kind = x.kind == DataGuess::Kind::Text ? DataKind::Text : x.kind == DataGuess::Kind::Words ? DataKind::Words : x.kind == DataGuess::Kind::Longs ? DataKind::Longs : DataKind::Pointers;
                if (x.kind != DataGuess::Kind::Zeros) (void)ann.setData(region);
            }
            Analysis an2(b.mem, b.from, b.to);
            an2.addEntry(b.sym("start"));
            an2.setDataBarrier([&ann](Address a) { return ann.dataCovering(a) != nullptr; });
            an2.analyzeFlow();
            ListingOptions lo;
            lo.from = b.from;
            lo.to = b.to;
            const ListingResult r = renderListing(b.mem, an2, ann, lo);
            CHECK(r.verified);
            if (!r.verified) std::fprintf(stderr, "%s\n--\n%s\n", src.c_str(), r.text.c_str());
        }
        CHECK(words > 60);
        CHECK(longs > 60);
    }

    // ---- random printable-biased bytes: no crash, annotations never break the listing -------------------------------------------------------------------------
    {
        std::mt19937 rng(77);
        int guesses = 0;
        for (int trial = 0; trial < 400; ++trial) {
            const unsigned size = 64 + static_cast<unsigned>(rng() % 100) * 2;
            Memory mem;
            for (unsigned i = 0; i < size; ++i) {
                const unsigned r = static_cast<unsigned>(rng() % 10);
                const Byte b = r < 5 ? static_cast<Byte>(0x20 + rng() % 0x5F) : r < 7 ? Byte{0} : static_cast<Byte>(rng());
                mem.write8(0x1000 + i, b);
            }
            Analysis an(mem, 0x1000, 0x1000 + size);
            an.addEntry(0x1000);
            an.analyzeFlow();
            const std::vector<DataGuess> g = an.guessData();
            guesses += static_cast<int>(g.size());
            Annotations ann;
            Address lastEnd = 0;
            for (const DataGuess& x : g) {
                CHECK(x.start >= lastEnd);                         // guesses never overlap
                lastEnd = x.start + x.length;
                CHECK(lastEnd <= 0x1000 + size);
                CHECK(!an.isCode(x.start));
                if (x.kind == DataGuess::Kind::Zeros) continue;
                DataRegion region;
                region.address = x.start;
                region.length = x.length;
                region.kind = x.kind == DataGuess::Kind::Text ? DataKind::Text : x.kind == DataGuess::Kind::Words ? DataKind::Words : x.kind == DataGuess::Kind::Longs ? DataKind::Longs : DataKind::Pointers;
                (void)ann.setData(region);
            }
            Analysis an2(mem, 0x1000, 0x1000 + size);
            an2.addEntry(0x1000);
            an2.setDataBarrier([&ann](Address a) { return ann.dataCovering(a) != nullptr; });
            an2.analyzeFlow();
            ListingOptions lo;
            lo.from = 0x1000;
            lo.to = 0x1000 + size;
            const ListingResult r = renderListing(mem, an2, ann, lo);
            CHECK(r.verified);
            if (!r.verified) std::fprintf(stderr, "trial %d: %s\n", trial, r.problem.c_str());
        }
        CHECK(guesses > 100);
        // pure random bytes hardly ever look like strings
        int falsePositives = 0;
        for (int trial = 0; trial < 400; ++trial) {
            Memory mem;
            for (unsigned i = 0; i < 128; ++i) mem.write8(0x1000 + i, static_cast<Byte>(rng()));
            Analysis an(mem, 0x1000, 0x1080);
            an.analyzeLinear(0x1000, 0x1000);   // nothing is code
            for (const DataGuess& x : an.guessData())
                if (x.kind == DataGuess::Kind::Text) ++falsePositives;
        }
        CHECK(falsePositives <= 8);
    }

    return finish("datatypes");
}
