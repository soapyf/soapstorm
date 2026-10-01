// Viewport Avatar Toolset - retime markers and splitting a dance at beats (spec 08 TE-5, TE-6).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/retime.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "vats/audio.h"
#include "vats/retarget.h"
#include "vats/time_edit.h"

namespace vats {

double snap_marker(const Clip& clip, double frame, bool whole_frames) {
    const double fps = std::max(clip.fps, 1);
    if (clip.audio && clip.audio->snap) frame = snap_to_beat(*clip.audio, frame / fps, 3.0 / fps) * fps;
    return whole_frames ? std::round(frame) : frame;
}

void drag_marker(Clip& clip, std::vector<double>& markers, size_t i, double to) {
    if (i >= markers.size()) return;
    const double prev = i ? markers[i - 1] : 0.0, from = markers[i];
    if (from <= prev) return;
    to = std::max(to, prev + 1);
    scale_time_to(clip, prev, from, to);
    for (size_t k = i; k < markers.size(); ++k) markers[k] += to - from;
}

std::vector<int> dance_cuts(const Clip& clip, double max_seconds) {
    const int fps = std::max(clip.fps, 1), end = std::max(clip.end_frame, 1);
    const int limit = std::max(1, int(std::floor(max_seconds * fps + 1e-9)));
    std::vector<int> beats;
    if (clip.audio)
        for (double t : beat_times(*clip.audio, 0, double(end) / fps)) beats.push_back(int(std::lround(t * fps)));
    std::vector<int> cuts;
    for (int start = 0; end - start > limit;) {
        int cut = start + limit;
        auto it = std::upper_bound(beats.begin(), beats.end(), cut);  // the last beat in (start, start + limit]
        if (it != beats.begin() && *std::prev(it) > start) cut = *std::prev(it);
        cuts.push_back(cut);
        start = cut;
    }
    return cuts;
}

std::vector<Clip> split_dance(const Clip& clip, const std::vector<int>& cuts) {
    std::vector<Clip> parts;
    int a = 0;
    for (size_t k = 0; k <= cuts.size(); ++k) {
        const int b = k < cuts.size() ? cuts[k] : clip.end_frame;
        Clip p = slice_clip(clip, a, b);
        p.loop = clip.loop, p.loop_in = 0, p.loop_out = p.end_frame;
        if (k) p.ease_in = 0;
        if (k < cuts.size()) p.ease_out = 0;
        parts.push_back(std::move(p));
        a = b;
    }
    return parts;
}

}  // namespace vats
