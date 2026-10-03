// Viewport Avatar Toolset - rig constraints and joint limits.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/rig_constraints.h"
#include "vats/pose_ops.h"

#include <algorithm>
#include <cmath>

namespace vats {

namespace {

// swing turned back to at most max radians, about its own axis.
Quat cap_swing(const Quat& swing, double max) {
    const Vec3 s{swing.x, swing.y, swing.z};
    const double sin_half = s.length();
    if (sin_half < 1e-9 || 2.0 * std::atan2(sin_half, swing.w) <= max) return swing;
    return Quat::axis_angle(s * (1.0 / sin_half), max);
}

// A cone's stop in the direction swing (about an axis across bone_axis) turns the bone's tip.
double swing_stop(const JointLimit& limit, const Quat& swing) {
    const Vec3 u = limit.bone_axis.length() > 1e-6 ? limit.bone_axis.normalized() : Vec3{0, 1, 0};
    return cone_stop(limit, Vec3{swing.x, swing.y, swing.z}.cross(u));
}

}  // namespace

Vec3 cone_stop_dir(const JointLimit& limit, int k, int n) {
    const Vec3 u = limit.bone_axis.length() > 1e-6 ? limit.bone_axis.normalized() : Vec3{0, 1, 0};
    Vec3 e1 = limit.cone_ref - u * limit.cone_ref.dot(u);
    e1 = e1.length() > 1e-3 ? e1.normalized() : u.cross(std::fabs(u.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}).normalized();
    const Vec3 e2 = u.cross(e1);
    const double phi = 2.0 * kPi * k / n;
    return e1 * std::cos(phi) + e2 * std::sin(phi);
}

namespace {

// Where tip_dir falls among a cone's n stops: between stop k and k + 1, a fraction f of the way.
bool stop_bracket(const JointLimit& limit, const Vec3& tip_dir, int& k, double& f) {
    const int n = static_cast<int>(limit.cone_stops.size());
    const Vec3 u = limit.bone_axis.length() > 1e-6 ? limit.bone_axis.normalized() : Vec3{0, 1, 0};
    const Vec3 d = tip_dir - u * tip_dir.dot(u);
    if (n == 0 || d.length() < 1e-9) return false;
    const Vec3 e1 = cone_stop_dir(limit, 0, 4), e2 = cone_stop_dir(limit, 1, 4);
    double t = std::atan2(d.dot(e2), d.dot(e1)) / (2.0 * kPi / n);
    if (t < 0) t += n;
    k = std::min(static_cast<int>(t), n - 1);
    f = t - k;
    return true;
}

}  // namespace

double cone_stop(const JointLimit& limit, const Vec3& tip_dir) {
    int k;
    double f;
    if (!stop_bracket(limit, tip_dir, k, f)) return limit.cone_angle;
    const int n = static_cast<int>(limit.cone_stops.size());
    return std::min(limit.cone_angle, limit.cone_stops[k] * (1 - f) + limit.cone_stops[(k + 1) % n] * f);
}

void widen_cone_swing(JointLimit& limit, const Quat& swing, double margin) {
    const Vec3 s{swing.x, swing.y, swing.z};
    const double want = std::min(kPi, 2.0 * std::atan2(s.length(), std::fabs(swing.w)) + margin);
    if (want <= swing_stop(limit, swing)) return;
    limit.cone_angle = std::max(limit.cone_angle, want);
    const Vec3 u = limit.bone_axis.length() > 1e-6 ? limit.bone_axis.normalized() : Vec3{0, 1, 0};
    int k;
    double f;
    if (!stop_bracket(limit, s.cross(u), k, f)) return;
    const int n = static_cast<int>(limit.cone_stops.size());
    limit.cone_stops[k] = std::max(limit.cone_stops[k], want);
    limit.cone_stops[(k + 1) % n] = std::max(limit.cone_stops[(k + 1) % n], want);
}

void decompose_swing_twist(const Quat& q, const Vec3& u, Quat& swing, Quat& twist) {
    const Vec3 n = u.normalized();
    Quat q0 = q.w < 0 ? -q : q;
    const Vec3 v{q0.x, q0.y, q0.z};
    const Vec3 p = n * v.dot(n);
    twist = Quat{q0.w, p.x, p.y, p.z};
    const double len2 = twist.dot(twist);
    if (len2 < 1e-12) {
        twist = Quat{1, 0, 0, 0};
    } else {
        twist = twist.normalized();
    }
    swing = (q0 * twist.conj()).normalized();
    if (swing.w < 0) swing = -swing;
}

Quat clamp_frame_rotation(const JointLimit& limit, const Quat& frame_rot) {
    if (!limit.is_limited()) return frame_rot;

    if (limit.kind == JointLimitKind::Hinge) {
        const Vec3 axis = limit.axis.length() > 1e-6 ? limit.axis.normalized() : Vec3{0, 1, 0};
        Quat swing, twist;
        decompose_swing_twist(frame_rot, axis, swing, twist);

        const Vec3 tv{twist.x, twist.y, twist.z};
        double angle = 2.0 * std::atan2(tv.dot(axis), twist.w);
        const double ref = 0.5 * (limit.min_angle + limit.max_angle);
        angle = wrap_near_pi(angle, ref);

        const double clamped_angle = std::clamp(angle, limit.min_angle, limit.max_angle);
        return (cap_swing(swing, kHingeOffAxis) * Quat::axis_angle(axis, clamped_angle)).normalized();
    }

    if (limit.kind == JointLimitKind::Cone) {
        const Vec3 bone_axis = limit.bone_axis.length() > 1e-6 ? limit.bone_axis.normalized() : Vec3{0, 1, 0};
        Quat swing, twist;
        decompose_swing_twist(frame_rot, bone_axis, swing, twist);

        // Twist clamping about bone_axis without wrapping
        const Vec3 tv{twist.x, twist.y, twist.z};
        double twist_angle = 2.0 * std::atan2(tv.dot(bone_axis), twist.w);
        const double ref_twist = 0.5 * (limit.twist_min + limit.twist_max);
        twist_angle = wrap_near_pi(twist_angle, ref_twist);
        const double clamped_twist = std::clamp(twist_angle, limit.twist_min, limit.twist_max);
        const Quat clamped_twist_q = Quat::axis_angle(bone_axis, clamped_twist);

        // Swing back to the cone's stop in the way it swings
        return (cap_swing(swing, swing_stop(limit, swing)) * clamped_twist_q).normalized();
    }

    return frame_rot;
}

Quat clamp_joint_rotation(const JointLimit& limit, const Quat& local_rot, const Shape* shape, int node) {
    if (!limit.is_limited()) return local_rot;
    const Quat frame_rot = to_joint_frame(shape, node, local_rot);
    const Quat clamped_frame = clamp_frame_rotation(limit, frame_rot);
    return from_joint_frame(shape, node, clamped_frame);
}

bool is_frame_rotation_within_limits(const JointLimit& limit, const Quat& frame_rot, double tol) {
    if (!limit.is_limited()) return true;

    if (limit.kind == JointLimitKind::Hinge) {
        const Vec3 axis = limit.axis.length() > 1e-6 ? limit.axis.normalized() : Vec3{0, 1, 0};
        Quat swing, twist;
        decompose_swing_twist(frame_rot, axis, swing, twist);
        // Off-axis rotation must be small
        if (swing.angle() > kHingeOffAxis + tol) return false;

        const Vec3 tv{twist.x, twist.y, twist.z};
        double angle = 2.0 * std::atan2(tv.dot(axis), twist.w);
        const double ref = 0.5 * (limit.min_angle + limit.max_angle);
        angle = wrap_near_pi(angle, ref);

        return angle >= limit.min_angle - tol && angle <= limit.max_angle + tol;
    }

    if (limit.kind == JointLimitKind::Cone) {
        const Vec3 bone_axis = limit.bone_axis.length() > 1e-6 ? limit.bone_axis.normalized() : Vec3{0, 1, 0};
        Quat swing, twist;
        decompose_swing_twist(frame_rot, bone_axis, swing, twist);

        const Vec3 s_vec{swing.x, swing.y, swing.z};
        const double alpha = 2.0 * std::atan2(s_vec.length(), swing.w);
        if (alpha > swing_stop(limit, swing) + tol) return false;

        const Vec3 tv{twist.x, twist.y, twist.z};
        double twist_angle = 2.0 * std::atan2(tv.dot(bone_axis), twist.w);
        const double ref_twist = 0.5 * (limit.twist_min + limit.twist_max);
        twist_angle = wrap_near_pi(twist_angle, ref_twist);

        return twist_angle >= limit.twist_min - tol && twist_angle <= limit.twist_max + tol;
    }

    return true;
}

bool is_rotation_within_limits(const JointLimit& limit, const Quat& local_rot, const Shape* shape, int node,
                               double tol) {
    if (!limit.is_limited()) return true;
    const Quat frame_rot = to_joint_frame(shape, node, local_rot);
    return is_frame_rotation_within_limits(limit, frame_rot, tol);
}

Json limit_to_json(const JointLimit& lim) {
    Json j = Json::object();
    if (lim.kind == JointLimitKind::Hinge) {
        j.set("kind", "hinge");
        Json ax = Json::array();
        ax.push(lim.axis.x);
        ax.push(lim.axis.y);
        ax.push(lim.axis.z);
        j.set("axis", std::move(ax));
        j.set("min", lim.min_angle);
        j.set("max", lim.max_angle);
    } else if (lim.kind == JointLimitKind::Cone) {
        j.set("kind", "cone");
        Json ax = Json::array();
        ax.push(lim.bone_axis.x);
        ax.push(lim.bone_axis.y);
        ax.push(lim.bone_axis.z);
        j.set("bone_axis", std::move(ax));
        j.set("cone", lim.cone_angle);
        j.set("twist_min", lim.twist_min);
        j.set("twist_max", lim.twist_max);
        if (!lim.cone_stops.empty()) {
            Json st = Json::array();
            for (double a : lim.cone_stops) st.push(a);
            j.set("stops", std::move(st));
            Json ref = Json::array();
            ref.push(lim.cone_ref.x);
            ref.push(lim.cone_ref.y);
            ref.push(lim.cone_ref.z);
            j.set("stops_from", std::move(ref));
        }
    } else {
        j.set("kind", "none");
    }
    if (lim.source != JointLimitSource::None) {
        const char* s = "manual";
        if (lim.source == JointLimitSource::Template) s = "template";
        else if (lim.source == JointLimitSource::BindPose) s = "bind_pose";
        else if (lim.source == JointLimitSource::Collision) s = "collision";
        else if (lim.source == JointLimitSource::Animation) s = "animation";
        j.set("source", s);
    }
    j.set("confidence", lim.confidence);
    return j;
}

bool limit_from_json(const Json& j, JointLimit& out) {
    if (!j.is_object()) return false;
    const Json* kind = j.find("kind");
    if (!kind || !kind->is_string()) return false;
    out = JointLimit{};
    if (kind->str == "hinge") {
        out.kind = JointLimitKind::Hinge;
        if (const Json* ax = j.find("axis"); ax && ax->is_array() && ax->arr.size() == 3) {
            out.axis = {ax->arr[0].num, ax->arr[1].num, ax->arr[2].num};
            if (out.axis.length() > 1e-6) out.axis = out.axis.normalized();
            else out.axis = {0, 1, 0};
        }
        if (const Json* mn = j.find("min"); mn && mn->is_number()) out.min_angle = mn->num;
        if (const Json* mx = j.find("max"); mx && mx->is_number()) out.max_angle = mx->num;
    } else if (kind->str == "cone") {
        out.kind = JointLimitKind::Cone;
        if (const Json* ax = j.find("bone_axis"); ax && ax->is_array() && ax->arr.size() == 3) {
            out.bone_axis = {ax->arr[0].num, ax->arr[1].num, ax->arr[2].num};
            if (out.bone_axis.length() > 1e-6) out.bone_axis = out.bone_axis.normalized();
            else out.bone_axis = {0, 1, 0};
        }
        if (const Json* cn = j.find("cone"); cn && cn->is_number()) out.cone_angle = cn->num;
        if (const Json* tmn = j.find("twist_min"); tmn && tmn->is_number()) out.twist_min = tmn->num;
        if (const Json* tmx = j.find("twist_max"); tmx && tmx->is_number()) out.twist_max = tmx->num;
        if (const Json* st = j.find("stops"); st && st->is_array())
            for (const Json& a : st->arr)
                if (a.is_number()) out.cone_stops.push_back(std::max(0.0, a.num));
        if (const Json* ref = j.find("stops_from"); ref && ref->is_array() && ref->arr.size() == 3)
            out.cone_ref = {ref->arr[0].num, ref->arr[1].num, ref->arr[2].num};
    } else {
        out.kind = JointLimitKind::None;
    }
    // A hand-edited or damaged file: keep min <= max and the cone non-negative, as every setter does (std::clamp needs it).
    if (out.min_angle > out.max_angle) std::swap(out.min_angle, out.max_angle);
    if (out.twist_min > out.twist_max) std::swap(out.twist_min, out.twist_max);
    if (!(out.cone_angle >= 0)) out.cone_angle = 0;
    if (const Json* src = j.find("source"); src && src->is_string()) {
        if (src->str == "template") out.source = JointLimitSource::Template;
        else if (src->str == "bind_pose") out.source = JointLimitSource::BindPose;
        else if (src->str == "collision") out.source = JointLimitSource::Collision;
        else if (src->str == "animation") out.source = JointLimitSource::Animation;
        else out.source = JointLimitSource::Manual;
    }
    if (const Json* conf = j.find("confidence"); conf && conf->is_number()) out.confidence = conf->num;
    return true;
}

Json constraints_to_json(const RigConstraints& constraints) {
    Json j = Json::object();
    for (const auto& [name, lim] : constraints.limits) {
        if (lim.is_limited()) {
            j.set(name, limit_to_json(lim));
        }
    }
    return j;
}

bool constraints_from_json(const Json& j, RigConstraints& out) {
    if (!j.is_object()) return false;
    out.limits.clear();
    for (const auto& [name, cj] : j.obj) {
        JointLimit lim;
        if (limit_from_json(cj, lim) && lim.is_limited()) {
            out.limits[name] = std::move(lim);
        }
    }
    return true;
}

void set_joint_limit(RigConstraints& c, const Skeleton& skel, const Shape* shape, int node, const JointLimit& lim,
                     bool mirror) {
    if (node < 0 || node >= skel.size()) return;
    const int other = mirror && skel.mirror(node) != node ? skel.mirror(node) : -1;  // a midline joint is its own
    for (int n : {node, other}) {
        if (n < 0) continue;
        const JointLimit l = n == node ? lim : mirror_joint_limit(skel, shape, node, n, lim);
        if (l.is_limited()) c.limits[skel[n].name] = l;
        else c.remove(skel[n].name);
    }
}

JointLimit mirror_joint_limit(const Skeleton& skel, const Shape* shape, int src_node, int dst_node,
                              const JointLimit& src_lim) {
    if (!src_lim.is_limited() || src_node < 0 || src_node >= skel.size() || dst_node < 0 || dst_node >= skel.size()) {
        return src_lim;
    }
    JointLimit dst_lim = src_lim;
    dst_lim.source = src_lim.source;
    dst_lim.confidence = src_lim.confidence;

    auto mirror_direction = [&](const Vec3& dir) -> Vec3 {
        if (dir.length() < 1e-6) return dir;
        Vec3 v_local = from_joint_frame(shape, src_node, dir.normalized());
        Vec3 v_avatar = skel[src_node].rest.rotate(v_local);
        Vec3 v_mirr_avatar{v_avatar.x, -v_avatar.y, v_avatar.z};
        Vec3 v_dst_local = skel[dst_node].rest.conj().rotate(v_mirr_avatar);
        Vec3 v_dst = to_joint_frame(shape, dst_node, v_dst_local);
        return v_dst.normalized();
    };

    if (src_lim.kind == JointLimitKind::Hinge) {
        Vec3 src_axis = src_lim.axis.length() > 1e-6 ? src_lim.axis.normalized() : Vec3{0, 1, 0};
        Quat q_ax = from_joint_frame(shape, src_node, Quat::axis_angle(src_axis, 0.1));
        Quat q_ax_mirr = mirror_rotation(skel, src_node, dst_node, q_ax);
        Quat q_ax_dst = to_joint_frame(shape, dst_node, q_ax_mirr);
        Vec3 ax_dst{q_ax_dst.x, q_ax_dst.y, q_ax_dst.z};
        if (ax_dst.length() > 1e-6) {
            dst_lim.axis = ax_dst.normalized();
        } else {
            dst_lim.axis = src_axis;
        }

        if (src_lim.bone_axis.length() > 1e-6) {
            dst_lim.bone_axis = mirror_direction(src_lim.bone_axis);
        }

        auto angle_to_dst = [&](double theta) -> double {
            Quat q = from_joint_frame(shape, src_node, Quat::axis_angle(src_axis, theta));
            Quat m = mirror_rotation(skel, src_node, dst_node, q);
            Quat f = to_joint_frame(shape, dst_node, m);
            Quat sw, tw;
            decompose_swing_twist(f, dst_lim.axis, sw, tw);
            Vec3 tv{tw.x, tw.y, tw.z};
            double a = 2.0 * std::atan2(tv.dot(dst_lim.axis), tw.w);
            while (a > kPi) a -= 2.0 * kPi;
            while (a < -kPi) a += 2.0 * kPi;
            return a;
        };

        double a1 = angle_to_dst(src_lim.min_angle);
        double a2 = angle_to_dst(src_lim.max_angle);
        dst_lim.min_angle = std::min(a1, a2);
        dst_lim.max_angle = std::max(a1, a2);
    } else if (src_lim.kind == JointLimitKind::Cone) {
        if (src_lim.bone_axis.length() > 1e-6) {
            dst_lim.bone_axis = mirror_direction(src_lim.bone_axis);
        } else {
            dst_lim.bone_axis = mirror_direction(Vec3{0, 1, 0});
        }
        if (src_lim.cone_ref.length() > 1e-6) dst_lim.cone_ref = mirror_direction(src_lim.cone_ref);
        dst_lim.cone_angle = src_lim.cone_angle;
        // Each stop of the other side's cone is this side's stop in the mirrored direction.
        const int n = static_cast<int>(src_lim.cone_stops.size());
        for (int k = 0; k < n; ++k) {
            const Vec3 d = cone_stop_dir(dst_lim, k, n);
            const Vec3 d_local = from_joint_frame(shape, dst_node, d);
            const Vec3 d_avatar = skel[dst_node].rest.rotate(d_local);
            const Vec3 back = skel[src_node].rest.conj().rotate(Vec3{d_avatar.x, -d_avatar.y, d_avatar.z});
            dst_lim.cone_stops[k] = cone_stop(src_lim, to_joint_frame(shape, src_node, back));
        }
        dst_lim.twist_min = -src_lim.twist_max;
        dst_lim.twist_max = -src_lim.twist_min;
    }

    return dst_lim;
}

JointLimit set_hinge_min(const JointLimit& lim, double angle, double snap_step) {
    JointLimit res = lim;
    if (snap_step > 1e-6) angle = std::round(angle / snap_step) * snap_step;
    angle = std::clamp(angle, -kPi, res.max_angle);
    res.min_angle = angle;
    res.source = JointLimitSource::Manual;
    return res;
}

JointLimit set_hinge_max(const JointLimit& lim, double angle, double snap_step) {
    JointLimit res = lim;
    if (snap_step > 1e-6) angle = std::round(angle / snap_step) * snap_step;
    angle = std::clamp(angle, res.min_angle, kPi);
    res.max_angle = angle;
    res.source = JointLimitSource::Manual;
    return res;
}

JointLimit set_hinge_axis(const JointLimit& lim, const Vec3& axis) {
    JointLimit res = lim;
    if (axis.length() > 1e-6) {
        res.axis = axis.normalized();
        res.source = JointLimitSource::Manual;
    }
    return res;
}

JointLimit set_cone_angle(const JointLimit& lim, double angle, double snap_step) {
    JointLimit res = lim;
    if (snap_step > 1e-6) angle = std::round(angle / snap_step) * snap_step;
    res.cone_angle = std::clamp(angle, 0.02, kPi * 0.95);
    // The per-direction stops widen and narrow with it, keeping their shape.
    if (lim.cone_angle > 1e-6)
        for (double& a : res.cone_stops) a *= res.cone_angle / lim.cone_angle;
    res.source = JointLimitSource::Manual;
    return res;
}

JointLimit set_cone_edge(const JointLimit& lim, const Vec3& new_bone_axis) {
    JointLimit res = lim;
    if (new_bone_axis.length() > 1e-6) {
        res.bone_axis = new_bone_axis.normalized();
        res.source = JointLimitSource::Manual;
    }
    return res;
}

JointLimit set_twist_min(const JointLimit& lim, double angle, double snap_step) {
    JointLimit res = lim;
    if (snap_step > 1e-6) angle = std::round(angle / snap_step) * snap_step;
    angle = std::clamp(angle, -kPi, res.twist_max);
    res.twist_min = angle;
    res.source = JointLimitSource::Manual;
    return res;
}

JointLimit set_twist_max(const JointLimit& lim, double angle, double snap_step) {
    JointLimit res = lim;
    if (snap_step > 1e-6) angle = std::round(angle / snap_step) * snap_step;
    angle = std::clamp(angle, res.twist_min, kPi);
    res.twist_max = angle;
    res.source = JointLimitSource::Manual;
    return res;
}

bool check_joint_clamp(const std::string& name, int node, const JointLimit& limit,
                       const Quat& unclamped_local_rot, const Shape* shape, ClampedJoint& out, double tol) {
    if (!limit.is_limited()) return false;
    const Quat frame_rot = to_joint_frame(shape, node, unclamped_local_rot);

    if (limit.kind == JointLimitKind::Hinge) {
        const Vec3 axis = limit.axis.length() > 1e-6 ? limit.axis.normalized() : Vec3{0, 1, 0};
        Quat swing, twist;
        decompose_swing_twist(frame_rot, axis, swing, twist);
        const Vec3 tv{twist.x, twist.y, twist.z};
        double angle = 2.0 * std::atan2(tv.dot(axis), twist.w);
        const double ref = 0.5 * (limit.min_angle + limit.max_angle);
        angle = wrap_near_pi(angle, ref);

        if (angle < limit.min_angle - tol) {
            out.joint = name;
            out.node = node;
            out.reason = ClampReason::HingeMin;
            out.angle_deg = limit.min_angle / kDegToRad;
            out.message = name + " stopped at " + std::to_string(int(std::round(out.angle_deg))) + " degrees (hinge min)";
            return true;
        }
        if (angle > limit.max_angle + tol) {
            out.joint = name;
            out.node = node;
            out.reason = ClampReason::HingeMax;
            out.angle_deg = limit.max_angle / kDegToRad;
            out.message = name + " stopped at " + std::to_string(int(std::round(out.angle_deg))) + " degrees (hinge max)";
            return true;
        }
    } else if (limit.kind == JointLimitKind::Cone) {
        const Vec3 bone_axis = limit.bone_axis.length() > 1e-6 ? limit.bone_axis.normalized() : Vec3{0, 1, 0};
        Quat swing, twist;
        decompose_swing_twist(frame_rot, bone_axis, swing, twist);

        const Vec3 s_vec{swing.x, swing.y, swing.z};
        const double alpha = 2.0 * std::atan2(s_vec.length(), swing.w);
        const double stop = swing_stop(limit, swing);
        if (alpha > stop + tol) {
            out.joint = name;
            out.node = node;
            out.reason = ClampReason::ConeSwing;
            out.angle_deg = stop / kDegToRad;
            out.message = name + " stopped at " + std::to_string(int(std::round(out.angle_deg))) + " degrees (cone limit)";
            return true;
        }

        const Vec3 tv{twist.x, twist.y, twist.z};
        double twist_angle = 2.0 * std::atan2(tv.dot(bone_axis), twist.w);
        const double ref_twist = 0.5 * (limit.twist_min + limit.twist_max);
        twist_angle = wrap_near_pi(twist_angle, ref_twist);

        if (twist_angle < limit.twist_min - tol) {
            out.joint = name;
            out.node = node;
            out.reason = ClampReason::TwistMin;
            out.angle_deg = limit.twist_min / kDegToRad;
            out.message = name + " stopped at " + std::to_string(int(std::round(out.angle_deg))) + " degrees (twist min)";
            return true;
        }
        if (twist_angle > limit.twist_max + tol) {
            out.joint = name;
            out.node = node;
            out.reason = ClampReason::TwistMax;
            out.angle_deg = limit.twist_max / kDegToRad;
            out.message = name + " stopped at " + std::to_string(int(std::round(out.angle_deg))) + " degrees (twist max)";
            return true;
        }
    }
    return false;
}

ClampReport query_clamped_joints(const Skeleton& skel, const Shape* shape,
                                 const RigConstraints& constraints, const Pose& pose, double tol) {
    ClampReport rep;
    for (const auto& [name, lim] : constraints.limits) {
        if (!lim.is_limited()) continue;
        int n = skel.find(name);
        if (n < 0 || n >= int(pose.rot.size())) continue;
        const Quat frame_rot = to_joint_frame(shape, n, pose.rot[n]);

        if (lim.kind == JointLimitKind::Hinge) {
            const Vec3 axis = lim.axis.length() > 1e-6 ? lim.axis.normalized() : Vec3{0, 1, 0};
            Quat swing, twist;
            decompose_swing_twist(frame_rot, axis, swing, twist);
            const Vec3 tv{twist.x, twist.y, twist.z};
            double angle = 2.0 * std::atan2(tv.dot(axis), twist.w);
            const double ref = 0.5 * (lim.min_angle + lim.max_angle);
            angle = wrap_near_pi(angle, ref);

            if (std::fabs(angle - lim.min_angle) <= tol) {
                ClampedJoint c;
                c.joint = name;
                c.node = n;
                c.reason = ClampReason::HingeMin;
                c.angle_deg = lim.min_angle / kDegToRad;
                c.message = name + " stopped at " + std::to_string(int(std::round(c.angle_deg))) + " degrees (hinge min)";
                rep.clamped.push_back(std::move(c));
            } else if (std::fabs(angle - lim.max_angle) <= tol) {
                ClampedJoint c;
                c.joint = name;
                c.node = n;
                c.reason = ClampReason::HingeMax;
                c.angle_deg = lim.max_angle / kDegToRad;
                c.message = name + " stopped at " + std::to_string(int(std::round(c.angle_deg))) + " degrees (hinge max)";
                rep.clamped.push_back(std::move(c));
            }
        } else if (lim.kind == JointLimitKind::Cone) {
            const Vec3 bone_axis = lim.bone_axis.length() > 1e-6 ? lim.bone_axis.normalized() : Vec3{0, 1, 0};
            Quat swing, twist;
            decompose_swing_twist(frame_rot, bone_axis, swing, twist);

            const Vec3 s_vec{swing.x, swing.y, swing.z};
            const double alpha = 2.0 * std::atan2(s_vec.length(), swing.w);
            const double stop = swing_stop(lim, swing);
            if (std::fabs(alpha - stop) <= tol) {
                ClampedJoint c;
                c.joint = name;
                c.node = n;
                c.reason = ClampReason::ConeSwing;
                c.angle_deg = stop / kDegToRad;
                c.message = name + " stopped at " + std::to_string(int(std::round(c.angle_deg))) + " degrees (cone limit)";
                rep.clamped.push_back(std::move(c));
                continue;
            }

            const Vec3 tv{twist.x, twist.y, twist.z};
            double twist_angle = 2.0 * std::atan2(tv.dot(bone_axis), twist.w);
            const double ref_twist = 0.5 * (lim.twist_min + lim.twist_max);
            twist_angle = wrap_near_pi(twist_angle, ref_twist);

            if (std::fabs(twist_angle - lim.twist_min) <= tol) {
                ClampedJoint c;
                c.joint = name;
                c.node = n;
                c.reason = ClampReason::TwistMin;
                c.angle_deg = lim.twist_min / kDegToRad;
                c.message = name + " stopped at " + std::to_string(int(std::round(c.angle_deg))) + " degrees (twist min)";
                rep.clamped.push_back(std::move(c));
            } else if (std::fabs(twist_angle - lim.twist_max) <= tol) {
                ClampedJoint c;
                c.joint = name;
                c.node = n;
                c.reason = ClampReason::TwistMax;
                c.angle_deg = lim.twist_max / kDegToRad;
                c.message = name + " stopped at " + std::to_string(int(std::round(c.angle_deg))) + " degrees (twist max)";
                rep.clamped.push_back(std::move(c));
            }
        }
    }
    return rep;
}

ClampReport query_clamped_joints(const Skeleton& skel, const Shape* shape,
                                 const RigConstraints& constraints,
                                 const Pose& unclamped_pose, const Pose& clamped_pose, double tol) {
    ClampReport rep;
    for (const auto& [name, lim] : constraints.limits) {
        if (!lim.is_limited()) continue;
        int n = skel.find(name);
        if (n < 0 || n >= int(unclamped_pose.rot.size()) || n >= int(clamped_pose.rot.size())) continue;
        ClampedJoint c;
        if (check_joint_clamp(name, n, lim, unclamped_pose.rot[n], shape, c, tol)) {
            rep.clamped.push_back(std::move(c));
        }
    }
    return rep;
}

}  // namespace vats
