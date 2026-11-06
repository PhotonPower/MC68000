#pragma once

#include <functional>
#include <string>

#include "m68k/analysis.hpp"
#include "m68k/annotations.hpp"
#include "m68k/memory.hpp"

namespace m68k {

struct ListingOptions {
    Address from = 0, to = 0;   ///< range [from, to)
    bool addresses = false;     ///< append each line's address as a comment (to refer to it in annotations)
    bool verify = true;         ///< assemble the listing again and compare the bytes
    std::size_t maxChars = 0;   ///< 0 = unlimited; otherwise cut at a line boundary (the cut text is not verified)
    /// Names of hardware addresses (custom chip registers); "" if none.
    std::function<std::string(Address)> platformSymbol;
};

struct ListingResult {
    std::string text;
    bool verified = false;      ///< the listing assembles to exactly the bytes of the range
    bool truncated = false;
    std::string problem;        ///< why verification failed
    unsigned unnamed = 0;       ///< generated placeholder labels (sub_/loc_/data_) that still await a real name
};

/// Writes an assembler listing of a range: instructions from the analysis (with names and comments
/// from the annotations), data as DC.B/DC.W/DC.L/DS.B, routine headers, labels. It is generated from
/// the real bytes, never from the annotations, and (with `verify`) assembled again and compared byte
/// for byte, so a wrong name or comment cannot change what the program does.
[[nodiscard]] ListingResult renderListing(const Memory& view, const Analysis& analysis, const Annotations& annotations,
                                          const ListingOptions& options);

}  // namespace m68k
