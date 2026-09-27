// Viewport Avatar Toolset - an animation clip.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/clip.h"

namespace vats {

bool Clip::has_channels(const std::string& track, const char* const (&channels)[3]) const {
    auto t = curves.find(track);
    if (t == curves.end()) return false;
    for (const char* c : channels) {
        auto ch = t->second.find(c);
        if (ch != t->second.end() && !ch->second.empty()) return true;
    }
    return false;
}

Pose evaluate_curves(const Skeleton& skel, const Clip& clip, double frame) {
    Pose pose(skel.size());
    for (auto& [name, track] : clip.curves) {
        int i = skel.find(name);
        if (i < 0) continue;
        auto eval = [&](const char* const (&channels)[3]) {
            Vec3 v;
            for (int a = 0; a < 3; ++a) {
                auto ch = track.find(channels[a]);
                if (ch != track.end()) v[a] = ch->second.evaluate(frame);
            }
            return v;
        };
        pose.rot[i] = euler_to_quat(eval(kRotChannels));
        pose.offset[i] = eval(kPosChannels);
    }
    return pose;
}

}  // namespace vats
