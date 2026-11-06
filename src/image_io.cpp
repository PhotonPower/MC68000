#include "m68k/image_io.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "m68k/format.hpp"

namespace m68k {

std::vector<Byte> readBinaryFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw IoError("cannot open '" + path + "'");
    return std::vector<Byte>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string readTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw IoError("cannot open '" + path + "'");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeBinaryFile(const std::string& path, const std::vector<Byte>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw IoError("cannot write '" + path + "'");
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) throw IoError("error writing '" + path + "'");
}

void writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw IoError("cannot write '" + path + "'");
    f << text;
    if (!f) throw IoError("error writing '" + path + "'");
}

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string srecLine(char type, std::uint32_t address, int addrBytes, const std::vector<Byte>& data) {
    std::vector<Byte> body;
    body.push_back(static_cast<Byte>(addrBytes + data.size() + 1));
    for (int i = addrBytes - 1; i >= 0; --i) body.push_back(static_cast<Byte>(address >> (8 * i)));
    for (const Byte b : data) body.push_back(b);
    unsigned sum = 0;
    for (Byte b : body) sum += b;
    body.push_back(static_cast<Byte>(~sum & 0xFF));
    std::string s = "S";
    s += type;
    for (Byte b : body) s += hexDigits(b, 2);
    return s;
}

}  // namespace

Image parseSRecords(const std::string& text) {
    Image img;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    auto bad = [&](const std::string& why) -> IoError {
        return IoError("S-record line " + std::to_string(lineNo) + ": " + why);
    };
    while (std::getline(in, line)) {
        ++lineNo;
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        std::size_t b = 0;
        while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
        line = line.substr(b);
        if (line.empty()) continue;
        if (line[0] != 'S' || line.size() < 4) throw bad("does not start with 'S<type>'");
        const char type = line[1];
        std::vector<Byte> bytes;
        for (std::size_t i = 2; i + 1 < line.size() + 0; i += 2) {
            const int hi = hexValue(line[i]), lo = hexValue(line[i + 1]);
            if (hi < 0 || lo < 0) throw bad("bad hexadecimal digit");
            bytes.push_back(static_cast<Byte>(hi * 16 + lo));
        }
        if ((line.size() - 2) % 2 != 0) throw bad("odd number of hexadecimal digits");
        if (bytes.empty() || bytes[0] + 1U != bytes.size()) throw bad("byte count does not match the line length");
        unsigned sum = 0;
        for (std::size_t i = 0; i + 1 < bytes.size(); ++i) sum += bytes[i];
        if (static_cast<Byte>(~sum & 0xFF) != bytes.back()) throw bad("checksum error");
        int addrBytes = 0;
        switch (type) {
            case '0': case '1': case '5': case '9': addrBytes = 2; break;
            case '2': case '6': case '8': addrBytes = 3; break;
            case '3': case '7': addrBytes = 4; break;
            default: throw bad(std::string("unsupported record type S") + type);
        }
        if (bytes.size() < static_cast<std::size_t>(1 + addrBytes + 1)) throw bad("record too short");
        Address addr = 0;
        for (int i = 0; i < addrBytes; ++i) addr = (addr << 8) | bytes[static_cast<std::size_t>(1 + i)];
        const std::vector<Byte> data(bytes.begin() + 1 + addrBytes, bytes.end() - 1);
        if (type == '1' || type == '2' || type == '3') {
            if (!img.segments.empty() &&
                img.segments.back().address + img.segments.back().bytes.size() == addr) {
                for (const Byte d : data) img.segments.back().bytes.push_back(d);
            } else {
                img.segments.push_back({addr, data});
            }
        } else if (type == '7' || type == '8' || type == '9') {
            img.entry = addr;
        }
    }
    return img;
}

std::string formatSRecords(const std::vector<Segment>& segments, std::optional<Address> entry, const std::string& name) {
    std::string out = srecLine('0', 0, 2, std::vector<Byte>(name.begin(), name.end())) + "\n";
    for (const Segment& s : segments) {
        for (std::size_t off = 0; off < s.bytes.size(); off += 32) {
            const std::size_t n = std::min<std::size_t>(32, s.bytes.size() - off);
            const std::vector<Byte> chunk(s.bytes.begin() + static_cast<std::ptrdiff_t>(off),
                                          s.bytes.begin() + static_cast<std::ptrdiff_t>(off + n));
            out += srecLine('2', (s.address + static_cast<Address>(off)) & kAddressMask, 3, chunk) + "\n";
        }
    }
    out += srecLine('8', entry.value_or(0) & kAddressMask, 3, {}) + "\n";
    return out;
}

bool looksLikeSRecordFile(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == "srec" || ext == "s19" || ext == "s28" || ext == "s37" || ext == "mot";
}

void loadSegments(Memory& memory, const std::vector<Segment>& segments) {
    for (const Segment& s : segments) memory.load(s.address, s.bytes);
}

}  // namespace m68k
