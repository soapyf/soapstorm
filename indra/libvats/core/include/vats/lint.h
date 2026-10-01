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

// Where the finding's Go to Frame button goes from frame `here`: the first frame of the next run of consecutive
// frames after the run holding `here`, wrapping to the first run. A held pose flagged on every frame is one run, so
// it goes to that run's first frame, not to here + 1. -1 when the finding has no frames.
int lint_goto_frame(const std::vector<int>& frames, int here);
// How many runs of consecutive frames there are (the stops Go to Frame steps through).
int lint_frame_runs(const std::vector<int>& frames);

// Another actor of the scene (08 GR), for the cross-actor contact rule: its name, and its joints' globals on every
// whole frame the check samples (0 to the clip's last), already in the checked actor's space (its placement applied).
struct LintPartner {
    std::string name;
    std::vector<std::vector<Xform>> frames;
};

// Checks the clip as export_anim would write it with opt. The clip's own export settings ("reduce", "leave_static")
// win over opt's, so a fix that changes them clears its finding. Rules named in off are skipped. The fixes may keep
// a pointer to skel: apply them before it goes. mesh_body: the shape of the mesh body shown, if any; the self-contact
// rule then measures on its proportions and adds its collision volumes. ao_state: the clip's AO state (ClipSlot), ""
// for none; an AO plays its own state's animations in place of each other, so the priority rule skips them. partners:
// the scene's other actors; the actor_contact rule checks this body against each of theirs (a hug, a handshake).
std::vector<LintFinding> lint_clip(const Skeleton& skel, const Clip& clip, const AnimExportOptions& opt,
                                   const std::vector<std::string>& off = {}, const Shape* mesh_body = nullptr,
                                   const std::string& ao_state = {}, const std::vector<LintPartner>& partners = {});

}  // namespace vats
