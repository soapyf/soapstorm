// Viewport Avatar Toolset - IK rigs, pins, follow bake and full pose evaluation.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/rig.h"

#include <algorithm>
#include <climits>
#include <cmath>

#include "vats/edit.h"

namespace vats {
namespace {

constexpr const char* kPoleChannels[3] = {"pole_x", "pole_y", "pole_z"};

Quat slerp(const Quat& a, Quat b, double t) {
    if (a.dot(b) < 0) b = -b;
    double c = std::min(1.0, a.dot(b));
    if (c > 0.9995) return nlerp(a, b, t);
    double th = std::acos(c), s = std::sin(th);
    double wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return Quat{a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb}.normalized();
}

// slerp(identity, q, 0.5)
Quat half(Quat q) {
    if (q.w < 0) q = -q;
    return Quat{1 + q.w, q.x, q.y, q.z}.normalized();
}

Vec3 any_perp(const Vec3& u) {
    Vec3 p = u.cross({0, 0, 1});
    if (p.length() < 1e-6) p = u.cross({1, 0, 0});
    return p.normalized();
}

Vec3 perp_to(const Vec3& v, const Vec3& axis) { return v - axis * v.dot(axis); }

// Shortest-arc rotation taking unit u onto unit v.
Quat arc(const Vec3& u, const Vec3& v) {
    double d = u.dot(v);
    if (d < -0.999999) return Quat::axis_angle(any_perp(u), kPi);
    Vec3 c = u.cross(v);
    return Quat{1 + d, c.x, c.y, c.z}.normalized();
}

// The rotation whose matrix columns are the orthonormal right-handed axes x, y, z.
Quat from_basis(const Vec3& x, const Vec3& y, const Vec3& z) {
    double m00 = x.x, m01 = y.x, m02 = z.x, m10 = x.y, m11 = y.y, m12 = z.y, m20 = x.z, m21 = y.z, m22 = z.z;
    double tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        double s = std::sqrt(tr + 1) * 2;
        q = {0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        double s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        double s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s};
    } else {
        double s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s};
    }
    return q.normalized();
}

Xform parent_global(const Skeleton& skel, const std::vector<Xform>& g, int i) {
    int p = skel[i].parent;
    return p >= 0 ? g[p] : Xform{};
}

// True when a is b or one of b's ancestors.
bool is_ancestor(const Skeleton& skel, int a, int b) {
    for (int i = b; i >= 0; i = skel[i].parent)
        if (i == a) return true;
    return false;
}

// Sets node i's pose so its global transform becomes world (parents unchanged). Inverts SK-16.
void set_world(const Skeleton& skel, int i, const Xform& world, Pose& pose, const std::vector<Xform>& g,
               const Shape* shape) {
    const Node& n = skel[i];
    Xform l = n.parent >= 0 ? g[n.parent].inverse() * world : world;
    pose.rot[i] = (n.rest.conj() * l.rot).normalized();
    Vec3 t = l.pos;
    if (shape) {
        if (n.parent >= 0) {
            const Vec3& s = shape->scale[n.parent];
            t = {t.x / s.x, t.y / s.y, t.z / s.z};
        }
        t = t - shape->offset[i];
    }
    pose.offset[i] = t - n.pos;
}

Vec3 eval3(const Clip& clip, const std::string& track, double frame, const char* const (&names)[3]) {
    Vec3 v;
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return v;
    for (int a = 0; a < 3; ++a) {
        auto c = t->second.find(names[a]);
        if (c != t->second.end()) v[a] = c->second.evaluate(frame);
    }
    return v;
}

std::string ik_track(const LimbInfo& l) { return "ik." + l.name; }

double blend_at(const Clip& clip, const std::string& track, double frame) {
    auto t = clip.curves.find(track);
    if (t == clip.curves.end()) return 0;
    auto c = t->second.find("blend");
    return c == t->second.end() || c->second.empty() ? 0 : std::clamp(c->second.evaluate(frame), 0.0, 1.0);
}

// Controllers of fingers live in the wrist's space, all others in avatar space (AM-52).
Xform controller_space(const Rig& rig, const LimbInfo& l, const std::vector<Xform>& g) {
    return l.finger ? parent_global(rig.skeleton(), g, l.root) : Xform{};
}

// What Switch to IK takes as the target (AM-55).
Xform end_world(const LimbInfo& l, const std::vector<Xform>& g) {
    return l.spine ? Xform{g[l.mid].rot, g[l.end].pos} : g[l.end];
}

// Steps 1-3 of 02 section 3.7: the mid bone's rotation for a root-target distance, and the first two axes
// of F1 in the root's frame. e = mid offset in the root, w = end offset in the mid.
Quat bend_frame(const LimbInfo& l, IkSolve mode, const Vec3& e, const Vec3& w, double reach, Vec3& x1, Vec3& y1) {
    double l1 = e.length(), l2 = w.length();
    double d = std::min(std::max(reach, std::fabs(l1 - l2) * 1.001 + 1e-4), (l1 + l2) * 0.9999);
    // The reach shrinks as the bend grows.
    double lo = 0, hi = 0.999 * kPi;
    for (int it = 0; it < 40; ++it) {
        double b = 0.5 * (lo + hi);
        ((e + Quat::axis_angle(l.hinge, b).rotate(w)).length() > d ? lo : hi) = b;
    }
    Quat bend = Quat::axis_angle(l.hinge, 0.5 * (lo + hi));
    x1 = (e + bend.rotate(w)).normalized();
    // VATs: towards the mid joint. Section 3.7 (Literal) takes x1 x hinge, but SL legs and fingers are not
    // planar about their hinge, so that axis twists the chain off the pose AM-57 derives the pole from.
    // The hinge is the same vector in the root and mid frames: the mid turns about it.
    y1 = mode == IkSolve::Literal ? x1.cross(l.hinge) : perp_to(e, x1);
    if (y1.length() < 1e-6 * l1) y1 = x1.cross(l.hinge);
    y1 = y1.length() < 1e-9 ? any_perp(x1) : y1.normalized();
    return bend;
}

