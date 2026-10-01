// Viewport Avatar Toolset - balance (spec 08 section 21, CM-1..CM-2).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/balance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vats/curve_ops.h"
#include "vats/edit.h"
#include "vats/footlock.h"
#include "vats/ragdoll.h"

namespace vats {
namespace {

constexpr double kPlanted = 0.05;    // m: a foot this close to the ground is planted
constexpr double kHeel = 0.05;       // m: the heel is this far behind the ankle
constexpr double kHalfWidth = 0.045; // m: half a foot's width
constexpr int kPasses = 12;          // auto-balance: hip corrections, the last third unsmoothed

double cross2(const Vec3& o, const Vec3& a, const Vec3& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Andrew's monotone chain, counter-clockwise, collinear points dropped.
std::vector<Vec3> hull(std::vector<Vec3> p) {
    std::sort(p.begin(), p.end(), [](const Vec3& a, const Vec3& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    if (p.size() < 3) return p;
    std::vector<Vec3> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && cross2(h[k - 2], h[k - 1], p[i]) <= 1e-12) --k;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross2(h[k - 2], h[k - 1], p[i - 1]) <= 1e-12) --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    return h;
}

// Signed distance from a convex counter-clockwise polygon's nearest edge line, + inside (exact inside).
double inside_by(const std::vector<Vec3>& poly, const Vec3& p) {
    double d = 1e9;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vec3 &a = poly[i], &b = poly[(i + 1) % poly.size()];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len > 1e-9) d = std::min(d, cross2(a, b, p) / len);
    }
    return d;
}

// The point of the polygon shrunk by margin nearest p (x, y only). The shrunk corners are where the inset edge
// lines meet; a polygon thinner than twice the margin gives its centroid.
Vec3 nearest_inside(const std::vector<Vec3>& poly, const Vec3& p, double margin) {
    const size_t n = poly.size();
    Vec3 centroid;
    for (const Vec3& v : poly) centroid += v * (1.0 / n);
    if (n < 3) return centroid;
    std::vector<Vec3> in(n);
    auto inset = [&](size_t i, Vec3& a, Vec3& d) {  // edge i -> i+1 moved inwards
        const Vec3 &p0 = poly[i], &p1 = poly[(i + 1) % n];
        d = Vec3{p1.x - p0.x, p1.y - p0.y, 0}.normalized();
        a = p0 + Vec3{-d.y, d.x, 0} * margin;
    };
    for (size_t i = 0; i < n; ++i) {
        Vec3 a0, d0, a1, d1;
        inset((i + n - 1) % n, a0, d0);
        inset(i, a1, d1);
        const double den = d0.x * d1.y - d0.y * d1.x;
        // Edges in line (a foot's side meeting the other's) keep the corner moved straight in.
        if (std::fabs(den) < 1e-3) {
            in[i] = a1;
        } else {
            const double t = ((a1.x - a0.x) * d1.y - (a1.y - a0.y) * d1.x) / den;
            in[i] = a0 + d0 * t;
        }
        if (inside_by(poly, in[i]) < margin - 1e-6) return centroid;
    }
    if (inside_by(in, p) >= 0) return p;
    Vec3 best = centroid;
    double best_d = 1e30;
    for (size_t i = 0; i < n; ++i) {
        const Vec3 &a = in[i], &b = in[(i + 1) % n];
        const Vec3 ab{b.x - a.x, b.y - a.y, 0};
        const double t = std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / std::max(ab.dot(ab), 1e-12), 0.0, 1.0);
        const Vec3 c{a.x + ab.x * t, a.y + ab.y * t, 0};
        const double d = std::hypot(p.x - c.x, p.y - c.y);
        if (d < best_d) best_d = d, best = c;
    }
    return best;
}

struct Foot {
    int ankle = -1, foot = -1, toe = -1;
};

Foot foot(const Skeleton& skel, const char* side) {
    return {skel.find(std::string("mAnkle") + side), skel.find(std::string("mFoot") + side), skel.find(std::string("mToe") + side)};
}

double lowest(const Foot& f, const std::vector<Xform>& g) {
    double z = 1e9;
    for (int n : {f.ankle, f.foot, f.toe})
        if (n >= 0) z = std::min(z, g[n].pos.z);
    return z;
}

}  // namespace

Balance balance_of(const Skeleton& skel, const std::vector<Xform>& g, const Shape* shape) {
    Balance out;
    const Foot feet[2] = {foot(skel, "Left"), foot(skel, "Right")};
    const std::vector<Xform> rest = skel.global_pose(Pose(skel.size()), shape);
    double ground = 1e9;
    for (const Foot& f : feet) ground = std::min(ground, lowest(f, rest));
    std::vector<Vec3> prints;
    for (const Foot& f : feet) {
        if (f.ankle < 0 || lowest(f, g) > ground + kPlanted) continue;
        // The footprint: a rectangle from behind the ankle to the toe, along the foot as seen from above.
        const Vec3 heel = g[f.ankle].pos, tip = g[f.toe >= 0 ? f.toe : f.foot >= 0 ? f.foot : f.ankle].pos;
        Vec3 along = Vec3{tip.x - heel.x, tip.y - heel.y, 0};
        along = along.length() > 1e-6 ? along.normalized() : Vec3{1, 0, 0};
        const Vec3 side{-along.y, along.x, 0}, back{heel.x, heel.y, ground}, front{tip.x, tip.y, ground};
        for (const Vec3& e : {back - along * kHeel, front})
            prints.push_back(e + side * kHalfWidth), prints.push_back(e - side * kHalfWidth);
    }
    if (prints.empty()) return out;
    RagdollSolver body(skel, Ragdoll{}, {});
    body.reset(g, g, 1.0);
    out.contact = true;
    out.com = body.centre_of_mass();
    out.ground = {out.com.x, out.com.y, ground};
    out.support = hull(prints);
    out.margin = inside_by(out.support, out.ground);
    return out;
}

