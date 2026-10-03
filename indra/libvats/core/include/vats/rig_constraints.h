// Viewport Avatar Toolset - rig constraints and joint limits.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 29 (JL). Joint limits that stop bones from bending past natural body ranges
// during posing (Auto IK, IK, body drag, rotate gizmo). Limits are VATs' own data, stored per body in the
// .vat project and never written into the exported .anim.
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vats/json.h"
#include "vats/math.h"
#include "vats/skeleton.h"

namespace vats {

enum class JointLimitKind {
    None,
    Hinge,
    Cone
};

enum class JointLimitSource {
    None,
    Template,
    BindPose,
    Collision,
    Animation,
    Manual
};

struct JointLimit {
    JointLimitKind kind = JointLimitKind::None;

    // For Hinge:
    Vec3 axis{0, 1, 0};        // hinge axis in the joint's own frame
    double min_angle = 0.0;    // radians
    double max_angle = 0.0;    // radians

    // For Cone + Twist:
    Vec3 bone_axis{0, 1, 0};   // bone direction in the joint's own frame
    double cone_angle = kPi;   // swing cone half-angle (radians), 0..pi
    double twist_min = -kPi;   // twist minimum (radians) about bone_axis
    double twist_max = kPi;    // twist maximum (radians) about bone_axis
    // Per-direction swing stops (radians, each <= cone_angle), evenly spaced around bone_axis from cone_ref: a thigh
    // may swing far forward but less inward, where the belly or the other leg is. Empty: cone_angle all round.
    std::vector<double> cone_stops;
    Vec3 cone_ref{0, 0, 0};  // where the first stop points (joint frame, across bone_axis); zero: a fixed choice

    // Provenance (Stage 2):
    JointLimitSource source = JointLimitSource::None;
    double confidence = 1.0;   // 0.0 .. 1.0

    bool is_limited() const { return kind != JointLimitKind::None; }
    bool operator==(const JointLimit&) const = default;
};

// A hinge keeps this much rotation off its axis: keyed or mocap knees carry a little, and wiping it on every touch made
// the leg jump.
constexpr double kHingeOffAxis = 5 * kDegToRad;

// The direction of cone stop k of n, in the joint's frame: across bone_axis, where the bone's tip swings toward.
Vec3 cone_stop_dir(const JointLimit& limit, int k, int n);
// The swing stop of a cone for the bone's tip swinging toward tip_dir (joint frame, across bone_axis).
double cone_stop(const JointLimit& limit, const Vec3& tip_dir);
// Widens a cone's swing limit (its stops near that direction too) to take a swing quaternion about an axis across
// bone_axis, plus margin.
void widen_cone_swing(JointLimit& limit, const Quat& swing, double margin = 0);

struct RigConstraints {
    std::unordered_map<std::string, JointLimit> limits;

