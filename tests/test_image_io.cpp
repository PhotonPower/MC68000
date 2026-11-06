#include "m68k/image_io.hpp"

#include <cstdio>

#include "test_util.hpp"

using namespace m68k;

namespace {
bool throwsIoError(const std::string& text) {
    try {
        (void)parseSRecords(text);
    } catch (const IoError&) {
        return true;
    }
    return false;
}
}  // namespace

int main() {
    // known S-record (checksums computed by hand): S1 with address 0x0010 and data 01 02 03
    {
        const Image img = parseSRecords("S1060010010203E3\n");
        CHECK_EQ(img.segments.size(), std::size_t{1});
        CHECK_EQ(img.segments[0].address, Address{0x10});
        CHECK((img.segments[0].bytes == std::vector<Byte>{1, 2, 3}));
    }
    // formatting and parsing agree
    {
        std::vector<Byte> data;
        for (int i = 0; i < 100; ++i) data.push_back(static_cast<Byte>(i * 7));
        const std::vector<Segment> segs = {{0x1000, data}, {0xFF0000, {0xDE, 0xAD}}};
        const std::string text = formatSRecords(segs, 0x1000, "TEST");
        CHECK(text.rfind("S0", 0) == 0);
        CHECK(text.find("S2") != std::string::npos);
        CHECK(text.find("S804001000EB") != std::string::npos);  // termination record with entry $1000
        const Image img = parseSRecords(text);
        CHECK_EQ(img.segments.size(), std::size_t{2});
        CHECK(img.segments[0].bytes == data);  // 4 records of up to 32 bytes were merged again
        CHECK_EQ(img.segments[1].address, Address{0xFF0000});
        CHECK(img.entry && *img.entry == 0x1000);
    }
    // S3 and S7 (32 bit addresses), S9, blank lines, CRLF
    {
        const Image img = parseSRecords("\r\nS30700001000AABB83\r\nS70500001000EA\r\n");
        CHECK_EQ(img.segments.size(), std::size_t{1});
        CHECK_EQ(img.segments[0].address, Address{0x1000});
        CHECK(img.entry && *img.entry == 0x1000);
    }
    // errors carry the line number
    CHECK(throwsIoError("S1060010010203E4\n"));      // wrong checksum
    CHECK(throwsIoError("S1070010010203E3\n"));      // wrong byte count
    CHECK(throwsIoError("X1060010010203E3\n"));      // not an S-record
    CHECK(throwsIoError("S1060010010203ZZ\n"));      // bad hex digit
    CHECK(throwsIoError("S4060010010203E3\n"));      // unsupported type
    CHECK(throwsIoError("S1\n"));                    // too short
    CHECK(throwsIoError("S1060010010203E\n"));       // odd number of digits
    try {
        (void)parseSRecords("S1060010010203E3\nS1060010010203E4\n");
        CHECK(false);
    } catch (const IoError& e) {
        CHECK(std::string(e.what()).find("line 2") != std::string::npos);
    }

    CHECK(looksLikeSRecordFile("prog.s19"));
    CHECK(looksLikeSRecordFile("PROG.SREC"));
    CHECK(!looksLikeSRecordFile("prog.bin"));
    CHECK(!looksLikeSRecordFile("prog"));

    // files
    {
        const std::string path = "m68k_test_io.tmp";
        writeBinaryFile(path, {1, 2, 3, 0, 255});
        CHECK((readBinaryFile(path) == std::vector<Byte>{1, 2, 3, 0, 255}));
        writeTextFile(path, "hello\n");
        CHECK_EQ(readTextFile(path), std::string("hello\n"));
        std::remove(path.c_str());
        bool threw = false;
        try {
            (void)readBinaryFile("does/not/exist.bin");
        } catch (const IoError&) {
            threw = true;
        }
        CHECK(threw);
    }

    // loading into memory
    {
        Memory mem;
        loadSegments(mem, {{0x2000, {0x4E, 0x75}}});
        CHECK_EQ(mem.read16(0x2000), Word{0x4E75});
    }
    return finish("image_io");
}
