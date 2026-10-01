// Viewport Avatar Toolset - motion quality numbers: shake, foot slide, hip drift, seam, size.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 15 (MQ). Each number is what one clean-up tool lowers: shake (Filter Curves),
// foot slide (Clean Up Foot Sliding), hip drift (Remove Hip Travel), the seam jump (Make Loop Seamless), bytes
// (Fit to 250 KB) and keys (Simplify Curves). Measured over the whole clip; nothing is changed.
#pragma once

#include <cstddef>
#include <vector>

#include "vats/anim_convert.h"
#include "vats/curve_filter.h"
#include "vats/rig.h"

namespace vats {

struct MotionQuality {
    std::vector<JointShake> shake;  // per track with rotation or position curves (shake_scores, frames 0..end)
    double jerk = 0;                // RMS over those tracks of their rotation shake, degrees/s^3
    double foot_slide = 0;          // metres the ankles move along the ground during foot contacts, summed
    int contacts = 0;               // foot contacts found (find_foot_contacts, default settings)
    double hip_drift = 0;           // metres mPelvis travels along the ground per loop (remove_travel on a copy)
    bool loops = false;             // the clip loops: the seam numbers mean something
    double seam_deg = 0, seam_mm = 0;  // the largest rotation and position jump at the loop seam
    std::size_t bytes = 0;          // the .anim export_anim makes with opt
    int keys = 0;                   // keys in the clip's curves
};

MotionQuality measure_quality(const Rig& rig, const Clip& clip, const AnimExportOptions& opt);

}  // namespace vats
