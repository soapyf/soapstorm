// Viewport Avatar Toolset - overlap / follow-through: each bone down a chain moves a little after its parent.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 11 (OV-1..OV-3). A keyframe tool, not a simulation: bone i of the chain plays
// its own rotation curve i * shift frames late, scaled about its average pose by falloff^i, and the result
// is baked to keys. The first bone is left as it is.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/rig.h"

namespace vats {

struct OverlapSettings {
    double shift = 1.0;    // frames of delay per bone down the chain (the window offers 0.5-3)
    double falloff = 1.0;  // amplitude multiplier per bone: bone i swings falloff^i as far
    bool settle = false;   // no loop: ease back to the unshifted pose over the last 2 * delay frames
};

// Why overlap cannot run on chain (root first, each bone a child of the one before), or "" when it can:
// fewer than two bones, or a bone in a limb that uses IK.
std::string overlap_refusal(const Clip& clip, const Rig& rig, const std::vector<int>& chain);

// OV-1..OV-2: rebakes the rotation keys of chain[1..] (bones with rotation keys only). With Loop on,
// frames in the loop range read their delayed pose round the loop, so the seam stays where it was.
// Check overlap_refusal first; one call is one undo step for the caller.
void apply_overlap(Clip& clip, const Skeleton& skel, const std::vector<int>& chain, const OverlapSettings& s);

}  // namespace vats
