// Viewport Avatar Toolset - procedural idle layers.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/idle.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "vats/dynamics.h"
#include "vats/loop_tools.h"

namespace vats {
namespace {

std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b) { return mix(a ^ mix(b + 0x9e3779b9u)); }

// 2D gradient noise with unit gradients at random angles; |value| <= sqrt(2)/2.
double gradient_noise(double x, double y, std::uint32_t seed) {
    const double fx = std::floor(x), fy = std::floor(y);
    const double dx = x - fx, dy = y - fy;
    const auto ix = std::uint32_t(std::int64_t(fx)), iy = std::uint32_t(std::int64_t(fy));
    auto grad = [&](std::uint32_t cx, std::uint32_t cy, double ox, double oy) {
        const double a = mix(mix(cx, cy), seed) * (2 * kPi / 4294967296.0);
        return std::cos(a) * ox + std::sin(a) * oy;
    };
    auto fade = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    const double u = fade(dx), v = fade(dy);
    const double a = grad(ix, iy, dx, dy), b = grad(ix + 1, iy, dx - 1, dy);
    const double c = grad(ix, iy + 1, dx, dy - 1), d = grad(ix + 1, iy + 1, dx - 1, dy - 1);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

bool is_breath(const IdleLayer& l) { return l.kind == "breath"; }

}  // namespace

IdleLayer idle_preset(const std::string& kind) {
    IdleLayer l;
    l.kind = kind;
    if (kind == "breath") {
        l.amplitude = 1.5, l.period = 4.0;
        l.bones = {"mChest", "mTorso"};
    } else {
        l.kind = "sway", l.amplitude = 1.0, l.period = 5.0;
        l.bones = {"mChest", "mTorso", "mHead", "mPelvis", "mCollarLeft", "mCollarRight"};
    }
    return l;
}

bool idle_bone_allowed(const Node& n) {
    return !n.attachment && !n.volume && n.name.rfind("mFace", 0) != 0 && n.name.rfind("mEye", 0) != 0;
}

double idle_period_frames(const Clip& clip, const IdleLayer& layer) {
    const LoopRange r = loop_range(clip);
    const double len = r.out - r.in;
    const double wanted = std::max(layer.period, 1e-3) * std::max(clip.fps, 1);
    return len / std::max(1.0, std::round(len / wanted));
}

std::vector<int> idle_nodes(const Skeleton& skel, const IdleLayer& layer) {
    std::vector<int> out;
    for (const std::string& b : layer.bones)
        if (int n = skel.find(b); n >= 0 && idle_bone_allowed(skel[n]) && std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(n);
    return out;
}

void apply_idle(const Skeleton& skel, const Clip& clip, const IdleLayer& layer, double frame, Pose& pose,
                const std::vector<int>& exclude) {
    const LoopRange r = loop_range(clip);
    const double len = r.out - r.in;
    const double cycles = std::round(len / idle_period_frames(clip, layer));  // a whole number (IL-2)
    const double theta = 2 * kPi * (frame - r.in) / len;                     // once round the circle per loop
    const double amp = layer.amplitude * kDegToRad;
    for (int n : idle_nodes(skel, layer)) {
        if (std::find(exclude.begin(), exclude.end(), n) != exclude.end()) continue;
        if (is_breath(layer)) {
            // In from rest and back out: 0 at the loop's start, full at half a breath.
            const double in = 0.5 - 0.5 * std::cos(theta * cycles);
            if (skel[n].name == "mTorso") pose.offset[n].z += layer.amplitude * 0.001 * in;
            else pose.rot[n] = (Quat::axis_angle({0, 1, 0}, -amp * in) * pose.rot[n]).normalized();  // chest lifts back
            continue;
        }
        // Noise read around a circle whose circumference is `cycles` lattice cells, so loop-in and loop-out
        // are the same point and the features come about one period apart.
        const double r = cycles / (2 * kPi);
        Vec3 v;
        for (int axis = 0; axis < 3; ++axis) {
            const std::uint32_t s = mix(mix(std::uint32_t(layer.seed), std::uint32_t(axis)), std::uint32_t(n));
            // A random centre per bone and axis, far from the others, keeps the curves unrelated.
            const double cx = (s & 0xffff) * 0.37 + 0.5, cy = (s >> 16) * 0.37 + 0.5;
            const double value = gradient_noise(cx + r * std::cos(theta), cy + r * std::sin(theta), s) * std::sqrt(2.0);
            (axis == 0 ? v.x : axis == 1 ? v.y : v.z) = value;
        }
        // Each axis reaches +-1; the vector is clamped to length 1 so the turn never exceeds the amplitude.
        const double len = v.length();
        if (len > 1) v = v * (1 / len);
        if (len > 1e-12) pose.rot[n] = (Quat::axis_angle(v.normalized(), amp * v.length()) * pose.rot[n]).normalized();
    }
}

namespace {

void restore(Clip& clip, const Skeleton& skel, IdleLayer& l) {
    for (int n : idle_nodes(skel, l)) {
        auto it = l.source.find(skel[n].name);
        if (it == l.source.end()) clip.curves.erase(skel[n].name);
        else clip.curves[it->first] = it->second;
    }
    l.source.clear();
    l.baked = false;
}

void bake_one(Clip& clip, const Skeleton& skel, IdleLayer& l) {
    const std::vector<int> nodes = idle_nodes(skel, l);
    for (int n : nodes)
        if (auto t = clip.curves.find(skel[n].name); t != clip.curves.end()) l.source[t->first] = t->second;
    std::vector<Pose> frames;
    for (int f = 0; f <= std::max(clip.end_frame, 0); ++f) {
        frames.push_back(evaluate_curves(skel, clip, f));
        apply_idle(skel, clip, l, f, frames.back());
    }
    std::vector<int> rot, pos;  // a breath only moves mTorso, so its rotation keys are left alone
    for (int n : nodes) (is_breath(l) && skel[n].name == "mTorso" ? pos : rot).push_back(n);
    // Tighter than the dynamics default: idle motion is a degree or two, and 0.1 degree would be 10% of it.
    bake_samples(clip, skel, rot, frames, 0.01, 0.00002);
    bake_samples(clip, skel, pos, frames, 0.01, 0.00002, pos, /*position_only=*/true);
    l.baked = true;
}

// Baked layers always sit on the curves in list order, each source holding the keys from before it, so a
// layer that shares bones with another can be baked or unbaked alone: every baked layer is unbaked newest
// first, then the wanted ones are baked again oldest first.
void rebake(Clip& clip, const Skeleton& skel, const std::vector<bool>& want) {
    for (int i = int(clip.idle.size()) - 1; i >= 0; --i)
        if (clip.idle[i].baked) restore(clip, skel, clip.idle[i]);
    for (size_t i = 0; i < clip.idle.size(); ++i)
        if (want[i]) bake_one(clip, skel, clip.idle[i]);
}

}  // namespace

void bake_idle(Clip& clip, const Skeleton& skel, int which) {
    std::vector<bool> want;
    for (int i = 0; i < int(clip.idle.size()); ++i) want.push_back(clip.idle[i].baked || which < 0 || i == which);
    rebake(clip, skel, want);
}

void unbake_idle(Clip& clip, const Skeleton& skel, int which) {
    if (which < 0 || which >= int(clip.idle.size()) || !clip.idle[which].baked) return;
    std::vector<bool> want;
    for (const IdleLayer& l : clip.idle) want.push_back(l.baked);
    want[which] = false;
    rebake(clip, skel, want);
}

}  // namespace vats
