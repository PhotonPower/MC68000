#include "m68k/hunk.hpp"

#include <cctype>

#include "m68k/format.hpp"

namespace m68k {

namespace {

constexpr std::uint32_t kHunkCode = 0x3E9, kHunkData = 0x3EA, kHunkBss = 0x3EB, kHunkReloc32 = 0x3EC,
                        kHunkSymbol = 0x3F0, kHunkDebug = 0x3F1, kHunkEnd = 0x3F2, kHunkHeader = 0x3F3,
                        kHunkName = 0x3E8;
constexpr std::uint32_t kSizeMask = 0x3FFFFFFF;  // the top two bits of a size are memory flags (chip/fast)
constexpr std::uint32_t kMaxHunks = 4096;
constexpr std::size_t kMaxTotal = 16u << 20;

class Reader {
public:
    explicit Reader(const std::vector<Byte>& b) : b_(b) {}
    bool atEnd() const { return pos_ >= b_.size(); }
    std::size_t pos() const { return pos_; }
    std::uint32_t u32() {
        if (pos_ + 4 > b_.size()) throw IoError("hunk file: unexpected end of file at offset " + std::to_string(pos_));
        const std::uint32_t v = (std::uint32_t{b_[pos_]} << 24) | (std::uint32_t{b_[pos_ + 1]} << 16) |
                                (std::uint32_t{b_[pos_ + 2]} << 8) | b_[pos_ + 3];
        pos_ += 4;
        return v;
    }
    std::vector<Byte> bytes(std::size_t longs) {
        if (longs > (b_.size() - pos_) / 4) throw IoError("hunk file: block longer than the file at offset " + std::to_string(pos_));
        std::vector<Byte> out(b_.begin() + static_cast<std::ptrdiff_t>(pos_), b_.begin() + static_cast<std::ptrdiff_t>(pos_ + longs * 4));
        pos_ += longs * 4;
        return out;
    }
    std::string name(std::size_t longs) {
        const std::vector<Byte> raw = bytes(longs);
        std::string s(raw.begin(), raw.end());
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }

private:
    const std::vector<Byte>& b_;
    std::size_t pos_ = 0;
};

struct Pending {
    std::string type;
    std::vector<Byte> data;
    std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> relocs;  // target hunk -> offsets
    std::vector<std::pair<std::string, std::uint32_t>> symbols;                // name, offset
};

}  // namespace

bool looksLikeHunkFile(const std::vector<Byte>& b) {
    return b.size() >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0x03 && b[3] == 0xF3;
}

HunkImage loadHunkFile(const std::vector<Byte>& file, Address base) {
    Reader in(file);
    if (in.u32() != kHunkHeader) throw IoError("hunk file: missing HUNK_HEADER");
    // resident library names (obsolete): a list of strings ended by a zero length
    for (std::uint32_t n; (n = in.u32()) != 0;) (void)in.name(n & kSizeMask);
    const std::uint32_t tableSize = in.u32();
    const std::uint32_t first = in.u32();
    const std::uint32_t last = in.u32();
    if (last < first || last - first + 1 > kMaxHunks || last >= tableSize)
        throw IoError("hunk file: inconsistent hunk table in the header");
    const std::uint32_t count = last - first + 1;
    std::vector<std::uint32_t> sizes;  // in longs
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t s = in.u32();
        if ((s >> 30) == 3) (void)in.u32();  // extended memory attributes follow
        sizes.push_back(s & kSizeMask);
    }

