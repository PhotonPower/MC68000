#pragma once

#include <optional>
#include <string>
#include <vector>

#include "m68k/types.hpp"

namespace m68k {

/// One entry of an AmigaDOS directory on a disk image.
struct AdfEntry {
    std::string name;        ///< the name in its directory ("Inner.Bin")
    std::string path;        ///< the path from the root ("sub/Inner.Bin")
    enum class Kind { File, Directory, SoftLink, HardLink, Other } kind = Kind::File;
    std::uint32_t size = 0;  ///< bytes (files)
    std::uint32_t block = 0; ///< the header block
    std::uint32_t protection = 0;
    std::string comment;
};

/// Read access to an Amiga floppy disk image (ADF): the boot block, the root block and the directory tree of an OFS or FFS volume, and the contents
/// of files. Only reading; nothing here writes. A disk with a custom boot loader (no AmigaDOS file system, typical for games and demos that load their
/// own tracks) is accepted: `isDos()` says so, and `bootBlock()` and `block()` still work.
class AdfImage {
public:
    /// Throws IoError if the size is not that of a double or high density disk (880 or 1760 KiB, a few extra tracks are tolerated).
    explicit AdfImage(std::vector<Byte> bytes);

    [[nodiscard]] unsigned blocks() const { return static_cast<unsigned>(data_.size() / 512); }
    [[nodiscard]] std::vector<Byte> block(unsigned n) const;
    /// The two boot blocks (1024 bytes).
    [[nodiscard]] std::vector<Byte> bootBlock() const { return std::vector<Byte>(data_.begin(), data_.begin() + 1024); }

    /// "DOS\\0".."DOS\\7" in the first four bytes: an AmigaDOS volume (with a file system that is read here).
    [[nodiscard]] bool isDos() const { return dos_; }
    [[nodiscard]] bool fastFileSystem() const { return dos_ && (flags_ & 1U); }
    [[nodiscard]] bool international() const { return dos_ && (flags_ & 2U); }
    [[nodiscard]] bool dirCache() const { return dos_ && (flags_ & 4U); }
    [[nodiscard]] std::string bootDescription() const;   ///< "DOS1 (FFS)", "KICK", "no file system: 4 bytes 00 00 03 F3"
    [[nodiscard]] bool bootChecksumOk() const;
    [[nodiscard]] unsigned rootBlock() const { return root_; }
    [[nodiscard]] bool rootValid() const { return rootOk_; }
    [[nodiscard]] const std::string& diskName() const { return diskName_; }

    /// The entries of a directory ("" = the root), in hash order. Empty if the file system cannot be read.
    [[nodiscard]] std::vector<AdfEntry> list(const std::string& directory = "") const;
    /// Every entry below `directory`, directories before their contents.
    [[nodiscard]] std::vector<AdfEntry> listAll(const std::string& directory = "") const;
    /// Looks a path up ("sub/Inner.Bin"; the names are compared without regard to case, as AmigaDOS does).
    [[nodiscard]] std::optional<AdfEntry> find(const std::string& path) const;
    /// The contents of a file. Throws IoError for a damaged chain of blocks.
    [[nodiscard]] std::vector<Byte> readFile(const AdfEntry& entry) const;

    /// What was wrong with the file system while it was read (checksums, loops, blocks outside the disk): the reading goes on where it can.
    [[nodiscard]] const std::vector<std::string>& problems() const { return problems_; }

private:
    [[nodiscard]] std::uint32_t word(unsigned block, unsigned index) const;   // long `index` (0..127) of a block
    [[nodiscard]] bool blockChecksumOk(unsigned block) const;
    [[nodiscard]] std::string nameOf(unsigned block) const;
    [[nodiscard]] std::optional<AdfEntry> entryAt(unsigned block, const std::string& parentPath) const;
    void collect(unsigned directoryBlock, const std::string& path, bool recursive, std::vector<AdfEntry>& out, unsigned depth) const;
    [[nodiscard]] unsigned hashOf(const std::string& name) const;
    [[nodiscard]] int directoryBlockOf(const std::string& path) const;

    std::vector<Byte> data_;
    bool dos_ = false;
    unsigned flags_ = 0;
    unsigned root_ = 880;
    bool rootOk_ = false;
    std::string diskName_;
    mutable std::vector<std::string> problems_;
};

}  // namespace m68k
