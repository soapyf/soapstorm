// Viewport Avatar Toolset - posing assists: live mirror, scratch pose, propagate pose and the graph's curve buffer.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/pose_tools.h"

#include <algorithm>

#include "vats/edit.h"
#include "vats/pose_ops.h"

namespace vats {
namespace {

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

Quat mirror_q(const Quat& q) { return {q.w, -q.x, q.y, -q.z}; }
Vec3 mirror_v(const Vec3& v) { return {v.x, -v.y, v.z}; }

}  // namespace

void mirror_live(Clip& clip, const Skeleton& skel, double frame, const std::vector<std::string>& tracks,
                 bool centre_in_place) {
    // No partner written is itself an edited track, so every source reads the same before and after (no copy).
    const Clip& before = clip;
    auto edited = [&](const std::string& t) { return std::find(tracks.begin(), tracks.end(), t) != tracks.end(); };
    for (const std::string& src : tracks) {
        if (starts_with(src, "ik.")) {
            const std::string dst = Skeleton::mirror_name(src);
            if (dst == src || edited(dst)) continue;
            auto t = before.curves.find(src);
            if (t == before.curves.end()) continue;
            for (auto& [ch, c] : t->second)  // pos and pole per channel; blend stays the partner's own
                if (!c.empty() && (starts_with(ch, "pos_") || starts_with(ch, "pole_")))
                    clip.curves[dst][ch].set_key(frame, mirror_flips(ch) ? -c.evaluate(frame) : c.evaluate(frame));
            if (before.has_channels(src, kRotChannels))
                key_rotation(clip, dst, frame, mirror_q(euler_to_quat(curve_euler(before, src, frame))));
            continue;
        }
        const int s = skel.find(src);
        if (s < 0) continue;  // pins and unknown tracks
        const int d = skel.mirror(s);
        const Quat rot = euler_to_quat(curve_euler(before, src, frame));
        const Vec3 off = curve_offset(before, src, frame);
        const bool pos = before.has_channels(src, kPosChannels);
        if (d == s) {
            if (!centre_in_place) continue;
            key_rotation(clip, src, frame, nlerp(rot, mirror_rotation(skel, s, s, rot), 0.5));
            if (pos) key_offset(clip, src, frame, {off.x, 0, off.z});
            continue;
        }
        const std::string& dn = skel[d].name;
        if (edited(dn)) continue;
        key_rotation(clip, dn, frame, mirror_rotation(skel, s, d, rot));
        if (pos || skel[d].attachment || before.has_channels(dn, kPosChannels)) key_offset(clip, dn, frame, mirror_v(off));
    }
}

std::vector<std::string> scratch_tracks(const Clip& base, const Clip& working) {
    std::vector<std::string> out;
    for (auto& [name, track] : working.curves) {
        auto b = base.curves.find(name);
        if (b == base.curves.end() || b->second != track) out.push_back(name);
    }
    for (auto& [name, track] : base.curves)
        if (!working.curves.count(name)) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

Clip scratch_commit(const Clip& base, const Clip& working, double frame, bool only_existing) {
    Clip out = working;
    out.curves = base.curves;
    for (auto& [name, track] : working.curves)
        for (auto& [ch, c] : track) {
            const FCurve* was = nullptr;
            if (auto t = base.curves.find(name); t != base.curves.end())
                if (auto k = t->second.find(ch); k != t->second.end()) was = &k->second;
            if (c.empty() || (was && *was == c) || (only_existing && (!was || was->empty()))) continue;
            out.curves[name][ch].set_key(frame, c.evaluate(frame));
        }
    return out;
}

int propagate_pose(Clip& clip, const std::vector<std::string>& tracks, double frame, PropagateTo to, double a,
                   double b) {
    int n = 0;
    for (const std::string& name : tracks) {
        auto t = clip.curves.find(name);
        if (t == clip.curves.end()) continue;
        double lo = std::max(a, frame), hi = b;
        if (to != PropagateTo::Range) {
            lo = frame, hi = 1e300;
            if (to == PropagateTo::NextKey) {
                const std::vector<double> frames = key_frames(clip, name);
                auto next = std::find_if(frames.begin(), frames.end(), [&](double f) { return f > frame && !same_frame(f, frame); });
                if (next == frames.end()) continue;
                hi = *next;
            }
        }
        for (auto& [ch, c] : t->second) {
            if (c.empty() || ch == "blend") continue;  // an ik track's IK/FK state is not pose
            const double v = c.evaluate(frame);
            std::vector<double> targets;
            for (const Key& k : c.keys)
                if (k.frame >= lo - 1e-9 && k.frame <= hi + 1e-9 && !same_frame(k.frame, frame) && k.value != v)
                    targets.push_back(k.frame);
            for (double f : targets) c.set_key(f, v);
            n += int(targets.size());
        }
    }
    return n;
}

const FCurve* CurveBuffer::curve(const std::string& track, const std::string& channel) const {
    if (!curves) return nullptr;
    auto t = curves->find(track);
    if (t == curves->end()) return nullptr;
    auto c = t->second.find(channel);
    return c == t->second.end() ? nullptr : &c->second;
}

}  // namespace vats
