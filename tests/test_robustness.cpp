// Randomised (but deterministic) robustness tests for everything that takes input from outside: a model talks to `m68ktool serve`, files come from
// disk. Nothing may crash, hang, run out of memory or throw anything but the documented errors:
//   * the JSON parser on garbage and on deeply nested input,
//   * the server: random commands with random arguments of every JSON type, in one long-lived session (so state builds up), each call with a time limit,
//   * the Amiga hunk loader, the S-record reader, snapshot files and annotation (project) files, fed with mutated valid data.
// Run it under the sanitizers as well (CI does).
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <string>

#include "m68k/annotations.hpp"
#include "m68k/assembler.hpp"
#include "m68k/cpu.hpp"
#include "m68k/hunk.hpp"
#include "m68k/image_io.hpp"
#include "m68k/json.hpp"
#include "m68k/session.hpp"
#include "m68k/snapshot.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

// M68K_FUZZ_SEED selects another random sequence (the default is fixed, so that CI is reproducible)
std::mt19937 rng(std::getenv("M68K_FUZZ_SEED") ? static_cast<std::uint32_t>(std::strtoul(std::getenv("M68K_FUZZ_SEED"), nullptr, 10)) : 0xBADC0DEU);

unsigned pick(unsigned n) { return static_cast<unsigned>(rng() % n); }

// M68K_FUZZ_SCALE (percent, default 100) scales the number of iterations: sanitizer builds are 10-20 times slower
// Sanitizer builds are far slower: the limit for one server call grows with them
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
constexpr double kSlowCallMs = 120000;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
constexpr double kSlowCallMs = 120000;
#else
constexpr double kSlowCallMs = 5000;
#endif
#else
constexpr double kSlowCallMs = 5000;
#endif

int scaled(int n) {
    const char* e = std::getenv("M68K_FUZZ_SCALE");
    const long percent = e ? std::strtol(e, nullptr, 10) : 100;
    return static_cast<int>(std::max<long>(20, n * percent / 100));
}

Json randomValue(int depth = 0) {
    static const char* const strings[] = {"", "0x1000", "$FF", "D0", "start", "loop", "sub_001000", "0x", "-", "1+", "(", "\"", "\\", "00FF", "ABC", "x", "SP", "A7",
                                          "PC", "SR", "0xFFFFFF", "0x1000000", "-1", "text", "bytes", "words", "longs", "pointers", "amiga500", "generic", "hunk", "srec",
                                          "raw", "r", "w", "rw", "high", "low", "medium", "address", "leaves", "\xF0\x9F\x98\x80", "a b c"};
    static const std::int64_t numbers[] = {0, 1, -1, 2, 3, 4, 7, 8, 100, 255, 256, 4096, 65535, 65536, 0x7FFFFF, 0xFFFFFF, 0x1000000, 0x7FFFFFFF, 0x80000000LL,
                                           0xFFFFFFFFLL, 0x100000000LL, 100000000, 100000001, std::int64_t{0x7FFFFFFFFFFFFFFF}, -std::int64_t{0x7FFFFFFFFFFFFFFF}};
    switch (pick(depth > 2 ? 6 : 9)) {
        case 0: return Json();
        case 1: return Json(rng() % 2 == 0);
        case 2:
        case 3: return Json(numbers[pick(sizeof numbers / sizeof numbers[0])]);
        case 4: return Json(1.5 * static_cast<double>(pick(1000)) - 300.0);
        case 5: return Json(std::string(strings[pick(sizeof strings / sizeof strings[0])]));
        case 6: {
            Json a;
            a.push(Json());  // an array
            Json arr = Json::parse("[]");
            for (unsigned i = pick(4); i > 0; --i) arr.push(randomValue(depth + 1));
            return arr;
        }
        case 7: {
            Json o = Json::parse("{}");
            for (unsigned i = pick(4); i > 0; --i) o.set(std::string(1, static_cast<char>('a' + pick(6))), randomValue(depth + 1));
            return o;
        }
        default: return Json(std::string(pick(2) ? 5000 : 70000, static_cast<char>('A' + pick(26))));   // long strings
    }
}

}  // namespace