    std::vector<Pending> hunks(count);
    std::uint32_t current = 0;
    bool open = false;  // a CODE/DATA/BSS block of `current` has been read and no END yet
    while (!in.atEnd()) {
        const std::uint32_t id = in.u32() & kSizeMask;
        switch (id) {
            case kHunkCode:
            case kHunkData:
            case kHunkBss: {
                if (current >= count) throw IoError("hunk file: more hunks than the header announces");
                if (open) throw IoError("hunk file: HUNK_END missing before the next hunk");
                Pending& h = hunks[current];
                const std::uint32_t longs = in.u32() & kSizeMask;
                if (longs > sizes[current]) throw IoError("hunk file: block is larger than the size in the header");
                h.type = id == kHunkCode ? "CODE" : id == kHunkData ? "DATA" : "BSS";
                if (id == kHunkBss) h.data.assign(std::size_t{longs} * 4, 0);
                else h.data = in.bytes(longs);
                // the header size may be larger than the stored data (zero filled at load time)
                h.data.resize(std::size_t{sizes[current]} * 4, 0);
                open = true;
                break;
            }
            case kHunkReloc32: {
                if (!open) throw IoError("hunk file: HUNK_RELOC32 outside a hunk");
                for (std::uint32_t n; (n = in.u32()) != 0;) {
                    const std::uint32_t target = in.u32();
                    if (target >= count) throw IoError("hunk file: relocation refers to a missing hunk");
                    std::vector<std::uint32_t> offsets;
                    for (std::uint32_t i = 0; i < n; ++i) {
                        const std::uint32_t off = in.u32();
                        if (std::size_t{off} + 4 > hunks[current].data.size())
                            throw IoError("hunk file: relocation offset outside the hunk");
                        offsets.push_back(off);
                    }
                    hunks[current].relocs.emplace_back(target, std::move(offsets));
                }
                break;
            }
            case kHunkSymbol: {
                if (!open) throw IoError("hunk file: HUNK_SYMBOL outside a hunk");
                for (std::uint32_t n; (n = in.u32() & 0x00FFFFFF) != 0;) {
                    std::string name = in.name(n);
                    hunks[current].symbols.emplace_back(std::move(name), in.u32());
                }
                break;
            }
            case kHunkDebug:
            case kHunkName:
                (void)in.bytes(in.u32() & kSizeMask);
                break;
            case kHunkEnd:
                if (!open) throw IoError("hunk file: unexpected HUNK_END");
                open = false;
                ++current;
                break;
            default:
                throw IoError("hunk file: unsupported block type $" + hexDigits(id, 3) + " at offset " +
                              std::to_string(in.pos() - 4));
        }
        if (current == count && !open) break;  // everything read (trailing data is ignored)
    }
    if (current != count) throw IoError("hunk file: fewer hunks than the header announces");

    // place and relocate
    HunkImage img;
    std::size_t total = 0;
    Address at = base & kAddressMask;
    std::vector<Address> bases;
    for (const Pending& h : hunks) {
        at = (at + 3U) & ~Address{3};
        bases.push_back(at);
        total += h.data.size();
        if (total > kMaxTotal) throw IoError("hunk file: program too large");
        at += static_cast<Address>(h.data.size());
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        Pending& h = hunks[i];
        for (const auto& [target, offsets] : h.relocs) {
            for (const std::uint32_t off : offsets) {
                std::uint32_t v = (std::uint32_t{h.data[off]} << 24) | (std::uint32_t{h.data[off + 1]} << 16) |
                                  (std::uint32_t{h.data[off + 2]} << 8) | h.data[off + 3];
                v += bases[target];
                h.data[off] = static_cast<Byte>(v >> 24);
                h.data[off + 1] = static_cast<Byte>(v >> 16);
                h.data[off + 2] = static_cast<Byte>(v >> 8);
                h.data[off + 3] = static_cast<Byte>(v);
            }
        }
        for (const auto& [name, off] : h.symbols) img.symbols.push_back({name, bases[i] + off});
        img.hunks.push_back({h.type, bases[i], static_cast<Address>(h.data.size())});
        if (img.entry == 0 && h.type == "CODE") img.entry = bases[i];
        img.segments.push_back({bases[i], std::move(h.data)});
    }
    img.end = at;
    if (img.entry == 0 && !img.hunks.empty()) img.entry = img.hunks[0].address;
    return img;
}

std::string asmSymbolName(const std::string& name) {
    std::string s;
    for (const char c : name) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) s.insert(s.begin(), '_');
    std::string u;
    for (const char c : s) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const bool reg = (u.size() == 2 && (u[0] == 'D' || u[0] == 'A') && u[1] >= '0' && u[1] <= '7') || u == "SP" ||
                     u == "PC" || u == "SR" || u == "CCR" || u == "USP";
    if (reg) s.insert(s.begin(), '_');
    return s;
}

}  // namespace m68k
