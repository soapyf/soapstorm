// Viewport Avatar Toolset - face animation by hand: expression sliders, the blink/saccade/look-at layer, look-at.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 12 (FA). Everything keys bones through the face table (facecap.h), so a face made
// with sliders, one recorded from a phone and one baked by the layer are the same kind of keys; the clip holds
// no slider values.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/facecap.h"
#include "vats/pose_ops.h"
#include "vats/rig.h"

namespace vats {

// --- Face panel (FA-1..FA-4) ---------------------------------------------------------------------

// Keys the face at frame for these slider weights (ARKit shape names and VRM preset names, 0..1); position keys
// only with positions (Move face bones). Without from, every table bone gets the weights' value, as key_face keys
// a recorded face. With from (the sliders before a change), only the bones whose value differs between the two are
// keyed, each moved by that difference from its current value: bones the change does not touch keep their keys,
// and a bone no slider combination explains moves with the slider instead of jumping.
void key_face_weights(Clip& clip, const FaceTable& table, const std::map<std::string, double>& weights,
                      bool positions, double frame, const std::map<std::string, double>* from = nullptr);

// Whether weights keyed now would give the face bones' current curve values at frame (within 0.01 degrees and
// 0.01 mm): the panel keeps its own slider values while this holds.
bool face_weights_match(const Clip& clip, const FaceTable& table, const std::map<std::string, double>& weights,
                        bool positions, double frame);

// The table bones' curve values at frame, in table.bones() order: Euler degrees, then with positions the offsets
// in millimetres. Cheap: the panel compares it frame to frame to know when to read the sliders back.
std::vector<double> face_bone_values(const Clip& clip, const FaceTable& table, double frame, bool positions);

// Slider read-back: the ARKit weights (0..1) whose keys come closest to the face bones' curves at frame (a
// bounded least-squares fit). Shapes that move the same bones the same way cannot be told apart; presets read
// back as their shapes; with positions off, shapes that only move bones read back as 0.
std::map<std::string, double> read_face_weights(const Clip& clip, const FaceTable& table, double frame,
                                                bool positions);

// Whether a shape keys anything: some bone turns, or positions is on and some bone moves.
bool face_shape_keys(const FaceTable& table, const std::string& shape, bool positions);

// A face pose for the pose library: every table bone's curve rotation at frame, and with positions the offsets
// of the bones the table moves. Kind "face", no side; apply_pose keys it like any pose.
LibraryItem make_face_pose(const Clip& clip, const FaceTable& table, double frame, bool positions);

// --- Blink, saccade and look-at layer (FA-5..FA-7) ------------------------------------------------

// Where a look-at looks at a frame: a point in the clip's avatar space. False = no target at that frame.
using LookTarget = std::function<bool(double frame, Vec3& world)>;

// The layer's events. Times are seconds on the clip's timeline (frame / fps).
struct FaceEvents {
    std::vector<double> blinks;  // start times
    struct Saccade {
        double start = 0, duration = 0;
        double yaw = 0, pitch = 0;  // where the eyes end up, degrees from where they look (Euler z and y)
    };
    std::vector<Saccade> saccades;
};

// Generates the events from the layer's seed. A clip that loops gets its intro (0..loop_in) and its loop
// (loop_in..loop_out) generated separately; each ends with the eyes back where they look and no blink running,
// and blinks keep blink_min apart across the loop's seam. Frames after loop_out, or after end_frame, get none.
FaceEvents face_layer_events(const Clip& clip, const FaceLayer& layer);

// 0..1: how closed the eyelids are at time t (close in 30% of the blink, hold 15%, open in the rest).
double blink_weight(const FaceEvents& ev, double t, double blink_length);
// The saccade offset at time t (yaw, pitch in degrees, as Vec3{0, pitch, yaw}).
Vec3 saccade_offset(const FaceEvents& ev, double t);

// Bakes clip.face_layer onto the eyes, the eyelids and, with a look-at and head_share > 0, mHead, keeping the
// pre-bake tracks in FaceLayer::source; re-baking starts from those. look may be empty (no target). With
// positions (Move face bones), blink shapes that move lids move them. One undo step for the caller.
void bake_face_layer(Clip& clip, const Rig& rig, const Shape* shape, const FaceTable& table, bool positions,
                     const LookTarget& look);
// Puts the pre-bake tracks back.
void unbake_face_layer(Clip& clip, const Skeleton& skel, const FaceTable& table);

// --- Look-at tool (FA-8) --------------------------------------------------------------------------

// The rotation (relative to rest, as Pose::rot) that turns node's forward (+X) at target from its current
// rotation g[node], keeping its roll; the forward ends at most max_deg from where the parent points it at rest.
Quat aim_rotation(const Skeleton& skel, const std::vector<Xform>& g, int node, const Vec3& target, double max_deg);

struct LookAtOptions {
    int from = 0, to = -1;    // frames; to = -1: the clip's end
    double max_turn = 60;     // degrees from straight ahead
    double weight = 1;        // 0..1 of the way from the animation to the aim
};

// Keys each node (head, eyes; parents before children) on every frame from..to to look at the target, like a
// Follow Target bake. False with why when nothing can be keyed.
bool look_at_bake(Clip& clip, const Rig& rig, const std::vector<int>& nodes, const LookTarget& target,
                  const LookAtOptions& opt, const Shape* shape, std::string& why);

}  // namespace vats
