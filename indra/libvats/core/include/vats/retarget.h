// Viewport Avatar Toolset - retargeting humanoid animations from other skeletons onto SL's.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/07. Every reader (BVH now, glTF, later FBX) fills a SourceAnim; mapping, rest
// correction and fitting to SL's limits work on that alone.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "vats/clip.h"
#include "vats/skeleton.h"

namespace vats {

struct SourceJoint {
    std::string name;
    int parent = -1;  // index into SourceAnim::joints, parents before children
    // Rest (bind) local transform in the source's own axes and units.
    Vec3 offset;
    Quat rot;
    double scale = 1;  // uniform; glTF armatures often carry 0.01
};

struct SourceAnim {
    std::vector<SourceJoint> joints;
    double fps = 30;
    // Per frame, per joint: local rotation and translation (source axes/units). Frame f joint j is
    // rot[f][j]; translation only differs from the rest offset where the file animates it.
    std::vector<std::vector<Quat>> rot;
    std::vector<std::vector<Vec3>> pos;
    bool has_bind = true;  // the rest pose comes from the file (BVH offsets, glTF bind); else frame 0
    std::vector<std::string> notes;  // reader remarks for the report

    int frames() const { return static_cast<int>(rot.size()); }
};

// BVH with any skeleton: zero rotations are the rest pose (RT-2.1).
bool read_bvh_source(std::string_view text, SourceAnim& out, std::string& err);
// glTF 2.0: .gltf text (external buffers are read from dir) or .glb bytes (RT-2.2). The first
// animation is sampled at 30 fps. VRM humanoid bone names replace node names when present.
bool read_gltf_source(const std::vector<std::uint8_t>& bytes, const std::string& dir, SourceAnim& out,
                      std::string& err);

// A rig family's name table (RT-3), loaded from data/retarget/*.json:
// {"name": "Mixamo", "bones": {"mPelvis": ["Hips"], "mTorso": ["Spine"], ...}}
struct RigTable {
    std::string name;
    std::string hint;  // optional text found in this family's joint names ("mixamorig"); breaks ties
    std::map<std::string, std::vector<std::string>> bones;  // SL joint -> candidate source names
};
bool parse_rig_table(std::string_view json, RigTable& out, std::string& err);

// SL joint -> source joint index.
using BoneMap = std::map<std::string, int>;

// Names match case-insensitively after dropping any "prefix:" (mixamorig:Hips); a table name starting with "*" matches
// any prefix ("* L Thigh"). Returns how many SL joints were mapped.
int apply_rig_table(const RigTable& table, const SourceAnim& src, BoneMap& out);
// The table that maps the most joints (its hint breaking ties), or -1.
int best_rig_table(const std::vector<RigTable>& tables, const SourceAnim& src, BoneMap& out);
// RT-5: pelvis, a spine joint, head, and the upper and lower bones of all four limbs. Missing
// SL joints are listed in missing.
bool map_is_usable(const BoneMap& map, std::vector<std::string>* missing = nullptr);
// The SL joints a mapping can fill, in skeleton order (for the manual picker).
std::vector<std::string> retarget_joints(const Skeleton& skel);

struct RetargetOptions {
    bool rest_from_frame0 = false;  // RT-7: override the file's bind pose
    int fps = 0;                    // 0 = the source rate, rounded, clamped to 1..60
    const Shape* shape = nullptr;   // SL body the rest pose is measured on
};

struct RetargetResult {
    Clip clip;
    std::vector<std::string> report;  // unmapped joints, axes chosen, hip scale
};

// RT-6..RT-8: every frame keyed (reduction happens when fitting and exporting).
RetargetResult retarget(const Skeleton& skel, const SourceAnim& src, const BoneMap& map, const RetargetOptions& opt = {});

// RT-10/RT-11: degrade until the clip exports under SL's limits. Each allow_* switches one kind of
// step on, so the user can forbid a trade and fit again.
struct FitOptions {
    bool allow_tolerance = true;
    bool allow_fps = true;
    bool allow_drop_face = true;
    bool allow_drop_fingers = true;
    bool allow_drop_toes = true;
    const Shape* shape = nullptr;
};
struct FitReport {
    bool fits = false;
    bool too_long = false;  // over 60 s: only trimming or splitting helps
    size_t bytes_before = 0, bytes_after = 0;
    double rot_tol_deg = 0, pos_tol_m = 0;
    int fps = 0;
    std::vector<std::string> steps;    // what was done, in order
    std::vector<std::string> dropped;  // joints removed
};
// The clip's keys are reduced (every step starts again from the full-rate clip). On return
// clip.export_settings["reduce"] holds the tolerances used, so export matches.
// Key reduction on the clip itself (export keeps every existing key as an anchor).
void reduce_clip_keys(Clip& clip, double rot_deg, double pos_m);
FitReport fit_to_limits(const Skeleton& skel, Clip& clip, const FitOptions& opt = {});

// RT-10.4: frames a..b as a clip starting at frame 0. Curves get a key at each cut that keeps
// their shape; pins are clipped to the range.
Clip slice_clip(const Clip& clip, int a, int b);
// RT-10.4: consecutive parts (each shares its first frame with the previous part's last), each
// under 60 s and fitted with fit_to_limits. Empty when even 2-second parts will not fit.
std::vector<Clip> split_to_fit(const Skeleton& skel, const Clip& clip, const FitOptions& opt = {},
                               std::vector<FitReport>* reports = nullptr);

}  // namespace vats
