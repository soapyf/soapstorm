// Viewport Avatar Toolset - overlap / follow-through.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/overlap.h"

#include <algorithm>
#include <cmath>

#include "vats/dynamics.h"
#include "vats/edit.h"
#include "vats/loop_tools.h"

namespace vats {
namespace {

// The rotation d turned s times as far about the same axis.
Quat scale_turn(Quat d, double s) {
    if (d.w < 0) d = -d;
    const Vec3 axis{d.x, d.y, d.z};
    if (axis.length() < 1e-12) return {};
    return Quat::axis_angle(axis.normalized(), d.angle() * s);
}

}  // namespace

std::string overlap_refusal(const Clip& clip, const Rig& rig, const std::vector<int>& chain) {
    if (chain.size() < 2) return "Overlap needs a chain of two bones or more";
    for (int n : chain)
        if (int l = rig.limb_of_bone(n); l >= 0 && uses_ik(clip, rig.limbs()[l]))
            return rig.limbs()[l].label + " uses IK; switch it to FK first";
    return "";
}

void apply_overlap(Clip& clip, const Skeleton& skel, const std::vector<int>& chain, const OverlapSettings& s) {
    const int end = std::max(clip.end_frame, 0);
    const LoopRange r = loop_range(clip);
    const bool wrap = clip.loop && clip.loop_out > clip.loop_in;
    const double len = r.out - r.in;
    auto source = [&](const std::string& track, double t) { return euler_to_quat(curve_euler(clip, track, t)); };
    std::vector<Pose> frames(end + 1, Pose(skel.size()));
    std::vector<int> baked;
    for (size_t i = 1; i < chain.size(); ++i) {
        const int n = chain[i];
        const std::string& track = skel[n].name;
        if (!clip.has_channels(track, kRotChannels)) continue;  // nothing to delay
        const double delay = s.shift * double(i), amp = std::pow(s.falloff, double(i));
        // The pose the swing is scaled about: the average over the range (a normalised sum, fine for the
        // spread of one bone's motion).
        Quat mean{0, 0, 0, 0};
        const Quat first = source(track, r.in);
        for (int f = r.in; f <= r.out; ++f) {
            Quat q = source(track, f);
            if (q.dot(first) < 0) q = -q;
            mean = {mean.w + q.w, mean.x + q.x, mean.y + q.y, mean.z + q.z};
        }
        mean = mean.normalized();
        auto scaled = [&](const Quat& q) { return (mean * scale_turn(mean.conj() * q, amp)).normalized(); };
        const double settle_from = end - 2 * delay;
        for (int f = 0; f <= end; ++f) {
            double t = f - delay;
            if (wrap && f >= r.in && f <= r.out) {
                t = std::fmod(t - r.in, len);
                if (t < 0) t += len;
                t += r.in;  // loop-in and loop-out both read loop-out - delay: the seam is unchanged
            }
            Quat q = scaled(source(track, std::max(t, 0.0)));
            if (s.settle && !wrap && f > settle_from) {
                const double u = std::min(1.0, (f - settle_from) / (end - settle_from));
                q = nlerp(q, scaled(source(track, f)), u * u * (3 - 2 * u));
            }
            frames[f].rot[n] = q;
        }
        baked.push_back(n);
    }
    bake_samples(clip, skel, baked, frames);
}

}  // namespace vats
