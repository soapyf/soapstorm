// Viewport Avatar Toolset - an animation clip.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/02 sections 1 and 2.1-2.2.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vats/fcurve.h"
#include "vats/prop.h"
#include "vats/skeleton.h"

namespace vats {

// Channel names of a bone track.
inline constexpr const char* kRotChannels[3] = {"rot_x", "rot_y", "rot_z"};
inline constexpr const char* kPosChannels[3] = {"pos_x", "pos_y", "pos_z"};

using Track = std::map<std::string, FCurve>;  // channel -> curve

// An SL .anim constraint, kept byte for byte so it survives import and re-export.
using AnimConstraint = std::array<std::uint8_t, 86>;

// An imported .anim joint that is not in the skeleton; written back unchanged on export.
struct OrphanJoint {
    std::string name;
    std::int32_t priority = -1;
    std::vector<std::pair<double, Quat>> rot;  // (seconds, local rotation)
    std::vector<std::pair<double, Vec3>> pos;  // (seconds, value as stored in the file)

    bool operator==(const OrphanJoint&) const = default;
};

// A pin (spec 02 AM-80): holds `joint` still in the world, or relative to the `target` bone, over
// frames from..to by moving `via` (the joint itself, or its parent for some attachment points).
struct Pin {
    std::string joint, via, target;  // target empty = held in the world (avatar space)
    int from = 0, to = -1;           // to = -1: held to the end
    Vec3 pos;                        // held transform, in avatar space or the target bone's space
    Quat rot;
    int start_key = -1, release_key = -1;  // frames of the helper keys VATs made, -1 = none
    Json extra = Json::object();           // unknown anchor fields, written back (IO-43)
    std::string target_actor;  // GR-4: target is a bone of this other actor ("" = own skeleton)

    bool active(double frame) const { return from <= frame && (to < 0 || frame <= to); }
    bool operator==(const Pin&) const = default;
};

// Cross-actor pins (spec 08 GR-4): where the pin's target bone of another actor is, in the pinned
// actor's space, at a frame. False when it cannot be resolved; the pin is then skipped.
using ExternalTarget = std::function<bool(const Pin& pin, double frame, Xform& out)>;

// A dynamic chain (spec 08 DY-1): simulated behind its animated parents and baked to keys. The
// simulation lives in dynamics.h; the settings live here so they save with the project and undo covers them.
struct DynChain {
    std::string root;         // first simulated node; its parent drives it
    int length = 1;           // joints down the first-child path (a collision volume is always 1)
    double stiffness = 0.25;  // 0..1 per sub-step: pull back towards the animated pose
    double damping = 0.2;     // 0..1 per sub-step: loss of motion relative to the animated pose
    double drag = 0.02;       // 0..1 per sub-step: loss of world-space motion (air)
    double gravity = 0.0;     // multiples of 9.81 m/s^2, downwards
    double radius = 0.02;     // metres kept outside the collision volumes
    bool baked = false;       // the chain's tracks hold a bake; source has what they held before
    std::map<std::string, Track> source;  // pre-bake tracks (absent = the track did not exist)
    Json extra = Json::object();          // unknown fields, written back

    bool operator==(const DynChain&) const = default;
};

// Ragdoll settings (spec 08 RD): which joints fall limp over which frames. The solver lives in ragdoll.h.
struct Ragdoll {
    bool whole_body = true;
    std::vector<std::string> bones;  // selected-bones mode: these joints and every ragdoll joint below them
    int start = 0, frames = 60;      // RD-1: simulated range [start, start + frames]
    int blend_in = 3, blend_out = 0;  // RD-3, frames
    double gravity = 1.0;            // multiples of 9.81 m/s^2
    double stiffness = 0.0;          // 0..1 per sub-step: joint drive towards the animated pose (0 = limp)
    double friction = 0.6;           // 0..1: ground and prop friction
    bool baked = false;
    std::map<std::string, Track> source;  // pre-bake tracks (absent = the track did not exist)
    Json extra = Json::object();          // unknown fields, written back

    bool operator==(const Ragdoll&) const = default;
};

// Two-bone IK frame (spec 02 section 3.7). VATs lines the mid joint up with the pole, so Switch to IK
// never twists the limb; Literal is section 3.7 as written, which poses .hxanim projects as the reference
// app did.
enum class IkSolve { VATs, Literal };

struct Clip {
    int fps = 30;
    int end_frame = 30;
    bool loop = false;
    int loop_in = 0, loop_out = 30;
    int priority = 3;
    double ease_in = 0.8, ease_out = 0.8;
    int hand_pose = 1;
    std::string emote;
    IkSolve ik_solve = IkSolve::VATs;

    std::map<std::string, Track> curves;       // track name -> channels
    std::map<std::string, int> joint_priority;  // per-joint overrides (-1 = use the clip's)
    std::vector<AnimConstraint> constraints;
    std::vector<OrphanJoint> orphans;
    std::vector<Pin> pins;  // in application order
    std::vector<Prop> props;  // meshes placed in the scene; paths absolute while in memory
    std::vector<DynChain> dynamics;  // spec 08 DY-1
    std::optional<Ragdoll> ragdoll;  // spec 08 RD
    // Export choices live on the clip so undo covers them (UI-28); saved as the project's "export"
    // and "mirror_export" keys.
    bool mirror_export = false;
    Json export_settings = Json::object();  // 03 section 3.4.4, plus shape and reduce

    bool has_channels(const std::string& track, const char* const (&channels)[3]) const;
    bool operator==(const Clip&) const = default;
};

// The pose the curves describe at a frame (FK only: no IK, no pins).
Pose evaluate_curves(const Skeleton& skel, const Clip& clip, double frame);

}  // namespace vats
