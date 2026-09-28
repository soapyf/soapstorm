// Viewport Avatar Toolset - ragdoll (spec 08 section 5).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/ragdoll.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

#include "vats/dynamics.h"

namespace vats {

// One row of the joint table. The ragdoll moves the rotation of these joints; every other joint keeps its
// animated local rotation. Rest rotations of these joints are identity in the SL skeleton, so a joint's
// limit axes are the same in its own frame and its parent's.
struct RagdollSolver::Joint {
    std::string name;
    std::string child;                 // the joint its bone points at, "" for a tip or a group
    double tip = 0;                    // capsule to this multiple of the display tail (hand, head)
    std::vector<std::string> group;    // rigid group members (hips, chest, foot)
    enum Kind { Root, Cone, Hinge } kind = Cone;
    Vec3 bend{1, 0, 0};                // Cone: direction the centre tilts to; Hinge: direction the bone bends to
    double tilt = 0, max = 45, twist = 45;  // degrees; Hinge uses 0..max
};

namespace {

using Joint = RagdollSolver::Joint;

constexpr int kStepsPerSecond = 480;     // sub-steps: XPBD likes many small ones
constexpr int kIterations = 4;           // solver passes per sub-step
constexpr double kLinearDamping = 0.1;   // per second
constexpr double kAngularDamping = 0.5;  // per second, in the world: a little, for calm resting
// Per second, on the spin between joined bodies: joint friction, so limbs stop swinging. Damping world
// spin instead (as before) also slowed the whole body's topple, which made falls fold up on the spot.
constexpr double kJointDamping = 5.0;
// Per second, on the spin of a body lying on the ground, a prop or another part: rolling resistance, so it
// settles instead of creeping (a face pressed to the floor, a head on an arm, rolling against a limit).
constexpr double kRollingDamping = 6.0;
constexpr double kSleepSpeed = 0.05, kSleepSpin = 1.0;  // m/s, rad/s: slower than this in contact = at rest
constexpr double kBodyMass = 70;         // kg; only the ratios matter to the motion
constexpr double kNudge = 0.8;           // rad/s: the topple that starts a limp standing fall
constexpr double kStandingPelvis = 0.8;  // m: a pelvis this high is standing (seated is about 0.55)
constexpr double kMaxSpeed = 8;          // m/s: a safety net against solver tangles
constexpr double kHingeSideways = 35 * kDegToRad;  // a knee or elbow may lean this far out of its plane

std::vector<Joint> make_table() {
    const Vec3 fwd{1, 0, 0}, back{-1, 0, 0}, down{0, 0, -1};
    std::vector<Joint> t;
    t.push_back({"mPelvis", "", 0, {"mHipLeft", "mHipRight", "mTorso"}, Joint::Root});
    t.push_back({"mTorso", "mChest", 0, {}, Joint::Cone, fwd, 15, 40, 30});
    t.push_back({"mChest", "", 0, {"mNeck", "mCollarLeft", "mCollarRight", "mShoulderLeft", "mShoulderRight"},
                 Joint::Cone, fwd, 0, 30, 35});
    t.push_back({"mNeck", "mHead", 0, {}, Joint::Cone, fwd, 10, 50, 60});
    t.push_back({"mHead", "", 2.0, {}, Joint::Cone, fwd, 0, 45, 70});
    for (const char* side : {"Left", "Right"}) {
        auto n = [&](const char* base) { return std::string(base) + side; };
        t.push_back({n("mShoulder"), n("mElbow"), 0, {}, Joint::Cone, fwd, 20, 110, 90});
        t.push_back({n("mElbow"), n("mWrist"), 0, {}, Joint::Hinge, fwd, 0, 150, 90});
        t.push_back({n("mWrist"), "", 2.5, {}, Joint::Cone, fwd, 0, 70, 90});
        t.push_back({n("mHip"), n("mKnee"), 0, {}, Joint::Cone, fwd, 35, 80, 45});
        t.push_back({n("mKnee"), n("mAnkle"), 0, {}, Joint::Hinge, back, 0, 150, 30});
        // Pointing the foot (about 52 degrees) goes further than lifting it (28): with the old centred cone
        // a foot lying face-down fought the floor.
        t.push_back({n("mAnkle"), "", 0, {n("mFoot"), n("mToe")}, Joint::Cone, down, 12, 40, 25});
    }
    return t;
}

// Segment mass as a percentage of the body (de Leva 1996, adjustments to Zatsiorsky-Seluyanov). The
// capsules' water-density mass made the pelvis a quarter of the body and the head under 3 %, so a
// collapse dropped straight down instead of toppling.
double mass_percent(std::string name) {
    for (const char* side : {"Left", "Right"})
        if (name.size() > 5 && name.compare(name.size() - std::strlen(side), std::string::npos, side) == 0)
            name.resize(name.size() - std::strlen(side));
    static const std::pair<const char*, double> kTable[] = {
        {"mPelvis", 11.17}, {"mTorso", 16.33}, {"mChest", 15.96}, {"mNeck", 0.94}, {"mHead", 6.0},
        {"mShoulder", 2.71}, {"mElbow", 1.62},  {"mWrist", 0.61}, {"mHip", 14.16}, {"mKnee", 4.33},
        {"mAnkle", 1.37}};
    for (auto& [n, p] : kTable)
        if (name == n) return p;
    return 1;
}

const std::vector<Joint>& table() {
    static const std::vector<Joint> t = make_table();
    return t;
}

double deg(double r) { return r / kDegToRad; }

Quat arc(const Vec3& u, const Vec3& v) {
    double d = u.dot(v);
    if (d < -0.999999) {
        Vec3 a = std::fabs(u.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        return Quat::axis_angle(u.cross(a).normalized(), kPi);
    }
    Vec3 c = u.cross(v);
    return Quat{1 + d, c.x, c.y, c.z}.normalized();
}

Vec3 perpendicular(const Vec3& v) {
    Vec3 a = std::fabs(v.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    return v.cross(a).normalized();
}

// Swing-twist split of q about a unit axis: q = swing * twist.
Quat twist_of(const Quat& q, const Vec3& axis) {
    double proj = Vec3{q.x, q.y, q.z}.dot(axis);
    Quat tw{q.w, axis.x * proj, axis.y * proj, axis.z * proj};
    double l = std::sqrt(tw.dot(tw));
    return l < 1e-12 ? Quat{} : Quat{tw.w / l, tw.x / l, tw.y / l, tw.z / l};
}

// Brings a bone direction d (parent frame, unit) inside a joint's swing limits: the cone, or the hinge's bend
// range with a little sideways give (the limb's own roll).
Vec3 limit_direction(const Joint& j, const Vec3& r0, Vec3 d) {
    if (j.kind == Joint::Hinge) {
        Vec3 h = r0.cross(j.bend);
        h = h.length() > 1e-9 ? h.normalized() : perpendicular(r0);
        const Vec3 bp = h.cross(r0).normalized();
        double a = d.dot(r0), b = d.dot(bp), c = d.dot(h);
        // To the nearer end of the range: a knee pushed past straight goes back to straight, one folded past
        // its fullest bend stays folded, never a jump across the whole range.
        const double hi = j.max * kDegToRad;
        double phi = std::atan2(b, a);
        if (phi < 0 || phi > hi) {
            double to_lo = std::fabs(std::remainder(phi, 2 * kPi)), to_hi = std::fabs(std::remainder(phi - hi, 2 * kPi));
            phi = to_lo <= to_hi ? 0.0 : hi;
        }
        const double m = std::hypot(a, b), side = m * std::tan(kHingeSideways);
        c = std::clamp(c, -side, side);
        return (r0 * (m * std::cos(phi)) + bp * (m * std::sin(phi)) + h * c).normalized();
    }
    if (j.kind == Joint::Cone) {
        Vec3 tilt_axis = r0.cross(j.bend);
        Vec3 c = tilt_axis.length() > 1e-9 ? Quat::axis_angle(tilt_axis.normalized(), j.tilt * kDegToRad).rotate(r0) : r0;
        double ang = std::acos(std::clamp(d.dot(c), -1.0, 1.0));
        if (ang > j.max * kDegToRad) {
            Vec3 a = c.cross(d);
            a = a.length() > 1e-9 ? a.normalized() : perpendicular(c);
            d = Quat::axis_angle(a, j.max * kDegToRad).rotate(c);
        }
    }
    return d;
}

// Clamps a joint's local rotation (parent frame) to its limits: swing as limit_direction, twist about the
// bone within +-twist.
Quat clamp_local(const Joint& j, const Vec3& axis, const Quat& q) {
    Quat tw = twist_of(q, axis);
    double twist = std::remainder(2 * std::atan2(Vec3{tw.x, tw.y, tw.z}.dot(axis), tw.w), 2 * kPi);
    twist = std::clamp(twist, -j.twist * kDegToRad, j.twist * kDegToRad);
    Vec3 d = limit_direction(j, axis, (q * tw.conj()).rotate(axis).normalized());
    return arc(axis, d) * Quat::axis_angle(axis, twist);
}

// The limit axis of a table joint: towards its child, its tip, or its first group member, from the rest pose.
Vec3 limit_axis(const Skeleton& skel, const std::vector<Xform>& rest, const Joint& j, int n) {
    Vec3 t;
    if (!j.child.empty()) t = rest[n].inverse().apply(rest[skel.find(j.child)].pos);
    else if (j.tip > 0) t = skel[n].end;
    else if (!j.group.empty()) t = rest[n].inverse().apply(rest[skel.find(j.group[0])].pos);
    return t.length() > 1e-9 ? t.normalized() : Vec3{0, 0, 1};
}

double angle_between(const Quat& a, const Quat& b) { return 2 * std::acos(std::min(1.0, std::fabs(a.dot(b)))); }

}  // namespace

// Closest points between segments p1-q1 and p2-q2 (Ericson, Real-Time Collision Detection 5.1.9).
void segment_closest_points(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1, Vec3& c2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const double a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
    double s = 0, t = 0;
    if (a < 1e-12 && e < 1e-12) {
        c1 = p1, c2 = p2;
        return;
    }
    if (a < 1e-12) {
        t = std::clamp(f / e, 0.0, 1.0);
    } else {
        const double c = d1.dot(r);
        if (e < 1e-12) {
            s = std::clamp(-c / a, 0.0, 1.0);
        } else {
            const double b = d1.dot(d2), denom = a * e - b * b;
            s = denom > 1e-12 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0) t = 0, s = std::clamp(-c / a, 0.0, 1.0);
            else if (t > 1) t = 1, s = std::clamp((b - c) / a, 0.0, 1.0);
        }
    }
    c1 = p1 + d1 * s, c2 = p2 + d2 * t;
}

std::vector<std::string> ragdoll_joint_names() {
    std::vector<std::string> out;
    for (const Joint& j : table()) out.push_back(j.name);
    return out;
}

RagdollSolver::RagdollSolver(const Skeleton& skel, const Ragdoll& settings, std::vector<RagdollBox> boxes)
    : skel_(skel), settings_(settings), boxes_(std::move(boxes)) {
    // RD-1: the whole body, or the selected joints and every table joint below them.
    std::vector<char> chosen(skel.size(), 0);
    for (const std::string& b : settings.bones)
        if (int n = skel.find(b); n >= 0) chosen[n] = 1;
    body_of_.assign(skel.size(), -1);
    row_.assign(skel.size(), nullptr);
    axis_.assign(skel.size(), {});
    const std::vector<Xform> rest = skel.global_pose(Pose(skel.size()));
    for (const Joint& j : table()) {
        int n = skel.find(j.name);
        if (n < 0) continue;
        row_[n] = &j;
        axis_[n] = limit_axis(skel, rest, j, n);
        bool on = settings.whole_body;
        for (int a = n; a >= 0 && !on; a = skel[a].parent) on = chosen[a];
        Body b;
        b.node = n;
        b.dynamic = on;
        for (int a = skel[n].parent; a >= 0 && b.parent < 0; a = skel[a].parent) b.parent = body_of_[a];
        body_of_[n] = int(bodies_.size());
        bodies_.push_back(b);
        if (on) sim_nodes_.push_back(n);
    }
    const int pelvis = skel.find("mPelvis");
    moves_pelvis_ = pelvis >= 0 && body_of_[pelvis] >= 0 && bodies_[body_of_[pelvis]].dynamic;

    // Capsule radii from the collision volumes (their smallest semi-axis). Pecs, handles, butt and back
    // volumes are soft-body extras, not the body's hull.
    for (const CollisionVolume& v : skel.volumes()) {
        if (v.joint < 0 || body_of_[v.joint] < 0 || v.name.find("PEC") != std::string::npos ||
            v.name.find("HANDLE") != std::string::npos || v.name == "BUTT" || v.name.find("BACK") != std::string::npos)
            continue;
        Body& b = bodies_[body_of_[v.joint]];
        double r = std::min({v.scale.x, v.scale.y, v.scale.z});
        if (b.radius == 0.04 || r < b.radius) b.radius = r;
    }
    for (Body& b : bodies_)
        if (row_[b.node]->tip > 0) b.radius *= 0.8;  // a hand or the top of the head is slimmer than its volume
}

void RagdollSolver::place_kinematic(Body& b, const std::vector<Xform>& g) const {
    b.q = g[b.node].rot;
    b.x = g[b.node].apply(b.com);
}

void RagdollSolver::reset(const std::vector<Xform>& g, const std::vector<Xform>& prev, double dt) {
    // Shape each body from the pose it starts in: capsule ends, mass (water density) and inertia.
    for (Body& b : bodies_) {
        const Joint& j = *row_[b.node];
        const Xform inv = g[b.node].inverse();
        b.ends.clear();
        if (!j.child.empty()) b.ends.push_back(inv.apply(g[skel_.find(j.child)].pos));
        if (j.tip > 0) b.ends.push_back(skel_[b.node].end * j.tip);
        for (auto& m : j.group)
            if (int n = skel_.find(m); n >= 0 && m.find("Collar") == std::string::npos) b.ends.push_back(inv.apply(g[n].pos));
        double mass = 0, inertia = 0;
        Vec3 com;
        std::vector<double> ms;
        for (const Vec3& e : b.ends) {
            double len = e.length();
            double m = 1000 * kPi * b.radius * b.radius * (len + 4.0 / 3.0 * b.radius);
            ms.push_back(m);
            mass += m;
            com += e * (0.5 * m);
        }
        com = mass > 0 ? com * (1 / mass) : Vec3{};
        for (size_t k = 0; k < b.ends.size(); ++k) {
            double len = b.ends[k].length();
            inertia += ms[k] * ((b.ends[k] * 0.5 - com).dot(b.ends[k] * 0.5 - com) + len * len / 12 + b.radius * b.radius / 4);
        }
        // The capsules give the shape; the segment table gives the mass (inertia scales with it).
        if (mass > 0) {
            const double m = kBodyMass * mass_percent(skel_[b.node].name) / 100;
            inertia *= m / mass;
            mass = m;
        }
        b.com = com;
        b.inv_mass = b.dynamic && mass > 0 ? 1 / mass : 0;
        b.inv_inertia = b.dynamic && inertia > 0 ? 1 / inertia : 0;
        place_kinematic(b, prev);
        b.x_prev = b.x, b.q_prev = b.q;
        place_kinematic(b, g);
        // Starting velocity from the animation, so a ragdoll let go mid-motion carries on.
        b.v = (b.x - b.x_prev) * (1 / dt);
        Quat dq = b.q * b.q_prev.conj();
        if (dq.w < 0) dq = -dq;
        b.w = Vec3{dq.x, dq.y, dq.z} * (2 / dt);
    }
    // Self-collision pairs: bodies that are not parent and child and do not already touch in the start pose.
    // (Pairs that start touching, legs together or a hand on the hip, are left out: checking them fed energy
    // into the rest pose through the joint limits.)
    self_pairs_.clear();
    for (int a = 0; a < int(bodies_.size()); ++a)
        for (int c = a + 1; c < int(bodies_.size()); ++c) {
            const Body &A = bodies_[a], &B = bodies_[c];
            if (A.parent == c || B.parent == a || (!A.dynamic && !B.dynamic)) continue;
            bool touching = false;
            for (const Vec3& ea : A.ends)
                for (const Vec3& eb : B.ends) {
                    Vec3 pa, pb;
                    segment_closest_points(joint_world(A), point_world(A, ea), joint_world(B), point_world(B, eb), pa, pb);
                    touching = touching || (pa - pb).length() < A.radius + B.radius + 0.01;
                }
            if (!touching) self_pairs_.push_back({a, c});
        }
    last_blend_ = 1;
}

Vec3 RagdollSolver::centre_of_mass() const {
    Vec3 sum;
    double mass = 0;
    for (const Body& b : bodies_)
        if (b.inv_mass > 0) sum += b.x * (1 / b.inv_mass), mass += 1 / b.inv_mass;
    return mass > 0 ? sum * (1 / mass) : Vec3{};
}

double RagdollSolver::lowest_surface() const {
    double z = 1e9;
    for (const Body& b : bodies_) {
        z = std::min(z, joint_world(b).z - b.radius);
        for (const Vec3& e : b.ends) z = std::min(z, point_world(b, e).z - b.radius);
    }
    return z;
}

std::vector<RagdollCapsule> RagdollSolver::capsules() const {
    std::vector<RagdollCapsule> out;
    for (const Body& b : bodies_) {
        RagdollCapsule c{b.node, b.parent >= 0 ? bodies_[b.parent].node : -1, b.radius, {}};
        for (const Vec3& e : b.ends) c.segments.push_back({joint_world(b), point_world(b, e)});
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<RagdollCapsule> ragdoll_capsules(const Skeleton& skel, const std::vector<Xform>& globals) {
    RagdollSolver s(skel, Ragdoll{}, {});
    s.reset(globals, globals, 1);
    return s.capsules();
}

void RagdollSolver::positional(int a, int b, const Vec3& pa, const Vec3& pb, const Vec3& correction) {
    const double c = correction.length();
    if (c < 1e-12) return;
    const Vec3 n = correction * (1 / c);
    Body* A = a >= 0 ? &bodies_[a] : nullptr;
    Body* B = b >= 0 ? &bodies_[b] : nullptr;
    Vec3 ra = A ? pa - A->x : Vec3{}, rb = B ? pb - B->x : Vec3{};
    double wa = A ? A->inv_mass + A->inv_inertia * ra.cross(n).dot(ra.cross(n)) : 0;
    double wb = B ? B->inv_mass + B->inv_inertia * rb.cross(n).dot(rb.cross(n)) : 0;
    if (wa + wb < 1e-12) return;
    const Vec3 P = n * (-c / (wa + wb));
    auto spin = [](Body& body, const Vec3& dw) {
        Quat d{0, dw.x, dw.y, dw.z};
        Quat r = d * body.q;
        body.q = Quat{body.q.w + 0.5 * r.w, body.q.x + 0.5 * r.x, body.q.y + 0.5 * r.y, body.q.z + 0.5 * r.z}.normalized();
    };
    if (A && A->dynamic) {
        A->x += P * A->inv_mass;
        spin(*A, ra.cross(P) * A->inv_inertia);
    }
    if (B && B->dynamic) {
        B->x = B->x - P * B->inv_mass;
        spin(*B, rb.cross(P) * -B->inv_inertia);
    }
}

void RagdollSolver::angular(int a, int b, const Vec3& rotation) {
    const double theta = rotation.length();
    if (theta < 1e-9) return;
    Body* A = a >= 0 ? &bodies_[a] : nullptr;
    Body& B = bodies_[b];
    const double wa = A ? A->inv_inertia : 0, wb = B.inv_inertia;
    if (wa + wb < 1e-12) return;
    const Vec3 n = rotation * (1 / theta);
    const double lambda = theta / (wa + wb);
    auto spin = [](Body& body, const Vec3& dw) {
        Quat d{0, dw.x, dw.y, dw.z};
        Quat r = d * body.q;
        body.q = Quat{body.q.w + 0.5 * r.w, body.q.x + 0.5 * r.x, body.q.y + 0.5 * r.y, body.q.z + 0.5 * r.z}.normalized();
    };
    // Rotating about the centre of mass would move the joint; the joint constraint pulls it back.
    if (B.dynamic) spin(B, n * (lambda * wb));
    if (A && A->dynamic) spin(*A, n * (-lambda * wa));
}

void RagdollSolver::solve_joints(const std::vector<Xform>& g, double drive) {
    for (int bi = 0; bi < int(bodies_.size()); ++bi) {
        Body& B = bodies_[bi];
        if (B.parent < 0) continue;
        Body& A = bodies_[B.parent];
        if (!A.dynamic && !B.dynamic) continue;
        const int n = B.node, pa = A.node, sp = skel_[n].parent;
        // The ball joint: B's joint sits where the animation puts it on A (collars and the Bento spine in
        // between keep their animated turns).
        const Vec3 attach = g[pa].inverse().apply(g[n].pos);
        const Vec3 wa = point_world(A, attach), wb = joint_world(B);
        positional(B.parent, bi, wa, wb, wa - wb);
        // Swing and twist limits (and the drive), in the frame of the joint's skeleton parent.
        const Quat Pf = (A.q * (g[pa].rot.conj() * g[sp].rot)).normalized();
        const Quat local = Pf.conj() * B.q;
        Quat target = local;
        if (drive > 0) target = nlerp(target, g[sp].rot.conj() * g[n].rot, drive);
        if (row_[n]->kind != Joint::Root) target = clamp_local(*row_[n], axis_[n], target);
        Quat d = Pf * target * local.conj() * Pf.conj();
        if (d.w < 0) d = -d;
        const Vec3 v{d.x, d.y, d.z};
        const double s = v.length();
        if (s > 1e-9) angular(B.parent, bi, v * (2 * std::atan2(s, d.w) / s));
    }
}

void RagdollSolver::solve_contacts(double friction) {
    auto contact = [&](int bi, const Vec3& local, const Vec3& to) {  // push a capsule end out, with friction
        Body& b = bodies_[bi];
        b.touching = true;
        Vec3 p = point_world(b, local);
        positional(bi, -1, p, to, p - to);
        if (friction <= 0) return;
        const Vec3 now = point_world(b, local), before = b.x_prev + b.q_prev.rotate(local - b.com);
        Vec3 n = (to - p);
        n = n.length() > 1e-12 ? n.normalized() : Vec3{0, 0, 1};
        Vec3 slide = now - before;
        slide = slide - n * slide.dot(n);
        positional(bi, -1, now, now - slide * friction, slide * friction);
    };
    for (int bi = 0; bi < int(bodies_.size()); ++bi) {
        Body& b = bodies_[bi];
        if (!b.dynamic) continue;
        std::vector<Vec3> points{Vec3{}};
        points.insert(points.end(), b.ends.begin(), b.ends.end());
        for (const Vec3& e : points) {
            Vec3 p = point_world(b, e);
            if (p.z < b.radius) contact(bi, e, {p.x, p.y, b.radius});  // RD-2: the ground at z = 0
            for (const RagdollBox& box : boxes_) {
                p = point_world(b, e);
                Vec3 l = box.frame.inverse().apply(p);
                Vec3 h = box.half + Vec3{b.radius, b.radius, b.radius};
                double pen[3] = {h.x - std::fabs(l.x), h.y - std::fabs(l.y), h.z - std::fabs(l.z)};
                if (pen[0] <= 0 || pen[1] <= 0 || pen[2] <= 0) continue;
                int axis = pen[0] < pen[1] ? (pen[0] < pen[2] ? 0 : 2) : (pen[1] < pen[2] ? 1 : 2);
                double* c = axis == 0 ? &l.x : axis == 1 ? &l.y : &l.z;
                double hv = axis == 0 ? h.x : axis == 1 ? h.y : h.z;
                *c = *c < 0 ? -hv : hv;
                contact(bi, e, box.frame.apply(l));
            }
        }
    }
    // Self-collision: capsule against capsule for bodies that are not neighbours.
    for (auto [a, c] : self_pairs_) {
        const Body &A = bodies_[a], &B = bodies_[c];
        for (const Vec3& ea : A.ends)
            for (const Vec3& eb : B.ends) {
                Vec3 pa, pb;
                segment_closest_points(joint_world(A), point_world(A, ea), joint_world(B), point_world(B, eb), pa, pb);
                Vec3 d = pa - pb;
                double dist = d.length(), want = A.radius + B.radius;
                if (dist >= want || dist < 1e-9) continue;
                positional(a, c, pa, pb, d * ((dist - want) / dist));
                bodies_[a].touching = bodies_[c].touching = true;  // resting on each other: rolling damping
            }
    }
}

// Joint friction: part of the relative spin of each joined pair is taken out, shared by inverse inertia.
void RagdollSolver::damp_joints(double h) {
    const double k = std::min(1.0, kJointDamping * h);
    for (Body& B : bodies_) {
        if (B.parent < 0) continue;
        Body& A = bodies_[B.parent];
        const double wa = A.dynamic ? A.inv_inertia : 0, wb = B.dynamic ? B.inv_inertia : 0;
        if (wa + wb < 1e-12) continue;
        const Vec3 dw = (B.w - A.w) * k;
        B.w = B.w - dw * (wb / (wa + wb));
        A.w = A.w + dw * (wa / (wa + wb));
    }
}

// Nobody balances perfectly: at the moment the body is let go it gets a small topple about the ground under
// the pelvis, towards Ragdoll::extra "fall_direction" ("forward" by default, "back", "left", "right",
// "random" (seeded by the start frame, so it is repeatable) or "none"). Only for a standing body whose
// pelvis falls too: seated, kneeling or lying bodies are already supported and slump on their own.
void RagdollSolver::nudge() {
    if (!moves_pelvis_) return;
    std::string dir = "forward";
    if (const Json* j = settings_.extra.find("fall_direction"); j && j->is_string()) dir = j->str;
    if (dir == "none") return;
    const Body& P = bodies_[body_of_[skel_.find("mPelvis")]];
    if (joint_world(P).z < kStandingPelvis) return;
    Vec3 fwd = P.q.rotate({1, 0, 0});
    fwd.z = 0;
    fwd = fwd.length() > 1e-6 ? fwd.normalized() : Vec3{1, 0, 0};
    const Vec3 left{-fwd.y, fwd.x, 0};
    double a = dir == "back" ? kPi : dir == "left" ? kPi / 2 : dir == "right" ? -kPi / 2 : 0;
    if (dir == "random") a = std::fmod(2.399963229728653 * (settings_.start + 1), 2 * kPi);  // golden angle
    const Vec3 d = fwd * std::cos(a) + left * std::sin(a), spin = Vec3{0, 0, 1}.cross(d) * kNudge;
    const Vec3 pivot{joint_world(P).x, joint_world(P).y, 0};
    for (Body& b : bodies_)
        if (b.dynamic) b.w += spin, b.v += spin.cross(b.x - pivot);
}

void RagdollSolver::step(const std::vector<Xform>& g, double h, double blend) {
    if (blend <= 0 && last_blend_ > 0) nudge();  // the moment it is let go
    const Vec3 gravity{0, 0, -9.81 * settings_.gravity};
    for (Body& b : bodies_) {
        b.x_prev = b.x, b.q_prev = b.q;
        b.touching = false;
        if (!b.dynamic) {
            place_kinematic(b, g);
            continue;
        }
        b.v = (b.v + gravity * h) * (1 - kLinearDamping * h);
        b.w = b.w * (1 - kAngularDamping * h);
        b.x += b.v * h;
        Quat r = Quat{0, b.w.x, b.w.y, b.w.z} * b.q;
        b.q = Quat{b.q.w + 0.5 * h * r.w, b.q.x + 0.5 * h * r.x, b.q.y + 0.5 * h * r.y, b.q.z + 0.5 * h * r.z}.normalized();
    }
    const double drive = std::clamp(settings_.stiffness, 0.0, 1.0);
    for (int it = 0; it < kIterations; ++it) {
        // Contacts first and the joints last, so each pass ends with the body in one piece and inside its limits.
        solve_contacts(it == kIterations - 1 ? std::clamp(settings_.friction, 0.0, 1.0) : 0.0);
        solve_joints(g, it == 0 ? drive : 0.0);
    }
    // A part resting on something holds up the body joined above it too (the neck under a head on the floor).
    for (Body& b : bodies_)
        if (b.touching && b.parent >= 0) bodies_[b.parent].touching = true;
    for (Body& b : bodies_) {
        if (!b.dynamic) continue;
        b.v = (b.x - b.x_prev) * (1 / h);
        Quat dq = b.q * b.q_prev.conj();
        if (dq.w < 0) dq = -dq;
        b.w = Vec3{dq.x, dq.y, dq.z} * (2 / h);
        if (b.v.length() > kMaxSpeed) b.v = b.v * (kMaxSpeed / b.v.length());
        if (b.touching) {
            b.w = b.w * (1 - std::min(1.0, kRollingDamping * h));
            // Resting: contact and limit corrections leave a small velocity every sub-step, which would
            // otherwise keep light parts (feet, hands) twitching on the floor.
            if (b.v.length() < kSleepSpeed && b.w.length() < kSleepSpin) b.v = {}, b.w = {};
        }
    }
    damp_joints(h);
    for (Body& b : bodies_) {
        if (!b.dynamic) continue;
        if (blend > 0) {  // RD-3 blend-in: held to the animation, released over time
            Body a = b;
            place_kinematic(a, g);
            b.x = b.x + (a.x - b.x) * blend;
            b.q = nlerp(b.q, a.q, blend);
            b.v = b.v * (1 - blend), b.w = b.w * (1 - blend);
        }
    }
    last_blend_ = blend;
}

void RagdollSolver::apply(const std::vector<Xform>& g, Pose& pose) const {
    if (last_blend_ >= 1) return;  // still held: exactly the animation
    std::vector<Quat> G(skel_.size());
    for (int i = 0; i < skel_.size(); ++i) {
        const int parent = skel_[i].parent;
        const int bi = body_of_[i];
        if (bi >= 0 && bodies_[bi].dynamic) G[i] = bodies_[bi].q;
        else G[i] = parent >= 0 ? G[parent] * (g[parent].rot.conj() * g[i].rot) : g[i].rot;
    }
    for (int n : sim_nodes_) {
        int parent = skel_[n].parent;
        Quat local = (parent >= 0 ? G[parent].conj() : Quat{}) * G[n];
        // The solver's limits are stiff but not rigid (a hard landing can push a joint a few degrees past);
        // the written pose is always inside them.
        if (parent >= 0 && row_[n]->kind != Joint::Root) local = clamp_local(*row_[n], axis_[n], local);
        pose.rot[n] = (skel_[n].rest.conj() * local).normalized();
        if (parent < 0 && moves_pelvis_) pose.offset[n] += joint_world(bodies_[body_of_[n]]) - g[n].pos;
    }
}

std::vector<Pose> simulate_ragdoll(const Rig& rig, const Clip& clip, const Shape* shape,
                                   const std::vector<RagdollBox>& boxes) {
    if (!clip.ragdoll) return {};
    const Ragdoll& rd = *clip.ragdoll;
    const Skeleton& skel = rig.skeleton();
    // Always driven by the pre-bake tracks, so re-baking never feeds on its own output.
    Clip drive = clip;
    if (rd.baked)
        for (int n = 0; n < skel.size(); ++n) {
            auto it = rd.source.find(skel[n].name);
            if (it != rd.source.end()) drive.curves[it->first] = it->second;
        }
    RagdollSolver probe(skel, rd, boxes);
    if (rd.baked)
        for (int n : probe.nodes())
            if (!rd.source.count(skel[n].name)) drive.curves.erase(skel[n].name);

    const int end = std::max(drive.end_frame, 0), fps = std::max(drive.fps, 1);
    const int sub = std::max(1, int(std::lround(double(kStepsPerSecond) / fps)));
    const double dt = 1.0 / (double(fps) * sub);
    std::vector<Pose> frames(end + 1);
    for (int f = 0; f <= end; ++f) frames[f] = evaluate(rig, drive, f, shape).pose;
    const int start = std::clamp(rd.start, 0, end), stop = std::clamp(rd.start + std::max(rd.frames, 0), start, end);
    if (stop <= start) return frames;

    RagdollSolver sim(skel, rd, boxes);
    auto globals = [&](double t) { return evaluate(rig, drive, t, shape).globals; };
    sim.reset(globals(start), globals(start > 0 ? start - 1.0 / sub : start), dt);
    for (int f = start; f < stop; ++f) {
        for (int s = 1; s <= sub; ++s) {
            double t = f + double(s) / sub;
            double blend = rd.blend_in > 0 ? std::max(0.0, 1 - (t - start) / rd.blend_in) : 0.0;
            sim.step(globals(t), dt, blend);
        }
        Evaluation e = evaluate(rig, drive, f + 1, shape);
        Pose p = e.pose;
        sim.apply(e.globals, p);
        // RD-3 blend-out: the last blend_out frames ease back to the animation.
        double u = rd.blend_out > 0 ? std::clamp(double(f + 1 - (stop - rd.blend_out)) / rd.blend_out, 0.0, 1.0) : 0.0;
        for (int n : sim.nodes()) {
            p.rot[n] = nlerp(p.rot[n], e.pose.rot[n], u);
            p.offset[n] = p.offset[n] + (e.pose.offset[n] - p.offset[n]) * u;
        }
        frames[f + 1] = p;
    }
    return frames;
}

void bake_ragdoll(Clip& clip, const Rig& rig, const Shape* shape, const std::vector<RagdollBox>& boxes) {
    if (!clip.ragdoll) return;
    const Skeleton& skel = rig.skeleton();
    std::vector<Pose> frames = simulate_ragdoll(rig, clip, shape, boxes);
    Ragdoll& rd = *clip.ragdoll;
    RagdollSolver probe(skel, rd, boxes);
    std::vector<int> nodes = probe.nodes(), with_position;
    if (probe.moves_pelvis()) with_position.push_back(skel.find("mPelvis"));
    if (rd.baked) {
        for (int n : nodes) {  // back to the source before re-baking over it
            auto it = rd.source.find(skel[n].name);
            if (it == rd.source.end()) clip.curves.erase(skel[n].name);
            else clip.curves[it->first] = it->second;
        }
    } else {
        rd.source.clear();
        for (int n : nodes)
            if (auto it = clip.curves.find(skel[n].name); it != clip.curves.end()) rd.source[it->first] = it->second;
    }
    bake_samples(clip, skel, nodes, frames, 0.1, 0.0005, with_position);
    rd.baked = true;
}

void unbake_ragdoll(Clip& clip, const Skeleton& skel) {
    if (!clip.ragdoll || !clip.ragdoll->baked) return;
    Ragdoll& rd = *clip.ragdoll;
    RagdollSolver probe(skel, rd, {});
    for (int n : probe.nodes()) {
        auto it = rd.source.find(skel[n].name);
        if (it == rd.source.end()) clip.curves.erase(skel[n].name);
        else clip.curves[it->first] = it->second;
    }
    rd.source.clear();
    rd.baked = false;
}

std::vector<LimitExcess> ragdoll_limit_excesses(const Skeleton& skel, const Pose& pose, double tol_deg) {
    const std::vector<Xform> rest = skel.global_pose(Pose(skel.size()));
    std::vector<LimitExcess> out;
    for (const Joint& j : table()) {
        int n = skel.find(j.name);
        if (n < 0 || j.kind == Joint::Root) continue;
        Quat q = skel[n].rest * pose.rot[n];
        Quat c = clamp_local(j, limit_axis(skel, rest, j, n), q);
        if (double d = deg(angle_between(q, c)); d > tol_deg) out.push_back({n, d, (skel[n].rest.conj() * c).normalized()});
    }
    return out;
}

double ragdoll_limit_excess(const Skeleton& skel, const Pose& pose) {
    double worst = 0;
    for (const LimitExcess& e : ragdoll_limit_excesses(skel, pose, 0)) worst = std::max(worst, e.deg);
    return worst;
}

}  // namespace vats
