// Viewport Avatar Toolset - pose-matched insertion and transitions.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/pose_match.h"

#include <algorithm>
#include <cmath>

#include "vats/edit.h"
#include "vats/loop_assist.h"

namespace vats {

namespace {

constexpr const char* kHips = "mPelvis";

// Degrees the hips face from +X about Z, from their curves.
double heading(const Clip& clip, double frame) {
    const Vec3 fwd = euler_to_quat(curve_euler(clip, kHips, frame)).rotate({1, 0, 0});
    return std::atan2(fwd.y, fwd.x) * kRadToDeg;
}

double eased(const std::optional<EaseShape>& e, double t) { return e ? ease(*e, EaseDir::InOut, t) : t; }

// Removes keys with lo < frame < hi from every channel of the track but `keep`.
void clear_between(Track& track, double lo, double hi, const char* keep = nullptr) {
    for (auto& [ch, c] : track) {
        if (keep && ch == keep) continue;
        c.keys.erase(std::remove_if(c.keys.begin(), c.keys.end(),
                                    [&](const Key& k) { return k.frame > lo + 1e-6 && k.frame < hi - 1e-6; }),
                     c.keys.end());
        c.recompute_handles();
    }
}

}  // namespace

PoseMatch match_poses(const Rig& rig, const Clip& current, const Clip& incoming, const MatchOptions& o,
                      const Shape* shape) {
    PoseMatch best;
    const int end_a = std::max(current.end_frame, 0), end_b = std::max(incoming.end_frame, 0);
    const int search = std::max(o.search, 1), blend = std::max(o.blend, 0);
    const int lo = std::max(end_a - search, 0), hi = std::max(end_a - blend, lo);
    const int last_b = std::min(search, end_b);
    const PoseTrace ta = pose_trace(rig, current, lo, hi, shape, o.align);
    const PoseTrace tb = pose_trace(rig, incoming, 0, last_b, shape, o.align);
    const PoseDistance dist(rig.skeleton(), {&ta, &tb});
    best.cut = hi, best.distance = 1e300;
    for (int i = hi; i >= lo; --i)  // latest cut first: it wins ties
        for (int j = 0; j <= last_b; ++j)
            if (const double d = dist(ta, i, tb, j); d < best.distance - 1e-3) best.cut = i, best.into = j, best.distance = d;
    if (o.align) {
        best.yaw = heading(current, best.cut) - heading(incoming, best.into);
        const Quat r = Quat::axis_angle({0, 0, 1}, best.yaw * kDegToRad);
        best.shift = curve_offset(current, kHips, best.cut) - r.rotate(curve_offset(incoming, kHips, best.into));
        best.shift.z = 0;
    }
    return best;
}

void align_hips(Clip& incoming, const PoseMatch& m) {
    const Quat r = Quat::axis_angle({0, 0, 1}, m.yaw * kDegToRad);
    const Clip was = incoming;
    // ponytail: re-keyed on every frame, so the turn is exact there; the hips' own key spacing is not kept.
    Track& t = incoming.curves[kHips];
    t.erase("rot_x"), t.erase("rot_y"), t.erase("rot_z"), t.erase("pos_x"), t.erase("pos_y"), t.erase("pos_z");
    for (int f = 0; f <= std::max(was.end_frame, 0); ++f) {
        key_rotation(incoming, kHips, f, r * euler_to_quat(curve_euler(was, kHips, f)));
        const Vec3 off = curve_offset(was, kHips, f), turned = r.rotate(off) + m.shift;
        key_offset(incoming, kHips, f, {turned.x, turned.y, off.z});
    }
}

void join_matched(Clip& current, const Clip& incoming, const PoseMatch& m, const MatchOptions& o) {
    const Clip a = current;
    Clip b = incoming;
    if (o.align) align_hips(b, m);
    const int off = m.cut - m.into, blend = std::max(o.blend, 0), done = m.cut + blend;
    for (auto& [name, track] : b.curves)  // into the current clip's time
        for (auto& [ch, c] : track)
            for (Key& k : c.keys) k.frame += off, k.lx += off, k.rx += off;
    for (const auto& [name, track] : b.curves) {
        Track& dst = current.curves[name];
        for (const auto& [ch, src] : track) {
            if (src.empty()) continue;
            FCurve& d = dst[ch];
            d.keys.erase(std::remove_if(d.keys.begin(), d.keys.end(), [&](const Key& k) { return k.frame > m.cut + 1e-6; }),
                         d.keys.end());
            if (ch == "blend") d.set_key(done, src.evaluate(done), Interp::Constant);  // an IK switch lands with the blend
            for (const Key& k : src.keys)
                if (k.frame > done + 1e-6) d.keys.push_back(k);
            d.recompute_handles();
        }
        for (int f = m.cut; f <= done; ++f)  // both playing: the current motion eases into the incoming one
            key_mix(current, name, f, a, f, b, f, blend ? eased(o.ease, double(f - m.cut) / blend) : 1.0);
    }
    current.end_frame = m.cut + std::max(incoming.end_frame, 0) - m.into;
    current.loop_out = std::min(current.loop_out, current.end_frame);
    current.loop_in = std::min(current.loop_in, current.loop_out);
}

int make_transition(Clip& clip, const Clip& from, double from_frame, const Clip& to, double to_frame, int at,
                    int frames, std::optional<EaseShape> ease) {
    const Clip a = from;
    Clip b = to;
    frames = std::max(frames, 1);
    // A bone only `from` has goes to rest (zero), which key_mix would otherwise leave alone.
    for (const auto& [name, track] : a.curves) {
        if (name.rfind("ik.", 0) == 0) continue;
        for (const char* const(*g)[3] : {&kRotChannels, &kPosChannels})
            if (a.has_channels(name, *g) && !b.has_channels(name, *g))
                for (const char* ch : *g) b.curves[name][ch].set_key(to_frame, 0);
    }
    int n = 0;
    for (const auto& [name, track] : b.curves) {
        clear_between(clip.curves[name], at, at + frames, "blend");
        for (int k = 0; k <= frames; ++k)
            key_mix(clip, name, at + k, a, from_frame, b, to_frame, eased(ease, double(k) / frames));
        ++n;
    }
    return n;
}

}  // namespace vats
