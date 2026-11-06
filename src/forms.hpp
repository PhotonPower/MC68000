#pragma once

// Internal: the table of MC68000 instruction forms shared by the decoder and the encoder.

#include <cstdint>
#include <vector>

#include "m68k/types.hpp"

namespace m68k::detail {

/// Effective-address modes as bits, used to describe which modes an operand allows.
enum EaBit : unsigned {
    kDn = 1u << 0,
    kAn = 1u << 1,
    kInd = 1u << 2,
    kPostInc = 1u << 3,
    kPreDec = 1u << 4,
    kD16An = 1u << 5,
    kD8AnXn = 1u << 6,
    kAbsW = 1u << 7,
    kAbsL = 1u << 8,
    kD16Pc = 1u << 9,
    kD8PcXn = 1u << 10,
    kImm = 1u << 11
};

inline constexpr unsigned kAll = 0xFFF;
inline constexpr unsigned kData = kAll & ~kAn;
inline constexpr unsigned kMemory = kAll & ~(kDn | kAn);
inline constexpr unsigned kControl = kInd | kD16An | kD8AnXn | kAbsW | kAbsL | kD16Pc | kD8PcXn;
inline constexpr unsigned kAlterable = kAll & ~(kD16Pc | kD8PcXn | kImm);
inline constexpr unsigned kDataAlt = kData & kAlterable;
inline constexpr unsigned kMemAlt = kMemory & kAlterable;
inline constexpr unsigned kCtlAlt = kControl & kAlterable;

/// The EaBit for an effective address field (mode, register), or 0 if the field is not valid.
inline unsigned eaBitOf(unsigned mode, unsigned reg) {
    switch (mode) {
        case 0: return kDn;
        case 1: return kAn;
        case 2: return kInd;
        case 3: return kPostInc;
        case 4: return kPreDec;
        case 5: return kD16An;
        case 6: return kD8AnXn;
        case 7:
            switch (reg) {
                case 0: return kAbsW;
                case 1: return kAbsL;
                case 2: return kD16Pc;
                case 3: return kD8PcXn;
                case 4: return kImm;
                default: return 0;
            }
        default: return 0;
    }
}

/// What an operand slot of a form holds.
enum class Op : std::uint8_t {
    None,
    Ea,        ///< effective address in bits 5-0 (mode 5-3, register 2-0)
    EaDst,     ///< MOVE destination: register in bits 11-9, mode in bits 8-6
    Dn9, Dn0,  ///< data register in bits 11-9 / 2-0
    An9, An0,  ///< address register in bits 11-9 / 2-0
    PostInc0, PostInc9,  ///< (An)+ with An in bits 2-0 / 11-9
    PreDec0, PreDec9,    ///< -(An) with An in bits 2-0 / 11-9
    Imm,       ///< immediate of the instruction size (extension word(s))
    ImmQ3,     ///< 1..8 in bits 11-9 (0 means 8)
    ImmMoveq,  ///< 8 bit in bits 7-0
    ImmTrap,   ///< 0..15 in bits 3-0
    ImmWord,   ///< 16 bit extension word, printed unsigned (STOP)
    ImmSWord,  ///< 16 bit extension word, printed signed (LINK)
    ImmBit,    ///< bit number 0..255 in an extension word
    Ccr, Sr, Usp,
    Branch,    ///< Bcc/BRA/BSR: 8 bit displacement in the opcode, or 16 bit in an extension word
    DbDisp,    ///< 16 bit displacement extension word (DBcc)
    RegList,   ///< MOVEM register mask extension word
    D16An0     ///< d16(An) with An in bits 2-0 and one extension word (MOVEP)
};

/// Where the operation size is stored (and how it is written).
enum class SizeKind : std::uint8_t {
    None,   ///< no size
    Any,    ///< optional .B/.L suffix, no size field (bit operations)
    B76,    ///< bits 7-6: 00 = B, 01 = W, 10 = L (11 never matches)
    Move,   ///< bits 13-12: 01 = B, 11 = W, 10 = L
    W8,     ///< bit 8: 0 = W, 1 = L
    W6,     ///< bit 6: 0 = W, 1 = L
    Fixed   ///< fixed size given in `fixed`
};

struct Form {
    const char* name = "";
    std::uint16_t mask = 0;   ///< opcode bits that identify the form
    std::uint16_t value = 0;  ///< their values
    SizeKind sk = SizeKind::None;
    Size fixed = Size::W;     ///< for SizeKind::Fixed
    bool print = false;       ///< write the size as a suffix (".W") when disassembling
    bool noAnByte = false;    ///< an address register operand is illegal with byte size
    Op op[2] = {Op::None, Op::None};
    std::uint16_t set[2] = {0, 0};  ///< allowed EA modes for Ea/EaDst operands
};

/// All forms in matching order: the decoder uses the first form whose mask matches.
const std::vector<Form>& forms();

/// Condition code names indexed by the 4 bit condition field.
inline constexpr const char* kCondNames[16] = {"T",  "F",  "HI", "LS", "CC", "CS", "NE", "EQ",
                                               "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE"};

}  // namespace m68k::detail
