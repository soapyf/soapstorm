// Viewport Avatar Toolset - BVH export and import.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Written so the SL viewer's LLBVHLoader reads it the same way VATs does. Spec: docs/spec/03 3.2.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "vats/clip.h"
#include "vats/skeleton.h"

namespace vats {

struct BvhExportOptions {
    bool all_bones = false;        // write every joint, not only animated ones and their ancestors
    bool joint_positions = false;  // 6 channels for non-hip joints with position keys (off by default)
    const Shape* shape = nullptr;  // body IK and pins are baked against (IO-13); null = no shape
    ExternalTarget external;       // cross-actor pin targets (GR-4); empty = those pins are skipped
    const Shape* positions = nullptr;  // as AnimExportOptions::positions, for joint_positions channels
};

struct BvhExportResult {
    std::string text;
    std::vector<std::string> lost;  // what this BVH cannot carry (see spec IO-29)
    int static_positions = 0;       // joints below the hip whose position channels moved nothing (IO-11a)
};

// IK and pins are baked into the rotations (IO-29); pin via bones count as having position keys.
BvhExportResult export_bvh(const Skeleton& skel, const Clip& clip, const BvhExportOptions& opt = {});

struct BvhImportResult {
    bool ok = false;
    bool foreign_rig = false;  // a valid BVH whose root is not mPelvis: another rig, for the retarget importer
    std::string error;
    Clip clip;
    std::vector<std::string> report;
};

// IO-35: every frame is keyed; tolerances above 0 then drop keys the way export does (IO-14).
struct BvhImportOptions {
    double reduce_rot_deg = 0;
    double reduce_pos_m = 0;
};

BvhImportResult import_bvh(const Skeleton& skel, std::string_view text, const BvhImportOptions& opt = {});

// BVH units are inches. VATs reads and writes with the exact 0.0254 m so its own files round-trip
// as text; the viewer reads with 0.02540005, 2 ppm (2 micrometres per metre) longer.
constexpr double kInchesPerMetre = 1.0 / 0.0254;
constexpr double kMetresPerInch = 0.0254;

}  // namespace vats
