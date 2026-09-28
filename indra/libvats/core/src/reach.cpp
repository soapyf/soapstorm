// Viewport Avatar Toolset - full-body reach (spec 08 section 21, RC-1).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/reach.h"

#include <algorithm>
#include <cmath>

#include "vats/edit.h"

namespace vats {
namespace {

constexpr double kMaxLean = 30 * kDegToRad;  // the most the spine leans for a reach
constexpr double kReach = 0.999;             // of the limb's length: inside the IK's own reach clamp
constexpr double kSlack = 0.0005;            // m past the limb's length that still counts as reached

}  // namespace

double ik_pull(const Clip& clip, const LimbInfo& limb) {
    auto p = clip.ik_pull.find(limb.name);
    return p == clip.ik_pull.end() ? 0 : std::clamp(p->second, 0.0, 1.0);
}

double reach_with_body(Clip& clip, const Rig& rig, double frame, int limb, double pull, const Shape* shape) {
    const Skeleton& skel = rig.skeleton();
    const LimbInfo& l = rig.limbs()[limb];
    pull = std::clamp(pull, 0.0, 1.0);
    if (pull <= 0 || l.spine || l.finger) return 0;
    Evaluation e = evaluate(rig, clip, frame, shape);
    const Vec3 target = e.limbs[limb].target.pos;
    const double length = (e.globals[l.mid].pos - e.globals[l.root].pos).length() +
                          (e.globals[l.end].pos - e.globals[l.mid].pos).length();
    // Out of reach is past the whole limb (a straight arm already reaches); the body then brings it to kReach.
    if ((target - e.globals[l.root].pos).length() <= length + kSlack) return 0;
    auto short_by = [&](const Vec3& root) { return (target - root).length() - kReach * length; };

    // The spine leans first, for the limbs it carries (the arms): about mTorso, towards the target.
    const int s = rig.find_limb("Spine");
    const LimbInfo* sp = s >= 0 ? &rig.limbs()[s] : nullptr;
    bool carried = false;
    for (int n = l.root; sp && n >= 0 && !carried; n = skel[n].parent) carried = n == sp->mid;
    if (carried) {
        const Vec3 pivot = e.globals[sp->root].pos, arm = e.globals[l.root].pos - pivot, to = target - pivot;
        const Vec3 axis = arm.cross(to);
        const double most = std::min(kMaxLean, std::acos(std::clamp(arm.normalized().dot(to.normalized()), -1.0, 1.0)));
        if (axis.length() > 1e-9 && most > 1e-6) {
            // The smallest lean that brings the shoulder within reach, or the most allowed.
            double lo = 0, hi = most;
            if (short_by(pivot + Quat::axis_angle(axis, hi).rotate(arm)) <= 0)
                for (int it = 0; it < 30; ++it) {
                    const double mid = 0.5 * (lo + hi);
                    (short_by(pivot + Quat::axis_angle(axis, mid).rotate(arm)) > 0 ? lo : hi) = mid;
                }
            const Quat lean = Quat::axis_angle(axis, hi);
            const Xform st = e.limbs[s].target;
            const Xform leaned{(lean * st.rot).normalized(), pivot + lean.rotate(st.pos - pivot)};
            if (e.limbs[s].blend > 0) {
                key_limb_target(clip, rig, frame, s, leaned, shape);
            } else {  // an FK spine: solve it on a copy with the spine in IK, and key the rotations it gives
                Clip solve = clip;
                key_limb_target(solve, rig, frame, s, leaned, shape);
                FCurve& blend = solve.curves["ik." + sp->name]["blend"];
                blend = FCurve{};
                blend.set_key(frame, 1);
                const Pose pose = evaluate(rig, solve, frame, shape).pose;
                key_rotation(clip, skel[sp->root].name, frame, pose.rot[sp->root]);
                key_rotation(clip, skel[sp->mid].name, frame, pose.rot[sp->mid]);
            }
            e = evaluate(rig, clip, frame, shape);
        }
    }
    // Then the hips, by what is still missing x Pull, straight towards the target.
    const Vec3 root = e.globals[l.root].pos;
    const double missing = short_by(root);
    if (missing <= 0 || skel.find("mPelvis") < 0) return 0;
    const Vec3 move = (target - root).normalized() * (missing * pull);
    key_offset(clip, "mPelvis", frame, curve_offset(clip, "mPelvis", frame) + move);
    return move.length();
}

}  // namespace vats
