#pragma once

#include <cstddef>
#include <cstdint>

namespace m68k {

using Byte = std::uint8_t;   ///< 8 bit  (.B)
using Word = std::uint16_t;  ///< 16 bit (.W)
using Long = std::uint32_t;  ///< 32 bit (.L)

/// Addresses are 32 bit wide in the programming model, but the MC68000 only drives A1-A23
/// (plus UDS/LDS), so only the lower 24 bits are significant.
using Address = std::uint32_t;

inline constexpr unsigned kAddressBits = 24;
inline constexpr std::size_t kAddressSpace = std::size_t{1} << kAddressBits;  // 16 MiB
inline constexpr Address kAddressMask = static_cast<Address>(kAddressSpace - 1);

/// Size of the exception vector table: 256 vectors of 4 bytes at 0x000000-0x0003FF.
inline constexpr std::size_t kVectorTableSize = 256 * 4;

/// Operand size of an instruction (.B/.W/.L).
enum class Size : std::uint8_t { B = 1, W = 2, L = 4 };

}  // namespace m68k
