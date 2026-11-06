#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "m68k/assembler.hpp"  // Diagnostic
#include "m68k/decoder.hpp"
#include "m68k/memory.hpp"

namespace m68k {

struct DisassemblerOptions {
    NumberStyle style = NumberStyle::Dollar;
    bool showBytes = true;    ///< listing: show the instruction words
    bool asmSource = false;   ///< output re-assemblable source (ORG, labels, no addresses/bytes)
    bool xrefs = true;        ///< listing: show who jumps/calls/references a label
};

/// Disassembles an address range [start, end) of a memory image.
///
/// Two modes (like the Z80 tools): `analyzeFlow()` follows BSR/JSR/BRA/Bcc/DBcc/JMP from the entry
/// points and separates code from data; `analyzeLinear()` decodes sequentially from `start`.
/// Bytes that are not code are written as DC.B. Targets inside the range get labels
/// (`L_XXXXXX`, or a user symbol).
class Disassembler {
public:
    Disassembler(const Memory& memory, Address start, Address end);

    void addEntry(Address address);                                    ///< starting point for analyzeFlow()
    void addSymbol(Address address, const std::string& name);          ///< user-chosen label name
    void addComment(Address address, const std::string& text);         ///< shown at the instruction at `address`

    void analyzeFlow();
    void analyzeLinear();

    /// Number of bytes recognised as code.
    [[nodiscard]] std::size_t codeBytes() const;

    [[nodiscard]] std::string render(const DisassemblerOptions& options = {}) const;

private:
    enum : std::uint8_t { kData = 0, kStart = 1, kCont = 2 };
    [[nodiscard]] bool inRange(Address a) const { return a >= start_ && a < end_; }
    [[nodiscard]] std::string labelName(Address a) const;
    void follow(Address a, std::vector<Address>& work);
    bool mark(Address a, const Instruction& in);
    void noteTargets(Address from, const Instruction& in, std::vector<Address>* work);

    const Memory& memory_;
    Address start_, end_;
    std::vector<std::uint8_t> state_;
    std::set<Address> entries_;
    std::set<Address> labels_;
    std::map<Address, std::string> symbols_;
    std::map<Address, std::string> comments_;
    std::map<Address, std::set<Address>> xrefs_;  // target -> addresses of instructions referring to it
};

/// Loads a comment file (lines `address,"text"`, ';' comments) into `dis`: every commented address
/// becomes an entry point and a comment. Returns diagnostics for malformed lines.
std::vector<Diagnostic> loadCommentFile(const std::string& text, Disassembler& dis);

}  // namespace m68k
