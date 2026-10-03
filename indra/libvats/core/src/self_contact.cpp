// Viewport Avatar Toolset - the self-penetration check (spec 08 section 27).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/self_contact.h"

#include <algorithm>
#include <map>

#include "vats/edit.h"
#include "vats/ragdoll.h"

namespace vats {
namespace {

constexpr double kRestGap = 0.01;  // m: pairs this close in the rest pose meet at a joint and are never checked
constexpr double kMargin = 0.005;  // m: Push Out clears the overlap by this much
constexpr int kPasses = 4;         // Push Out: keying moves the curve around the key, so repeat
constexpr double kThighRadius = 0.06;  // m: the SL default body's thigh reaches this far out from the hip-knee line

}  // namespace

// The ragdoll's capsules are padded for falling (RD-2) and much wider than the body at the hips: the pelvis capsule
// (radius 0.12 m, run out to each hip joint) reaches 0.25 m to the side and the thighs (0.09 m) 0.21 m, where the SL
// default body (Ruth, measured from its mesh) is 0.175 m. An arm hanging at the side sat 5 to 7 cm inside them, so
// every ordinary standing pose was flagged. The check's hull: the pelvis without its segments out to the hips, and
// thighs as slim as the body's outside (0.06 m) starting that far below the hip joint, where Ruth's hip is 0.167 m
// wide and a relaxed forearm rests against it. The pairs are still chosen on the ragdoll's own capsules.
// ponytail: one fitted body (Ruth, female); a mesh body's own hull would need its mesh here.
std::vector<RagdollCapsule> contact_hull(const Skeleton& skel, const std::vector<Xform>& globals) {
    std::vector<RagdollCapsule> caps = ragdoll_capsules(skel, globals);
    for (RagdollCapsule& c : caps) {
        const std::string& n = skel[c.node].name;
        if (n == "mPelvis" && c.segments.size() > 1) {  // keep the one up to the torso
            const Vec3 up = globals[c.node].rot.rotate({0, 0, 1});
            auto upness = [&](const std::pair<Vec3, Vec3>& sg) { return (sg.second - sg.first).normalized().dot(up); };
            c.segments = {*std::max_element(c.segments.begin(), c.segments.end(),
                                            [&](auto& a, auto& b) { return upness(a) < upness(b); })};
        }
        if (n.rfind("mHip", 0) == 0 && !c.segments.empty()) {  // starts a radius below the hip joint: no bulge there
            c.radius = std::min(c.radius, kThighRadius);
            auto& [top, knee] = c.segments.front();
            const double len = (knee - top).length();
            if (len > 2 * c.radius) top += (knee - top) * (c.radius / len);
        }
    }
    return caps;
}

Hit capsule_hit(const RagdollCapsule& A, const RagdollCapsule& B) {
    Hit h;
    for (auto& [p1, q1] : A.segments)
        for (auto& [p2, q2] : B.segments) {
            Vec3 a, b;
            segment_closest_points(p1, q1, p2, q2, a, b);
            const Vec3 d = a - b;
            const double dist = d.length(), depth = A.radius + B.radius - dist;
            if (depth > h.depth) h = {depth, dist > 1e-9 ? d * (1 / dist) : Vec3{0, 0, 1}};
        }
    return h;
}

// ponytail: the capsule's nearest point is found in the ellipsoid's unit-sphere space and its gap measured along the
// radius there: close for the round volumes, loose for long thin ones. A true ellipsoid distance if that matters.
Hit volume_hit(const RagdollCapsule& A, const Xform& g, const Vec3& s) {
    const Xform inv = g.inverse();
    auto unit = [&](const Vec3& w) {
        const Vec3 l = inv.apply(w);
        return Vec3{l.x / s.x, l.y / s.y, l.z / s.z};
    };
    Hit h;
    for (auto& [p, q] : A.segments) {
        Vec3 u, origin;
        segment_closest_points(unit(p), unit(q), {}, {}, u, origin);
        double k = u.length();
        if (k < 1e-9) u = {0, 0, 1}, k = 0;  // through the centre: out along its z
        const Vec3 surf = (k > 0 ? u * (1 / k) : u).mul(s), at = u.mul(s);
        const double gap = (at - surf).length() * (k >= 1 ? 1 : -1);
        if (A.radius - gap > h.depth)
            h = {A.radius - gap, g.rot.rotate(Vec3{surf.x / (s.x * s.x), surf.y / (s.y * s.y), surf.z / (s.z * s.z)}).normalized()};
    }
    return h;
}

bool push_out_moves(const std::string& n) {
    return n.rfind("mShoulder", 0) == 0 || n.rfind("mElbow", 0) == 0 || n.rfind("mWrist", 0) == 0;
}

SelfContactCheck::SelfContactCheck(const Skeleton& skel, const Shape* shape, bool volumes, double tol)
    : skel_(skel), shape_(shape), tol_(tol) {
    const std::vector<Xform> rest = skel.global_pose(Pose(skel.size()), shape);
    const std::vector<RagdollCapsule> caps = ragdoll_capsules(skel, rest);
    std::vector<int> parent(skel.size(), -2);  // -2: not a capsule's joint
    for (const RagdollCapsule& c : caps) parent[c.node] = c.parent;
    auto joined = [&](int x, int y) { return x == y || parent[x] == y || parent[y] == x; };
    for (int i = 0; i < int(caps.size()); ++i)
        for (int j = i + 1; j < int(caps.size()); ++j)
            if (!joined(caps[i].node, caps[j].node) && capsule_hit(caps[i], caps[j]).depth <= -kRestGap) pairs_.push_back({i, j});
    if (!volumes) return;
    for (const CollisionVolume& v : skel.volumes()) {
        int body = v.joint;  // the capsule the volume rides with
        while (body >= 0 && parent[body] == -2) body = skel[body].parent;
        volume_body_.push_back(body);
        const Vec3 s = shape ? v.scale.mul(shape->scale[v.joint]) : v.scale;
        if (body < 0 || std::min({s.x, s.y, s.z}) < 1e-4) continue;
        for (int i = 0; i < int(caps.size()); ++i)
            if (!joined(caps[i].node, body) && volume_hit(caps[i], rest[v.node], s).depth <= -kRestGap)
                volumes_.push_back({i, int(volume_body_.size()) - 1});
    }
}

std::vector<SelfContact> SelfContactCheck::find(const std::vector<Xform>& globals) const {
    const std::vector<RagdollCapsule> caps = contact_hull(skel_, globals);
    std::map<std::pair<int, int>, Hit> best;
    auto add = [&](int a, int b, Hit h) {
        if (h.depth <= tol_) return;
        if (a > b) std::swap(a, b), h.push = -h.push;
        auto [it, fresh] = best.try_emplace({a, b}, h);
        if (!fresh && h.depth > it->second.depth) it->second = h;
    };
    for (auto [i, j] : pairs_) add(caps[i].node, caps[j].node, capsule_hit(caps[i], caps[j]));
    for (auto [i, k] : volumes_) {
        const CollisionVolume& v = skel_.volumes()[k];
        const Vec3 s = shape_ ? v.scale.mul(shape_->scale[v.joint]) : v.scale;
        add(caps[i].node, volume_body_[k], volume_hit(caps[i], globals[v.node], s));
    }
    std::vector<SelfContact> out;
    for (auto& [ab, h] : best) out.push_back({ab.first, ab.second, h.depth, h.push});
    return out;
}

std::vector<SelfContact> cross_contacts(const Skeleton& skel, const std::vector<Xform>& globals,
                                        const std::vector<Xform>& other, double tol) {
    const std::vector<RagdollCapsule> mine = contact_hull(skel, globals), theirs = contact_hull(skel, other);
    std::vector<SelfContact> out;
    for (const RagdollCapsule& A : mine)
        for (const RagdollCapsule& B : theirs)
            if (const Hit h = capsule_hit(A, B); h.depth > tol) out.push_back({A.node, B.node, h.depth, h.push});
    return out;
}

void push_out(Clip& clip, const Rig& rig, int a, int b, const std::vector<int>& frames, const Shape* shape,
              const Shape* mesh_body) {
    const Skeleton& skel = rig.skeleton();
    const int node = push_out_moves(skel[a].name) ? a : b;
    const std::string& name = skel[node].name;
    if (!push_out_moves(name)) return;
    const int limb = rig.find_limb(name.ends_with("Left") ? "ArmLeft" : "ArmRight");
    if (limb < 0) return;
    const LimbInfo& l = rig.limbs()[limb];
    const SelfContactCheck check(skel, mesh_body ? mesh_body : shape, mesh_body != nullptr);
    for (int pass = 0; pass < kPasses; ++pass) {
        bool moved = false;
        for (int f : frames) {
            const Evaluation e = evaluate(rig, clip, f, shape);
            const std::vector<Xform> g = mesh_body ? skel.global_pose(e.pose, mesh_body) : e.globals;
            for (const SelfContact& s : check.find(g)) {
                if (s.a != std::min(a, b) || s.b != std::max(a, b)) continue;
                Xform target = e.globals[l.end];
                target.pos += (s.a == node ? s.push : -s.push) * (s.depth + kMargin);
                if (e.limbs[limb].ik_on) {
                    key_limb_target(clip, rig, f, limb, target, shape);
                } else {  // FK: the rotations IK finds for the moved target, keyed on the arm's own curves
                    Clip ik = clip;
                    switch_to_ik(ik, rig, f, limb, shape);
                    key_limb_target(ik, rig, f, limb, target, shape);
                    const Pose p = evaluate(rig, ik, f, shape).pose;
                    for (int j : {l.root, l.mid, l.end}) key_rotation(clip, skel[j].name, f, p.rot[j]);
                }
                moved = true;
            }
        }
        if (!moved) break;
    }
}

}  // namespace vats
