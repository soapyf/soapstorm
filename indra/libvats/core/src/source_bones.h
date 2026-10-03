// Viewport Avatar Toolset - the file's own armature as the mesh readers report it (core-private).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <vector>

#include "vats/dae.h"

namespace vats {

// Places every bone no skin binds (an IK target, an _end tip) on its parent's bind, as the node tree has it relative
// to that parent. scene: each bone's node world, in SourceBone::bind's space. Parents come first.
inline void place_unskinned_bones(std::vector<SourceBone>& bones, const std::vector<Xform>& scene) {
    for (size_t i = 0; i < bones.size() && i < scene.size(); ++i) {
        if (bones[i].skinned) continue;
        const int p = bones[i].parent;
        bones[i].bind = p >= 0 ? bones[p].bind * (scene[p].inverse() * scene[i]) : scene[i];
    }
}

}  // namespace vats
