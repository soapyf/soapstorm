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

namespace vats {

// Live mirror (PT-1). Keys the mirror partner of each edited track at frame from the track's curves there. A bone
// gives its partner the mirrored rotation, and the mirrored position where either side has position keys or the
// partner is an attachment point (as Mirror Bone does); an ik track gives its partner its pos, rot and pole channels,
// mirrored. A centre bone (its own partner) is made symmetric in place only with centre_in_place: it keeps the
// halfway rotation between its pose and its mirror image, and a sideways position of 0. Pins, centre ik tracks and
// tracks the skeleton does not know are left alone.
void mirror_live(Clip& clip, const Skeleton& skel, double frame, const std::vector<std::string>& tracks,
                 bool centre_in_place);

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
