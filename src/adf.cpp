#include "m68k/adf.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

#include "m68k/image_io.hpp"

namespace m68k {

namespace {

constexpr unsigned kBlock = 512;
constexpr unsigned kLongs = kBlock / 4;
constexpr std::int32_t kTypeHeader = 2, kTypeData = 8, kTypeList = 16;
constexpr std::int32_t kSecRoot = 1, kSecDir = 2, kSecSoftLink = 3, kSecLinkDir = 4, kSecFile = -3, kSecLinkFile = -4;
constexpr unsigned kHashSize = 72;   // entries of the hash table of a 512 byte block

char upperAscii(unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); }

std::string describeBytes(const std::vector<Byte>& d, unsigned n) {
    std::string s;
    char buf[8];
    for (unsigned i = 0; i < n && i < d.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%s%02X", i ? " " : "", d[i]);
        s += buf;
    }
    return s;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : path) {
        if (c == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

}  // namespace

AdfImage::AdfImage(std::vector<Byte> bytes) : data_(std::move(bytes)) {
    const std::size_t n = data_.size();
    if (n % kBlock != 0 || n < 880 * 1024 || n > 4 * 1024 * 1024)
        throw IoError("not an ADF disk image: " + std::to_string(n) + " bytes (a double density disk has 901120, a high density disk 1802240)");
    dos_ = data_[0] == 'D' && data_[1] == 'O' && data_[2] == 'S' && data_[3] <= 7;
    flags_ = dos_ ? data_[3] : 0;
    root_ = blocks() >= 3520 ? 1760 : 880;
    if (!dos_) return;
    rootOk_ = word(root_, 0) == static_cast<std::uint32_t>(kTypeHeader) && static_cast<std::int32_t>(word(root_, kLongs - 1)) == kSecRoot && blockChecksumOk(root_);
    if (!rootOk_) problems_.push_back("the root block (" + std::to_string(root_) + ") is not valid: wrong type, secondary type or checksum");
    diskName_ = nameOf(root_);
}

std::uint32_t AdfImage::word(unsigned block, unsigned index) const {
    const std::size_t at = static_cast<std::size_t>(block) * kBlock + static_cast<std::size_t>(index) * 4;
    if (at + 4 > data_.size()) return 0;
    return (static_cast<std::uint32_t>(data_[at]) << 24) | (static_cast<std::uint32_t>(data_[at + 1]) << 16) | (static_cast<std::uint32_t>(data_[at + 2]) << 8) | data_[at + 3];
}

std::vector<Byte> AdfImage::block(unsigned n) const {
    if (n >= blocks()) throw IoError("block " + std::to_string(n) + " is outside the disk (" + std::to_string(blocks()) + " blocks)");
    return std::vector<Byte>(data_.begin() + static_cast<std::ptrdiff_t>(n) * kBlock, data_.begin() + static_cast<std::ptrdiff_t>(n + 1) * kBlock);
}

bool AdfImage::blockChecksumOk(unsigned blockNo) const {
    std::uint32_t sum = 0;
    for (unsigned i = 0; i < kLongs; ++i) sum += word(blockNo, i);
    return sum == 0;
}

bool AdfImage::bootChecksumOk() const {
    std::uint64_t sum = 0;
    for (unsigned i = 0; i < 256; ++i) {
        sum += (static_cast<std::uint32_t>(data_[i * 4]) << 24) | (static_cast<std::uint32_t>(data_[i * 4 + 1]) << 16) | (static_cast<std::uint32_t>(data_[i * 4 + 2]) << 8) | data_[i * 4 + 3];
        if (sum > 0xFFFFFFFFULL) sum = (sum & 0xFFFFFFFFULL) + 1;   // the carry wraps around
    }
    return sum == 0xFFFFFFFFULL;
}

std::string AdfImage::bootDescription() const {
    if (dos_) return std::string("DOS") + static_cast<char>('0' + flags_) + (fastFileSystem() ? " (FFS" : " (OFS") + (international() ? ", international" : "") + (dirCache() ? ", directory cache" : "") + ")";
    if (data_[0] == 'K' && data_[1] == 'I' && data_[2] == 'C' && data_[3] == 'K') return "KICK (a Kickstart disk)";
    return "no AmigaDOS file system: the first bytes are " + describeBytes(data_, 8);
}

std::string AdfImage::nameOf(unsigned blockNo) const {
    const std::size_t at = static_cast<std::size_t>(blockNo) * kBlock + (kLongs - 20) * 4;
    if (at + 32 > data_.size()) return "";
    const unsigned len = std::min<unsigned>(data_[at], 30);
    return std::string(reinterpret_cast<const char*>(&data_[at + 1]), len);
}

unsigned AdfImage::hashOf(const std::string& name) const {
    std::uint32_t h = static_cast<std::uint32_t>(name.size());
    for (const char ch : name) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (international() && c >= 0xE0 && c <= 0xFE && c != 0xF7) c = static_cast<unsigned char>(c - 0x20);
        else c = static_cast<unsigned char>(upperAscii(c));
        h = (h * 13 + c) & 0x7FF;
    }
    return h % kHashSize;
}

std::optional<AdfEntry> AdfImage::entryAt(unsigned blockNo, const std::string& parentPath) const {
    if (blockNo == 0 || blockNo >= blocks()) {
        problems_.push_back("a directory entry points to block " + std::to_string(blockNo) + ", outside the disk");
        return std::nullopt;
    }
    if (word(blockNo, 0) != static_cast<std::uint32_t>(kTypeHeader)) {
        problems_.push_back("block " + std::to_string(blockNo) + " is no header block (type " + std::to_string(static_cast<std::int32_t>(word(blockNo, 0))) + ")");
        return std::nullopt;
    }
    if (!blockChecksumOk(blockNo)) problems_.push_back("the checksum of block " + std::to_string(blockNo) + " is wrong (" + nameOf(blockNo) + ")");
    AdfEntry e;
    e.block = blockNo;
    e.name = nameOf(blockNo);
    e.path = parentPath.empty() ? e.name : parentPath + "/" + e.name;
    e.protection = word(blockNo, kLongs - 48);
    e.size = word(blockNo, kLongs - 47);
    const std::size_t cat = static_cast<std::size_t>(blockNo) * kBlock + (kLongs - 46) * 4;
    e.comment = std::string(reinterpret_cast<const char*>(&data_[cat + 1]), std::min<unsigned>(data_[cat], 79));
    switch (static_cast<std::int32_t>(word(blockNo, kLongs - 1))) {
        case kSecFile: e.kind = AdfEntry::Kind::File; break;
        case kSecDir: e.kind = AdfEntry::Kind::Directory; e.size = 0; break;
        case kSecSoftLink: e.kind = AdfEntry::Kind::SoftLink; e.size = 0; break;
        case kSecLinkDir:
        case kSecLinkFile: e.kind = AdfEntry::Kind::HardLink; break;
        default: e.kind = AdfEntry::Kind::Other; break;
    }
    return e;
}

void AdfImage::collect(unsigned directoryBlock, const std::string& path, bool recursive, std::vector<AdfEntry>& out, unsigned depth) const {
    if (depth > 32) {
        problems_.push_back("directories nested deeper than 32 levels at " + path);
        return;
    }
    for (unsigned slot = 0; slot < kHashSize; ++slot) {
        unsigned b = word(directoryBlock, 6 + slot);
        std::set<unsigned> seen;
        while (b != 0) {
            if (!seen.insert(b).second) {
                problems_.push_back("a loop in the hash chain at block " + std::to_string(b));
                break;
            }
            const std::optional<AdfEntry> e = entryAt(b, path);
            if (!e) break;
            out.push_back(*e);
            if (recursive && e->kind == AdfEntry::Kind::Directory) collect(b, e->path, true, out, depth + 1);
            b = word(b, kLongs - 4);
        }
    }
}

int AdfImage::directoryBlockOf(const std::string& path) const {
    unsigned dir = root_;
    for (const std::string& part : splitPath(path)) {
        bool found = false;
        std::vector<AdfEntry> entries;
        collect(dir, "", false, entries, 0);
        for (const AdfEntry& e : entries) {
            if (e.kind != AdfEntry::Kind::Directory) continue;
            if (hashOf(e.name) == hashOf(part) && std::equal(e.name.begin(), e.name.end(), part.begin(), part.end(), [](char a, char b) { return upperAscii(static_cast<unsigned char>(a)) == upperAscii(static_cast<unsigned char>(b)); })) {
                dir = e.block;
                found = true;
                break;
            }
        }
        if (!found) return -1;
    }
    return static_cast<int>(dir);
}

std::vector<AdfEntry> AdfImage::list(const std::string& directory) const {
    std::vector<AdfEntry> out;
    if (!dos_) return out;
    const int dir = directoryBlockOf(directory);
    if (dir < 0) return out;
    collect(static_cast<unsigned>(dir), splitPath(directory).empty() ? "" : directory, false, out, 0);
    return out;
}

std::vector<AdfEntry> AdfImage::listAll(const std::string& directory) const {
    std::vector<AdfEntry> out;
    if (!dos_) return out;
    const int dir = directoryBlockOf(directory);
    if (dir < 0) return out;
    collect(static_cast<unsigned>(dir), splitPath(directory).empty() ? "" : directory, true, out, 0);
    return out;
}

std::optional<AdfEntry> AdfImage::find(const std::string& path) const {
    const std::vector<std::string> parts = splitPath(path);
    if (parts.empty() || !dos_) return std::nullopt;
    std::string parent;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) parent += (i ? "/" : "") + parts[i];
    for (const AdfEntry& e : list(parent)) {
        const std::string& want = parts.back();
        if (e.name.size() == want.size() &&
            std::equal(e.name.begin(), e.name.end(), want.begin(), [](char a, char b) { return upperAscii(static_cast<unsigned char>(a)) == upperAscii(static_cast<unsigned char>(b)); }))
            return e;
    }
    return std::nullopt;
}

