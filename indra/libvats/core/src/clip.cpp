// Viewport Avatar Toolset - an animation clip.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/clip.h"

#include <algorithm>

namespace vats {

Clip new_project_clip() {
    Clip c;
    c.loop_tangents = true;
    c.ease_in = c.ease_out = 0.3;
    return c;
}

void set_last_frame(Clip& c, int last) {
    if (c.loop_out == c.end_frame) c.loop_out = last;
    c.end_frame = last;
    c.loop_out = std::min(c.loop_out, last);
    c.loop_in = std::min(c.loop_in, c.loop_out);
}

void set_loop(Clip& c, bool on) {
    c.loop = on;
    if (on && (c.loop_out == 0 || (c.loop_in == 0 && c.loop_out == Clip{}.loop_out))) c.loop_out = c.end_frame;
}

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

void for_each_track_map(Clip& clip, const std::function<void(std::map<std::string, Track>&)>& fn) {
    fn(clip.curves);
    for (DynChain& d : clip.dynamics) fn(d.source);
    for (IdleLayer& l : clip.idle) fn(l.source);
    if (clip.ragdoll) fn(clip.ragdoll->source);
    if (clip.face_layer) fn(clip.face_layer->source);
}

}  // namespace vats
