#pragma once

#include <cstdint>

namespace m68k {

// Programs with SECTIONs are assembled relocatably: the address of a label is not a number but "offset inside
// section n". Such values travel through the expression evaluator as ordinary 64 bit integers with the section in
// the bits above bit 40, so that the usual arithmetic does the right thing: label + 4 stays in the section,
// label2 - label1 inside one section is an ordinary number (the tags cancel), anything else leaves a tag that the
// encoder refuses unless the value ends up in a long absolute address, where it becomes a relocation.
// The tag of section n is 2n+1, i.e. always odd: the difference of labels in two different sections (an even tag) and the sum
// of two addresses (even as well) can therefore be told from a real section-relative value and are refused.
inline constexpr int kSectionShift = 40;
inline constexpr std::int64_t kSectionHalf = std::int64_t{1} << (kSectionShift - 1);

/// The tag of section `index` (0-based).
inline constexpr int tagOfSection(unsigned index) { return static_cast<int>(2 * index + 1); }
/// Value of `offset` inside section `index` (0-based).
inline constexpr std::int64_t relocatable(unsigned index, std::int64_t offset) {
    return (static_cast<std::int64_t>(tagOfSection(index)) << kSectionShift) + offset;
}
/// 0 for a plain number; otherwise the raw tag (odd = a section-relative value, anything else = an unsupported mixture).
inline constexpr int sectionTag(std::int64_t value) { return static_cast<int>((value + kSectionHalf) >> kSectionShift); }
inline constexpr bool isSectionTag(int tag) { return tag > 0 && (tag & 1) != 0; }
/// The section a valid tag refers to.
inline constexpr unsigned tagSection(int tag) { return static_cast<unsigned>((tag - 1) / 2); }
inline constexpr std::int64_t sectionOffset(std::int64_t value) {
    return value - (static_cast<std::int64_t>(sectionTag(value)) << kSectionShift);
}

}  // namespace m68k