// The pole Switch to IK keys: one the section 3.7 solve in `mode` turns back into the pose (exact when the
// mid bone is bent about its hinge only). AM-57's foot point and distance, but along the solver's own
// F1 second axis as the pose carries it, so there is no straight-chain fallback, and under Literal the
// pole is turned about the root-end line to (root-end direction) x (world hinge).
Vec3 switch_pole(const Rig& rig, int limb, const std::vector<Xform>& g, IkSolve mode) {
    const LimbInfo& l = rig.limbs()[limb];
    Vec3 a = g[l.root].pos, m = g[l.mid].pos, u = g[l.end].pos - a, x1, y1;
    if (u.length() < 1e-9) return derive_pole(rig, limb, g);
    const Quat& r = g[l.root].rot;
    bend_frame(l, mode, r.conj().rotate(m - a), g[l.mid].rot.conj().rotate(g[l.end].pos - m), u.length(), x1, y1);
    u = u.normalized();
    Vec3 dir = perp_to(r.rotate(y1), u);
    if (dir.length() < 1e-6) return derive_pole(rig, limb, g);
    return a + u * (m - a).dot(u) + dir.normalized() * (l.finger ? 0.04 : 0.40);
}

void read_controller(const Rig& rig, const Clip& clip, int limb, double frame, const std::vector<Xform>& g,
                     LimbState& s) {
    const LimbInfo& l = rig.limbs()[limb];
    std::string name = ik_track(l);
    Xform space = controller_space(rig, l, g);
    if (clip.has_channels(name, kPosChannels) || clip.has_channels(name, kRotChannels))
        s.target = space * Xform{euler_to_quat(curve_euler(clip, name, frame)), curve_offset(clip, name, frame)};
    else
        s.target = end_world(l, g);
    if (l.spine) return;
    if (clip.has_channels(name, kPoleChannels))
        s.pole = space.apply(eval3(clip, name, frame, kPoleChannels));
    else  // Literal (section 3.7 as written) derives a pole-less controller's pole from the pose
        s.pole = clip.ik_solve == IkSolve::VATs ? switch_pole(rig, limb, g, clip.ik_solve) : derive_pole(rig, limb, g);
}

// IK local rotations (rest included) of root, mid and end (02 section 3.7).
void solve_two_bone(const Skeleton& skel, const LimbInfo& l, IkSolve mode, const Pose& pose,
                    const std::vector<Xform>& g, const Shape* shape, const Xform& target, const Vec3& pole,
                    Quat out[3]) {
    Xform parent = parent_global(skel, g, l.root);
    Vec3 a = g[l.root].pos, x1, y1;
    Quat bend = bend_frame(l, mode, skel.local_xform(l.mid, pose, shape).pos,
                           skel.local_xform(l.end, pose, shape).pos, (target.pos - a).length(), x1, y1);
    Vec3 x2 = target.pos - a;
    x2 = x2.length() < 1e-9 ? g[l.root].rot.rotate(x1) : x2.normalized();
    Vec3 y2 = perp_to(pole - a, x2);
    if (y2.length() < 1e-5) y2 = perp_to(parent.rot.rotate(l.fallback_pole), x2);
    y2 = y2.length() < 1e-9 ? any_perp(x2) : y2.normalized();

    Quat world = (from_basis(x2, y2, x2.cross(y2)) * from_basis(x1, y1, x1.cross(y1)).conj()).normalized();
    out[0] = (parent.rot.conj() * world).normalized();
    out[1] = bend;
    out[2] = ((world * bend).conj() * target.rot).normalized();
}

// mTorso and mChest local rotations (02 section 3.8).
void solve_spine(const Skeleton& skel, const LimbInfo& l, const Pose& pose, const std::vector<Xform>& g,
                 const Shape* shape, const Xform& target, Quat out[2]) {
    Xform parent = parent_global(skel, g, l.root);
    Vec3 oc = skel.local_xform(l.mid, pose, shape).pos, on = skel.local_xform(l.end, pose, shape).pos;
    Vec3 want = parent.rot.conj().rotate(target.pos - g[l.root].pos);
    Quat q = (parent.rot.conj() * target.rot).normalized();
    for (int it = 0; it < 4; ++it) {
        Quat h = half(q);
        Vec3 implied = h.rotate(oc + h.rotate(on));
        if (implied.length() < 1e-5 || want.length() < 1e-5) break;
        Quat lean = arc(implied.normalized(), want.normalized());
        if (lean.angle() < 1e-5) break;
        q = (lean * q).normalized();
    }
    out[0] = out[1] = half(q);
}

Xform pin_offset(const Clip& clip, const std::string& joint, double frame) {
    std::string track = "pin:" + joint;
    return {euler_to_quat(curve_euler(clip, track, frame)), curve_offset(clip, track, frame)};
}

// What a pin's goal is relative to: its target bone, the world, or another actor's bone (GR-4).
bool pin_base(const Rig& rig, const Pin& p, double frame, const std::vector<Xform>& g, Xform& out) {
    if (!p.target_actor.empty()) return rig.external && rig.external(p, frame, out);
    out = {};
    if (p.target.empty()) return true;
    int t = rig.skeleton().find(p.target);
    if (t < 0) return false;
    out = g[t];
    return true;
}

// 02 section 3.9: moves via so the joint lands on the pin's goal. A pin on a limb's end instead solves the
// limb's IK for that goal, so the arm or leg follows and nothing detaches (the reach clamp applies).
void apply_pin(const Rig& rig, const Clip& clip, const Pin& p, double frame, Evaluation& ev, const Shape* shape) {
    if (!p.active(frame)) return;
    const Skeleton& skel = rig.skeleton();
    Pose& pose = ev.pose;
    std::vector<Xform>& g = ev.globals;
    int j = skel.find(p.joint), v = skel.find(p.via);
    Xform base;
    if (j <= 0 || v <= 0 || !is_ancestor(skel, v, j) || !pin_base(rig, p, frame, g, base)) return;
    Xform goal = base * Xform{p.rot, p.pos} * pin_offset(clip, p.joint, frame);
    if (int limb = pin_limb(rig, p); limb >= 0) {
        const LimbInfo& l = rig.limbs()[limb];
        // The bend plane of the limb as it stands: its IK pole when IK drives it, else the animated elbow/knee.
        Vec3 pole = ev.limbs[limb].blend > 0 ? ev.limbs[limb].pole : switch_pole(rig, limb, g, clip.ik_solve);
        Quat ik[3];
        solve_two_bone(skel, l, clip.ik_solve, pose, g, shape, goal, pole, ik);
        const int bones[3] = {l.root, l.mid, l.end};
        for (int k = 0; k < 3; ++k) pose.rot[bones[k]] = (skel[bones[k]].rest.conj() * ik[k]).normalized();
    } else {
        Xform jl = g[v].inverse() * g[j];
        set_world(skel, v, goal * jl.inverse(), pose, g, shape);
    }
    g = skel.global_pose(pose, shape);
}

