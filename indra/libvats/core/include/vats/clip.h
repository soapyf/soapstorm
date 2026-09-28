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
#include "vats/reference.h"
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

// A procedural idle layer (spec 08 IL): a breath or a gradient-noise sway over a bone set, baked to keys.
// The motion lives in idle.h; the settings live here, like DynChain, so they save and undo with the clip.
struct IdleLayer {
    std::string kind = "sway";       // "breath" (mTorso rises, the other bones pitch) or "sway" (noise, 3 axes)
    double amplitude = 1.0;          // degrees (breath: also millimetres of mTorso rise)
    double period = 5.0;             // seconds, snapped so a whole number fits the loop (IL-2)
    int seed = 1;                    // sway: which noise
    std::vector<std::string> bones;  // face and eye bones are never moved
    bool baked = false;
    std::map<std::string, Track> source;  // pre-bake tracks (absent = the track did not exist)
    Json extra = Json::object();          // unknown fields, written back

    bool operator==(const IdleLayer&) const = default;
};

// The project's audio track (spec 08 AU): played with the animation, never written into the .anim.
// `offset` is in timeline seconds (frame / fps); beats are in the audio's own seconds, so they travel with
// the music when it is slid along the timeline.
struct AudioTrack {
    std::string path;          // as stored: relative to the project (like props) or absolute
    double offset = 0;         // where the audio's start sits on the timeline; later = positive
    double volume = 1;         // 0..2
    double bpm = 0;            // beat grid, 0 = none
    double beat_offset = 0;    // audio time of the grid's first beat
    std::vector<double> beats; // tapped beat markers (audio time), sorted
    bool snap = false;         // scrubbing and range picks snap to beats
    Json extra = Json::object();  // unknown fields, written back (IO-43)

    bool operator==(const AudioTrack&) const = default;
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

// The face layer (spec 08 FA-5..FA-7): blinks, saccades and a look-at target, generated from a seed and baked
// onto the eyes, the eyelids and (with a look-at) the head. The generator and the bake live in face_anim.h.
struct FaceLayer {
    std::uint32_t seed = 1;
    bool blinks = true;
    double blink_min = 2, blink_max = 6;  // seconds between blinks
    double blink_length = 0.25;           // seconds, closing to open again
    bool saccades = true;
    double saccade_interval = 0.8;  // median seconds between saccades (log-normal)
    double eye_limit = 10;          // degrees: no saccade takes the eyes further than this from where they look
    std::string look;               // look-at target: "" none, "point", "prop", "camera", "actor"
    Vec3 point;                     // "point": avatar space
    std::string prop;               // "prop": the prop's name
    std::string actor, bone;        // "actor": a bone of another actor (GR)
    double head_share = 0.3;        // 0..1 of the turn towards the target the head takes
    double head_max = 45;           // degrees the head may turn from straight ahead
    bool baked = false;
    bool head_baked = false;              // the bake keyed mHead (a look-at with head_share > 0)
    std::map<std::string, Track> source;  // pre-bake tracks (absent = the track did not exist)
    Json extra = Json::object();          // unknown fields, written back

    bool operator==(const FaceLayer&) const = default;
};

// A named group of bones to select at once (spec 08 SS-1). The functions live in selection_sets.h; the sets live on
// the clip so they save with the project and undo covers them.
struct SelectionSet {
    std::string name;
    std::vector<std::string> bones;

    bool operator==(const SelectionSet&) const = default;
};

// Lip sync (spec 08 LS): mouth shapes on frames, keyed onto the mouth bones as ARKit shapes. Made and keyed by
// lip_sync.h; kept so the shapes show on the timeline and a nudge re-keys them.
struct LipSync {
    struct Cue {
        int frame = 0;
        std::string shape;  // a mouth shape of data/retarget/lip-shapes.json: A-H, X, open, rounded, wide
        bool operator==(const Cue&) const = default;
    };
    int from = 0, to = 0;       // the keyed frames
    bool positions = false;     // Move face bones when keyed: taking the moves back uses the same
    std::vector<Cue> cues;      // by frame; each holds until the next
    std::vector<double> level;  // 0..1 per frame from..to (the loudness, tier 1); empty = 1 everywhere
    Json extra = Json::object();  // unknown fields, written back

    bool operator==(const LipSync&) const = default;
};

// Two-bone IK frame (spec 02 section 3.7). VATs lines the mid joint up with the pole, so Switch to IK
// never twists the limb; Literal is section 3.7 as written, the solve converted projects use.
enum class IkSolve { VATs, Literal };

struct Clip {
    int fps = 30;
    int end_frame = 30;
    bool loop = false;
    int loop_in = 0, loop_out = 30;
    bool loop_tangents = false;  // spec 08 LP-7: tangents at the loop points see across the seam (on for new projects)
    int priority = 3;
    double ease_in = 0.8, ease_out = 0.8;
    int hand_pose = 1;
    std::string emote;
    IkSolve ik_solve = IkSolve::VATs;

    std::map<std::string, Track> curves;       // track name -> channels
    std::map<std::string, int> joint_priority;  // per-joint overrides (-1 = use the clip's)
    std::map<std::string, double> ik_pull;      // spec 08 RC-1: limb name -> Pull 0..1 (absent = 0), reach.h
    std::vector<AnimConstraint> constraints;
    std::vector<OrphanJoint> orphans;
    std::vector<Pin> pins;  // in application order
    std::vector<Prop> props;  // meshes placed in the scene; paths absolute while in memory
    std::vector<DynChain> dynamics;  // spec 08 DY-1
    std::vector<IdleLayer> idle;     // spec 08 IL
    std::optional<Ragdoll> ragdoll;  // spec 08 RD
    std::optional<AudioTrack> audio;  // spec 08 AU
    std::optional<FaceLayer> face_layer;  // spec 08 FA-5; absent = off
    std::vector<SelectionSet> selection_sets;  // spec 08 SS-1
    std::optional<LipSync> lip_sync;      // spec 08 LS; absent = none
    std::optional<Reference> reference;   // spec 08 RF; absent = none
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
