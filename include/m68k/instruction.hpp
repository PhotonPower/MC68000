#pragma once

#include <optional>
#include <string>
#include <vector>

#include "m68k/types.hpp"

namespace m68k {

/// How an instruction affects the flow of control.
enum class FlowKind {
    Normal,    ///< continues with the next instruction
    Jump,      ///< unconditional: BRA, JMP
    CondJump,  ///< conditional: Bcc, DBcc (may fall through)
    Call,      ///< BSR, JSR (returns to the next instruction)
    Return     ///< RTS, RTE, RTR, ILLEGAL: the path ends
};

/// One decoded instruction (or one undecodable word, shown as DC.W / DC.B).
struct Instruction {
    Address address = 0;
    std::vector<Byte> bytes;  ///< all words of the instruction, big-endian
    bool valid = false;       ///< false: not a MC68000 instruction (mnemonic is DC.W / DC.B)
    std::string mnemonic;     ///< e.g. "MOVE.L", "BRA.S", "DBRA"
    std::string operands;     ///< e.g. "D0,(A0)+"; empty if none
    FlowKind flow = FlowKind::Normal;
    /// Direct control-flow target: branches, DBcc, and JMP/JSR with an absolute or PC-relative address.
    std::optional<Address> target;
    /// Address of an absolute or PC-relative data operand (LEA label(PC),A0 ...).
    std::optional<Address> reference;

    [[nodiscard]] std::size_t length() const { return bytes.size(); }
    [[nodiscard]] std::string text() const {
        return operands.empty() ? mnemonic : mnemonic + " " + operands;
    }
};

}  // namespace m68k
