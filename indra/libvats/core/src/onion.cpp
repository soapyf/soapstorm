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
    // A looping clip, at a frame in its loop, shows the loop as it plays (user test: the seam is where ghosts matter
    // most): past Loop out they go on from just after Loop in (the same pose), before Loop in from just before Loop out.
    const double in = clip.loop_in, end = clip.loop_out, len = end - in;
    const bool loops = clip.loop && len > 0 && frame >= in && frame <= end;
    auto wrap = [&](double f) { return !loops ? f : f > end ? f - len : f < in ? f + len : f; };
    if (s.keyed_only) {
        std::set<double> keyed;
        for (const auto& [name, track] : clip.curves)
            for (const auto& [ch, curve] : track)
                for (const Key& k : curve.keys)
                    if (k.frame >= 0 && k.frame <= last) keyed.insert(k.frame);
        if (loops)  // the loop's keys a loop earlier and later too, within one loop of the frame
            for (double k : std::set<double>(keyed))
                if (k >= in && k <= end) keyed.insert(k - len), keyed.insert(k + len);
        int n = 0;
        for (auto it = keyed.lower_bound(frame); it != keyed.begin() && n < before;) {
            --it;
            if (loops && *it <= frame - len) break;
            if (!same_frame(*it, frame) && !same_frame(wrap(*it), frame)) add(wrap(*it), -++n, before);
        }
        n = 0;
        for (auto it = keyed.upper_bound(frame); it != keyed.end() && n < after; ++it) {
            if (loops && *it >= frame + len) break;
            if (!same_frame(*it, frame) && !same_frame(wrap(*it), frame)) add(wrap(*it), ++n, after);
        }
        return out;
    }
    const int step = std::max(s.step, 1);
    for (int k = 1; k <= before && (loops ? k * step < len : frame - k * step >= 0); ++k) add(wrap(frame - k * step), -k, before);
    for (int k = 1; k <= after && (loops ? k * step < len : frame + k * step <= last); ++k) add(wrap(frame + k * step), k, after);
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

double target_frame(const Clip& target, double frame) { return std::clamp(frame, 0.0, double(std::max(target.end_frame, 0))); }

double bone_angle_apart(const Skeleton& skel, const std::vector<Xform>& a, const std::vector<Xform>& b, int bone) {
    if (bone < 0 || bone >= int(a.size()) || bone >= int(b.size())) return 0;
    const int parent = skel[bone].parent;
    auto local = [&](const std::vector<Xform>& g) { return parent >= 0 ? g[parent].rot.conj() * g[bone].rot : g[bone].rot; };
    return (local(a).conj() * local(b)).angle() * kRadToDeg;
}

}  // namespace vats
