// Viewport Avatar Toolset - posing assists: live mirror, scratch pose, propagate pose and the graph's curve buffer.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section PT. Name mapping goes through Skeleton::mirror_name, as in pose_ops.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/rig.h"
#include "vats/rig_constraints.h"

namespace vats {

// Live mirror (PT-1). Keys the mirror partner of each edited track at frame from the track's curves there. A bone
// gives its partner the mirrored rotation, and the mirrored position where either side has position keys or the
// partner is an attachment point (as Mirror Bone does); an ik track gives its partner its pos, rot and pole channels,
// mirrored. A centre bone (its own partner) is made symmetric in place only with centre_in_place: it keeps the
// halfway rotation between its pose and its mirror image, and a sideways position of 0. Pins, centre ik tracks and
// tracks the skeleton does not know are left alone.
void mirror_live(Clip& clip, const Skeleton& skel, double frame, const std::vector<std::string>& tracks,
                 bool centre_in_place);

// The Hand Poser (spec 06 4.6). A finger segment's rotation for a dot dragged curl_deg down and spread_deg sideways,
// from start (its rotation at the press): curled about the bone's curl axis (across the palm; the thumb's slants), and
// the first segment (first) swung sideways too. limit, when given: the result kept inside it, as a gizmo drag is.
Quat finger_segment_rotation(const Skeleton& skel, int node, const Quat& start, double curl_deg, double spread_deg,
                             bool first, const JointLimit* limit, const Shape* shape);
// How far a finger is curled: its segments' turns about their curl axes at frame, added, in degrees (toward the palm
// positive). Segments the skeleton does not know count 0.
double finger_curl_degrees(const Skeleton& skel, const Clip& clip, const std::vector<std::string>& segments, double frame);

// Sit on This (a seat prop): a sit at frame done as the sit tutorial does it by hand. The hips drop until the feet
// reach the floor, both ankles are held in the world there (Hold in World), then the hips go to where the bottom of the
// thighs (their capsules) rests on the seat at seat_z (world height), the held feet bending the knees. The pelvis is
// keyed at frame only. False, with why, when an ankle cannot be held; else report says what moved.
bool sit_on_seat(Clip& clip, const Rig& rig, double frame, double seat_z, const Shape* shape, std::string& report);
// Where the thighs are at frame, hip to knee, both sides (at 0, 1/4, 1/2 and 3/4 of the way): the points to look under
// for the seat.
std::vector<Vec3> thigh_points(const Rig& rig, const Clip& clip, double frame, const Shape* shape);

// Scratch pose (PT-2). The tracks whose curves differ between base and working, sorted.
std::vector<std::string> scratch_tracks(const Clip& base, const Clip& working);
// What committing a scratch pose leaves: working with base's curves, plus a key at frame, holding working's value
// there, on every channel whose curve changed. only_existing: a channel with no keys in base is not keyed.
Clip scratch_commit(const Clip& base, const Clip& working, double frame, bool only_existing);

// Propagate Pose (PT-3). Each channel of tracks gets its value at frame written onto its later keys: those at the
// track's next key (the first frame after `frame` holding a key on any of its channels), those after frame in [a, b],
// or every one after frame (an ik track's blend channel excepted). Keys keep their frames, interpolation and handle offsets. Returns how many keys
// changed value.
enum class PropagateTo { NextKey, Range, End };
int propagate_pose(Clip& clip, const std::vector<std::string>& tracks, double frame, PropagateTo to, double a = 0,
                   double b = 0);

// The graph's curve buffer (PT-4): a snapshot of every curve, drawn in grey until cleared.
struct CurveBuffer {
    std::optional<std::map<std::string, Track>> curves;

    void snapshot(const Clip& clip) { curves = clip.curves; }
    void clear() { curves.reset(); }
    // Exchanges the live curves and the snapshot; false without a snapshot.
    bool swap(Clip& clip) {
        if (!curves) return false;
        std::swap(clip.curves, *curves);
        return true;
    }
    // The snapshot's curve for a channel, or null.
    const FCurve* curve(const std::string& track, const std::string& channel) const;
};

}  // namespace vats