// The bones a pin's helper keys (start and release) go on: the whole limb, or the via bone.
std::vector<int> pin_bones(const Rig& rig, const Pin& p) {
    if (int limb = pin_limb(rig, p); limb >= 0) {
        const LimbInfo& l = rig.limbs()[limb];
        return {l.root, l.mid, l.end};
    }
    int v = rig.skeleton().find(p.via);
    return v >= 0 ? std::vector<int>{v} : std::vector<int>{};
}

// Evaluation with only the first npins pins applied.
Evaluation run(const Rig& rig, const Clip& clip, double frame, const Shape* shape, size_t npins) {
    const Skeleton& skel = rig.skeleton();
    const auto& limbs = rig.limbs();
    Evaluation ev;
    ev.pose = evaluate_curves(skel, clip, frame);
    ev.globals = skel.global_pose(ev.pose, shape);
    ev.limbs.resize(limbs.size());
    for (int stage = 0; stage < 3; ++stage) {
        for (size_t i = 0; i < limbs.size(); ++i) {
            const LimbInfo& l = limbs[i];
            if ((l.spine ? 0 : l.finger ? 2 : 1) != stage) continue;
            LimbState& s = ev.limbs[i];
            s.uses_ik = uses_ik(clip, l);
            s.blend = blend_at(clip, ik_track(l), frame);
            s.ik_on = s.blend >= 0.5;
            read_controller(rig, clip, static_cast<int>(i), frame, ev.globals, s);
            if (s.blend <= 0) continue;
            Quat ik[3];
            if (l.spine)
                solve_spine(skel, l, ev.pose, ev.globals, shape, s.target, ik);
            else
                solve_two_bone(skel, l, clip.ik_solve, ev.pose, ev.globals, shape, s.target, s.pole, ik);
            const int bones[3] = {l.root, l.mid, l.end};
            for (int k = 0; k < (l.spine ? 2 : 3); ++k) {
                const Node& n = skel[bones[k]];
                Quat q = s.blend >= 1 ? ik[k] : slerp(n.rest * ev.pose.rot[bones[k]], ik[k], s.blend);
                ev.pose.rot[bones[k]] = (n.rest.conj() * q).normalized();
            }
            ev.globals = skel.global_pose(ev.pose, shape);
        }
    }
    npins = std::min(npins, clip.pins.size());
    for (size_t p = 0; p < npins; ++p) apply_pin(rig, clip, clip.pins[p], frame, ev, shape);
    if (npins > 0)  // finger controllers ride the (possibly pinned) wrist
        for (size_t i = 0; i < limbs.size(); ++i)
            if (limbs[i].finger) read_controller(rig, clip, static_cast<int>(i), frame, ev.globals, ev.limbs[i]);
    return ev;
}

void key_controller(Clip& clip, const Rig& rig, double frame, int limb, const Xform& target, const Vec3& pole,
                    const std::vector<Xform>& g) {
    const LimbInfo& l = rig.limbs()[limb];
    std::string name = ik_track(l);
    Xform inv = controller_space(rig, l, g).inverse();
    Xform local = inv * target;
    key_rotation(clip, name, frame, local.rot);
    key_offset(clip, name, frame, local.pos);
    if (l.spine) return;
    Vec3 p = inv.apply(pole);
    for (int a = 0; a < 3; ++a) clip.curves[name][kPoleChannels[a]].set_key(frame, p[a]);
}

int joint_of(const Rig& rig, const Pin& p) { return rig.skeleton().find(p.joint); }

// The attachment point's parent bone holds it when the parent is a hind limb, tail, wing or groin bone (AM-80).
int via_of(const Skeleton& skel, int node) {
    const Node& n = skel[node];
    if (n.attachment && n.parent >= 0) {
        Category c = skel[n.parent].category;
        if (c == Category::HindLimbs || c == Category::Tail || c == Category::Wings || c == Category::Groin)
            return n.parent;
    }
    return node;
}

void key_pose(Clip& clip, const std::string& bone, double frame, const Pose& pose, int i) {
    key_rotation(clip, bone, frame, pose.rot[i]);
    key_offset(clip, bone, frame, pose.offset[i]);
}

// Keys via's current FK values (rotation and position).
void key_fk(Clip& clip, const std::string& bone, double frame) {
    Vec3 e = curve_euler(clip, bone, frame), o = curve_offset(clip, bone, frame);
    key_euler(clip, bone, frame, e);
    key_offset(clip, bone, frame, o);
}

// Keys a pin's bones at frame with their current FK values (limb bones: rotation only, so no position
// track appears on shoulders or hips).
void key_pin_fk(Clip& clip, const Rig& rig, const Pin& p, double frame) {
    if (pin_limb(rig, p) < 0) return key_fk(clip, p.via, frame);
    for (int b : pin_bones(rig, p)) {
        const std::string& n = rig.skeleton()[b].name;
        key_euler(clip, n, frame, curve_euler(clip, n, frame));
    }
}

// Keys a pin's bones at frame with an evaluated pose.
void key_pin_pose(Clip& clip, const Rig& rig, const Pin& p, double frame, const Pose& pose) {
    bool limb = pin_limb(rig, p) >= 0;
    for (int b : pin_bones(rig, p)) {
        const std::string& n = rig.skeleton()[b].name;
        if (limb) key_rotation(clip, n, frame, pose.rot[b]);
        else key_pose(clip, n, frame, pose, b);
    }
}

void delete_pin_keys(Clip& clip, const Rig& rig, const Pin& p, int frame) {
    for (int b : pin_bones(rig, p)) delete_keys_at(clip, rig.skeleton()[b].name, frame);
}

