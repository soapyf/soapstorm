// Viewport Avatar Toolset - loop assists.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/loop_assist.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "vats/footlock.h"
#include "vats/time_edit.h"

namespace vats {

namespace {

constexpr double kDeg = 57.29577951308232;

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// How much a joint counts in a pose match: the hips and legs carry a cycle, fingers and face barely show.
double joint_weight(const Node& n) {
    if (n.name == "mPelvis") return 4;
    switch (n.category) {
        case Category::Body:
            for (const char* leg : {"Hip", "Knee", "Ankle", "Foot", "Toe"})
                if (contains(n.name, leg)) return 3;
            return 1;
        case Category::Hands:
        case Category::Face: return 0.1;
        case Category::Wings:
        case Category::Tail:
        case Category::HindLimbs:
        case Category::Groin: return 0.5;
        default: return 0;
    }
}

// The rotation vector (axis x angle, degrees) of q.
Vec3 rotvec(Quat q) {
    if (q.w < 0) q = -q;
    const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-12) return {};
    const double a = 2 * std::atan2(s, q.w) * kDeg / s;
    return {q.x * a, q.y * a, q.z * a};
}

bool is_auto(Handle h) { return h == Handle::AutoClamped || h == Handle::Auto || h == Handle::Plateau; }

Key shifted(Key k, double df, double dv) {
    k.frame += df, k.lx += df, k.rx += df;
    k.value += dv, k.ly += dv, k.ry += dv;
    return k;
}

}  // namespace

PoseTrace pose_trace(const Rig& rig, const Clip& clip, int from, int to, const Shape* shape, bool heading_free) {
    const Skeleton& sk = rig.skeleton();
    const int last = std::max(clip.end_frame, 0);
    PoseTrace t;
    t.from = std::clamp(from, 0, last);
    const int n = std::clamp(to, t.from, last) - t.from + 1;
    // One frame more on each side where the clip has it, so the velocities at the edges are central too.
    const int lo = std::max(t.from - 1, 0), hi = std::min(t.from + n, last);
    std::vector<std::vector<Quat>> rot(hi - lo + 1);
    t.height.resize(n);
    const int pelvis = sk.find("mPelvis");
    for (int f = lo; f <= hi; ++f) {
        Evaluation e = evaluate(rig, clip, f, shape);
        if (pelvis >= 0 && heading_free) {  // turn the hips to face +X: a clip turned about Z then matches
            const Vec3 fwd = e.pose.rot[pelvis].rotate({1, 0, 0});
            e.pose.rot[pelvis] = Quat::axis_angle({0, 0, 1}, -std::atan2(fwd.y, fwd.x)) * e.pose.rot[pelvis];
        }
        rot[f - lo] = std::move(e.pose.rot);
        if (f >= t.from && f < t.from + n) t.height[f - t.from] = pelvis >= 0 ? e.globals[pelvis].pos.z * 100 : 0;  // cm
    }
    // Angular velocity per joint and frame, in degrees per 0.1 s (so the frame rate does not matter).
    const double per = std::max(clip.fps, 1) * 0.1;
    t.rot.resize(n);
    t.vel.resize(n);
    for (int k = 0; k < n; ++k) {
        const int f = t.from + k, f0 = std::max(f - 1, lo), f1 = std::min(f + 1, hi);
        t.rot[k] = rot[f - lo];
        t.vel[k].resize(sk.joint_count());
        for (int b = 0; b < sk.joint_count(); ++b)
            t.vel[k][b] = rotvec(rot[f1 - lo][b] * rot[f0 - lo][b].conj()) * (per / std::max(f1 - f0, 1));
    }
    return t;
}