int main() {
    // ---- the JSON parser -----------------------------------------------------------------------------------------------------------------
    {
        static const char* const tokens[] = {"{", "}", "[", "]", ",", ":", "\"a\"", "\"", "1", "-", "-0", "1.5e999", "1e-999", "true", "false", "null", "nul", "\\", "\\u00",
                                             "\\ud800", "\"\\ud800\\udc00\"", " ", "\n", "99999999999999999999999999", "0x10", ".5", "\"\\n\\t\\\"\"", "\xFF", "\xC3", "\xE2\x82"};
        int parsed = 0;
        for (int i = 0, n = scaled(40000); i < n; ++i) {
            std::string text;
            for (unsigned k = 1 + pick(14); k > 0; --k) text += tokens[pick(sizeof tokens / sizeof tokens[0])];
            try {
                const Json j = Json::parse(text);
                const std::string again = j.dump();
                CHECK_EQ(Json::parse(again).dump(), again);      // what was parsed can be written and read back identically
                ++parsed;
            } catch (const JsonError&) {
            }
        }
        CHECK(parsed > scaled(40000) / 100);
        // nesting: refused beyond a limit, not by overflowing the stack
        for (const char open : {'[', '{'}) {
            std::string deep(200000, open);
            if (open == '{') {
                deep.clear();
                for (int i = 0; i < 100000; ++i) deep += "{\"a\":";
            }
            bool refused = false;
            try {
                (void)Json::parse(deep);
            } catch (const JsonError&) {
                refused = true;
            }
            CHECK(refused);
        }
        // a line that is not even text
        std::string binary;
        for (int i = 0; i < 5000; ++i) binary += static_cast<char>(rng());
        try {
            (void)Json::parse(binary);
        } catch (const JsonError&) {
        }
    }

    // ---- the server ---------------------------------------------------------------------------------------------------------------------
    {
        Session s;
        auto send = [&](const std::string& line) { return s.handleLine(line); };
        // a program, an analysis and some annotations, so that every command has something to work on
        send("{\"cmd\":\"asm\",\"source\":\"        ORG $1000\\nstart:  LEA tab(PC),A0\\n        MOVEQ #3,D1\\nloop:   ADD.W (A0)+,D0\\n        DBRA D1,loop\\n        BSR sub\\n        RTS\\n"
             "sub:    MOVE.W D0,$2000.W\\n        RTS\\ntab:    DC.W 1,2,3,4\\nmsg:    DC.B \\\"Hello robust world\\\",0\\n\",\"entry\":\"start\"}");
        send("{\"cmd\":\"analyze\",\"entries\":[\"start\"]}");
        send("{\"cmd\":\"ann_label\",\"addr\":\"loop\",\"name\":\"add_loop\"}");
        send("{\"cmd\":\"checkpoint\",\"name\":\"base\"}");
        const Json help = Json::parse(send("{\"cmd\":\"help\"}"));
        std::vector<std::string> commands;
        for (const Json& c : help.find("commands")->items()) commands.push_back(c.asString());
        CHECK(commands.size() > 40);
        static const char* const keys[] = {"addr", "from", "to", "len", "length", "n", "count", "value", "hex", "bytes", "name", "text", "source", "entries", "entry", "regs",
                                           "registers", "type", "kind", "size", "comment", "block", "order", "max_instructions", "max_chars", "verify", "addresses",
                                           "undocumented_only", "chip_kb", "slow_kb", "fast_kb", "os", "apply", "reset", "level", "vector", "limit", "summary", "inputs",
                                           "outputs", "clobbers", "confidence", "base", "org", "symbols", "file", "id", "title", "max_entries", "trace", "clear", "mode", "var"};
        using Clock = std::chrono::steady_clock;
        int ok = 0, failed = 0;
        double slowest = 0;
        std::string slowestLine;
        const int calls = scaled(12000);
        for (int i = 0; i < calls; ++i) {
            Json request = Json::parse("{}");
            const std::string cmd = pick(25) == 0 ? std::string("garbage") + std::to_string(pick(100)) : commands[pick(static_cast<unsigned>(commands.size()))];
            if (pick(60) != 0) request.set("cmd", Json(cmd));
            for (unsigned k = pick(7); k > 0; --k) request.set(keys[pick(sizeof keys / sizeof keys[0])], randomValue());
            // `reset` and `machine` change the machine under our feet: allowed, but rarely, so that the state does build up
            if ((cmd == "machine" || cmd == "reset") && pick(4) != 0) continue;
            const std::string line = request.dump();
            const auto t0 = Clock::now();
            const std::string reply = send(line);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (ms > slowest) slowest = ms, slowestLine = line.substr(0, 200);
            Json r;
            bool valid = true;
            try {
                r = Json::parse(reply);
            } catch (const JsonError&) {
                valid = false;
            }
            CHECK(valid);
            if (!valid) {
                std::fprintf(stderr, "  not JSON: %.200s\n  request: %.300s\n", reply.c_str(), line.c_str());
                continue;
            }
            const Json* okField = r.find("ok");
            CHECK(r.isObject() && okField != nullptr && (okField->isBool()));
            if (okField && okField->asBool()) ++ok;
            else {
                ++failed;
                const Json* err = r.find("error");
                CHECK(err != nullptr && err->isString());
            }
            CHECK(ms < kSlowCallMs);
            if (ms >= kSlowCallMs) std::fprintf(stderr, "  slow call (%.0f ms): %.300s\n", ms, line.c_str());
        }
        CHECK(ok > scaled(12000) / 25);        // the fuzz is not just bouncing off argument checks
        CHECK(failed > scaled(12000) / 25);
        std::fprintf(stderr, "  server fuzz: %d ok, %d errors, slowest call %.0f ms (%s)\n", ok, failed, slowest, slowestLine.c_str());
        // the session still works afterwards
        CHECK(Json::parse(send("{\"cmd\":\"help\"}")).find("ok")->asBool());
        // limits: no argument can hang the server
        send("{\"cmd\":\"machine\",\"type\":\"generic\"}");
        send("{\"cmd\":\"asm\",\"source\":\"        ORG $1000\\nspin:   BRA.S spin\\n\",\"entry\":\"spin\"}");
        CHECK(!Json::parse(send("{\"cmd\":\"run\",\"max_instructions\":9000000000000000000}")).find("ok")->asBool());
        CHECK(!Json::parse(send("{\"cmd\":\"step\",\"n\":9000000000000000000}")).find("ok")->asBool());
        CHECK(!Json::parse(send("{\"cmd\":\"call\",\"addr\":\"spin\",\"max_instructions\":100000001}")).find("ok")->asBool());
    }

    // ---- files: mutated valid data -------------------------------------------------------------------------------------------------------------
    auto mutate = [](std::vector<Byte> bytes) {
        switch (pick(5)) {
            case 0:   // flip some bytes
                for (unsigned k = 1 + pick(8); k > 0 && !bytes.empty(); --k) bytes[pick(static_cast<unsigned>(bytes.size()))] ^= static_cast<Byte>(1U << pick(8));
                break;
            case 1:   // truncate
                if (!bytes.empty()) bytes.resize(pick(static_cast<unsigned>(bytes.size())));
                break;
            case 2:   // overwrite a longword with an extreme value
                if (bytes.size() >= 4) {
                    const unsigned at = pick(static_cast<unsigned>(bytes.size() - 3)) & ~3U;
                    static const std::uint32_t extremes[] = {0, 1, 0xFFFFFFFF, 0x80000000, 0x40000000, 0x3FFFFFFF, 0x7FFFFFFF, 0x00FFFFFF, 0x01000000, 0x000003F2, 0x000003EC};
                    const std::uint32_t v = extremes[pick(sizeof extremes / sizeof extremes[0])];
                    for (int i = 0; i < 4 && at + static_cast<unsigned>(i) < bytes.size(); ++i) bytes[at + static_cast<unsigned>(i)] = static_cast<Byte>(v >> (24 - 8 * i));
                }
                break;
            case 3:   // insert random bytes
                for (unsigned k = 1 + pick(12); k > 0; --k) bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(pick(static_cast<unsigned>(bytes.size() + 1))), static_cast<Byte>(rng()));
                break;
            default:  // append garbage
                for (unsigned k = pick(64); k > 0; --k) bytes.push_back(static_cast<Byte>(rng()));
        }
        return bytes;
    };

    {   // hunk files
        const AssemblyResult r = assemble(
            "        SECTION code,CODE\n        XDEF start\nstart:  MOVE.L #tab,A0\n        LEA cnt,A1\n        RTS\n        SECTION data,DATA_C\ntab:    DC.L tab,start\n"
            "        SECTION bss,BSS\ncnt:    DS.L 4\n");
        CHECK(r.ok());
        const std::vector<Byte> valid = writeHunkFile(r.sections);
        int accepted = 0, refused = 0;
        for (int i = 0, n = scaled(12000); i < n; ++i) {
            std::vector<Byte> bytes = valid;
            for (unsigned k = 1 + pick(3); k > 0; --k) bytes = mutate(bytes);
            const Address base = pick(3) == 0 ? Address{0xFFFFF0} : pick(2) ? Address{0x20000} : Address{0};
            try {
                (void)loadHunkFile(bytes, base);
                ++accepted;
            } catch (const IoError&) {
                ++refused;
            }
            (void)looksLikeHunkFile(bytes);
        }
        CHECK(accepted > scaled(12000) / 120);
        CHECK(refused > scaled(12000) / 12);
    }
    {   // S-records
        const std::string valid = formatSRecords({{0x1000, {1, 2, 3, 4, 5, 6, 7, 8}}, {0x2000, {9, 9}}}, 0x1000);
        const std::vector<Byte> asBytes(valid.begin(), valid.end());
        int refused = 0;
        for (int i = 0, n = scaled(12000); i < n; ++i) {
            std::vector<Byte> bytes = mutate(asBytes);
            try {
                (void)parseSRecords(std::string(bytes.begin(), bytes.end()));
            } catch (const IoError&) {
                ++refused;
            }
        }
        CHECK(refused > scaled(12000) / 12);
    }
    {   // snapshot files
        Memory mem;
        mem.write32(0x1000, 0xDEADBEEF);
        mem.write32(0x30000, 0xCAFEBABE);
        Cpu cpu(mem);
        const std::vector<Byte> valid = serialize(capture(cpu));
        int refused = 0, accepted = 0;
        for (int i = 0, n = scaled(6000); i < n; ++i) {
            const std::vector<Byte> bytes = mutate(valid);
            try {
                Snapshot snap = deserializeSnapshot(bytes);
                Memory m2;
                Cpu c2(m2);
                restore(c2, snap);
                ++accepted;
            } catch (const IoError&) {
                ++refused;
            }
        }
        CHECK(refused > scaled(6000) / 12);
        CHECK(accepted > scaled(6000) / 60);
    }
    {   // project (annotation) files
        Annotations ann;
        (void)ann.setLabel(0x1000, "start");
        RoutineDoc doc;
        doc.name = "main_routine";
        doc.summary = "does things";
        doc.confidence = "high";
        (void)ann.setRoutine(0x1000, doc);
        VariableDoc var;
        var.address = 0x2000;
        var.name = "counter";
        var.size = 2;
        (void)ann.setVariable(var);
        DataRegion region;
        region.address = 0x3000;
        region.length = 8;
        region.kind = DataKind::Words;
        (void)ann.setData(region);
        ann.setComment(0x1004, "a comment");
        const std::string valid = ann.toJson().dump();
        const std::vector<Byte> asBytes(valid.begin(), valid.end());
        int accepted = 0, refused = 0;
        for (int i = 0, n = scaled(12000); i < n; ++i) {
            std::vector<Byte> bytes = mutate(asBytes);
            const std::string text(bytes.begin(), bytes.end());
            try {
                const Annotations loaded = Annotations::fromJson(Json::parse(text));
                (void)loaded.toJson().dump();
                ++accepted;
            } catch (const JsonError&) {
                ++refused;
            }
        }
        CHECK(accepted > scaled(12000) / 120);
        CHECK(refused > scaled(12000) / 12);
    }

    return finish("robustness");
}