std::vector<Byte> AdfImage::readFile(const AdfEntry& entry) const {
    if (entry.kind != AdfEntry::Kind::File && entry.kind != AdfEntry::Kind::HardLink) throw IoError(entry.path + " is not a file");
    std::vector<Byte> out;
    out.reserve(entry.size);
    const bool ffs = fastFileSystem();
    std::set<unsigned> seenHeaders;
    unsigned header = entry.block;
    while (header != 0) {
        if (header >= blocks() || !seenHeaders.insert(header).second) throw IoError(entry.path + ": the chain of extension blocks is damaged (block " + std::to_string(header) + ")");
        const std::int32_t type = static_cast<std::int32_t>(word(header, 0));
        if (type != kTypeHeader && type != kTypeList) throw IoError(entry.path + ": block " + std::to_string(header) + " is neither a header nor an extension block");
        const unsigned count = std::min<unsigned>(word(header, 2), kHashSize);
        for (unsigned i = 0; i < count && out.size() < entry.size; ++i) {
            const unsigned b = word(header, kLongs - 51 - i);   // the table runs backwards from the end of the block
            if (b == 0 || b >= blocks()) throw IoError(entry.path + ": data block pointer " + std::to_string(b) + " is outside the disk");
            const std::size_t at = static_cast<std::size_t>(b) * kBlock;
            if (ffs) {
                const std::size_t n = std::min<std::size_t>(kBlock, entry.size - out.size());
                out.insert(out.end(), data_.begin() + static_cast<std::ptrdiff_t>(at), data_.begin() + static_cast<std::ptrdiff_t>(at + n));
            } else {
                if (static_cast<std::int32_t>(word(b, 0)) != kTypeData) throw IoError(entry.path + ": block " + std::to_string(b) + " is no data block");
                const std::size_t n = std::min<std::size_t>({static_cast<std::size_t>(word(b, 3)), 488, static_cast<std::size_t>(entry.size) - out.size()});
                out.insert(out.end(), data_.begin() + static_cast<std::ptrdiff_t>(at + 24), data_.begin() + static_cast<std::ptrdiff_t>(at + 24 + n));
            }
        }
        header = word(header, kLongs - 2);   // the next extension block
    }
    if (out.size() != entry.size) throw IoError(entry.path + ": the data blocks hold " + std::to_string(out.size()) + " bytes, the header says " + std::to_string(entry.size));
    return out;
}

}  // namespace m68k
