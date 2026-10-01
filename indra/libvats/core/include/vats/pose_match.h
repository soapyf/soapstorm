// Viewport Avatar Toolset - pose-matched insertion and transitions.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 23 (PM-1..PM-3). The edits change the clip in place; callers wrap them in an undo step.
// Second Life blends whole animations itself (their ease in and out); this builds one file from several pieces.
#pragma once

#include <optional>
#include <string>

#include "vats/rig.h"
#include "vats/tween.h"

namespace vats {

struct MatchOptions {
    int search = 15;    // frames searched at the end of the current clip and at the start of the incoming one
    int blend = 6;      // frames the join crossfades over; the cut leaves this many of the current clip after it
    bool align = true;  // turn and move the incoming hips onto the current ones (yaw and XY; Z is kept)
    std::optional<EaseShape> ease = EaseShape::Sine;  // In-Out; none = linear
};

// Where the incoming clip takes over: its frame `into` lands on the current clip's frame `cut`. With align, the
// incoming hips turn by yaw degrees about Z and their offset gains shift (Z 0): offset' = Rz(yaw) offset + shift,
// Z kept, so the hips at `into` sit on the current ones at `cut`.
struct PoseMatch {
    int cut = 0, into = 0;
    double distance = 0;  // LP-5's pose distance there (hips heading-free with align)
    double yaw = 0;
    Vec3 shift;
};

// PM-1: the pair with the least LP-5 distance, the cut among the current clip's last `search` frames (but at least
// `blend` before its end) and `into` among the incoming clip's first `search`. Pairs within 0.001 of each other
// count as equal, and the later cut wins (it keeps more of the current clip).
PoseMatch match_poses(const Rig& rig, const Clip& current, const Clip& incoming, const MatchOptions& o,
                      const Shape* shape = nullptr);

// PM-2: the incoming mPelvis turned and moved by the match (rotation and position re-keyed on every frame).
void align_hips(Clip& incoming, const PoseMatch& m);

// PM-2: joins incoming onto current at the match. Tracks the incoming clip has lose their keys after the cut; over
// cut..cut+blend they are keyed every frame, eased from the current clip's motion into the incoming one's (both
// playing), and the incoming keys after that follow. Other tracks keep their keys. The clip ends at
// cut + (incoming end - into). With o.align, the incoming hips are aligned first.
void join_matched(Clip& current, const Clip& incoming, const PoseMatch& m, const MatchOptions& o);

// PM-3: keys frames at..at+frames of every track from or to has, eased from from's pose at from_frame to to's pose at
// to_frame (both held still); keys strictly inside are removed first. A bone only `from` animates goes to rest.
// from and to may be the clip itself. Returns the number of tracks keyed.
int make_transition(Clip& clip, const Clip& from, double from_frame, const Clip& to, double to_frame, int at,
                    int frames, std::optional<EaseShape> ease = EaseShape::Sine);

}  // namespace vats
