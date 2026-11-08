#include "m68k/annotations.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#include "m68k/amiga.hpp"
#include "m68k/encoder.hpp"
#include "m68k/format.hpp"

namespace m68k {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

const std::set<std::string>& reservedWords() {
    static const std::set<std::string> words = [] {
        std::set<std::string> w = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7",
                                   "SP", "PC", "SR", "CCR", "USP", "ORG", "EQU", "DC", "DS", "DCB", "EVEN", "END", "SECTION",
                                   "XDEF", "XREF"};
        for (unsigned off = 0; off < 0x200; off += 2) {
            const std::string n = Amiga500::customRegisterName(0xDFF000 + off);
            if (!n.empty()) w.insert(upper(n));
        }
        return w;
    }();
    return words;
}

bool isPlaceholder(const std::string& name) {
    // sub_XXXXXX / loc_XXXXXX / data_XXXXXX are generated for unnamed addresses
    for (const char* prefix : {"sub_", "loc_", "data_"}) {
        const std::string p = prefix;
        if (name.size() == p.size() + 6 && upper(name.substr(0, p.size())) == upper(p)) {
            bool allHex = true;
            for (std::size_t i = p.size(); i < name.size(); ++i) allHex = allHex && std::isxdigit(static_cast<unsigned char>(name[i]));
            if (allHex) return true;
        }
    }
    return false;
}

}  // namespace

const char* toString(DataKind k) {
    switch (k) {
        case DataKind::Bytes: return "bytes";
        case DataKind::Words: return "words";
        case DataKind::Longs: return "longs";
        case DataKind::Text: return "text";
        case DataKind::Pointers: return "pointers";
    }
    return "bytes";
}

std::optional<DataKind> dataKindFromString(const std::string& t) {
    if (t == "bytes") return DataKind::Bytes;
    if (t == "words") return DataKind::Words;
    if (t == "longs") return DataKind::Longs;
    if (t == "text") return DataKind::Text;
    if (t == "pointers") return DataKind::Pointers;
    return std::nullopt;
}

std::optional<std::string> Annotations::checkName(const std::string& name) {
    if (name.empty()) return "the name is empty";
    if (name.size() > 64) return "the name is longer than 64 characters";
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
        return "a name must start with a letter or '_'";
    for (const char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return std::string("a name may only contain letters, digits and '_' (found '") + c + "')";
    const std::string u = upper(name);
    if (reservedWords().count(u)) return "'" + name + "' is reserved (register, directive or hardware register name)";
    if (isMnemonic(name)) return "'" + name + "' is an instruction mnemonic";
    if (isPlaceholder(name)) return "'" + name + "' looks like a generated placeholder (sub_/loc_/data_ + 6 hex digits)";
    return std::nullopt;
}

std::optional<std::string> Annotations::claimName(Address address, const std::string& name) {
    address &= kAddressMask;
    if (auto err = checkName(name)) return err;
    const std::string key = upper(name);
    if (const auto it = byName_.find(key); it != byName_.end() && it->second != address)
        return "the name '" + name + "' is already used for " + formatHex(it->second, 6, NumberStyle::Dollar);
    if (const auto old = labels_.find(address); old != labels_.end()) byName_.erase(upper(old->second));
    labels_[address] = name;
    byName_[key] = address;
    // a name is the name of everything documented at that address: keep the records consistent
    if (const auto it = routines_.find(address); it != routines_.end()) it->second.name = name;
    for (VariableDoc& v : variables_)
        if (v.address == address) v.name = name;
    if (const auto it = data_.find(address); it != data_.end()) it->second.name = name;
    return std::nullopt;
}

std::optional<std::string> Annotations::setLabel(Address address, const std::string& name) { return claimName(address, name); }

std::optional<std::string> Annotations::setRoutine(Address address, const RoutineDoc& doc) {
    address &= kAddressMask;
    if (!doc.name.empty()) {
        if (auto err = claimName(address, doc.name)) return err;
    }
    RoutineDoc d = doc;
    if (d.name.empty()) {
        if (const auto n = nameAt(address)) d.name = *n;
    }
    if (!d.confidence.empty() && d.confidence != "high" && d.confidence != "medium" && d.confidence != "low")
        return "confidence must be high, medium or low";
    routines_[address] = d;
    return std::nullopt;
}

std::optional<std::string> Annotations::setVariable(const VariableDoc& v) {
    VariableDoc var = v;
    var.address &= kAddressMask;
    if (var.size < 1 || var.size > 0x10000) return "size must be 1..65536";
    if (auto err = claimName(var.address, var.name)) return err;
    for (VariableDoc& x : variables_) {
        if (x.address == var.address) {
            x = var;
            return std::nullopt;
        }
    }
    variables_.push_back(var);
    return std::nullopt;
}

std::optional<std::string> Annotations::setData(const DataRegion& r) {
    DataRegion reg = r;
    reg.address &= kAddressMask;
    if (reg.length < 1 || reg.length > 0x100000) return "length must be 1..1048576";
    if (reg.kind == DataKind::Words && (reg.address & 1U)) return "word data must start at an even address";
    if (reg.kind == DataKind::Longs && (reg.address & 1U)) return "long data must start at an even address";
    if (reg.kind == DataKind::Pointers && (reg.address & 1U)) return "pointer data must start at an even address";
    if (reg.kind == DataKind::Words && reg.length % 2) return "word data needs an even length";
    if ((reg.kind == DataKind::Longs || reg.kind == DataKind::Pointers) && reg.length % 4) return "long/pointer data needs a length that is a multiple of 4";
    // a new region may replace one that starts at the same address, but not overlap others
    for (const auto& [a, old] : data_) {
        if (a == reg.address) continue;
        if (reg.address < a + old.length && a < reg.address + reg.length)
            return "overlaps the data region at " + formatHex(a, 6, NumberStyle::Dollar);
    }
    if (!reg.name.empty()) {
        if (auto err = claimName(reg.address, reg.name)) return err;
    }
    data_[reg.address] = reg;
    return std::nullopt;
}

bool Annotations::removeLabel(Address address) {
    address &= kAddressMask;
    bool any = false;
    if (const auto it = labels_.find(address); it != labels_.end()) {
        byName_.erase(upper(it->second));
        labels_.erase(it);
        any = true;
    }
    any = routines_.erase(address) > 0 || any;
    const std::size_t before = variables_.size();
    variables_.erase(std::remove_if(variables_.begin(), variables_.end(), [&](const VariableDoc& v) { return v.address == address; }),
                     variables_.end());
    any = variables_.size() != before || any;
    if (const auto it = data_.find(address); it != data_.end()) {
        data_.erase(it);
        any = true;
    }
    return any;
}

bool Annotations::removeComment(Address address) {
    address &= kAddressMask;
    return comments_.erase(address) + blockComments_.erase(address) + constants_.erase(address) > 0;
}

void Annotations::clear() { *this = Annotations{}; }

std::optional<std::string> Annotations::nameAt(Address a) const {
    const auto it = labels_.find(a & kAddressMask);
    if (it == labels_.end()) return std::nullopt;
    return it->second;
}

namespace {
/// The offset of a "name+offset" expression, written like the other numbers of a listing: hexadecimal ("$26"); below ten the
/// digit is the same in both bases and stays bare ("2").
std::string plusHex(Address offset) {
    char buf[16];
    if (offset < 10) std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(offset));
    else std::snprintf(buf, sizeof buf, "$%X", static_cast<unsigned>(offset));
    return buf;
}
}  // namespace