PoseDistance::PoseDistance(const Skeleton& sk, const std::vector<const PoseTrace*>& traces) {
    pelvis_ = sk.find("mPelvis") >= 0;
    wsum_ = pelvis_ ? 4 : 0;  // the pelvis height
    // Joints that move somewhere in the traces (or differ between them); still ones would only dilute the average.
    const PoseTrace* first = nullptr;
    for (const PoseTrace* t : traces)
        if (t && !t->rot.empty()) first = first ? first : t;
    if (!first) return;
    for (int b = 0; b < sk.joint_count(); ++b) {
        const double w = joint_weight(sk[b]);
        if (w <= 0) continue;
        bool moves = false;
        for (const PoseTrace* t : traces)
            for (size_t f = 0; t && f < t->rot.size() && !moves; ++f)
                moves = std::fabs(t->rot[f][b].dot(first->rot[0][b])) < 1 - 1e-9;
        if (moves) joints_.emplace_back(b, w), wsum_ += w;
    }
}

double PoseDistance::operator()(const PoseTrace& a, int fa, const PoseTrace& b, int fb) const {
    if (wsum_ <= 0) return 0;
    const int i = fa - a.from, j = fb - b.from;
    const double dh = a.height[i] - b.height[j];
    double d = pelvis_ ? 4 * dh * dh : 0;
    for (const auto& [bone, w] : joints_) {
        const double ang = std::acos(std::min(1.0, std::fabs(a.rot[i][bone].dot(b.rot[j][bone])))) * 2 * kDeg;
        const Vec3 dv = a.vel[i][bone] - b.vel[j][bone];
        d += w * (ang * ang + dv.dot(dv));
    }
    return std::sqrt(d / wsum_);
}

std::vector<LoopCandidate> find_loop_points(const Rig& rig, const Clip& clip, int min_length, int count,
                                            const Shape* shape) {
    const int n = std::max(clip.end_frame, 0) + 1;
    min_length = std::max(min_length, 1);
    std::vector<LoopCandidate> out;
    if (n <= min_length) return out;
    const PoseTrace t = pose_trace(rig, clip, 0, n - 1, shape);
    const PoseDistance dist(rig.skeleton(), {&t});
    if (!dist.any()) return out;
    auto distance = [&](int i, int j) { return dist(t, i, t, j); };
    // ponytail: every pair, O(frames^2 x joints); a coarse-to-fine search if minute-long clips get slow.
    std::vector<LoopCandidate> all;
    for (int i = 0; i + min_length < n; ++i) {
        std::vector<double> row(n, 0);
        for (int j = i + min_length; j < n; ++j) row[j] = distance(i, j);
        for (int j = i + min_length; j < n; ++j)  // only the valleys along j: the rest are worse neighbours
            if ((j == i + min_length || row[j] <= row[j - 1]) && (j == n - 1 || row[j] <= row[j + 1]))
                all.push_back({i, j, row[j]});
    }
    std::sort(all.begin(), all.end(), [](const LoopCandidate& a, const LoopCandidate& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.length() < b.length();
    });
    for (const LoopCandidate& c : all) {
        if (int(out.size()) >= count) break;
        bool near = false;
        for (const LoopCandidate& o : out) near = near || (std::abs(o.in - c.in) <= 3 && std::abs(o.out - c.out) <= 3);
        if (!near) out.push_back(c);
    }
    return out;
}

BeatFit fit_to_beats(double bpm, int fps, int beats) {
    BeatFit r;
    if (!(bpm > 0) || fps <= 0 || beats <= 0) return r;
    r.seconds = beats * 60.0 / bpm;
    r.frames = std::max(1, int(std::lround(r.seconds * fps)));
    r.residual_ms = (double(r.frames) / fps - r.seconds) * 1000;
    if (std::fabs(r.residual_ms) > 1e-6) r.loops_to_drift = long(std::ceil(1000.0 / fps / std::fabs(r.residual_ms) - 1e-9));
    auto whole = [&](int f) {
        const double x = 60.0 * f / bpm;
        return std::fabs(x - std::round(x)) < 1e-6;
    };
    for (int d = 0; d <= 50 && !r.suggested_fps; ++d)
        for (int f : {fps + d, fps - d})
            if (!r.suggested_fps && f >= 10 && f <= 60 && whole(f)) r.suggested_fps = f;
    return r;
}

void stretch_loop(Clip& clip, int frames) {
    const LoopRange r = loop_range(clip);
    scale_time(clip, r.in, r.out, std::max(frames, 1));
}