std::string auto_balance(Clip& clip, const Rig& rig, const AutoBalanceOptions& opt) {
    const Skeleton& skel = rig.skeleton();
    const int a = std::max(0, opt.from), b = opt.to < 0 ? clip.end_frame : std::min(opt.to, clip.end_frame);
    const int pelvis = skel.find("mPelvis"), torso = skel.find("mTorso"), neck = skel.find("mNeck");
    if (b < a || pelvis < 0) return "Nothing to balance in that range";
    // ponytail: counter-lean keys mTorso as FK, so a spine in IK hides it; key the Spine target if that matters.
    const bool lean = opt.counter_lean && torso >= 0 && neck >= 0;

    // The frames either side are keyed without changing the curves (a split), so only the range changes.
    auto anchor = [&](int f) {
        if (f < 0 || f > clip.end_frame) return;
        for (const char* ch : kPosChannels) insert_on_curve(clip.curves["mPelvis"][ch], f);
        if (lean)
            for (const char* ch : kRotChannels) insert_on_curve(clip.curves["mTorso"][ch], f);
    };
    anchor(a - 1);
    anchor(b + 1);
    const Clip anchored = clip;
    // Planted feet are held by leg IK, as Clean Up Foot Sliding holds them.
    FootLockOptions fl;
    fl.from = a, fl.to = b, fl.shape = opt.shape;
    if (b > a) lock_feet(clip, rig, fl);

    const int n = b - a + 1;
    std::vector<Vec3> base(n), off(n), need(n);
    std::vector<Quat> torso0(n), pelvis_rot(n);
    double spine = 0.4;
    for (int i = 0; i < n; ++i) {
        base[i] = curve_offset(clip, "mPelvis", a + i);
        if (!lean) continue;
        torso0[i] = euler_to_quat(curve_euler(clip, "mTorso", a + i));
        Evaluation e = evaluate(rig, clip, a + i, opt.shape);
        pelvis_rot[i] = e.globals[skel[torso].parent].rot * skel[torso].rest;
        spine = (e.globals[neck].pos - e.globals[torso].pos).length();
    }
    auto apply = [&] {
        for (int i = 0; i < n; ++i) {
            key_offset(clip, "mPelvis", a + i, base[i] + off[i]);
            const double d = std::hypot(off[i].x, off[i].y);
            if (!lean || d < 1e-9) continue;
            // Against the imbalance: the torso tilts the way the hips moved, towards the support, by half the angle
            // their offset makes over the spine's length, so the hips need to move less.
            const Quat r = Quat::axis_angle(Vec3{0, 0, 1}.cross(off[i]), 0.5 * std::atan2(d, spine));
            key_rotation(clip, "mTorso", a + i, (pelvis_rot[i].conj() * r * pelvis_rot[i] * torso0[i]).normalized());
        }
    };

    int outside = 0, contact = 0;
    for (int pass = 0; pass <= kPasses; ++pass) {
        outside = contact = 0;
        for (int i = 0; i < n; ++i) {
            Balance bal = balance_of(skel, evaluate(rig, clip, a + i, opt.shape).globals, opt.shape);
            need[i] = {};
            if (!bal.contact) continue;
            ++contact;
            if (bal.margin >= opt.margin - 1e-4) continue;
            ++outside;
            const Vec3 to = nearest_inside(bal.support, bal.ground, opt.margin);
            need[i] = {to.x - bal.ground.x, to.y - bal.ground.y, 0};
        }
        if (!outside || pass == kPasses) break;
        const int w = pass < kPasses * 2 / 3 ? opt.smooth : 0;
        for (int i = 0; i < n; ++i) {
            Vec3 s;
            const int lo = std::max(0, i - w), hi = std::min(n - 1, i + w);
            for (int k = lo; k <= hi; ++k) s += need[k] * (1.0 / (hi - lo + 1));
            off[i] += s;
        }
        apply();
    }
    keep_outer_handles(clip, anchored, "mPelvis", a - 1, b + 1);
    if (lean) keep_outer_handles(clip, anchored, "mTorso", a - 1, b + 1);
    double most = 0;
    for (const Vec3& o : off) most = std::max(most, std::hypot(o.x, o.y));
    char buf[160];
    if (!contact)
        std::snprintf(buf, sizeof buf, "No foot is on the ground in frames %d-%d", a, b);
    else
        std::snprintf(buf, sizeof buf, "Balanced frames %d-%d: the hips moved up to %.1f cm%s", a, b, most * 100,
                      outside ? (", " + std::to_string(outside) + " frame(s) still off balance").c_str() : "");
    return buf;
}

}  // namespace vats
