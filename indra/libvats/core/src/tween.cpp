// Viewport Avatar Toolset - tween (breakdown) keys, pose blend on apply and easing presets.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/tween.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "vats/edit.h"

namespace vats {
namespace {

constexpr const char* kPoleChannels[3] = {"pole_x", "pole_y", "pole_z"};

// ponytail: third copy (rig.cpp, mocap.cpp keep theirs private); move one into math.h when a fourth appears.
// Works for t outside [0, 1] too: it keeps turning along the same arc.
Quat slerp(const Quat& a, Quat b, double t) {
    if (a.dot(b) < 0) b = -b;
    const double c = std::min(1.0, a.dot(b));
    if (c > 0.9995) return nlerp(a, b, t);
    const double th = std::acos(c), s = std::sin(th);
    const double wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return Quat{a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb}.normalized();
}

bool is_ik(const std::string& track) { return track.rfind("ik.", 0) == 0; }

Vec3 sample(const Clip& clip, const std::string& track, const char* const (&names)[3], double frame) {
    Vec3 v;
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return v;
    for (int a = 0; a < 3; ++a)
        if (auto c = t->second.find(names[a]); c != t->second.end()) v[a] = c->second.evaluate(frame);
    return v;
}

// Frames with a key on any channel of the track but an IK blend (a stepped switch, not a pose).
std::vector<double> pose_frames(const Clip& clip, const std::string& track) {
    std::vector<double> out;
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return out;
    for (auto& [ch, curve] : t->second)
        if (ch != "blend")
            for (const Key& k : curve.keys) out.push_back(k.frame);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return same_frame(a, b); }), out.end());
    return out;
}

// Keys track at frame, t of the way from a's pose at fa to b's at fb: rotation, position and pole groups.
// A group b lacks is left alone. A group a lacks is rest (zero) on a bone; on an IK controller it means
// "follow the end bone" or "derive the pole", which has no value to blend from, so b's is keyed as it is.
void key_mix(Clip& out, const std::string& track, double frame, const Clip& a, double fa, const Clip& b, double fb,
             double t) {
    const bool ik = is_ik(track);
    struct Keyed {
        const char* const* names;
        Vec3 v;
    };
    std::vector<Keyed> keyed;
    for (const char* const(*g)[3] : {&kRotChannels, &kPosChannels, &kPoleChannels}) {
        if (!b.has_channels(track, *g)) continue;
        const double w = ik && !a.has_channels(track, *g) ? 1.0 : t;
        const Vec3 va = sample(a, track, *g, fa), vb = sample(b, track, *g, fb);
        if (g == &kRotChannels) {
            // Slerp, then the Euler triple nearest what the curve holds at frame (AM-30).
            Quat q = slerp(euler_to_quat(va), euler_to_quat(vb), w);
            keyed.push_back({*g, nearest_euler(q, sample(out, track, *g, frame))});
        } else {
            keyed.push_back({*g, va + (vb - va) * w});
        }
    }
    Track& tr = out.curves[track];  // every value first: out may be a or b
    for (const Keyed& k : keyed)
        for (int i = 0; i < 3; ++i) tr[k.names[i]].set_key(frame, k.v[i]);
}

}  // namespace

