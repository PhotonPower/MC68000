#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "m68k/assembler.hpp"
#include "m68k/memory.hpp"

namespace m68k {

struct IoError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Reads/writes whole files. Throw IoError.
std::vector<Byte> readBinaryFile(const std::string& path);
std::string readTextFile(const std::string& path);
void writeBinaryFile(const std::string& path, const std::vector<Byte>& bytes);
void writeTextFile(const std::string& path, const std::string& text);

/// Memory contents as address ranges (what an S-record file holds).
struct Image {
    std::vector<Segment> segments;
    std::optional<Address> entry;  ///< start address from the termination record, if any
};

/// Parses Motorola S-records (S0, S1-S3, S5-S9). Checksums are verified; throws IoError with the
/// line number on malformed input.
Image parseSRecords(const std::string& text);

/// Writes S-records: S0 header, S2 data records (24 bit addresses, up to 32 bytes each) and an S8
/// termination record with `entry` (default 0).
std::string formatSRecords(const std::vector<Segment>& segments, std::optional<Address> entry = std::nullopt,
                           const std::string& name = "M68K");

/// True for file names that look like S-record files (.srec .s19 .s28 .s37 .mot).
bool looksLikeSRecordFile(const std::string& path);

/// Copies all segments into `memory`.
void loadSegments(Memory& memory, const std::vector<Segment>& segments);

}  // namespace m68k
