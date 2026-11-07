#pragma once

#include <functional>
#include <vector>
#include <string>

#include "m68k/analysis.hpp"
#include "m68k/annotations.hpp"
#include "m68k/memory.hpp"

namespace m68k {

/// A part of the image with a known kind (the hunks of a loaded Amiga program).
struct ListingSegment {
    Address start = 0, end = 0;   ///< [start, end)
    std::string kind;             ///< "CODE", "DATA" or "BSS"
};

struct ListingOptions {
    Address from = 0, to = 0;   ///< range [from, to)
    bool addresses = false;     ///< append each line's address as a comment (to refer to it in annotations)
    bool verify = true;         ///< assemble the listing again and compare the bytes
    std::size_t maxChars = 0;   ///< 0 = unlimited; otherwise cut at a line boundary (the cut text is not verified)
    /// Hunks of a loaded program: a comment marks where each begins, and zero bytes in a BSS hunk are written as DS.B from 4 bytes on (else from 16).
    std::vector<ListingSegment> segments;
    /// EQU lines for names that cannot stand on a line of their own. They are needed to assemble the listing again; a reader of an unverified
    /// excerpt can do without them (they are left out only if `verify` is off).
    bool equates = true;
    /// Names of hardware addresses (custom chip registers); "" if none.
    std::function<std::string(Address)> platformSymbol;
};

struct ListingResult {
    std::string text;
    bool verified = false;      ///< the listing assembles to exactly the bytes of the range
    bool truncated = false;
    std::string problem;        ///< why verification failed
    unsigned unnamed = 0;       ///< generated placeholder labels (sub_/loc_/data_) that still await a real name
    unsigned instructions = 0;  ///< instruction lines in the listing
    unsigned userComments = 0;  ///< instruction lines that carry a comment from the annotations
};

/// Writes an assembler listing of a range: instructions from the analysis (with names and comments
/// from the annotations), data as DC.B/DC.W/DC.L/DS.B, routine headers, labels. It is generated from
/// the real bytes, never from the annotations, and (with `verify`) assembled again and compared byte
/// for byte, so a wrong name or comment cannot change what the program does.
[[nodiscard]] ListingResult renderListing(const Memory& view, const Analysis& analysis, const Annotations& annotations,
                                          const ListingOptions& options);

}  // namespace m68k
