// Viewport Avatar Toolset - balance (spec 08 section 21, CM-1..CM-2).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/balance.h"

#include <algorithm>
#include <map>
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
    bool is_hind = false;
};

std::vector<Foot> avatar_feet(const Skeleton& skel, const Shape* shape,
                             const std::function<bool(int)>* is_weighted) {
    const Foot candidates[4] = {
        {skel.find("mAnkleLeft"), skel.find("mFootLeft"), skel.find("mToeLeft"), false},
        {skel.find("mAnkleRight"), skel.find("mFootRight"), skel.find("mToeRight"), false},
        {skel.find("mHindLimb3Left"), skel.find("mHindLimb4Left"), -1, true},
        {skel.find("mHindLimb3Right"), skel.find("mHindLimb4Right"), -1, true}
    };
    std::vector<Foot> result;
    if (is_weighted && *is_weighted) {
        for (const Foot& f : candidates) {
            if (f.is_hind) {
                if ((f.foot >= 0 && (*is_weighted)(f.foot)) || (f.ankle >= 0 && (*is_weighted)(f.ankle)))
                    result.push_back(f);
            } else {
                if ((f.ankle >= 0 && (*is_weighted)(f.ankle)) ||
                    (f.foot >= 0 && (*is_weighted)(f.foot)) ||
                    (f.toe >= 0 && (*is_weighted)(f.toe)))
                    result.push_back(f);
            }
        }
    } else {
        // Fallback when no weighting predicate is provided:
        for (size_t i = 0; i < 2; ++i) {
            if (candidates[i].ankle >= 0) result.push_back(candidates[i]);
        }
        if (shape) {
            for (size_t i = 2; i < 4; ++i) {
                const Foot& f = candidates[i];
                const int n = f.foot >= 0 ? f.foot : f.ankle;
                if (n >= 0 && (has_rig_axes(shape, n) ||
                               (n < static_cast<int>(shape->offset.size()) && shape->offset[n].length() > 1e-4) ||
                               (n < static_cast<int>(shape->tails.size()) && shape->tails[n].length() > 1e-4))) {
                    result.push_back(f);
                }
            }
        }
    }
    if (result.empty()) {
        if (candidates[0].ankle >= 0) result.push_back(candidates[0]);
        if (candidates[1].ankle >= 0) result.push_back(candidates[1]);
    }
    return result;
}

Vec3 foot_tip(const Foot& f, const Skeleton& skel, const std::vector<Xform>& g, const Shape* shape) {
    if (f.is_hind) {
        const int n = f.foot >= 0 ? f.foot : f.ankle;
        Vec3 tail = (shape && n < static_cast<int>(shape->tails.size()) && shape->tails[n].length() > 1e-4)
                        ? shape->tails[n]
                        : skel[n].end;
        if (tail.length() < 1e-4) tail = Vec3{0.12, 0, 0};
        return g[n].apply(tail);
    }
    const int tip_node = f.toe >= 0 ? f.toe : f.foot >= 0 ? f.foot : f.ankle;
    return g[tip_node].pos;
}

Vec3 foot_heel(const Foot& f, const std::vector<Xform>& g) {
    const int heel_node = f.is_hind ? (f.foot >= 0 ? f.foot : f.ankle) : f.ankle;
    return g[heel_node].pos;
}

double lowest(const Foot& f, const Skeleton& skel, const std::vector<Xform>& g, const Shape* shape) {
    double z = 1e9;
    if (f.is_hind) {
        const int n = f.foot >= 0 ? f.foot : f.ankle;
        if (n >= 0 && n < static_cast<int>(g.size())) {
            z = std::min(z, g[n].pos.z);
            z = std::min(z, foot_tip(f, skel, g, shape).z);
        }
    } else {
        for (int n : {f.ankle, f.foot, f.toe})
            if (n >= 0 && n < static_cast<int>(g.size())) z = std::min(z, g[n].pos.z);
    }
    return z;
}

}  // namespace

