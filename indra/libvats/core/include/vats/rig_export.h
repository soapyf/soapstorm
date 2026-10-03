// Viewport Avatar Toolset - the SL rig export: an uploadable rigged COLLADA file, checked first.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 6 (RG-4, RG-5, RG-7). The writer follows the SL mesh uploader's own reader
// (the viewer's lldaeloader.cpp) rule for rule:
// - Joint nodes are pure translations, parent-relative, in SL's frames (every SL joint rests at identity
//   rotation), metres, Z up. The uploader reads only those translations, by name, and applies each as a joint
//   position when it is over 0.1 mm from the default and the joint is in the skin's joint list.
// - The joint list holds every weighted joint and every moved one (with no weights), never mRoot, at most 110,
//   SL names only. Inverse binds are the inverse of each joint's bind transform; a collision volume's carries
//   SL's volume rotation and scale, which its in-world transform has.
// - At most 4 weights per vertex, normalised; bind_shape_matrix is the identity (the vertices are written in
//   SL space, at the bind pose, as VATs holds them).
// Content boundary (RG-7): only meshes loaded from the user's files reach this; nothing from the world does.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "vats/dae.h"

namespace vats {

struct RigExportOptions {
    bool joint_positions = true;  // write each bound joint where the file bound it (upload with "Include joint positions")
    bool bind_pose_only = false;  // the joints at SL's defaults: the inverse binds still carry the pose the mesh was
                                  // modelled in (an A-pose), so it skins onto SL's skeleton with no joint positions
    bool pelvis_offset = false;   // also write mPelvis's position: it changes the avatar's height and hover
    bool rest_pose = false;       // RG-9: an upright humanoid modelled in another pose (an A-pose) and bound as it stands
                                  // (a mapped rig): its joint positions are written in SL's rest pose (a T-pose), its
                                  // bones at their own lengths, and the inverse binds carry the pose it was modelled in
    bool shape_proof = false;     // RG-10: every listed joint and every joint above one gets a position (0.11 mm off
                                  // the default where it sits on it), so "Lock scale if joint position defined" locks
                                  // them all against the wearer's shape sliders
};


// One mesh to write: the part as imported, with its import report's unknown joints for the check.
struct RigPart {
    const DaeModel* model = nullptr;
    std::string name;  // the mesh's name in the file (a file stem); "" = "mesh<n>"
    std::vector<std::string> unmapped_joints;  // DaeReport::unmapped_joints
    double measured_scale = 0, scale = 1;      // DaeReport::measured_scale and ::scale
};

// Spec 08 RG-8: where Second Life will stand the exported mesh, from the viewer's own formulas
// (LLAvatarAppearance::computeBodySize, LLVOAvatar::updateCharacter). The region keeps the agent at the wearer's shape
// size (a viewer in a server-baked region never sends its own), while the viewer draws the root
// 0.5 * body size - pelvis_to_foot below the agent, from the live joint positions, the mesh's joint positions
// included once they land. So the mesh stands half the height change lower, plus the change in pelvis_to_foot,
// plus the uploader's Z offset; mPelvis's own position does not count (the pelvis_fix motion holds it at zero).
// For the default shape; the ground is where SL's default body has its soles (z = 0 in VATs).
struct RigHeight {
    bool valid = false;        // some part is rigged
    double sole = 0;           // the file's ground (z 0 where it was bound) over SL's, metres: > 0 floats, < 0 sinks
    double body = 0;           // SL's body size with the file's joint positions (sl_body_size().height)
    double default_body = 0;   // and with SL's defaults: what the region keeps
    int skull_part = -1;       // the part an mSkull counter position goes in (-1: none can take it, mSkull is weighted)
};
RigHeight rig_in_world_height(const Skeleton& skel, const std::vector<RigPart>& parts, const RigExportOptions& opt);

// The joint positions the file would carry, per SK-40 index (joints, mRoot, collision volumes).
struct RigJoint {
    int node = -1;         // SK-40 index
    std::string name;      // SL name
    bool bound = false;    // the part bound it (DaeModel::bound)
    bool weighted = false; // some vertex is weighted to it
    bool listed = false;   // in the skin's joint list: weighted, or moved by more than the threshold
    Vec3 local;            // the translation written on its joint node (parent-relative, metres)
    Vec3 offset;           // local minus SL's default local; the joint position SL would apply
    double offset_mm = 0;  // |offset| in mm
    bool uploads = false;  // listed and offset_mm > 0.1: SL applies it
    bool noise = false;    // 0.1 mm < offset_mm <= 1 mm: a false offset from float export, most likely
    bool nudged = false;   // moved 0.11 mm off its default by shape-proof (RigExportOptions::shape_proof), not noise
};

// The positions one part writes with opt, as the uploader will read them. Every SK-40 index but mRoot is
// returned (defaults included), in skeleton order then volumes.
std::vector<RigJoint> rig_joints(const Skeleton& skel, const DaeModel& model, const RigExportOptions& opt);

// Moves a bound joint's bind position so its written translation is SL's default (the "false offset" fix). Its
// bind rotation and its vertices are untouched: the mesh is skinned from where it is, as SL would show it
// without the offset. False when node is not bound (nothing to snap). opt: the export's, for the frames it writes in.
bool snap_joint_to_default(const Skeleton& skel, DaeModel& model, int node, const RigExportOptions& opt = {});
// Moves a joint's bind position so the export writes local (parent-relative) for it, binding it when it was not.
// Its vertices are untouched. False for mRoot or an index out of range.
bool set_written_local(const Skeleton& skel, DaeModel& model, int node, const Vec3& local, const RigExportOptions& opt = {});

enum class RigSeverity { Error, Warning, Info };  // Error: the uploader refuses or breaks it; export is refused

struct RigFinding {
    std::string rule;  // rig_export_rules()' id
    RigSeverity severity = RigSeverity::Warning;
    int part = -1;                   // index into the parts, or -1 for the whole file
    std::vector<std::string> joints; // SL names it is about, when any
    std::string message;
    std::string fix_label;                             // "" = no automatic fix
    std::function<void(DaeModel&)> fix;                // applied to parts[part]'s model (the caller drops caches)
};

struct RigRule {
    const char* id;
    const char* title;
};
const std::vector<RigRule>& rig_export_rules();

// Checks what write_rig_dae would write. Errors refuse the export; the writer runs the check itself.
std::vector<RigFinding> check_rig_export(const Skeleton& skel, const std::vector<RigPart>& parts, const RigExportOptions& opt);
inline bool rig_export_refused(const std::vector<RigFinding>& f) {
    for (const RigFinding& x : f)
        if (x.severity == RigSeverity::Error) return true;
    return false;
}

// The COLLADA text. False with err (the first Error finding) when the check refuses it.
bool write_rig_dae(const Skeleton& skel, const std::vector<RigPart>& parts, const RigExportOptions& opt, std::string& out,
                   std::string& err);

// Spec 08 RG-5 test harness: the joint positions the SL uploader reads from a COLLADA text, by joint name, exactly as
// lldaeloader does (the translation of each JOINT node: <translate sid="translate">, else <translate sid="location">,
// else the first <translate>, else a <matrix>'s translation; no unit or up-axis applied). Empty on a broken file.
std::vector<std::pair<std::string, Vec3>> uploader_joint_translations(std::string_view dae_text);

}  // namespace vats