std::optional<std::string> Annotations::expressionFor(Address a) const {
    a &= kAddressMask;
    if (const auto n = nameAt(a)) return n;
    for (const VariableDoc& v : variables_)
        if (a > v.address && a < v.address + v.size) return v.name + "+" + plusHex(a - v.address);
    for (const auto& [start, r] : data_)
        if (!r.name.empty() && a > start && a < start + r.length) return r.name + "+" + plusHex(a - start);
    return std::nullopt;
}

std::optional<Address> Annotations::addressOf(const std::string& name) const {
    const auto it = byName_.find(upper(name));
    if (it == byName_.end()) return std::nullopt;
    return it->second;
}

const RoutineDoc* Annotations::routineAt(Address a) const {
    const auto it = routines_.find(a & kAddressMask);
    return it == routines_.end() ? nullptr : &it->second;
}
const std::string* Annotations::commentAt(Address a) const {
    const auto it = comments_.find(a & kAddressMask);
    return it == comments_.end() ? nullptr : &it->second;
}
const std::string* Annotations::blockCommentAt(Address a) const {
    const auto it = blockComments_.find(a & kAddressMask);
    return it == blockComments_.end() ? nullptr : &it->second;
}
const DataRegion* Annotations::dataAt(Address a) const {
    const auto it = data_.find(a & kAddressMask);
    return it == data_.end() ? nullptr : &it->second;
}
const DataRegion* Annotations::dataCovering(Address a) const {
    a &= kAddressMask;
    auto it = data_.upper_bound(a);
    if (it == data_.begin()) return nullptr;
    --it;
    return a < it->first + it->second.length ? &it->second : nullptr;
}
const VariableDoc* Annotations::variableAt(Address a) const {
    for (const VariableDoc& v : variables_)
        if (v.address == (a & kAddressMask)) return &v;
    return nullptr;
}