MeshContactFloor compute_mesh_contact_floor(const std::vector<std::span<const float>>& mesh_parts,
                                            double threshold) {
    MeshContactFloor out;
    double lowest_z = 1e30;
    bool has_mesh = false;
    for (const auto& part : mesh_parts) {
        const float* p = part.data();
        const size_t n = part.size();
        if (n >= 3) has_mesh = true;
        for (size_t i = 2; i < n; i += 3) {
            if (p[i] < lowest_z) lowest_z = p[i];
        }
    }
    if (!has_mesh || lowest_z >= 1e29) return out;

    out.has_mesh = true;
    out.lowest_z = lowest_z;
    // Feet, not a single lowest point: a body stands on several feet whose soles are rarely level (a creature's
    // front feet a few cm above its hind ones). Vertices near the bottom are grouped by where they are on the ground
    // (5 cm cells, joined to their neighbours); a group whose own lowest point is within `planted` of the lowest
    // overall is a planted foot, and its sole (the vertices within threshold of the group's own lowest) is contact.
    // ponytail: grid clustering by position, not by skin weights; a foot touching another merges with it, fine here.
    constexpr double band = 0.15, cell = 0.05, planted = 0.06;
    struct Low { double x, y, z; };
    std::vector<Low> lows;
    for (const auto& part : mesh_parts) {
        const float* p = part.data();
        for (size_t i = 0; i + 2 < part.size(); i += 3)
            if (p[i + 2] <= lowest_z + band) lows.push_back({p[i], p[i + 1], p[i + 2]});
    }
    auto key = [&](double v) { return static_cast<long long>(std::floor(v / cell)); };
    std::map<std::pair<long long, long long>, int> cells;  // cell -> group
    std::vector<int> parent;
    auto find = [&](int g) { while (parent[g] != g) g = parent[g] = parent[parent[g]]; return g; };
    for (const Low& l : lows) {
        const auto c = std::make_pair(key(l.x), key(l.y));
        if (cells.count(c)) continue;
        const int g = static_cast<int>(parent.size());
        parent.push_back(g);
        cells[c] = g;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (auto it = cells.find({c.first + dx, c.second + dy}); it != cells.end()) parent[find(it->second)] = g;
    }
    std::vector<int> group(lows.size());
    std::map<int, double> group_low;
    for (size_t i = 0; i < lows.size(); ++i) {
        group[i] = find(cells[{key(lows[i].x), key(lows[i].y)}]);
        auto [it, fresh] = group_low.emplace(group[i], lows[i].z);
        if (!fresh) it->second = std::min(it->second, lows[i].z);
    }
    for (size_t i = 0; i < lows.size(); ++i) {
        const double sole = group_low[group[i]];
        if (sole <= lowest_z + planted && lows[i].z <= sole + threshold)
            out.contact_points.push_back({lows[i].x, lows[i].y, lowest_z});
    }
    return out;
}

Balance balance_of(const Skeleton& skel, const std::vector<Xform>& g, const Shape* shape,
                   const std::function<bool(int)>* is_weighted,
                   const std::vector<std::span<const float>>& mesh_parts,
                   const MeshContactFloor* cached_floor) {
    Balance out;
    if (g.empty() || skel.size() == 0) return out;

    RagdollSolver body(skel, Ragdoll{}, {});
    body.reset(g, g, 1.0);
    out.com = body.centre_of_mass();

    const MeshContactFloor local_floor = cached_floor ? MeshContactFloor{} : compute_mesh_contact_floor(mesh_parts);
    const MeshContactFloor& floor = cached_floor ? *cached_floor : local_floor;

    if (floor.has_mesh) {
        const std::vector<Foot> feet = avatar_feet(skel, shape, is_weighted);
        const std::vector<Xform> rest = skel.global_pose(Pose(skel.size()), shape);
        double bone_rest_ground = 1e9;
        for (const Foot& f : feet) bone_rest_ground = std::min(bone_rest_ground, lowest(f, skel, rest, shape));

        double bone_current_ground = 1e9;
        for (const Foot& f : feet) bone_current_ground = std::min(bone_current_ground, lowest(f, skel, g, shape));

        if (bone_rest_ground < 1e8 && bone_current_ground > bone_rest_ground + kPlanted) {
            return out;
        }

        if (!floor.contact_points.empty()) {
            out.contact = true;
            out.ground = {out.com.x, out.com.y, floor.lowest_z};
            out.support = hull(floor.contact_points);
            out.margin = inside_by(out.support, out.ground);
            return out;
        }
    }

    const std::vector<Foot> feet = avatar_feet(skel, shape, is_weighted);
    const std::vector<Xform> rest = skel.global_pose(Pose(skel.size()), shape);
    double ground = 1e9;
    for (const Foot& f : feet) ground = std::min(ground, lowest(f, skel, rest, shape));

    std::vector<Vec3> prints;
    for (const Foot& f : feet) {
        if (lowest(f, skel, g, shape) > ground + kPlanted) continue;
        // The footprint: a rectangle from behind the ankle to the toe, along the foot as seen from above.
        const Vec3 heel = foot_heel(f, g), tip = foot_tip(f, skel, g, shape);
        Vec3 along = Vec3{tip.x - heel.x, tip.y - heel.y, 0};
        along = along.length() > 1e-6 ? along.normalized() : Vec3{1, 0, 0};
        const Vec3 side{-along.y, along.x, 0}, back{heel.x, heel.y, ground}, front{tip.x, tip.y, ground};
        for (const Vec3& e : {back - along * kHeel, front})
            prints.push_back(e + side * kHalfWidth), prints.push_back(e - side * kHalfWidth);
    }
    if (prints.empty()) return out;
    out.contact = true;
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
            const std::vector<Xform> g = evaluate(rig, clip, a + i, opt.shape).globals;
            const MeshContactFloor floor = opt.mesh_floor ? opt.mesh_floor(g) : MeshContactFloor{};
            Balance bal = balance_of(skel, g, opt.shape, opt.is_weighted ? &opt.is_weighted : nullptr,
                                     std::vector<std::span<const float>>{}, &floor);
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
                      outside ? (", " + std::to_string(outside) + (outside == 1 ? " frame" : " frames") + " still off balance").c_str() : "");
    return buf;
}

}  // namespace vats
