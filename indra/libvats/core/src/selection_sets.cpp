// Viewport Avatar Toolset - selection sets: named groups of bones, in the project and in the library.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/selection_sets.h"

#include <algorithm>

namespace vats {
namespace {
constexpr const char* kFormat = "vats-selection-sets";
}  // namespace

int store_selection_set(std::vector<SelectionSet>& sets, const std::string& name, std::vector<std::string> bones) {
    auto it = std::find_if(sets.begin(), sets.end(), [&](const SelectionSet& s) { return s.name == name; });
    if (it == sets.end()) it = sets.insert(sets.end(), SelectionSet{name, {}});
    it->bones = std::move(bones);
    return int(it - sets.begin());
}

std::vector<int> recall_selection_set(const Skeleton& skel, const SelectionSet& set) {
    std::vector<int> out;
    for (const std::string& b : set.bones)
        if (int i = skel.find(b); i >= 0 && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    return out;
}

int edit_selection_set(SelectionSet& set, const std::vector<std::string>& bones, bool add) {
    int n = 0;
    for (const std::string& b : bones) {
        auto it = std::find(set.bones.begin(), set.bones.end(), b);
        if (add && it == set.bones.end()) set.bones.push_back(b), ++n;
        else if (!add && it != set.bones.end()) set.bones.erase(it), ++n;
    }
    return n;
}

Json selection_sets_to_json(const std::vector<SelectionSet>& sets) {
    Json out = Json::array();
    for (const SelectionSet& s : sets) {
        Json e = Json::object();
        e.set("name", s.name);
        Json& bones = e.set("bones", Json::array());
        for (const std::string& b : s.bones) bones.push(b);
        out.push(std::move(e));
    }
    return out;
}

bool selection_sets_from_json(const Json& v, std::vector<SelectionSet>& out, std::string& err) {
    if (!v.is_array()) return err = "selection_sets is not an array", false;
    std::vector<SelectionSet> sets;
    for (size_t n = 0; n < v.arr.size(); ++n) {
        const std::string w = "selection_sets[" + std::to_string(n) + "]";
        const Json& e = v.arr[n];
        const Json* name = e.find("name");
        const Json* bones = e.find("bones");
        if (!name || !name->is_string()) return err = w + ".name is not a string", false;
        if (!bones || !bones->is_array()) return err = w + ".bones is not an array", false;
        SelectionSet s{name->str, {}};
        for (const Json& b : bones->arr) {
            if (!b.is_string()) return err = w + ".bones holds a non-string", false;
            s.bones.push_back(b.str);
        }
        sets.push_back(std::move(s));
    }
    out = std::move(sets);
    return true;
}

bool load_selection_sets(std::string_view text, std::vector<SelectionSet>& out, std::string& err) {
    Json doc;
    if (!parse_json(text, doc, err)) return false;
    const Json* format = doc.find("format");
    if (!format || !format->is_string() || format->str != kFormat) return err = "not a VATs selection set file", false;
    const Json* sets = doc.find("sets");
    if (!sets) return err = "sets is missing", false;
    return selection_sets_from_json(*sets, out, err);
}

std::string save_selection_sets(const std::vector<SelectionSet>& sets) {
    Json doc = Json::object();
    doc.set("format", kFormat);
    doc.set("sets", selection_sets_to_json(sets));
    return write_json(doc);
}

}  // namespace vats
