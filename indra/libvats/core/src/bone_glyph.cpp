// Viewport Avatar Toolset - the viewport's bone glyphs as plain triangles.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/bone_glyph.h"

#include <algorithm>
#include <cmath>

namespace vats {

void bone_glyph(std::vector<Vec3>& tris, const Vec3& head, const Vec3& tail, const Quat& frame) {
    const Vec3 d = tail - head;
    const double len = d.length();
    if (len < 1e-9) return;
    const Vec3 dir = d * (1.0 / len);
    Vec3 roll = frame.rotate({1, 0, 0});
    if (std::fabs(roll.dot(dir)) > 0.9) roll = frame.rotate({0, 0, 1});
    const Vec3 u = (roll - dir * roll.dot(dir)).normalized(), v = dir.cross(u);  // u, v, dir right-handed
    const Vec3 mid = head + dir * (0.15 * len);
    const double w = 0.09 * len;
    const Vec3 ring[4] = {mid + u * w, mid + v * w, mid - u * w, mid - v * w};  // counter-clockwise about dir
    for (int k = 0; k < 4; ++k) {
        const Vec3 &a = ring[k], &b = ring[(k + 1) % 4];
        tris.insert(tris.end(), {head, b, a, tail, a, b});
    }
}

void joint_ring(std::vector<Vec3>& tris, const Vec3& at, const Vec3& axis, double radius) {
    constexpr int kAround = 20, kTube = 6;
    const Vec3 n = axis.normalized();
    Vec3 u = (std::fabs(n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}).cross(n).normalized();
    const Vec3 v = n.cross(u);
    const double tube = radius * 0.2;
    auto p = [&](int i, int j) {
        const double a = 2 * kPi * i / kAround, b = 2 * kPi * j / kTube;
        const Vec3 out = u * std::cos(a) + v * std::sin(a);  // from the centre to the tube's middle
        return at + out * (radius + tube * std::cos(b)) + n * (tube * std::sin(b));
    };
    for (int i = 0; i < kAround; ++i)
        for (int j = 0; j < kTube; ++j) {
            const int i1 = (i + 1) % kAround, j1 = (j + 1) % kTube;  // the same points where the ring closes
            const Vec3 a = p(i, j), b = p(i1, j), c = p(i1, j1), d = p(i, j1);
            tris.insert(tris.end(), {a, b, c, a, c, d});
        }
}

Vec3 glyph_tail(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape, int i) {
    // A mesh body's rig axes give the bone its own tail (rig_axes_from_parts), already at the body's size.
    if (shape && i < static_cast<int>(shape->tails.size()) && shape->tails[i].length() > 0)
        return globals[i].apply(shape->tails[i]);
    const Vec3 end = shape ? skel[i].end.mul(shape->scale[i]) : skel[i].end;
    return globals[i].apply(end);
}

std::vector<int> glyph_kinds(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape) {
    const int n = skel.size();
    std::vector<int> kind(static_cast<size_t>(n), -1);
    std::vector<Vec3> tail(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        tail[i] = glyph_tail(skel, globals, shape, i);
        if ((tail[i] - globals[i].pos).length() < 1e-5) kind[i] = -2;
    }
    for (int i = 0; i < skel.joint_count(); ++i) {  // attachment points and volumes keep their spikes
        if (kind[i] == -2) continue;
        const Vec3 h = globals[i].pos, t = tail[i];
        const double tol = std::max(1e-3, 0.02 * (t - h).length());
        auto near = [tol](const Vec3& a, const Vec3& b) { return (a - b).length() < tol; };
        bool folded = false;  // on an earlier glyph: an ancestor folded back (mSpine1..4) or a twin (mFaceEyeAlt*)
        for (int j = 0; j < i && !folded; ++j)
            if (kind[j] != -2)
                folded = (near(h, globals[j].pos) && near(t, tail[j])) || (near(h, tail[j]) && near(t, globals[j].pos));
        if (!folded) continue;
        kind[i] = 0;
        for (int j = 0; j < i; ++j)
            if (kind[j] >= 0 && near(globals[j].pos, h)) ++kind[i];
    }
    return kind;
}

}  // namespace vats
