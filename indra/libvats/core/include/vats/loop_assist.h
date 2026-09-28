// Viewport Avatar Toolset - loop assists: best loop points, fit a loop to beats, loop-aware tangents, the walk treadmill.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 7.4 (LP-5..LP-8). The edits change the clip in place; callers wrap them in an undo step.
#pragma once

#include <vector>

#include "vats/loop_tools.h"
#include "vats/rig.h"

namespace vats {

// LP-5: frame pairs whose poses match best, as loop in/out. The distance is a weighted RMS in degrees over the
// joints' rotations and angular velocities (per 0.1 s), plus the pelvis height in centimetres; hips and legs weigh
// most, fingers and face least. Pairs are at least min_length frames apart; near-duplicates (both ends within
// 3 frames of a better pair) are dropped. Best first.
struct LoopCandidate {
    int in = 0, out = 0;
    double distance = 0;
    int length() const { return out - in; }
};
std::vector<LoopCandidate> find_loop_points(const Rig& rig, const Clip& clip, int min_length, int count = 5,
                                            const Shape* shape = nullptr);

// LP-5's pose distance, shared with pose-matched insertion (08 PM-1). A trace holds frames from..to of a clip
// (clamped to it) as the distance compares them: every node's local rotation (heading_free: the hips turned to
// face +X, so a clip turned about Z matches), its angular velocity in degrees per 0.1 s and the pelvis height in cm.
struct PoseTrace {
    int from = 0;
    std::vector<std::vector<Quat>> rot;
    std::vector<std::vector<Vec3>> vel;
    std::vector<double> height;
};
PoseTrace pose_trace(const Rig& rig, const Clip& clip, int from, int to, const Shape* shape = nullptr,
                     bool heading_free = false);
// The distance between frame fa of one trace and fb of another (clip frames, inside the traces). The joints
// compared are the weighted ones that move somewhere in the traces given here.
class PoseDistance {
public:
    PoseDistance(const Skeleton& sk, const std::vector<const PoseTrace*>& traces);
    double operator()(const PoseTrace& a, int fa, const PoseTrace& b, int fb) const;
    bool any() const { return wsum_ > 0; }

private:
    std::vector<std::pair<int, double>> joints_;  // node, weight
    double wsum_ = 0;
    bool pelvis_ = false;
};

// LP-6: a loop of `beats` beats at `bpm` and `fps`. frames is the nearest whole number of frames; residual_ms
// is how far each loop ends from the beat (positive = late), loops_to_drift how many loops until that adds up
// to one frame (0 = never), suggested_fps the frame rate nearest fps (10..60) on which every beat is a whole
// number of frames (0 = none).
struct BeatFit {
    int frames = 0;
    double seconds = 0, residual_ms = 0;
    long loops_to_drift = 0;
    int suggested_fps = 0;
};
BeatFit fit_to_beats(double bpm, int fps, int beats);
// Stretches the loop range (or the whole clip) to `frames`; later keys, the loop points and the length follow.
void stretch_loop(Clip& clip, int frames);

// LP-7: with Loop and the clip's loop_tangents on, the automatic tangents (Auto, Spline, Plateau) of the keys at
// loop in and loop out are computed as if the loop went on: the key before loop out is loop in's left neighbour,
// the key after loop in is loop out's right neighbour (moved by the seam's jump, so a whole-turn rotation works).
// Only curves keyed at both ends with a key between. Idempotent. Returns the number of keys whose handles changed.
// Every edit recomputes handles the ordinary way, so the app applies this after each one (every frame).
int apply_loop_tangents(Clip& clip);

// LP-8: Second Life's default ground speeds, m/s on level ground, from
// https://wiki.secondlife.com/wiki/Default_Avatar_Movement_Speeds (walk 3.20, run 5.13, crouch walk 2.00, fly 16.00).
struct SlSpeed {
    const char* name;
    double mps;
};
inline constexpr SlSpeed kSlSpeeds[] = {{"Walk", 3.20}, {"Run", 5.13}, {"Crouch Walk", 2.00}, {"Fly", 16.00}};

// LP-8: the cycle as the foot contacts (footlock.h, height only) show it over the loop range. speed is how fast
// the body moves over a planted foot (the ground speed the cycle implies, travel included), cycle the loop's
// length over the most contacts one foot starts in it, stride = speed x cycle. contacts = 0: none found.
struct Gait {
    double speed = 0, cycle = 0, stride = 0;
    int contacts = 0;
};
Gait measure_gait(const Rig& rig, const Clip& clip, const Shape* shape = nullptr);
// Makes the cycle's implied speed `target`: stretches the loop's time by speed / target. Returns the new length.
int match_speed_by_time(Clip& clip, const Gait& g, double target);
// Scales the hips' travel along the ground (loop_tools' remove_travel / add_travel) to `target` m/s; the timing
// stays, so planted feet slide by the difference. False when the hips do not travel.
bool match_speed_by_travel(Clip& clip, double target, const std::string& hip = "mPelvis");

}  // namespace vats
