// Viewport Avatar Toolset - the .anim as Second Life plays it, and what its bytes go on.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 10 (SP: preview as SL plays it; UM: upload meter). The app and the viewer share one
// in-memory export (export_anim + write_anim + parse_anim) for both; this is the logic they run on it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vats/anim_convert.h"
#include "vats/anim_file.h"
#include "vats/clip.h"
#include "vats/rig.h"
#include "vats/skeleton.h"

namespace vats {

// SP-1: the pose SL plays from a .anim at a time in seconds, as LLKeyframeMotion interpolates it: rotation keys
// by nlerp, position keys linearly, the first and last key held outside them. Joints without a record stay at
// rest; records of joints not in the skeleton are ignored. positions: the joint positions the export wrote
// non-pelvis position keys from (AnimExportOptions::positions), so they come back as the same offsets.
// ponytail: ease in/out and priorities are not applied (they blend with other animations, of which there are none).
Pose anim_pose(const Skeleton& skel, const AnimFile& f, double seconds, const Shape* positions = nullptr);

// SP-2: how far SL's playback of a bone departs from the clip's own, in the world (avatar space).
struct BoneDeviation {
    int node = -1;
    double mm = 0, deg = 0;         // the largest distance and rotation angle over every whole frame
    int frame_mm = 0, frame_deg = 0;  // where each happens
};
// Every node except the collision volumes, compared on every whole frame 0..end_frame: the clip through the full
// evaluation (IK and pins, rig.external included) against anim_pose, both on shape. Worst first: by mm, then degrees.
// clip must be the clip f was exported from (mirrored when the export was).
std::vector<BoneDeviation> anim_deviation(const Rig& rig, const Clip& clip, const AnimFile& f, const Shape* shape,
                                          const Shape* positions = nullptr);

// UM-1/UM-2: where a .anim's bytes go.
struct AnimCost {
    std::size_t total = 0;  // write_anim(f).size()
    std::size_t header = 0;  // the fixed header, emote and constraints
    std::size_t records = 0;  // per-joint overhead: name, priority, key counts
    std::size_t rot = 0, pos = 0;  // rotation and position keys
    struct Part {
        std::string name;  // "Face", "Left Hand", "Torso", "Attachment points", "Other" (joints not in the skeleton)
        std::size_t bytes = 0, rot = 0, pos = 0;  // bytes = records + rot + pos of its joints
        int joints = 0;
    };
    std::vector<Part> parts;  // most bytes first
};
AnimCost anim_cost(const Skeleton& skel, const AnimFile& f);

// UM-3: "Fit to 250 KB". Raises the key-reduction tolerances step by step (x1.5 each, rotation and position
// together, starting from opt's and at least 0.05 deg / 0.5 mm, capped at 5 deg / 50 mm, the Reduce keys fields'
// range) until the export is under kAnimMaxUploadBytes. The clip's own keys anchor the reduction (export_anim keeps
// them), so a clip keyed on most frames may not fit at any tolerance. With world reduction on (opt.reduce_world_m
// > 0, 08 WR-5) only that tolerance is raised, the same way, from at least 0.5 mm up to 50 mm.
struct BudgetFit {
    bool fits = false;
    bool too_long = false;  // over 60 s: no tolerance helps (split the clip)
    double rot_deg = 0, pos_m = 0;  // the tolerances of the last export tried
    double world_m = 0;             // its world tolerance (0 = per-bone reduction)
    std::size_t bytes = 0;          // its size
    int steps = 0;                  // tolerance steps taken (0 = it fitted as it was)
    double max_mm = 0, max_deg = 0;  // its worst world-space error (anim_deviation), when it fits
    std::string worst_mm, worst_deg;  // the bones they are on
};
// ponytail: every step re-samples the clip (export_anim); sample once and reduce per step if big clips get slow.
BudgetFit fit_anim_budget(const Rig& rig, const Clip& clip, AnimExportOptions opt);

// SP-3: a hash of everything that goes into the clip's .anim (timing, curves, pins, priorities, orphans,
// constraints, the export settings, mirror_export) and of the shapes it bakes on, for caches. Props, audio and
// unbaked dynamics or ragdoll settings are left out: they never reach the file.
std::uint64_t anim_hash(const Clip& clip, const Shape* shape = nullptr, const Shape* positions = nullptr);

}  // namespace vats