// Re-keys via at to + 1 with the pose evaluated as if the pin still covered that frame (AM-84).
void rekey_release(Clip& clip, const Rig& rig, size_t k, const Shape* shape) {
    Pin& p = clip.pins[k];
    if (p.to < 0 || pin_bones(rig, p).empty()) {
        p.release_key = -1;
        return;
    }
    int f = p.to + 1, to = p.to;
    p.to = f;
    Evaluation ev = evaluate(rig, clip, f, shape);
    clip.pins[k].to = to;
    key_pin_pose(clip, rig, clip.pins[k], f, ev.pose);
    clip.pins[k].release_key = f;
}

// End of the previous pin and start of the next one on the same joint (INT_MIN / INT_MAX when none).
void neighbours(const Clip& clip, const Rig& rig, size_t k, int& prev_end, int& next_start) {
    prev_end = INT_MIN;
    next_start = INT_MAX;
    const Pin& p = clip.pins[k];
    int j = joint_of(rig, p);
    for (size_t i = 0; i < clip.pins.size(); ++i) {
        const Pin& o = clip.pins[i];
        if (i == k || joint_of(rig, o) != j) continue;
        if (o.from < p.from && o.to >= 0) prev_end = std::max(prev_end, o.to);
        if (o.from > p.from) next_start = std::min(next_start, o.from);
    }
}

}  // namespace

Rig::Rig(const Skeleton& skel) : skel_(skel), limb_of_(skel.size(), -1) {
    auto add = [&](std::string name, std::string label, const std::string& r, const std::string& m,
                   const std::string& e, Vec3 hinge, Vec3 pole, bool finger = false, bool spine = false) {
        LimbInfo l{std::move(name), std::move(label), skel.find(r), skel.find(m), skel.find(e), hinge, pole, finger,
                   spine};
        if (l.root < 0 || l.mid < 0 || l.end < 0) return;
        if (finger) {  // AM-51
            Vec3 h = skel[l.mid].end.normalized().cross({0, 0, -1});
            l.hinge = h.length() < 1e-4 ? Vec3{1, 0, 0} : h.normalized();
        }
        int n = static_cast<int>(limbs_.size());
        limb_of_[l.root] = limb_of_[l.mid] = n;
        if (!spine) limb_of_[l.end] = n;
        limbs_.push_back(std::move(l));
    };
    add("ArmLeft", "Left Arm", "mShoulderLeft", "mElbowLeft", "mWristLeft", {0, 0, -1}, {-1, 0, 0});
    add("ArmRight", "Right Arm", "mShoulderRight", "mElbowRight", "mWristRight", {0, 0, 1}, {-1, 0, 0});
    for (std::string s : {"Left", "Right"}) add("Leg" + s, s + " Leg", "mHip" + s, "mKnee" + s, "mAnkle" + s,
                                               {0, 1, 0}, {1, 0, 0});
    for (std::string s : {"Left", "Right"})
        for (std::string f : {"Thumb", "Index", "Middle", "Ring", "Pinky"})
            add(f + s, s + " " + f, "mHand" + f + "1" + s, "mHand" + f + "2" + s, "mHand" + f + "3" + s, {},
                {0, 0, 1}, true);
    add("Spine", "Spine", "mTorso", "mChest", "mNeck", {}, {}, false, true);
    for (std::string s : {"Left", "Right"})
        add("HindLeg" + s, s + " Hind Leg", "mHindLimb1" + s, "mHindLimb2" + s, "mHindLimb3" + s, {0, 1, 0},
            {1, 0, 0});
    add("WingLeft", "Left Wing", "mWing1Left", "mWing2Left", "mWing3Left", {0, 0, -1}, {-1, 0, 0});
    add("WingRight", "Right Wing", "mWing1Right", "mWing2Right", "mWing3Right", {0, 0, 1}, {-1, 0, 0});
}

int Rig::limb_of_bone(int node) const {
    return node >= 0 && node < static_cast<int>(limb_of_.size()) ? limb_of_[node] : -1;
}

int Rig::find_limb(std::string_view name) const {
    for (size_t i = 0; i < limbs_.size(); ++i)
        if (limbs_[i].name == name) return static_cast<int>(i);
    return -1;
}

bool uses_ik(const Clip& clip, const LimbInfo& limb) {
    auto t = clip.curves.find(ik_track(limb));
    if (t == clip.curves.end()) return false;
    auto c = t->second.find("blend");
    if (c == t->second.end()) return false;
    return std::any_of(c->second.keys.begin(), c->second.keys.end(), [](const Key& k) { return k.value > 0; });
}

Evaluation evaluate(const Rig& rig, const Clip& clip, double frame, const Shape* shape) {
    return run(rig, clip, frame, shape, clip.pins.size());
}

Vec3 derive_pole(const Rig& rig, int limb, const std::vector<Xform>& g) {
    const LimbInfo& l = rig.limbs()[limb];
    Vec3 a = g[l.root].pos, m = g[l.mid].pos, t = g[l.end].pos;
    Vec3 u = (t - a).length() > 1e-9 ? (t - a).normalized() : any_perp(m - a);
    Vec3 foot = a + u * (m - a).dot(u);
    Vec3 off = m - foot;
    double bend = std::acos(std::clamp((m - a).normalized().dot((t - m).normalized()), -1.0, 1.0));
    if (bend < 15 * kDegToRad || off.length() < 0.005) {
        off = perp_to(parent_global(rig.skeleton(), g, l.root).rot.rotate(l.fallback_pole), u);
        if (off.length() < 1e-9) off = any_perp(u);
    }
    return foot + off.normalized() * (l.finger ? 0.04 : 0.40);
}

void key_blend(Clip& clip, const Rig& rig, double frame, int limb, double value) {
    FCurve& c = clip.curves[ik_track(rig.limbs()[limb])]["blend"];
    if (c.empty() && frame > 0 && !same_frame(frame, 0)) c.set_key(0, 1 - value, Interp::Constant);
    c.set_key(frame, value, Interp::Constant);
}

