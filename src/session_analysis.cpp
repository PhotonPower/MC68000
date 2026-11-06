// The reverse-engineering commands of the agent protocol: analyze, routines, listing and the
// annotations (names and comments). See docs/reverse-engineering.md.

#include <algorithm>
#include <sstream>

#include "m68k/amiga.hpp"
#include "m68k/format.hpp"
#include "m68k/image_io.hpp"
#include "m68k/listing.hpp"
#include "m68k/session.hpp"

#include "session_impl.hpp"

namespace m68k {

namespace {

std::string hexAddrS(Address a) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%06X", a & kAddressMask);
    return buf;
}

}  // namespace

bool Session::Impl::isAnalysisCommand(const std::string& cmd) const {
    static const char* names[] = {"analyze", "jump_tables", "data_regions", "routines", "listing", "ann_label", "ann_routine", "ann_variable", "ann_data",
                                  "ann_comment", "ann_remove", "ann_title", "ann_list", "ann_save", "ann_load"};
    for (const char* n : names)
        if (cmd == n) return true;
    return false;
}

Json Session::Impl::cmdAnalysis(const std::string& cmd) {
    Annotations& ann = s.ann_;
    auto strOpt = [&](const char* name) -> std::string {
        const Json* j = arg(name);
        return j && j->isString() ? j->asString() : std::string();
    };
    auto failIf = [](const std::optional<std::string>& err) {
        if (err) throw SessionError(*err);
    };

    // ---- analysis ----
    if (cmd == "analyze") {
        if (flag("reset")) {
            s.analysisEntries_.clear();
            s.linearRanges_.clear();
        }
        if (const Json* e = arg("entries")) {
            if (!e->isArray()) throw SessionError("'entries' must be an array");
            for (const Json& x : e->items()) s.analysisEntries_.insert(address(x));
        }
        if (const Json* l = arg("linear")) {
            s.linearRanges_.emplace_back(address(l->find("from") ? *l->find("from") : throw SessionError("linear needs 'from'")),
                                         address(l->find("to") ? *l->find("to") : throw SessionError("linear needs 'to'")));
        }
        if (s.analysisEntries_.empty() && s.linearRanges_.empty()) s.analysisEntries_.insert(s.cpu_->registers().pc);
        Address from, to;
        if (arg("from") && arg("to")) {
            from = address("from");
            to = address("to");
        } else if (s.programRange_) {
            from = s.programRange_->first;
            to = s.programRange_->second;
        } else {
            throw SessionError("nothing was loaded or assembled yet: give 'from' and 'to'");
        }
        if (to <= from) throw SessionError("'to' must be above 'from'");
        if (to - from > 0x400000) throw SessionError("the range is larger than 4 MiB");
        s.analysisView_ = std::make_unique<Memory>();
        s.analysisView_->load(from, readBytes(from, to - from));
        s.analysis_ = std::make_unique<Analysis>(*s.analysisView_, from, to);
        s.analysis_->setDataBarrier([&ann](Address a) { return ann.dataCovering(a) != nullptr; });
        for (const Address e : s.analysisEntries_) s.analysis_->addEntry(e);
        // a region the user (or the model) marked as `pointers` is a table of code addresses: follow them
        for (const auto& [ra, region] : ann.dataRegions())
            if (region.kind == DataKind::Pointers) s.analysis_->addPointerTable(ra, region.length);
        s.analysis_->analyzeFlow();
        for (const auto& [lf, lt] : s.linearRanges_) s.analysis_->analyzeLinear(lf, lt);
        Json j = Json::object();
        j.set("from", hexAddrS(from)).set("to", hexAddrS(to));
        j.set("code_bytes", s.analysis_->codeBytes()).set("range_bytes", to - from);
        j.set("instructions", s.analysis_->code().size());
        j.set("routines", s.analysis_->routines().size());
        j.set("jump_tables", s.analysis_->jumpTables().size());
        Json entries = Json::array();
        for (const Address e : s.analysisEntries_) entries.push(hexAddrS(e));
        j.set("entries", entries);
        return j;
    }

    auto needAnalysis = [&]() -> Analysis& {
        if (!s.analysis_) throw SessionError("no analysis yet: use 'analyze' first");
        return *s.analysis_;
    };

    if (cmd == "data_regions") {
        const Analysis& an = needAnalysis();
        const bool apply = flag("apply");
        const std::vector<DataGuess> guesses = an.guessData([&ann](Address a) { return ann.expressionFor(a).has_value(); });
        Json arr = Json::array();
        unsigned applied = 0;
        for (const DataGuess& g : guesses) {
            Json e = Json::object();
            const char* kind = g.kind == DataGuess::Kind::Text ? "text" : g.kind == DataGuess::Kind::Pointers ? "pointers" : "zeros";
            e.set("addr", hexAddrS(g.start)).set("length", g.length).set("kind", kind).set("reason", g.reason);
            if (!g.preview.empty()) e.set("preview", g.preview);
            if (apply && g.kind != DataGuess::Kind::Zeros) {  // zero runs need no annotation: the listing writes them as DS.B
                DataRegion region;
                region.address = g.start;
                region.length = g.length;
                region.kind = g.kind == DataGuess::Kind::Text ? DataKind::Text : DataKind::Pointers;
                const bool done = !ann.setData(region).has_value();
                e.set("applied", done);
                if (done) ++applied;
            }
            arr.push(e);
        }
        Json j = Json::object();
        j.set("regions", arr);
        if (apply) j.set("applied", applied);
        return j;
    }

    if (cmd == "jump_tables") {
        const Analysis& an = needAnalysis();
        const std::size_t maxEntries = arg("max_entries") ? static_cast<std::size_t>(number(*arg("max_entries"))) : 64;
        Json arr = Json::array();
        for (const JumpTable& t : an.jumpTables()) {
            Json e = Json::object();
            e.set("kind", t.kind == JumpTable::Kind::Branch ? "branch" : t.kind == JumpTable::Kind::Offset ? "offset" : "pointers");
            if (t.jump) e.set("jump", hexAddrS(t.jump));
            e.set("table", hexAddrS(t.table)).set("bytes", t.bytes).set("followed", t.followed);
            if (t.kind == JumpTable::Kind::Offset) e.set("base", hexAddrS(t.base));
            e.set("entry_count", t.slots.size());
            Json entries = Json::array();
            for (std::size_t i = 0; i < t.slots.size() && i < maxEntries; ++i) {
                Json x = Json::object();
                x.set("slot", hexAddrS(t.slots[i])).set("target", hexAddrS(t.targets[i]));
                entries.push(x);
            }
            e.set("entries", entries);
            if (!t.followed) e.set("hint", "looks like a table of code addresses; mark it with ann_data kind=pointers and analyze again to follow it");
            arr.push(e);
        }
        Json j = Json::object();
        j.set("tables", arr);
        return j;
    }

    if (cmd == "routines") {
        const Analysis& an = needAnalysis();
        std::vector<RoutineInfo> list = an.routines();
        const std::string order = arg("order") ? str("order") : "bottom_up";
        if (order != "bottom_up" && order != "address") throw SessionError("order must be bottom_up or address");
        if (order == "bottom_up")
            std::stable_sort(list.begin(), list.end(), [](const RoutineInfo& a, const RoutineInfo& b) { return a.depth < b.depth; });
        const bool onlyUndocumented = flag("undocumented_only");
        auto nameOf = [&](Address a) {
            if (const auto n = ann.nameAt(a)) return *n;
            return "sub_" + hexDigits(a, 6);
        };
        Json out = Json::array();
        for (const RoutineInfo& r : list) {
            const RoutineDoc* doc = ann.routineAt(r.entry);
            const bool documented = doc != nullptr;
            if (onlyUndocumented && documented) continue;
            Json e = Json::object();
            e.set("addr", hexAddrS(r.entry)).set("name", nameOf(r.entry)).set("documented", documented);
            e.set("confidence", doc ? doc->confidence : std::string());
            e.set("start", hexAddrS(r.start)).set("end", hexAddrS(r.end));
            e.set("instructions", r.instructions).set("bytes", r.bytes).set("depth", r.depth);
            Json callers = Json::array();
            for (const Address c : r.callers) callers.push(hexAddrS(c));
            e.set("callers", callers);
            Json callees = Json::array();
            for (const Address c : r.callees) {
                Json cj = Json::object();
                cj.set("addr", hexAddrS(c)).set("name", nameOf(c)).set("documented", ann.routineAt(c) != nullptr);
                callees.push(cj);
            }
            e.set("callees", callees);
            Json ext = Json::array();
            for (const Address c : r.externalCalls) ext.push(hexAddrS(c));
            e.set("external_calls", ext);
            Json lib = Json::array();
            for (const LibraryCall& l : r.libraryCalls) {
                Json lj = Json::object();
                lj.set("addr", hexAddrS(l.address)).set("offset", l.offset);
                lib.push(lj);
            }
            e.set("library_calls", lib);
            out.push(e);
        }
        Json j = Json::object();
        j.set("routines", out);
        return j;
    }

    if (cmd == "listing") {
        Analysis& an = needAnalysis();
        ListingOptions lo;
        lo.from = arg("from") ? address("from") : an.from();
        lo.to = arg("to") ? address("to") : an.to();
        lo.addresses = flag("addresses");
        lo.verify = flag("verify", true);
        if (const Json* m = arg("max_chars")) lo.maxChars = static_cast<std::size_t>(number(*m));
        if (s.amiga_) lo.platformSymbol = [](Address a) { return Amiga500::customRegisterName(a); };
        if (lo.from < an.from() || lo.to > an.to()) throw SessionError("the range is outside of the analysed range " + hexAddrS(an.from()) + "-" + hexAddrS(an.to()));
        const ListingResult r = renderListing(*s.analysisView_, an, ann, lo);
        Json j = Json::object();
        j.set("listing", r.text).set("verified", r.verified).set("truncated", r.truncated).set("unnamed", r.unnamed);
        j.set("instructions", r.instructions).set("user_comments", r.userComments);
        if (!r.problem.empty()) j.set("problem", r.problem);
        return j;
    }

    // ---- annotations ----
    if (cmd == "ann_label") {
        failIf(ann.setLabel(address("addr"), str("name")));
        return Json::object();
    }
    if (cmd == "ann_routine") {
        RoutineDoc d;
        d.name = strOpt("name");
        d.summary = strOpt("summary");
        d.inputs = strOpt("inputs");
        d.outputs = strOpt("outputs");
        d.clobbers = strOpt("clobbers");
        d.confidence = strOpt("confidence");
        failIf(ann.setRoutine(address("addr"), d));
        return Json::object();
    }
    if (cmd == "ann_variable") {
        VariableDoc v;
        v.name = str("name");
        v.address = address("addr");
        v.size = arg("size") ? static_cast<unsigned>(number(need("size"))) : 1;
        v.comment = strOpt("comment");
        failIf(ann.setVariable(v));
        return Json::object();
    }
    if (cmd == "ann_data") {
        DataRegion r;
        r.address = address("addr");
        r.length = static_cast<unsigned>(number(need("length")));
        const std::string kind = arg("kind") ? str("kind") : "bytes";
        const auto k = dataKindFromString(kind);
        if (!k) throw SessionError("kind must be bytes, words, longs, text or pointers");
        r.kind = *k;
        r.name = strOpt("name");
        r.comment = strOpt("comment");
        if (s.analysis_) {
            for (Address a = r.address; a < r.address + r.length; ++a)
                if (s.analysis_->isCode(a))
                    throw SessionError("the range contains code (at " + hexAddrS(a) + "); data regions must not overlap analysed instructions");
        }
        failIf(ann.setData(r));
        return Json::object();
    }
    if (cmd == "ann_comment") {
        const Address a = address("addr");
        if (s.analysis_ && s.analysis_->isCode(a) && !s.analysis_->at(a)) {
            throw SessionError("address " + hexAddrS(a) + " is in the middle of an instruction; the comment would never appear. Use the address of the instruction's first byte");
        }
        if (flag("block")) ann.setBlockComment(a, str("text"));
        else ann.setComment(a, str("text"));
        return Json::object();
    }
    if (cmd == "ann_remove") {
        const Address a = address("addr");
        const bool removed = ann.removeLabel(a);
        const bool removedComment = ann.removeComment(a);
        Json j = Json::object();
        j.set("removed", removed || removedComment);
        return j;
    }
    if (cmd == "ann_title") {
        ann.setTitle(str("title"));
        return Json::object();
    }
    if (cmd == "ann_list") {
        Json j = Json::object();
        j.set("annotations", ann.toJson());
        return j;
    }
    if (cmd == "ann_save") {
        requireFiles("ann_save");
        writeTextFile(str("file"), ann.toJson().dump());
        return Json::object();
    }
    if (cmd == "ann_load") {
        requireFiles("ann_load");
        try {
            s.ann_ = Annotations::fromJson(Json::parse(readTextFile(str("file"))));
        } catch (const JsonError& e) {
            throw SessionError(std::string("cannot load annotations: ") + e.what());
        }
        return Json::object();
    }
    throw SessionError("unknown command");
}

}  // namespace m68k