std::vector<std::string> tween_tracks(const Rig& rig, const Clip& clip, double frame,
                                      const std::vector<std::string>& tracks) {
    std::vector<std::string> out;
    for (const std::string& name : tracks) {
        std::string t = name;
        if (!is_ik(name) && name.rfind("pin:", 0) != 0) {
            const int limb = rig.limb_of_bone(rig.skeleton().find(name));
            if (limb >= 0) {
                const std::string ik = "ik." + rig.limbs()[limb].name;
                auto tr = clip.curves.find(ik);
                if (tr != clip.curves.end())
                    if (auto b = tr->second.find("blend"); b != tr->second.end() && b->second.evaluate(frame) >= 0.5) t = ik;
            }
        }
        if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    return out;
}

int tween(Clip& clip, const std::vector<std::string>& tracks, double frame, double t, TweenMode mode) {
    t = std::clamp(t, kTweenMin, kTweenMax);
    int n = 0;
    for (const std::string& track : tracks) {
        const std::vector<double> frames = pose_frames(clip, track);
        if (mode == TweenMode::Breakdown) {
            const double* prev = nullptr;
            const double* next = nullptr;
            for (const double& f : frames) {
                if (same_frame(f, frame)) continue;
                if (f < frame) prev = &f;
                else if (!next) next = &f;
            }
            if (!prev || !next) continue;
            key_mix(clip, track, frame, clip, *prev, clip, *next, t);
        } else {
            const bool keyed = std::any_of(frames.begin(), frames.end(), [&](double f) { return same_frame(f, frame); });
            if (!keyed || frames.size() < 2) continue;
            Clip without;  // the track as its neighbours would draw it with no key at frame
            without.curves[track] = clip.curves.at(track);
            for (auto& [ch, curve] : without.curves[track])
                if (ch != "blend") curve.remove_key(frame);
            key_mix(clip, track, frame, clip, frame, without, frame, t);
        }
        ++n;
    }
    return n;
}

int blend_pose(Clip& clip, const Clip& before, const Clip& after, double frame, double amount) {
    int n = 0;
    for (const auto& [name, track] : after.curves) {
        auto old = before.curves.find(name);
        if (old != before.curves.end() && old->second == track) continue;
        clip.curves[name] = track;  // back to as applied, then keyed at the blend (100% is as applied, exactly)
        if (amount != 1) key_mix(clip, name, frame, before, frame, after, frame, amount);
        ++n;
    }
    return n;
}

// Easing (TW-3). The curves are written here from their definitions, not taken from a library.

namespace {

// Back: t^2 ((s + 1) t - s). Its lowest point is -4 s^3 / (27 (s + 1)^2); s = 1.70158 makes that -10%.
constexpr double kBack = 1.70158;

// A spring settling on 1: three wobbles under an exponential decay, rescaled to end exactly on 1.
double elastic_out(double t) {
    const double floor = std::exp2(-10.0);
    const double decay = (std::exp2(-10 * t) - floor) / (1 - floor);
    return 1 - decay * std::cos(6 * kPi * t);
}

// A ball dropped onto 1 that bounces three times, keeping half its speed each time: the drop is x^2 and
// each bounce a parabola of the same curvature, half as wide as the one before.
double bounce_out(double t) {
    constexpr double r = 0.5;
    constexpr int bounces = 3;
    double total = 1, w = 1;
    for (int k = 0; k < bounces; ++k) w *= r, total += 2 * w;
    const double x = t * total;
    if (x < 1) return x * x;
    double start = 1;
    w = 1;
    for (int k = 1; k <= bounces; ++k) {
        w *= r;
        if (x <= start + 2 * w || k == bounces) {
            const double u = x - (start + w);
            return std::min(1.0, 1 - w * w + u * u);
        }
        start += 2 * w;
    }
    return 1;
}

double ease_in(EaseShape s, double t) {
    switch (s) {
        case EaseShape::Quad: return t * t;
        case EaseShape::Cubic: return t * t * t;
        case EaseShape::Sine: return 1 - std::cos(t * kPi / 2);
        case EaseShape::Back: return t * t * ((kBack + 1) * t - kBack);
        case EaseShape::Elastic: return 1 - elastic_out(1 - t);
        case EaseShape::Bounce: return 1 - bounce_out(1 - t);
    }
    return t;
}

// The two inner control points of a unit Bezier (from (0,0) to (1,1)) that draws the shape. With the x
// of the points at thirds, x runs linearly in the curve parameter, so t^2 and t^3 come out exact; Sine
// matches the end slopes (0 and pi/2). In-Out keeps both ends flat and matches the slope at the middle,
// m (2, 3, pi/2), with the points at x = a and 1 - a, a = 1 - 1/m.
std::pair<Vec3, Vec3> unit_handles(EaseShape s, EaseDir d) {
    if (d == EaseDir::InOut) {
        const double m = s == EaseShape::Quad ? 2 : s == EaseShape::Cubic ? 3 : kPi / 2;
        const double a = 1 - 1 / m;
        return {{a, 0, 0}, {1 - a, 1, 0}};
    }
    const Vec3 p1{1.0 / 3, 0, 0};
    const Vec3 p2{2.0 / 3, s == EaseShape::Quad ? 1.0 / 3 : s == EaseShape::Cubic ? 0.0 : 1 - kPi / 6, 0};
    if (d == EaseDir::In) return {p1, p2};
    return {{1 - p2.x, 1 - p2.y, 0}, {1 - p1.x, 1 - p1.y, 0}};  // Out: the In curve turned half a turn
}

void shape_segment(FCurve& c, double f0, EaseShape s, EaseDir d) {
    const int i = c.find(f0);
    if (i < 0 || i + 1 >= int(c.keys.size())) return;
    const double v0 = c.keys[i].value, f1 = c.keys[i + 1].frame, v1 = c.keys[i + 1].value;
    const double df = f1 - f0, dv = v1 - v0;
    if (ease_is_baked(s)) {
        c.keys[i].interp = Interp::Linear;
        for (double f = std::floor(f0) + 1; f < f1 && !same_frame(f, f1); ++f)
            c.set_key(f, v0 + dv * ease(s, d, (f - f0) / df), Interp::Linear);
        return;
    }
    auto [p1, p2] = unit_handles(s, d);
    Key& a = c.keys[i];
    Key& b = c.keys[i + 1];
    a.interp = Interp::Bezier;
    a.right = b.left = Handle::Free;
    a.rx = f0 + p1.x * df, a.ry = v0 + p1.y * dv;
    b.lx = f0 + p2.x * df, b.ly = v0 + p2.y * dv;
    c.recompute_handles();
}

}  // namespace

double ease(EaseShape shape, EaseDir dir, double t) {
    t = std::clamp(t, 0.0, 1.0);
    switch (dir) {
        case EaseDir::In: return ease_in(shape, t);
        case EaseDir::Out: return 1 - ease_in(shape, 1 - t);
        case EaseDir::InOut: return t < 0.5 ? ease_in(shape, 2 * t) / 2 : 1 - ease_in(shape, 2 - 2 * t) / 2;
    }
    return t;
}

bool ease_is_baked(EaseShape shape) {
    return shape == EaseShape::Back || shape == EaseShape::Elastic || shape == EaseShape::Bounce;
}

int apply_ease(Clip& clip, std::vector<KeyRef>& sel, EaseShape shape, EaseDir dir) {
    // Selected frames per curve; indices change once keys are baked in, frames do not.
    std::map<std::pair<std::string, std::string>, std::vector<double>> frames;
    std::vector<double> sel_frames;
    for (const KeyRef& s : sel) {
        const double f = clip.curves.at(s.track).at(s.channel).keys.at(s.index).frame;
        frames[{s.track, s.channel}].push_back(f);
        sel_frames.push_back(f);
    }
    int n = 0;
    for (auto& [tc, fs] : frames) {
        FCurve& c = clip.curves[tc.first][tc.second];
        std::sort(fs.begin(), fs.end());
        if (fs.size() > 1) fs.pop_back();  // the last of several ends the eased span
        for (double f : fs) {
            const int i = c.find(f);
            if (i < 0 || i + 1 >= int(c.keys.size())) continue;
            shape_segment(c, f, shape, dir);
            ++n;
        }
    }
    for (size_t k = 0; k < sel.size(); ++k) sel[k].index = clip.curves[sel[k].track][sel[k].channel].find(sel_frames[k]);
    return n;
}

}  // namespace vats
