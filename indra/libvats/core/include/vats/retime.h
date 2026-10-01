// Viewport Avatar Toolset - retime markers and splitting a dance at beats (spec 08 TE-5, TE-6).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <vector>

#include "vats/clip.h"

namespace vats {

// TE-5: where a retime marker lands for the frame under the mouse: on the nearest beat (grid or tapped) within
// 3 frames when the audio track snaps to beats, then on a whole frame when whole_frames is on.
double snap_marker(const Clip& clip, double frame, bool whole_frames);

// TE-5: moves markers[i] to `to`, kept at least a frame after the marker before it (frame 0 for the first). The
// keys between that marker and this one scale, every later key and marker moves by the difference, and pins,
// loop points and ik. blend keys follow as in Stretch Range (scale_time_to on every track). The app re-runs this
// from the clip and markers as they were at the press, so a drag is one edit. A marker at frame 0 does not move.
void drag_marker(Clip& clip, std::vector<double>& markers, size_t i, double to);

// TE-6: the frames at which a dance is cut so no part is longer than max_seconds: each cut on the last beat
// (the audio track's grid or tapped beats, rounded to a whole frame) at or before the limit; where no beat falls
// in reach, at the limit itself. Empty when the clip fits in one part.
std::vector<int> dance_cuts(const Clip& clip, double max_seconds = 60);
// The parts between the cuts (slice_clip): each starts on the frame the previous one ends, so the joins match.
// Every part keeps the fps and the Loop setting (a looping part loops whole); ease in stays on the first part and
// ease out on the last, the joins get none so a dance HUD chains them without a dip.
std::vector<Clip> split_dance(const Clip& clip, const std::vector<int>& cuts);

}  // namespace vats
