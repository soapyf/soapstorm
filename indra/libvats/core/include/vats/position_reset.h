// Viewport Avatar Toolset - joint position reset and unended position detection.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "vats/anim_file.h"
#include "vats/clip.h"
#include "vats/json.h"
#include "vats/shape.h"
#include "vats/skeleton.h"

namespace vats {

// The smallest position change a .anim can store (its 16-bit range of +-5 m): an offset smaller than this is
// written as rest, or as the code next to it.
constexpr double kAnimPositionStep = 2.0 * kAnimMaxOffset / 65535;

// Returns all non-pelvis joints in skel whose position is keyed in clip and does not end at rest
// (curve offset at the clip's last frame at least kAnimPositionStep).
std::vector<std::string> joints_ending_moved_by_position(const Skeleton& skel, const Clip& clip);

// Returns all non-pelvis joints whose position is keyed in clip and leaves rest on any whole frame (by more than
// kAnimPositionStep): where a looping animation can be stopped.
std::vector<std::string> joints_moved_by_position(const Skeleton& skel, const Clip& clip);

// Whether the export writes position keys for track `name` (IO-11a): its position channels move it more than tol
// metres from rest on some whole frame. Channels keyed at rest are left out of the file, so they reset nothing.
bool writes_position(const Clip& clip, const std::string& name, double tol);

// The position tolerance IO-11a uses with these export settings: "reduce"'s metres, else the default.
double position_tolerance(const Json& export_settings);

// Returns all non-pelvis joints that this clip rotates (has rot_* curves or is turned by IK).
std::vector<std::string> clip_rotated_joints(const Skeleton& skel, const Clip& clip);

// Returns all non-pelvis joints that are moved by position in any of the project's other clips (as their exports
// write them: channels keyed at rest move nothing).
std::vector<std::string> other_clips_position_joints(const Skeleton& skel, const std::vector<const Clip*>& other_clips);

// Rest position of joint `node` on `skel` taking into account joint offsets from `shape` (the bake shape or
// export_positions; IO-11). Clamped to [-kAnimMaxOffset, kAnimMaxOffset].
Vec3 rest_joint_position(const Skeleton& skel, int node, const Shape* shape = nullptr);

// Two position keys (at normalized times 0 and 65535) holding joint `node` at rest_joint_position.
std::vector<std::array<std::uint16_t, 4>> rest_position_keys(const Skeleton& skel, int node, const Shape* shape = nullptr);

// Creates an AnimJoint with rest position keys at time 0 and 65535, with empty rotation.
AnimJoint make_rest_joint(const Skeleton& skel, const std::string& name, int priority, const Shape* shape = nullptr);

// Resolves the list of joints to reset positions for based on export_settings:
// - "rotated": joints this clip rotates (default)
// - "other_clips": joints moved by position in other clips
// - "pick": explicitly picked joints in "reset_positions_joints"
// Joints whose position the clip writes itself (writes_position) are left out: their own keys reset them.
// Pass the clip as it exports (mirrored_clip with Export mirrored, which mirrors the picked joints too).
std::vector<std::string> resolve_reset_position_joints(const Skeleton& skel, const Clip& clip,
                                                       const std::vector<const Clip*>& other_clips,
                                                       const Json& export_settings);

}  // namespace vats
