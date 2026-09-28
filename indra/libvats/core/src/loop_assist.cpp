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

std::vector<LoopCandidate> find_loop_points(const Rig& rig, const Clip& clip, int min_length, int count,
                                            const Shape* shape) {
    const Skeleton& sk = rig.skeleton();
    const int n = std::max(clip.end_frame, 0) + 1;
    min_length = std::max(min_length, 1);
    std::vector<LoopCandidate> out;
    if (n <= min_length) return out;

    std::vector<std::vector<Quat>> rot(n);
    std::vector<double> height(n);
    const int pelvis = sk.find("mPelvis");
    for (int f = 0; f < n; ++f) {
        Evaluation e = evaluate(rig, clip, f, shape);
        rot[f] = std::move(e.pose.rot);
        height[f] = pelvis >= 0 ? e.globals[pelvis].pos.z * 100 : 0;  // centimetres, compared like degrees
    }
    // Joints that move somewhere in the clip; still ones would only dilute the average.
    std::vector<std::pair<int, double>> joints;
    double wsum = pelvis >= 0 ? 4 : 0;  // the pelvis height
    for (int b = 0; b < sk.joint_count(); ++b) {
        const double w = joint_weight(sk[b]);
        if (w <= 0) continue;
        bool moves = false;
        for (int f = 1; f < n && !moves; ++f) moves = std::fabs(rot[f][b].dot(rot[0][b])) < 1 - 1e-9;
        if (moves) joints.emplace_back(b, w), wsum += w;
    }
    if (wsum <= 0) return out;
    // Angular velocity per joint and frame, in degrees per 0.1 s (so the frame rate does not matter).
    const double per = std::max(clip.fps, 1) * 0.1;
    std::vector<std::vector<Vec3>> vel(n, std::vector<Vec3>(joints.size()));
    for (int f = 0; f < n; ++f) {
        const int f0 = std::max(f - 1, 0), f1 = std::min(f + 1, n - 1);
        for (size_t k = 0; k < joints.size(); ++k) {
            const int b = joints[k].first;
            vel[f][k] = rotvec(rot[f1][b] * rot[f0][b].conj()) * (per / std::max(f1 - f0, 1));
        }
    }
    auto distance = [&](int i, int j) {
        double d = pelvis >= 0 ? 4 * (height[i] - height[j]) * (height[i] - height[j]) : 0;
        for (size_t k = 0; k < joints.size(); ++k) {
            const int b = joints[k].first;
            const double ang = std::acos(std::min(1.0, std::fabs(rot[i][b].dot(rot[j][b])))) * 2 * kDeg;
            const Vec3 dv = vel[i][k] - vel[j][k];
            d += joints[k].second * (ang * ang + dv.dot(dv));
        }
        return std::sqrt(d / wsum);
    };
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
