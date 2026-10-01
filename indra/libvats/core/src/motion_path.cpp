// Viewport Avatar Toolset - motion paths.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/motion_path.h"

#include <algorithm>
#include <cmath>

#include "vats/dope_sheet.h"

namespace vats {

std::vector<double> motion_path_frames(const Clip& clip, double frame, const MotionPathSettings& s) {
    const int last = std::max(clip.end_frame, 0);
    int from = 0, to = last;
    if (!s.whole_clip) {
        const int at = std::clamp(int(std::floor(frame + 1e-9)), 0, last);
        from = std::max(0, at - std::clamp(s.before, 1, 120));
        to = std::min(last, at + std::clamp(s.after, 1, 120));
    }
    std::vector<double> out;
    for (int f = from; f <= to; ++f) out.push_back(f);
    return out;
}

std::vector<std::string> motion_path_tracks(const Rig& rig, int node) {
    const Skeleton& skel = rig.skeleton();
    std::vector<int> chain;
    for (int n = node; n >= 0; n = skel[n].parent) chain.push_back(n);
    std::vector<std::string> out;
    for (int n : chain) out.push_back(skel[n].name), out.push_back("pin:" + skel[n].name);
    for (const LimbInfo& l : rig.limbs())
        if (std::find(chain.begin(), chain.end(), l.end) != chain.end()) out.push_back("ik." + l.name);
    return out;
}

std::vector<MotionPath> motion_paths(const Rig& rig, const Clip& clip, const Shape* shape, const std::vector<int>& nodes,
                                     double frame, const MotionPathSettings& s) {
    const Skeleton& skel = rig.skeleton();
    std::vector<MotionPath> out;
    std::vector<std::vector<double>> keyed;
    for (int n : nodes) {
        out.push_back({n, {}});
        keyed.push_back(keyed_frames(clip, motion_path_tracks(rig, n)));
    }
    for (double f : motion_path_frames(clip, frame, s)) {
        const std::vector<Xform> g = evaluate(rig, clip, f, shape).globals;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const int n = nodes[i];
            const bool k = std::any_of(keyed[i].begin(), keyed[i].end(), [&](double kf) { return same_frame(kf, f); });
            const Vec3 end = shape ? skel[n].end.mul(shape->scale[n]) : skel[n].end;  // as the view draws the bone
            out[i].points.push_back({f, g[n].apply(end), k});
        }
    }
    return out;
}

}  // namespace vats