void switch_to_ik(Clip& clip, const Rig& rig, double frame, int limb, const Shape* shape) {
    Evaluation ev = evaluate(rig, clip, frame, shape);
    const LimbInfo& l = rig.limbs()[limb];
    Vec3 pole = l.spine ? Vec3{} : switch_pole(rig, limb, ev.globals, clip.ik_solve);
    key_controller(clip, rig, frame, limb, end_world(l, ev.globals), pole, ev.globals);
    key_blend(clip, rig, frame, limb, 1);
}

void switch_to_fk(Clip& clip, const Rig& rig, double frame, int limb, const Shape* shape) {
    Evaluation ev = evaluate(rig, clip, frame, shape);
    const LimbInfo& l = rig.limbs()[limb];
    const int bones[3] = {l.root, l.mid, l.end};
    for (int k = 0; k < (l.spine ? 2 : 3); ++k)
        key_rotation(clip, rig.skeleton()[bones[k]].name, frame, ev.pose.rot[bones[k]]);
    key_blend(clip, rig, frame, limb, 0);
}

void key_limb_target(Clip& clip, const Rig& rig, double frame, int limb, const Xform& world_target,
                     const Shape* shape) {
    Evaluation ev = evaluate(rig, clip, frame, shape);
    key_controller(clip, rig, frame, limb, world_target, ev.limbs[limb].pole, ev.globals);
}

void key_limb_pole(Clip& clip, const Rig& rig, double frame, int limb, const Vec3& world_pole, const Shape* shape) {
    if (rig.limbs()[limb].spine) return;
    Evaluation ev = evaluate(rig, clip, frame, shape);
    key_controller(clip, rig, frame, limb, ev.limbs[limb].target, world_pole, ev.globals);
}

bool pin_here(Clip& clip, const Rig& rig, double frame, int node, int target_bone, const Shape* shape,
              std::string& why) {
    const Skeleton& skel = rig.skeleton();
    if (node <= 0 || node >= skel.size()) {
        why = "Pick a bone or attachment point to pin. mPelvis cannot be pinned: move the hips instead.";
        return false;
    }
    int via = via_of(skel, node);
    if (target_bone >= skel.size()) {
        why = "The pin target is not in the skeleton.";
        return false;
    }
    if (target_bone >= 0 && (target_bone == node || is_ancestor(skel, via, target_bone))) {
        why = "A pin cannot ride on the bone it pins (" + skel[via].name + " or anything it carries).";
        return false;
    }
    int f = static_cast<int>(std::lround(frame));
    Evaluation ev = evaluate(rig, clip, f, shape);
    Pin p;
    p.joint = skel[node].name;
    p.via = skel[via].name;
    p.target = target_bone >= 0 ? skel[target_bone].name : "";
    p.from = f;
    Xform tg = target_bone >= 0 ? ev.globals[target_bone] : Xform{};
    Xform held = tg.inverse() * ev.globals[node] * pin_offset(clip, p.joint, f).inverse();
    p.pos = held.pos;
    p.rot = held.rot.normalized();

    int old = pin_at(clip, rig, node, f);
    if (old >= 0) {
        Pin& o = clip.pins[old];
        p.to = o.to;
        p.release_key = o.release_key;
        if (o.from >= f) {
            clip.pins.erase(clip.pins.begin() + old);
        } else {
            o.to = f - 1;
            o.release_key = -1;
        }
    } else {
        for (auto& o : clip.pins)
            if (joint_of(rig, o) == node && o.from > f && (p.to < 0 || o.from - 1 < p.to)) p.to = o.from - 1;
    }
    clip.pins.push_back(p);
    if (p.release_key >= 0) rekey_release(clip, rig, clip.pins.size() - 1, shape);
    return true;
}

bool pin_to_actor(Clip& clip, const Rig& rig, double frame, int node, const std::string& actor, const std::string& bone,
                  const Shape* shape, std::string& why) {
    int f = static_cast<int>(std::lround(frame));
    Pin probe;
    probe.target = bone;
    probe.target_actor = actor;
    Xform tg;
    if (actor.empty() || !rig.external || !rig.external(probe, f, tg)) {
        why = "The other actor's bone could not be found.";
        return false;
    }
    if (!pin_here(clip, rig, f, node, -1, shape, why)) return false;  // held in the world first
    int k = pin_at(clip, rig, node, f);
    Pin& p = clip.pins[k];
    Xform held = tg.inverse() * Xform{p.rot, p.pos};
    p.target = bone;
    p.target_actor = actor;
    p.pos = held.pos;
    p.rot = held.rot.normalized();
    return true;
}

bool unpin_here(Clip& clip, const Rig& rig, double frame, int node, const Shape* shape, std::string& why) {
    int k = pin_at(clip, rig, node, frame);
    if (k < 0) {
        why = "Nothing is pinned here at this frame.";
        return false;
    }
    int f = static_cast<int>(std::lround(frame));
    if (f <= clip.pins[k].from) {
        delete_pin(clip, rig, k);
        return true;
    }
    if (pin_bones(rig, clip.pins[k]).empty()) {
        why = "The pin's bone \"" + clip.pins[k].via + "\" is not in the skeleton.";
        return false;
    }
    Evaluation ev = evaluate(rig, clip, f, shape);
    Pin& p = clip.pins[k];
    if (p.from > 0) {
        key_pin_fk(clip, rig, p, p.from);
        p.start_key = p.from;
    }
    p.to = f - 1;
    key_pin_pose(clip, rig, p, f, ev.pose);
    p.release_key = f;
    return true;
}

void delete_pin(Clip& clip, const Rig& rig, size_t pin) {
    if (pin >= clip.pins.size()) return;
    Pin p = clip.pins[pin];
    if (p.release_key >= 0) delete_pin_keys(clip, rig, p, p.release_key);
    if (p.start_key >= 0) delete_pin_keys(clip, rig, p, p.start_key);
    clip.pins.erase(clip.pins.begin() + static_cast<std::ptrdiff_t>(pin));
    if (std::none_of(clip.pins.begin(), clip.pins.end(), [&](const Pin& o) { return o.joint == p.joint; }))
        clip.curves.erase("pin:" + p.joint);
}