int apply_loop_tangents(Clip& clip) {
    if (!clip.loop || !clip.loop_tangents || clip.loop_out <= clip.loop_in) return 0;
    const double a = clip.loop_in, b = clip.loop_out, len = b - a;
    int changed = 0;
    for (auto& [name, track] : clip.curves)
        for (auto& [ch, c] : track) {
            const int ia = c.find(a), ib = c.find(b);
            if (ia < 0 || ib < 0 || ib - ia < 2) continue;
            const double jump = c.keys[ib].value - c.keys[ia].value;
            // The key's handles as FCurve computes them with these neighbours (three keys, the middle one is it).
            auto fix = [&](int i, const Key& prev, const Key& next) {
                Key& k = c.keys[i];
                if (!is_auto(k.left) && !is_auto(k.right)) return;
                FCurve tmp;
                tmp.keys = {prev, k, next};
                tmp.recompute_handles();
                const Key& m = tmp.keys[1];
                if (std::fabs(m.lx - k.lx) + std::fabs(m.ly - k.ly) + std::fabs(m.rx - k.rx) + std::fabs(m.ry - k.ry) < 1e-9)
                    return;
                if (is_auto(k.left)) k.lx = m.lx, k.ly = m.ly;
                if (is_auto(k.right)) k.rx = m.rx, k.ry = m.ry;
                ++changed;
            };
            const Key before_out = c.keys[ib - 1], after_in = c.keys[ia + 1];
            fix(ia, shifted(before_out, -len, -jump), after_in);
            fix(ib, before_out, shifted(after_in, len, jump));
        }
    return changed;
}

Gait measure_gait(const Rig& rig, const Clip& clip, const Shape* shape) {
    Gait g;
    const LoopRange r = loop_range(clip);
    FootLockOptions o;
    o.heel_toe = false;  // the ankle alone: its travel is what the gait measures
    o.speed = 1e9;  // an in-place cycle's planted foot slides at the walk speed: height alone tells a contact
    o.from = r.in, o.to = r.out, o.shape = shape;
    const std::vector<FootContact> contacts = find_foot_contacts(rig, clip, o);
    const int pelvis = rig.skeleton().find("mPelvis");
    const double fps = std::max(clip.fps, 1);
    double dist = 0, time = 0;
    std::map<int, int> starts;  // per leg: contacts that start inside the loop (not carried over its start)
    for (const FootContact& c : contacts) {
        if (c.from > r.in) ++starts[c.limb];
        if (c.to <= c.from || pelvis < 0) continue;
        const int ankle = rig.limbs()[c.limb].end;
        const Evaluation e0 = evaluate(rig, clip, c.from, shape), e1 = evaluate(rig, clip, c.to, shape);
        const Vec3 rel = (e1.globals[pelvis].pos - e0.globals[pelvis].pos) - (e1.globals[ankle].pos - e0.globals[ankle].pos);
        dist += std::hypot(rel.x, rel.y);
        time += (c.to - c.from) / fps;
    }
    g.contacts = int(contacts.size());
    if (time <= 0) return g;
    int cycles = 1;
    for (auto& [limb, k] : starts) cycles = std::max(cycles, k);
    g.speed = dist / time;
    g.cycle = (r.out - r.in) / fps / cycles;
    g.stride = g.speed * g.cycle;
    return g;
}

int match_speed_by_time(Clip& clip, const Gait& g, double target) {
    const LoopRange r = loop_range(clip);
    if (!(g.speed > 0) || !(target > 0)) return r.out - r.in;
    const int frames = std::max(1, int(std::lround((r.out - r.in) * g.speed / target)));
    stretch_loop(clip, frames);
    return frames;
}

bool match_speed_by_travel(Clip& clip, double target, const std::string& hip) {
    const Travel t = remove_travel(clip, hip);
    if (t.speed() < 1e-3) {
        add_travel(clip, t, hip);  // put back whatever tiny drift there was
        return false;
    }
    const double k = target / t.speed();
    add_travel(clip, {t.vx * k, t.vy * k}, hip);
    return true;
}

}  // namespace vats
