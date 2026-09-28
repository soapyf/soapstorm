// Viewport Avatar Toolset - onion skinning.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/onion.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace vats {

std::vector<OnionFrame> onion_frames(const Clip& clip, double frame, const OnionSettings& s) {
    const int before = std::clamp(s.before, 0, 5), after = std::clamp(s.after, 0, 5);
    const double last = std::max(clip.end_frame, 0);
    std::vector<OnionFrame> out;
    auto add = [&](double f, int offset, int count) {
        out.push_back({f, offset, float(count + 1 - std::abs(offset)) / float(count + 1)});
    };
    if (s.keyed_only) {
        std::set<double> keyed;
        for (const auto& [name, track] : clip.curves)
            for (const auto& [ch, curve] : track)
                for (const Key& k : curve.keys)
                    if (k.frame >= 0 && k.frame <= last) keyed.insert(k.frame);
        int n = 0;
        for (auto it = keyed.lower_bound(frame); it != keyed.begin() && n < before;) {
            --it;
            if (same_frame(*it, frame)) continue;
            add(*it, -++n, before);
        }
        n = 0;
        for (auto it = keyed.upper_bound(frame); it != keyed.end() && n < after; ++it)
            if (!same_frame(*it, frame)) add(*it, ++n, after);
        return out;
    }
    const int step = std::max(s.step, 1);
    for (int k = 1; k <= before && frame - k * step >= 0; ++k) add(frame - k * step, -k, before);
    for (int k = 1; k <= after && frame + k * step <= last; ++k) add(frame + k * step, k, after);
    return out;
}

std::vector<OnionGhost> onion_ghosts(const Rig& rig, const Clip& clip, double frame, const Shape* shape,
                                     const OnionSettings& s) {
    std::vector<OnionGhost> out;
    for (const OnionFrame& f : onion_frames(clip, frame, s)) out.push_back({f, evaluate(rig, clip, f.frame, shape).globals});
    return out;
}

std::vector<Xform> pose_ghost(const Rig& rig, const Clip& clip, double frame, const Shape* shape, const LibraryItem& pose) {
    Clip posed = clip;  // ponytail: a whole-clip copy per drawn frame; fine for a few pinned poses
    apply_pose(posed, rig.skeleton(), pose, frame, false);
    return evaluate(rig, posed, frame, shape).globals;
}

}  // namespace vats