void move_pin_start(Clip& clip, const Rig& rig, size_t pin, int frame, const Shape*) {
    if (pin >= clip.pins.size()) return;
    int prev_end, next_start;
    neighbours(clip, rig, pin, prev_end, next_start);
    Pin& p = clip.pins[pin];
    int lo = std::max(0, prev_end == INT_MIN ? 0 : prev_end + 1);
    int hi = p.to >= 0 ? p.to : clip.end_frame;
    int from = std::min(std::max(frame, lo), hi);
    if (p.start_key >= 0 && p.start_key == p.from) {
        delete_pin_keys(clip, rig, p, p.start_key);
        p.start_key = -1;
    }
    p.from = from;
    if (from > 0 && p.to >= 0) {
        key_pin_fk(clip, rig, p, from);
        p.start_key = from;
    }
}

void move_pin_end(Clip& clip, const Rig& rig, size_t pin, int frame, const Shape* shape) {
    if (pin >= clip.pins.size()) return;
    int prev_end, next_start;
    neighbours(clip, rig, pin, prev_end, next_start);
    Pin& p = clip.pins[pin];
    int hi = next_start == INT_MAX ? clip.end_frame + 1 : std::min(clip.end_frame + 1, next_start - 1);
    int to = std::max(std::min(std::max(frame, p.from + 1), hi), p.from);
    if (p.release_key >= 0 && p.to >= 0 && p.release_key == p.to + 1) delete_pin_keys(clip, rig, p, p.release_key);
    p.release_key = -1;
    p.to = to > clip.end_frame ? -1 : to;
    rekey_release(clip, rig, pin, shape);
}

int pin_limb(const Rig& rig, const Pin& p) {
    int j = rig.skeleton().find(p.joint);
    int limb = rig.limb_of_bone(j);
    if (limb < 0 || p.via != p.joint) return -1;
    const LimbInfo& l = rig.limbs()[limb];
    return !l.spine && l.end == j ? limb : -1;
}

int pin_at(const Clip& clip, const Rig& rig, int node, double frame) {
    for (size_t i = 0; i < clip.pins.size(); ++i)
        if (clip.pins[i].active(frame) && joint_of(rig, clip.pins[i]) == node) return static_cast<int>(i);
    return -1;
}

bool key_pinned_point(Clip& clip, const Rig& rig, double frame, int node, const Xform& world, const Shape* shape) {
    int k = pin_at(clip, rig, node, frame);
    if (k < 0) return false;
    Evaluation ev = run(rig, clip, frame, shape, static_cast<size_t>(k));  // the target as this pin sees it
    const Pin& p = clip.pins[k];
    Xform tg;
    if (!pin_base(rig, p, frame, ev.globals, tg)) return false;
    Xform off = (tg * Xform{p.rot, p.pos}).inverse() * world;
    std::string track = "pin:" + p.joint;
    key_rotation(clip, track, frame, off.rot);
    key_offset(clip, track, frame, off.pos);
    return true;
}

bool follow_bake(Clip& clip, const Rig& rig, int target, int follower, int f0, int f1, bool keep_offset,
                 const Shape* shape, std::string& why) {
    const Skeleton& skel = rig.skeleton();
    if (target < 0 || target >= skel.size() || follower < 0 || follower >= skel.size()) {
        why = "Pick a target and a follower.";
        return false;
    }
    int keyed = skel[follower].attachment ? skel[follower].parent : follower;
    if (keyed <= 0) {
        why = "mPelvis cannot follow a target: it carries the whole body.";
        return false;
    }
    if (is_ancestor(skel, keyed, target)) {
        why = skel[keyed].name + " carries the target, so it cannot follow it.";
        return false;
    }
    f1 = std::max(f1, f0);
    Xform offset;
    if (keep_offset) {
        Evaluation ev = evaluate(rig, clip, f0, shape);
        offset = ev.globals[target].inverse() * ev.globals[follower];
    }
    const std::string& name = skel[keyed].name;
    for (int f = f0; f <= f1; ++f) {
        Evaluation ev = evaluate(rig, clip, f, shape);
        Xform jl = ev.globals[keyed].inverse() * ev.globals[follower];
        set_world(skel, keyed, ev.globals[target] * offset * jl.inverse(), ev.pose, ev.globals, shape);
        key_pose(clip, name, f, ev.pose, keyed);
    }
    return true;
}

namespace {

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// The first bone of a limb's chain, where a default Auto IK chain stops (AI-2).
bool chain_stop(const std::string& name) {
    for (const char* p : {"mCollar", "mHipLeft", "mHipRight", "mHindLimb1", "mWing1", "mTail1"})
        if (starts_with(name, p)) return true;
    return starts_with(name, "mHand") && name.find('1') != std::string::npos;  // a finger's first joint
}

// The bone does not turn by its keys at frame: an IK limb (blend > 0) or a pin drives it.
bool driven(const Rig& rig, const Clip& clip, double frame, int node) {
    const Skeleton& skel = rig.skeleton();
    if (int limb = rig.limb_of_bone(node); limb >= 0) {
        const LimbInfo& l = rig.limbs()[limb];
        if (blend_at(clip, ik_track(l), frame) > 0 && (node == l.root || node == l.mid || (!l.spine && node == l.end)))
            return true;
    }
    for (const Pin& p : clip.pins) {
        if (!p.active(frame)) continue;
        if (int limb = pin_limb(rig, p); limb >= 0) {
            const LimbInfo& l = rig.limbs()[limb];
            if (node == l.root || node == l.mid || node == l.end) return true;
        } else if (skel.find(p.via) == node) {
            return true;
        }
    }
    return false;
}

// Bento's mSpine1..4: their offsets cancel in pairs, so a chain passes through them without turning them (AI-3).
bool passive(const std::string& name) { return starts_with(name, "mSpine"); }

// How freely a bone turns in a solve: the collar and the spine give less than the limbs (AI-4).
double stiffness_weight(const std::string& name) {
    if (passive(name)) return 0;
    if (starts_with(name, "mCollar")) return 0.25;
    if (name == "mTorso" || name == "mChest") return 0.1;
    if (name == "mNeck") return 0.3;
    return 1;
}

// y = m^-1 e for a symmetric positive definite 3 x 3 m.
Vec3 solve3(const double m[3][3], const Vec3& e) {
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2],
                 c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (std::fabs(det) < 1e-30) return {};
    const double inv[3][3] = {{c00, m[0][2] * m[2][1] - m[0][1] * m[2][2], m[0][1] * m[1][2] - m[0][2] * m[1][1]},
                              {c01, m[0][0] * m[2][2] - m[0][2] * m[2][0], m[0][2] * m[1][0] - m[0][0] * m[1][2]},
                              {c02, m[0][1] * m[2][0] - m[0][0] * m[2][1], m[0][0] * m[1][1] - m[0][1] * m[1][0]}};
    Vec3 y;
    for (int r = 0; r < 3; ++r) y[r] = (inv[r][0] * e.x + inv[r][1] * e.y + inv[r][2] * e.z) / det;
    return y;
}

}  // namespace

