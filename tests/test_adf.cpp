// The ADF reader (m68k::AdfImage): disks are built here from the documented layout of the AmigaDOS file system (OFS and FFS); tools/crosscheck_adf.py
// compares the reader with disks that amitools' xdftool writes, which is the check that does not rely on this file's understanding of the format.
#include <random>
#include <string>

#include "m68k/adf.hpp"
#include "m68k/image_io.hpp"
#include "test_util.hpp"

using namespace m68k;

namespace {

constexpr unsigned kBlocks = 1760;

struct Disk {
    std::vector<Byte> img = std::vector<Byte>(kBlocks * 512, 0);
    bool ffs = false;
    unsigned next = 2;   // next free block
    unsigned dosFlags() const { return ffs ? 1U : 0U; }

    void put(unsigned block, unsigned index, std::uint32_t v) {
        const std::size_t at = block * 512U + index * 4U;
        img[at] = static_cast<Byte>(v >> 24);
        img[at + 1] = static_cast<Byte>(v >> 16);
        img[at + 2] = static_cast<Byte>(v >> 8);
        img[at + 3] = static_cast<Byte>(v);
    }
    std::uint32_t get(unsigned block, unsigned index) const {
        const std::size_t at = block * 512U + index * 4U;
        return (static_cast<std::uint32_t>(img[at]) << 24) | (static_cast<std::uint32_t>(img[at + 1]) << 16) | (static_cast<std::uint32_t>(img[at + 2]) << 8) | img[at + 3];
    }
    void seal(unsigned block) {   // the sum of the 128 longs of a block is 0
        put(block, 5, 0);
        std::uint32_t sum = 0;
        for (unsigned i = 0; i < 128; ++i) sum += get(block, i);
        put(block, 5, 0U - sum);
    }
    void putName(unsigned block, const std::string& name) {
        const std::size_t at = block * 512U + (128 - 20) * 4U;
        img[at] = static_cast<Byte>(name.size());
        for (std::size_t i = 0; i < name.size(); ++i) img[at + 1 + i] = static_cast<Byte>(name[i]);
    }
    static unsigned hashOf(const std::string& name) {
        std::uint32_t h = static_cast<std::uint32_t>(name.size());
        for (const char c : name) h = (h * 13 + static_cast<unsigned char>(c >= 'a' && c <= 'z' ? c - 32 : c)) & 0x7FF;
        return h % 72;
    }
    void format(const std::string& diskName) {
        img[0] = 'D', img[1] = 'O', img[2] = 'S', img[3] = static_cast<Byte>(dosFlags());
        put(880, 0, 2);
        put(880, 3, 72);
        put(880, 78, 0xFFFFFFFF);
        putName(880, diskName);
        put(880, 127, 1);
        seal(880);
    }
    // links a header block into the hash table of `parent`
    void link(unsigned parent, unsigned header, const std::string& name) {
        const unsigned slot = 6 + hashOf(name);
        put(header, 124, get(parent, slot));
        put(parent, slot, header);
        put(header, 125, parent);
        seal(header);
        seal(parent);
    }
    unsigned makeDir(unsigned parent, const std::string& name) {
        const unsigned b = next++;
        put(b, 0, 2);
        put(b, 1, b);
        putName(b, name);
        put(b, 127, 2);
        link(parent, b, name);
        return b;
    }
    unsigned makeFile(unsigned parent, const std::string& name, const std::vector<Byte>& content) {
        const unsigned per = ffs ? 512 : 488;
        const unsigned count = static_cast<unsigned>((content.size() + per - 1) / per);
        const unsigned header = next++;
        std::vector<unsigned> data;
        for (unsigned i = 0; i < count; ++i) data.push_back(next++);
        // data blocks
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t from = static_cast<std::size_t>(i) * per, n = std::min<std::size_t>(per, content.size() - from);
            const std::size_t at = data[i] * 512U;
            if (ffs) {
                for (std::size_t k = 0; k < n; ++k) img[at + k] = content[from + k];
            } else {
                put(data[i], 0, 8);
                put(data[i], 1, header);
                put(data[i], 2, i + 1);
                put(data[i], 3, static_cast<std::uint32_t>(n));
                put(data[i], 4, i + 1 < count ? data[i + 1] : 0);
                for (std::size_t k = 0; k < n; ++k) img[at + 24 + k] = content[from + k];
                seal(data[i]);
            }
        }
        // the header block holds the first 72 pointers, an extension block every further 72
        unsigned block = header;
        unsigned done = 0;
        bool first = true;
        while (true) {
            const unsigned here = std::min<unsigned>(72, count - done);
            put(block, 0, first ? 2U : 16U);
            put(block, 1, header);
            put(block, 2, here);
            for (unsigned i = 0; i < here; ++i) put(block, 127 - 50 - i, data[done + i]);
            done += here;
            if (first) {
                put(block, 4, count ? data[0] : 0);
                put(block, 80, 0xF);
                put(block, 81, static_cast<std::uint32_t>(content.size()));
                putName(block, name);
                put(block, 127, 0xFFFFFFFDU);   // ST_FILE = -3
            } else {
                put(block, 125, header);
                put(block, 127, 0xFFFFFFFDU);
            }
            if (done >= count) break;
            const unsigned ext = next++;
            put(block, 126, ext);
            if (!first) seal(block);
            block = ext;
            first = false;
        }
        if (!first) seal(block);
        link(parent, header, name);
        return header;
    }
};

