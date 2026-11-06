#pragma once

#include <string>
#include <vector>

#include "m68k/assembler.hpp"  // Segment
#include "m68k/image_io.hpp"   // IoError

namespace m68k {

struct HunkSymbol {
    std::string name;   ///< as stored in the file
    Address address;    ///< hunk base + offset
};

struct HunkInfo {
    std::string type;   ///< "CODE", "DATA" or "BSS"
    Address address;    ///< where the hunk was placed
    Address size;       ///< in bytes
};

/// An AmigaDOS executable placed in memory and relocated.
struct HunkImage {
    std::vector<Segment> segments;   ///< one per hunk (BSS as zero bytes)
    std::vector<HunkInfo> hunks;
    std::vector<HunkSymbol> symbols; ///< from HUNK_SYMBOL blocks
    Address entry = 0;               ///< first byte of the first CODE hunk
    Address end = 0;                 ///< address after the last hunk
};

/// True if `bytes` starts with the HUNK_HEADER id ($000003F3).
[[nodiscard]] bool looksLikeHunkFile(const std::vector<Byte>& bytes);

/// Parses an AmigaDOS load file and relocates it: the hunks are placed one after the other starting
/// at `base`, each aligned to 4 bytes, and every HUNK_RELOC32 offset gets the base of its target
/// hunk added. Supported blocks: HUNK_HEADER, HUNK_CODE, HUNK_DATA, HUNK_BSS, HUNK_RELOC32,
/// HUNK_SYMBOL, HUNK_DEBUG, HUNK_NAME, HUNK_END. Anything else (overlays, libraries, other
/// relocation kinds) and every malformed length throws IoError.
[[nodiscard]] HunkImage loadHunkFile(const std::vector<Byte>& bytes, Address base);

/// A symbol name that the assembler accepts as a label: other characters become '_', a leading
/// digit or a register-like name gets a '_' prefix.
[[nodiscard]] std::string asmSymbolName(const std::string& name);

}  // namespace m68k
