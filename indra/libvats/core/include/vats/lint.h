// Viewport Avatar Toolset - the Animation Check: problems SL will show that the editor does not.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 9 (CK). Every finding names the bones and frames it is about, and its fix (when it has
// one) is an existing operation applied to the clip: the caller wraps it in one undo step.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "vats/anim_convert.h"

namespace vats {

enum class LintSeverity { Error, Warning, Info };  // Error: SL refuses or breaks it; Info: may be meant

struct LintFix {
    std::string label;                 // the Fix button's tooltip, e.g. "Make Loop Seamless"
    std::function<void(Clip&)> apply;  // empty = no automatic fix
};

struct LintFinding {
    std::string rule;  // one of lint_rules()' ids
    LintSeverity severity = LintSeverity::Warning;
    std::vector<std::string> bones;  // track names
    std::vector<int> frames;         // sorted
    std::string message;
    LintFix fix;
};

struct LintRule {
    const char* id;
    const char* title;  // for the per-rule switches
};
// Every rule, for the per-rule switches.
const std::vector<LintRule>& lint_rules();

// Checks the clip as export_anim would write it with opt. The clip's own export settings ("reduce", "leave_static")
// win over opt's, so a fix that changes them clears its finding. Rules named in off are skipped. The fixes may keep
// a pointer to skel: apply them before it goes. mesh_body: the shape of the mesh body shown, if any; the self-contact
// rule then measures on its proportions and adds its collision volumes.
std::vector<LintFinding> lint_clip(const Skeleton& skel, const Clip& clip, const AnimExportOptions& opt,
                                   const std::vector<std::string>& off = {}, const Shape* mesh_body = nullptr);

}  // namespace vats