std::vector<Byte> pattern(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<Byte> v(n);
    for (Byte& b : v) b = static_cast<Byte>(rng());
    return v;
}

}  // namespace

int main() {
    for (const bool ffs : {false, true}) {
        Disk d;
        d.ffs = ffs;
        d.format("Test");
        const std::vector<Byte> one = pattern(772, 1), big = pattern(50000, 2), small = pattern(700, 3), nothing;
        d.makeFile(880, "1", one);
        d.makeFile(880, "big.bin", big);
        d.makeFile(880, "empty", nothing);
        const unsigned sub = d.makeDir(880, "Sub");
        d.makeFile(sub, "Inner.Bin", small);
        // two names with the same hash value in one chain
        std::string a, b;
        for (unsigned i = 0; i < 1000 && b.empty(); ++i) {
            const std::string name = "n" + std::to_string(i);
            if (a.empty()) a = name;
            else if (Disk::hashOf(name) == Disk::hashOf(a)) b = name;
        }
        CHECK(!b.empty());
        d.makeFile(880, a, pattern(10, 4));
        d.makeFile(880, b, pattern(20, 5));

        const AdfImage disk(d.img);
        CHECK(disk.isDos());
        CHECK_EQ(disk.fastFileSystem(), ffs);
        CHECK(disk.rootValid());
        CHECK_EQ(disk.diskName(), std::string("Test"));
        CHECK_EQ(disk.bootDescription().substr(0, 4), std::string(ffs ? "DOS1" : "DOS0"));
        CHECK_EQ(disk.rootBlock(), 880U);
        const std::vector<AdfEntry> root = disk.list();
        CHECK_EQ(root.size(), std::size_t{6});                                   // 1, big.bin, empty, Sub and the two names of one hash chain
        const std::vector<AdfEntry> all = disk.listAll();
        CHECK_EQ(all.size(), std::size_t{7});                                    // plus Sub/Inner.Bin
        CHECK(disk.problems().empty());

        auto content = [&](const std::string& path) {
            const std::optional<AdfEntry> e = disk.find(path);
            CHECK(e.has_value());
            return e ? disk.readFile(*e) : std::vector<Byte>{};
        };
        CHECK(content("1") == one);
        CHECK(content("big.bin") == big);                                        // 103 (OFS) or 98 (FFS) blocks: an extension block is needed
        CHECK(content("empty") == nothing);
        CHECK(content("Sub/Inner.Bin") == small);
        CHECK(content("sub/inner.bin") == small);                                // AmigaDOS ignores the case
        CHECK(content("SUB/INNER.BIN") == small);
        CHECK(content(a) == pattern(10, 4));
        CHECK(content(b) == pattern(20, 5));                                     // same hash chain
        CHECK(!disk.find("nosuch").has_value());
        CHECK(!disk.find("Sub/nosuch").has_value());
        CHECK(!disk.find("1/Inner.Bin").has_value());                            // a file is no directory
        CHECK(disk.find("Sub")->kind == AdfEntry::Kind::Directory);
        CHECK_EQ(disk.find("big.bin")->size, 50000U);
        CHECK_EQ(disk.list("Sub").size(), std::size_t{1});
        CHECK_EQ(disk.list("Sub")[0].path, std::string("Sub/Inner.Bin"));
        CHECK(disk.list("nosuchdir").empty());

        // a damaged checksum is reported, the file can still be read
        Disk bad = d;
        const unsigned header = disk.find("1")->block;
        bad.img[header * 512U + 100] ^= 0x55;
        const AdfImage damaged(bad.img);
        CHECK(damaged.find("1").has_value());
        CHECK(!damaged.problems().empty());
        // a data block pointer that leaves the disk is an error with a message
        Disk wild = d;
        wild.put(header, 127 - 50, 99999);
        wild.seal(header);
        const AdfImage wildDisk(wild.img);
        bool threw = false;
        try {
            (void)wildDisk.readFile(*wildDisk.find("1"));
        } catch (const IoError& e) {
            threw = std::string(e.what()).find("outside the disk") != std::string::npos;
        }
        CHECK(threw);
        // a loop in a hash chain ends
        Disk loop = d;
        const unsigned first = loop.get(880, 6 + Disk::hashOf(a));
        loop.put(loop.get(first, 124) ? loop.get(first, 124) : first, 124, first);
        loop.seal(880);
        const AdfImage loopDisk(loop.img);
        (void)loopDisk.listAll();
        CHECK(!loopDisk.problems().empty());
    }

    // a disk with its own loader: no file system, but the blocks can be read
    {
        std::vector<Byte> img(kBlocks * 512, 0);
        img[0] = 0x00, img[1] = 0x00, img[2] = 0x03, img[3] = 0xF3;
        const AdfImage disk(img);
        CHECK(!disk.isDos());
        CHECK(disk.list().empty());
        CHECK(!disk.find("1").has_value());
        CHECK(disk.bootDescription().find("no AmigaDOS file system") != std::string::npos);
        CHECK_EQ(disk.bootBlock().size(), std::size_t{1024});
        CHECK_EQ(disk.block(5).size(), std::size_t{512});
        bool threw = false;
        try {
            (void)disk.block(kBlocks);
        } catch (const IoError&) {
            threw = true;
        }
        CHECK(threw);
        // a Kickstart disk
        img[0] = 'K', img[1] = 'I', img[2] = 'C', img[3] = 'K';
        CHECK(AdfImage(img).bootDescription().find("KICK") != std::string::npos);
    }
    // sizes
    for (const std::size_t n : {std::size_t{0}, std::size_t{512}, std::size_t{901119}, std::size_t{901120 + 100}, std::size_t{10} << 20}) {
        bool threw = false;
        try {
            (void)AdfImage(std::vector<Byte>(n, 0));
        } catch (const IoError&) {
            threw = true;
        }
        CHECK(threw);
    }
    CHECK_EQ(AdfImage(std::vector<Byte>(1802240, 0)).rootBlock(), 1760U);        // high density

    // damaged disks: nothing crashes or hangs, only IoError
    {
        Disk d;
        d.format("Fuzz");
        const unsigned sub = d.makeDir(880, "Dir");
        d.makeFile(880, "a", pattern(3000, 7));
        d.makeFile(sub, "b", pattern(60000, 8));
        std::mt19937 rng(99);
        auto pick = [&rng](unsigned n) { return static_cast<unsigned>(rng() % n); };
        int read = 0, refused = 0;
        for (int trial = 0; trial < 400; ++trial) {
            std::vector<Byte> img = d.img;
            for (unsigned k = 1 + pick(40); k > 0; --k) {
                const unsigned block = pick(3) ? 880 + pick(30) : pick(kBlocks);
                img[block * 512U + pick(512)] = static_cast<Byte>(rng());
            }
            try {
                const AdfImage disk(img);
                for (const AdfEntry& e : disk.listAll()) {
                    if (e.kind == AdfEntry::Kind::File) {
                        try {
                            (void)disk.readFile(e);
                            ++read;
                        } catch (const IoError&) {
                            ++refused;
                        }
                    }
                }
                (void)disk.find("Dir/b");
            } catch (const IoError&) {
                ++refused;
            }
        }
        CHECK(read > 100);
        CHECK(refused > 0);                                                          // some of the damage is noticed, most is tolerated
    }
    return finish("adf");
}