namespace {
std::string hexAddr(Address a) { return formatHex(a & kAddressMask, 6, NumberStyle::CPrefix); }
Address parseAddr(const Json& j) {
    if (j.isNumber()) return static_cast<Address>(j.asInt()) & kAddressMask;
    if (j.isString()) {
        try {
            return static_cast<Address>(std::stoul(j.asString(), nullptr, 0)) & kAddressMask;
        } catch (const std::exception&) {
        }
    }
    throw JsonError("bad address in annotations");
}
const Json& need(const Json& j, const char* key) {
    const Json* v = j.find(key);
    if (!v) throw JsonError(std::string("annotations: missing field '") + key + "'");
    return *v;
}
std::string getStr(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isString() ? v->asString() : std::string();
}
}  // namespace

Json Annotations::toJson() const {
    Json j = Json::object();
    j.set("version", 1);
    j.set("title", title_);
    Json labels = Json::array();
    for (const auto& [a, n] : labels_) {
        Json e = Json::object();
        e.set("addr", hexAddr(a)).set("name", n);
        labels.push(e);
    }
    j.set("labels", labels);
    Json routines = Json::array();
    for (const auto& [a, r] : routines_) {
        Json e = Json::object();
        e.set("addr", hexAddr(a)).set("name", r.name).set("summary", r.summary).set("inputs", r.inputs);
        e.set("outputs", r.outputs).set("clobbers", r.clobbers).set("confidence", r.confidence);
        routines.push(e);
    }
    j.set("routines", routines);
    Json vars = Json::array();
    for (const VariableDoc& v : variables_) {
        Json e = Json::object();
        e.set("name", v.name).set("addr", hexAddr(v.address)).set("size", v.size).set("comment", v.comment);
        vars.push(e);
    }
    j.set("variables", vars);
    Json data = Json::array();
    for (const auto& [a, r] : data_) {
        Json e = Json::object();
        e.set("addr", hexAddr(a)).set("length", r.length).set("kind", toString(r.kind)).set("name", r.name).set("comment", r.comment);
        data.push(e);
    }
    j.set("data", data);
    Json comments = Json::array();
    for (const auto& [a, t] : comments_) {
        Json e = Json::object();
        e.set("addr", hexAddr(a)).set("text", t);
        comments.push(e);
    }
    j.set("comments", comments);
    Json blocks = Json::array();
    for (const auto& [a, t] : blockComments_) {
        Json e = Json::object();
        e.set("addr", hexAddr(a)).set("text", t);
        blocks.push(e);
    }
    j.set("block_comments", blocks);
    Json constants = Json::array();
    for (const Address a : constants_) {
        Json e = Json::object();
        e.set("addr", hexAddr(a));
        constants.push(e);
    }
    j.set("constants", constants);
    return j;
}

Annotations Annotations::fromJson(const Json& j) {
    if (!j.isObject()) throw JsonError("annotations must be a JSON object");
    Annotations a;
    auto fail = [](const std::string& what, const std::string& why) { throw JsonError(what + ": " + why); };
    a.title_ = getStr(j, "title");
    if (const Json* l = j.find("labels"))
        for (const Json& e : l->items()) {
            if (auto err = a.setLabel(parseAddr(need(e, "addr")), getStr(e, "name"))) fail("label", *err);
        }
    if (const Json* l = j.find("routines"))
        for (const Json& e : l->items()) {
            RoutineDoc d;
            d.name = getStr(e, "name");
            d.summary = getStr(e, "summary");
            d.inputs = getStr(e, "inputs");
            d.outputs = getStr(e, "outputs");
            d.clobbers = getStr(e, "clobbers");
            d.confidence = getStr(e, "confidence");
            if (auto err = a.setRoutine(parseAddr(need(e, "addr")), d)) fail("routine", *err);
        }
    if (const Json* l = j.find("variables"))
        for (const Json& e : l->items()) {
            VariableDoc v;
            v.name = getStr(e, "name");
            v.address = parseAddr(need(e, "addr"));
            v.size = static_cast<unsigned>(e.find("size") ? e.find("size")->asInt() : 1);
            v.comment = getStr(e, "comment");
            if (auto err = a.setVariable(v)) fail("variable", *err);
        }
    if (const Json* l = j.find("data"))
        for (const Json& e : l->items()) {
            DataRegion r;
            r.address = parseAddr(need(e, "addr"));
            r.length = static_cast<unsigned>(e.find("length") ? e.find("length")->asInt() : 0);
            const auto kind = dataKindFromString(getStr(e, "kind"));
            if (!kind) fail("data", "unknown kind");
            r.kind = *kind;
            r.name = getStr(e, "name");
            r.comment = getStr(e, "comment");
            if (auto err = a.setData(r)) fail("data", *err);
        }
    if (const Json* l = j.find("comments"))
        for (const Json& e : l->items()) a.setComment(parseAddr(need(e, "addr")), getStr(e, "text"));
    if (const Json* l = j.find("block_comments"))
        for (const Json& e : l->items()) a.setBlockComment(parseAddr(need(e, "addr")), getStr(e, "text"));
    if (const Json* l = j.find("constants"))
        for (const Json& e : l->items()) a.setConstant(parseAddr(need(e, "addr")), true);
    return a;
}

}  // namespace m68k