int auto_ik_default_length(const Skeleton& skel, int node) {
    if (node <= 0 || node >= skel.joint_count() || skel[node].category == Category::Face) return 0;
    int n = 0;
    for (int i = skel[node].parent; i > 0 && n < 6; i = skel[i].parent) {
        if (passive(skel[i].name)) continue;
        ++n;
        if (chain_stop(skel[i].name)) return n;
    }
    return std::min(n, 2);
}

AutoIkChain auto_ik_chain(const Rig& rig, const Clip& clip, double frame, int node, int length) {
    const Skeleton& skel = rig.skeleton();
    AutoIkChain c;
    c.end = node;
    if (node <= 0 || node >= skel.size()) {
        c.why = "Auto IK needs a bone with bones above it: the pelvis moves the whole body";
        return c;
    }
    const std::string& name = skel[node].name;
    if (node >= skel.joint_count() || skel[node].category == Category::Face) {
        c.why = name + " moves by position, not Auto IK";
        return c;
    }
    if (clip.has_channels(name, kPosChannels)) {
        c.why = name + " has position keys, so Move moves it";
        return c;
    }
    if (pin_at(clip, rig, node, frame) >= 0 || driven(rig, clip, frame, node)) {
        c.why = name + " is held by a pin or an IK limb here";
        return c;
    }
    const int want = length > 0 ? length : auto_ik_default_length(skel, node);
    std::vector<int> up;  // every bone the chain may take, nearest first
    for (int i = skel[node].parent; i > 0 && !driven(rig, clip, frame, i); i = skel[i].parent) {
        up.push_back(i);
        c.longest += !passive(skel[i].name);
    }
    if (c.longest == 0) {
        c.why = "The bone above " + name + " is held by a pin or an IK limb here";
        return c;
    }
    // length counts the bones that turn; the mSpine bones between them come along, none at the root.
    for (int i = 0; c.turning < std::clamp(want, 1, c.longest); ++i) {
        c.bones.insert(c.bones.begin(), up[i]);
        c.turning += !passive(skel[up[i]].name);
    }
    return c;
}

std::vector<Vec3> auto_ik_hinges(const Rig& rig, const Shape* shape) {
    const Skeleton& skel = rig.skeleton();
    std::vector<Vec3> h(static_cast<size_t>(skel.size()));
    for (const LimbInfo& l : rig.limbs()) {
        if (l.spine) continue;
        h[l.mid] = l.hinge;
        if (l.finger) h[l.end] = l.hinge;  // fingers curl at both joints
        if (starts_with(skel[l.root].name, "mHindLimb")) h[l.end] = l.hinge;  // the hock bends as the knee does
    }
    // A body that bends a hinge at rest (a creature's legs rigged bent) bends it in that plane: without rig axes, the
    // plane's normal is the hinge (AI-4).
    const std::vector<Xform> rest = shape ? skel.global_pose(Pose(skel.size()), shape) : std::vector<Xform>{};
    for (int j = 0; j < skel.size() && shape; ++j) {
        const int up = skel[j].parent;
        const int down = skel[j].children.empty() ? -1 : skel[j].children.front();
        if (h[j].length() < 1e-9 || has_rig_axes(shape, j) || up < 0 || down < 0 || down >= skel.joint_count()) continue;
        const Vec3 a = rest[j].pos - rest[up].pos, b = rest[down].pos - rest[j].pos, n = a.cross(b);
        if (a.length() < 1e-4 || b.length() < 1e-4 || n.length() < std::sin(20 * kDegToRad) * a.length() * b.length()) continue;
        const Vec3 hinge = rest[up].rot.conj().rotate(n.normalized());
        h[j] = hinge * (hinge.dot(h[j]) < 0 ? -1.0 : 1.0);
    }
    for (int j = 0; j < skel.size(); ++j) {
        if (h[j].length() < 1e-9 || !has_rig_axes(shape, j)) continue;
        // The rig axis nearest the SL hinge, of the two across the bone, pointing the same way (AI-4).
        const Quat to_parent = skel[j].rest * shape->axes[j];
        const Vec3 tail = j < static_cast<int>(shape->tails.size()) && shape->tails[j].length() > 0 ? shape->tails[j] : skel[j].end;
        const Vec3 along = skel[j].rest.rotate(tail).normalized();
        Vec3 cand[3];
        int skip = 0;
        for (int k = 0; k < 3; ++k) {
            Vec3 e;
            e[k] = 1;
            cand[k] = to_parent.rotate(e);
            if (std::fabs(cand[k].dot(along)) > std::fabs(cand[skip].dot(along))) skip = k;
        }
        int best = skip == 0 ? 1 : 0;
        for (int k = 0; k < 3; ++k)
            if (k != skip && std::fabs(cand[k].dot(h[j])) > std::fabs(cand[best].dot(h[j]))) best = k;
        h[j] = cand[best] * (cand[best].dot(h[j]) < 0 ? -1.0 : 1.0);
    }
    return h;
}

