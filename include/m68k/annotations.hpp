#pragma once

#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>

#include "m68k/json.hpp"
#include "m68k/types.hpp"

namespace m68k {

/// Documentation of a subroutine (shown as a header block above its label).
struct RoutineDoc {
    std::string name;
    std::string summary;     ///< what it does
    std::string inputs;      ///< e.g. "A0 = source, D0 = length"
    std::string outputs;     ///< e.g. "D0 = checksum"
    std::string clobbers;    ///< e.g. "D1 A1"
    std::string confidence;  ///< "", "high" (verified by a test), "medium" or "low" (a guess, to be checked)
};

/// A named memory location (a variable, or a table).
struct VariableDoc {
    std::string name;
    Address address = 0;
    unsigned size = 1;  ///< bytes
    std::string comment;
};

enum class DataKind { Bytes, Words, Longs, Text, Pointers };
const char* toString(DataKind kind);
std::optional<DataKind> dataKindFromString(const std::string& text);

/// A range that is data, not code, and how to show it.
struct DataRegion {
    Address address = 0;
    unsigned length = 0;
    DataKind kind = DataKind::Bytes;
    std::string name;     ///< optional label at the start
    std::string comment;
};

/// Everything a human or an LLM has learned about a program: names and comments. It contains no
/// code; the listing is always generated from the real bytes, so annotations cannot change what the
/// program does (the listing is re-assembled and compared, see renderListing).
class Annotations {
public:
    /// All setters return an error message, or nullopt on success.
    std::optional<std::string> setLabel(Address address, const std::string& name);
    std::optional<std::string> setRoutine(Address address, const RoutineDoc& doc);
    std::optional<std::string> setVariable(const VariableDoc& variable);
    std::optional<std::string> setData(const DataRegion& region);
    void setComment(Address address, const std::string& text) { comments_[address & kAddressMask] = text; }
    void setBlockComment(Address address, const std::string& text) { blockComments_[address & kAddressMask] = text; }
    /// Marks the instruction at `address` as one whose immediate operand is a plain number, not an address: the listing
    /// then neither invents a label for the value nor adds an `[#imm = ...]` note.
    void setConstant(Address address, bool on) {
        if (on) constants_.insert(address & kAddressMask);
        else constants_.erase(address & kAddressMask);
    }
    void setTitle(const std::string& title) { title_ = title; }

    bool removeLabel(Address address);  ///< also removes a routine/variable/data name at that address
    bool removeComment(Address address);
    void clear();

    /// Checks a name without registering it: a valid identifier that is not a register, mnemonic,
    /// directive, hardware register name or generated placeholder (`sub_001000`).
    [[nodiscard]] static std::optional<std::string> checkName(const std::string& name);

    // ---- lookups ----
    /// The name given to exactly this address (label, routine, variable or data region), or nullopt.
    [[nodiscard]] std::optional<std::string> nameAt(Address address) const;
    /// "name" or "name+k" if `address` lies inside a named variable/data region.
    [[nodiscard]] std::optional<std::string> expressionFor(Address address) const;
    [[nodiscard]] std::optional<Address> addressOf(const std::string& name) const;
    [[nodiscard]] const RoutineDoc* routineAt(Address address) const;
    [[nodiscard]] const std::string* commentAt(Address address) const;
    [[nodiscard]] bool isConstant(Address address) const { return constants_.count(address & kAddressMask) != 0; }
    [[nodiscard]] const std::string* blockCommentAt(Address address) const;
    [[nodiscard]] const DataRegion* dataAt(Address address) const;     ///< region *starting* at address
    [[nodiscard]] const DataRegion* dataCovering(Address address) const;  ///< region that contains address
    [[nodiscard]] const VariableDoc* variableAt(Address address) const;   ///< variable *starting* at address

    [[nodiscard]] const std::map<Address, std::string>& labels() const { return labels_; }
    [[nodiscard]] const std::map<Address, RoutineDoc>& routines() const { return routines_; }
    [[nodiscard]] const std::vector<VariableDoc>& variables() const { return variables_; }
    [[nodiscard]] const std::map<Address, DataRegion>& dataRegions() const { return data_; }
    [[nodiscard]] const std::map<Address, std::string>& comments() const { return comments_; }
    [[nodiscard]] const std::map<Address, std::string>& blockComments() const { return blockComments_; }
    [[nodiscard]] const std::set<Address>& constants() const { return constants_; }
    [[nodiscard]] const std::string& title() const { return title_; }

    [[nodiscard]] Json toJson() const;
    /// Throws JsonError on malformed input or rule violations (duplicate names, ...).
    static Annotations fromJson(const Json& json);

private:
    std::optional<std::string> claimName(Address address, const std::string& name);

    std::map<Address, std::string> labels_;   ///< address -> name (also routine/variable/data names)
    std::map<std::string, Address> byName_;   ///< upper-case name -> address
    std::map<Address, RoutineDoc> routines_;
    std::vector<VariableDoc> variables_;
    std::map<Address, DataRegion> data_;
    std::map<Address, std::string> comments_;
    std::map<Address, std::string> blockComments_;
    std::set<Address> constants_;   ///< instructions whose immediate is a number, not a pointer
    std::string title_;
};

}  // namespace m68k
