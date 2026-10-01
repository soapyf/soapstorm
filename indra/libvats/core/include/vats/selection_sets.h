// Viewport Avatar Toolset - selection sets: named groups of bones, in the project and in the library.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 24 (SS-1..SS-3). The sets of a project are Clip::selection_sets, saved as its
// "selection_sets" field; the library's are a file of their own (selection_sets.json beside the pose library).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "vats/clip.h"
#include "vats/json.h"
#include "vats/skeleton.h"

namespace vats {

// SS-1: saves bones under name: replaces the set of that name, else appends one. Returns its index.
int store_selection_set(std::vector<SelectionSet>& sets, const std::string& name, std::vector<std::string> bones);
// SS-2: the skeleton nodes of the set's bones, in its order; names this skeleton lacks are skipped.
std::vector<int> recall_selection_set(const Skeleton& skel, const SelectionSet& set);
// SS-3: adds bones to the set (add) or takes them out. Returns how many changed.
int edit_selection_set(SelectionSet& set, const std::vector<std::string>& bones, bool add);

// [{name, bones: [...]}], as the project and the library file store them.
Json selection_sets_to_json(const std::vector<SelectionSet>& sets);
bool selection_sets_from_json(const Json& v, std::vector<SelectionSet>& out, std::string& err);

// The library file: {"format": "vats-selection-sets", "sets": [...]}. On failure load returns false, sets err and
// leaves out unchanged.
bool load_selection_sets(std::string_view text, std::vector<SelectionSet>& out, std::string& err);
std::string save_selection_sets(const std::vector<SelectionSet>& sets);

}  // namespace vats