namespace {

// Damped least squares over chain (AI-4).
void dls(const Skeleton& skel, const Shape* shape, const AutoIkChain& chain, const std::vector<Vec3>& hinges,
         const Vec3& target, Pose& pose) {
    const std::vector<int>& b = chain.bones;
    const int n = static_cast<int>(b.size());
    if (n == 0 || chain.end < 0) return;
    const Xform base = parent_global(skel, skel.global_pose(pose, shape), b[0]);
    std::vector<Xform> g(n);
    Vec3 end;
    auto fk = [&] {
        Xform cur = base;
        for (int k = 0; k < n; ++k) g[k] = cur = cur * skel.local_xform(b[k], pose, shape);
        end = cur.apply(skel.local_xform(chain.end, pose, shape).pos);
    };
    auto parent_rot = [&](int k) { return k > 0 ? g[k - 1].rot : base.rot; };
    auto hinge_of = [&](int k) { return b[k] < static_cast<int>(hinges.size()) ? hinges[b[k]] : Vec3{}; };
    // The signed bend at hinge k, about its world hinge: from the bone above it to its own bone.
    auto bend = [&](int k, const Vec3& axis) {
        const Vec3 above = g[k].pos - (k > 0 ? g[k - 1].pos : base.pos), own = (k + 1 < n ? g[k + 1].pos : end) - g[k].pos;
        const Vec3 a = perp_to(above, axis), o = perp_to(own, axis);
        return a.length() < 1e-6 || o.length() < 1e-6 ? 0.0 : std::atan2(axis.dot(a.cross(o)), a.dot(o));
    };
    fk();
    double reach = (end - g[0].pos).length();
    for (int k = 1; k < n; ++k) reach += (g[k].pos - g[k - 1].pos).length();
    reach = std::max(reach, 1e-3);
    // A hinge keeps the side it bends to (straight counts as SL's usual side): it never folds through straight.
    std::vector<double> side(n, 0);
    for (int k = 0; k < n; ++k)
        if (Vec3 h = hinge_of(k); h.length() > 1e-9) side[k] = bend(k, parent_rot(k).rotate(h).normalized()) < -kDegToRad ? -1 : 1;
    const double min_bend = 1 * kDegToRad, max_bend = 178 * kDegToRad, lambda = 0.05 * reach;
    struct Dof {
        int k;
        Vec3 axis, col;
        double w;
    };
    std::vector<Dof> dofs;
    for (int it = 0; it < 150; ++it) {
        Vec3 e = target - end;
        if (e.length() < 1e-6) break;
        if (e.length() > 0.2 * reach) e = e.normalized() * (0.2 * reach);  // small steps: the solve follows a path
        dofs.clear();
        for (int k = 0; k < n; ++k) {
            const Vec3 r = end - g[k].pos, h = hinge_of(k);
            const double w = stiffness_weight(skel[b[k]].name);
            if (w == 0) continue;
            if (h.length() > 1e-9) {
                const Vec3 axis = parent_rot(k).rotate(h).normalized();
                dofs.push_back({k, axis, axis.cross(r), w});
            } else {
                for (Vec3 axis : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) dofs.push_back({k, axis, axis.cross(r), w});
            }
        }
        // A hinge at its stop that the step would push further takes no part in it (the others make up for it), so
        // the solve is done again without it.
        std::vector<Vec3> turn(n);  // world rotation vector per bone
        for (int pass = 0; pass < 2; ++pass) {
            double m[3][3] = {{lambda * lambda, 0, 0}, {0, lambda * lambda, 0}, {0, 0, lambda * lambda}};
            for (const Dof& d : dofs)
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) m[r][c] += d.w * d.col[r] * d.col[c];
            const Vec3 y = solve3(m, e);
            bool stopped = false;
            std::fill(turn.begin(), turn.end(), Vec3{});
            for (Dof& d : dofs) {
                double a = d.w * d.col.dot(y);
                if (side[d.k] != 0) {  // a hinge: the bend changes by exactly a
                    const double s = bend(d.k, d.axis) * side[d.k];
                    const double clamped = std::clamp(a * side[d.k], min_bend - s, max_bend - s) * side[d.k];
                    if (pass == 0 && d.w > 0 && std::fabs(clamped - a) > 1e-9 && std::fabs(clamped) < 1e-6)
                        d.w = 0, stopped = true;
                    a = clamped;
                }
                turn[d.k] += d.axis * a;
            }
            if (!stopped) break;
        }
        for (int k = 0; k < n; ++k) {
            const double a = turn[k].length();
            if (a < 1e-12) continue;
            const Quat pr = parent_rot(k), r = Quat::axis_angle(turn[k], a);
            const Node& node = skel[b[k]];
            const Quat local = (pr.conj() * r * pr * node.rest * pose.rot[b[k]]).normalized();
            pose.rot[b[k]] = (node.rest.conj() * local).normalized();
        }
        fk();
    }
}

}  // namespace

void solve_auto_ik(const Skeleton& skel, const Shape* shape, const AutoIkChain& chain, const std::vector<Vec3>& hinges,
                   const Vec3& target, Pose& pose) {
    // A chain longer than its limb's default reaches with the limb first; the bones above take only what is left, as
    // the spine does for an IK target's Pull (RC-1). Solved all at once, a straight arm pulled in would bend the chest
    // (the only bone that moves the hand towards the shoulder at first) instead of the elbow.
    const int limb = auto_ik_default_length(skel, chain.end);
    if (limb > 0 && chain.turning > limb) {
        AutoIkChain inner = chain;
        while (inner.turning > limb || passive(skel[inner.bones.front()].name)) {
            inner.turning -= !passive(skel[inner.bones.front()].name);
            inner.bones.erase(inner.bones.begin());
        }
        dls(skel, shape, inner, hinges, target, pose);
    }
    dls(skel, shape, chain, hinges, target, pose);
}

std::vector<std::string> key_auto_ik(Clip& clip, const Rig& rig, double frame, const AutoIkChain& chain,
                                     const Evaluation& start, const Vec3& target, const Shape* shape, Pose* solved) {
    std::vector<std::string> keyed;
    if (chain.bones.empty()) return keyed;
    Pose pose = start.pose;
    solve_auto_ik(rig.skeleton(), shape, chain, auto_ik_hinges(rig, shape), target, pose);
    for (int b : chain.bones) {
        if (passive(rig.skeleton()[b].name)) continue;  // never turned
        keyed.push_back(rig.skeleton()[b].name);
        key_rotation(clip, keyed.back(), frame, pose.rot[b]);
    }
    if (solved) *solved = std::move(pose);
    return keyed;
}

}  // namespace vats
