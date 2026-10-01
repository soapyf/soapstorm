// Viewport Avatar Toolset - the deformer tool: position-key animations that reshape the avatar, without the sink.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/09 section 0l. Every SL viewer keeps an animated joint's position after the animation stops, and
// takes the avatar's height (sl_body_size) from the torso, chest, neck, head, skull and left-leg positions whenever an
// animation starts or stops: a deformer that makes that height taller sinks the wearer by half the growth. These are
// the ways out: end at rest, counter-key mSkull, or a paired undeformer.
#pragma once

#include <string>
#include <vector>

#include "vats/anim_convert.h"

namespace vats {

constexpr float kUndeformSeconds = 0.5f;  // the undeformer's length: long enough to play a few frames anywhere

// The bones SL's height counts (LLAvatarAppearance::computeBodySize); the right leg, the spine and the hip do not.
bool counts_for_sl_height(const std::string& bone);

// End at rest: every bone other than the hip whose position the clip keys and that is off rest on the last frame gets
// a key at its rest position one frame after it; the clip grows by that frame. The curves up to the old last frame keep
// their shape. Returns the bones keyed. A looping clip stops wherever it is, so this helps only one played to its end.
std::vector<std::string> end_at_rest(Clip& clip, const Skeleton& skel);

struct HoldResult {
    double head_scale = 1;     // the mHead scale the counter-keys use
    bool head_scale_known = false;  // from the bake shape; false = 1, assumed
    bool combined = false;     // mSkull already had position keys: the counter-move was added to them
    bool keyed = false;        // mSkull got counter-keys (false: nothing changes height, or refused)
    std::string refused;       // non-empty: why nothing was done
    double shortfall = 0;      // m of height change left where mSkull hit the 5 m limit (0 = none)
    std::vector<int> short_frames;
};

// Hold without sinking: keys mSkull's Z on every frame (reduced, linear) so that sl_body_size's height stays at rest
// height, the skull counting √2 × mHead's scale (shape's, else 1). A taller body lowers the skull, a shorter
// one raises it. mSkull's own position keys are kept and the counter-move added to them. Refuses when mSkull's own keys
// are all that change the height (countering them would undo them). shape: the body (deformer_body), may be null.
HoldResult hold_without_sinking(Clip& clip, const Skeleton& skel, const Shape* shape);

// The body whose height SL takes, as the export knows it: the worn avatar's joints when the export writes positions
// from them (Your avatar, with its scales), else the bake shape; null = the skeleton's defaults.
inline const Shape* deformer_body(const AnimExportOptions& opt) { return opt.positions ? opt.positions : opt.shape; }

// The clip export_anim writes when opt asks for end_at_rest or hold_without_sinking: those applied to a copy. Their
// notes (a refusal, the 5 m limit) are added to notes.
Clip with_deformer_options(const Skeleton& skel, const Clip& clip, const AnimExportOptions& opt,
                           std::vector<std::string>* notes = nullptr);

// The undeformer: the deformer's position-keyed joints (the hip aside) at their rest positions, as its export writes
// rest (the skeleton's, plus positions' offsets when given), at the same priorities, kUndeformSeconds long, no eases.
// Playing it puts the joints back and SL's height with them when it stops.
AnimFile make_undeformer(const Skeleton& skel, const AnimFile& deformer, const Shape* positions = nullptr);

// "<stem>_undeform": the undeformer's name for a deformer called stem (no extension).
inline std::string undeformer_name(const std::string& stem) { return stem + "_undeform"; }

}  // namespace vats