    bool empty() const {
        for (const auto& [name, lim] : limits)
            if (lim.is_limited()) return false;
        return true;
    }
    const JointLimit* find(const std::string& joint) const {
        auto it = limits.find(joint);
        return (it != limits.end() && it->second.is_limited()) ? &it->second : nullptr;
    }
    JointLimit* find(const std::string& joint) {
        auto it = limits.find(joint);
        return (it != limits.end() && it->second.is_limited()) ? &it->second : nullptr;
    }
    JointLimit& get_or_create(const std::string& joint) {
        return limits[joint];
    }
    void remove(const std::string& joint) {
        limits.erase(joint);
    }
    bool operator==(const RigConstraints&) const = default;
};

// Continuous wrap of angle v to the interval (ref - pi, ref + pi].
inline double wrap_near_pi(double v, double ref) {
    double d = std::fmod(v - ref, 2.0 * kPi);
    if (d > kPi) d -= 2.0 * kPi;
    if (d <= -kPi) d += 2.0 * kPi;
    return ref + d;
}

// Converts local rotation (relative to rest) into the joint's own frame (rig axes frame if present, else SL's).
inline Quat to_joint_frame(const Shape* shape, int node, const Quat& local_rot) {
    return has_rig_axes(shape, node) ? to_rig_axes(shape->axes[node], local_rot) : local_rot;
}

// Converts a rotation in the joint's own frame back to local rotation (relative to rest).
inline Quat from_joint_frame(const Shape* shape, int node, const Quat& frame_rot) {
    return has_rig_axes(shape, node) ? from_rig_axes(shape->axes[node], frame_rot) : frame_rot;
}

inline Vec3 to_joint_frame(const Shape* shape, int node, const Vec3& local_v) {
    return has_rig_axes(shape, node) ? shape->axes[node].conj().rotate(local_v) : local_v;
}

inline Vec3 from_joint_frame(const Shape* shape, int node, const Vec3& frame_v) {
    return has_rig_axes(shape, node) ? shape->axes[node].rotate(frame_v) : frame_v;
}

// Decomposes q into swing and twist about unit vector u: q = swing * twist, where twist is about u.
void decompose_swing_twist(const Quat& q, const Vec3& u, Quat& swing, Quat& twist);

// Clamps a rotation already in the joint's own frame.
Quat clamp_frame_rotation(const JointLimit& limit, const Quat& frame_rot);

// Clamps a local rotation (relative to rest). If limit is not active, returns local_rot unchanged.
Quat clamp_joint_rotation(const JointLimit& limit, const Quat& local_rot, const Shape* shape, int node);

// Checks if a rotation in the joint's own frame is within limits (within tolerance radians).
bool is_frame_rotation_within_limits(const JointLimit& limit, const Quat& frame_rot, double tol = 0.02);

// Checks if a local rotation is within limits (within tolerance radians).
bool is_rotation_within_limits(const JointLimit& limit, const Quat& local_rot, const Shape* shape, int node,
                               double tol = 0.02);

// JSON serialization for JointLimit and RigConstraints
Json limit_to_json(const JointLimit& limit);
bool limit_from_json(const Json& j, JointLimit& out);

Json constraints_to_json(const RigConstraints& constraints);
bool constraints_from_json(const Json& j, RigConstraints& out);

// Snaps an angle to the given increment in radians (if step > 1e-6).
inline double snap_angle(double rad, double step) {
    if (step <= 1e-6) return rad;
    return std::round(rad / step) * step;
}

// Mirrors a JointLimit across the sagittal plane from src_node to dst_node.
JointLimit mirror_joint_limit(const Skeleton& skel, const Shape* shape, int src_node, int dst_node,
                              const JointLimit& src_lim);

// Writes node's limit into c (an unlimited one removes it), and with mirror on gives the other side the mirror image.
void set_joint_limit(RigConstraints& c, const Skeleton& skel, const Shape* shape, int node, const JointLimit& lim,
                     bool mirror);

// Direct manipulation helpers with snapping and bounds clamping:
JointLimit set_hinge_min(const JointLimit& lim, double angle, double snap_step = 0.0);
JointLimit set_hinge_max(const JointLimit& lim, double angle, double snap_step = 0.0);
JointLimit set_hinge_axis(const JointLimit& lim, const Vec3& axis);
JointLimit set_cone_angle(const JointLimit& lim, double angle, double snap_step = 0.0);
JointLimit set_cone_edge(const JointLimit& lim, const Vec3& new_bone_axis);
JointLimit set_twist_min(const JointLimit& lim, double angle, double snap_step = 0.0);
JointLimit set_twist_max(const JointLimit& lim, double angle, double snap_step = 0.0);

enum class ClampReason {
    None,
    HingeMin,
    HingeMax,
    ConeSwing,
    TwistMin,
    TwistMax
};

struct ClampedJoint {
    std::string joint;
    int node = -1;
    ClampReason reason = ClampReason::None;
    double angle_deg = 0.0;
    std::string message;
};

struct ClampReport {
    std::vector<ClampedJoint> clamped;

    bool empty() const { return clamped.empty(); }
    bool contains(std::string_view name) const {
        for (const auto& c : clamped) if (c.joint == name) return true;
        return false;
    }
    const ClampedJoint* find(std::string_view name) const {
        for (const auto& c : clamped) if (c.joint == name) return &c;
        return nullptr;
    }
    const ClampedJoint* find(int node) const {
        for (const auto& c : clamped) if (c.node == node) return &c;
        return nullptr;
    }
};

bool check_joint_clamp(const std::string& name, int node, const JointLimit& limit,
                       const Quat& unclamped_local_rot, const Shape* shape, ClampedJoint& out, double tol = 0.005);

ClampReport query_clamped_joints(const Skeleton& skel, const Shape* shape,
                                 const RigConstraints& constraints, const Pose& pose, double tol = 0.01);

ClampReport query_clamped_joints(const Skeleton& skel, const Shape* shape,
                                 const RigConstraints& constraints,
                                 const Pose& unclamped_pose, const Pose& clamped_pose, double tol = 0.005);

}  // namespace vats
