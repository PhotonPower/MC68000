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
                    region.kind = g.kind == DataGuess::Kind::Text ? DataKind::Text : DataKind::Pointers;
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
                region.kind = x.kind == DataGuess::Kind::Text ? DataKind::Text : DataKind::Pointers;
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
