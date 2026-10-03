// Viewport Avatar Toolset - suggest joint limits implementation.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/suggest_limits.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>

#include "vats/pose_ops.h"
#include "vats/ragdoll.h"
#include "vats/rig.h"
#include "vats/self_contact.h"

namespace vats {
namespace {

// Helpers for joint frame conversion
Vec3 to_frame(const Shape* shape, int node, const Vec3& sl_vec) {
    return to_joint_frame(shape, node, sl_vec);
}

Vec3 from_frame(const Shape* shape, int node, const Vec3& frame_vec) {
    return from_joint_frame(shape, node, frame_vec);
}

Vec3 perp_to_axis(const Vec3& v, const Vec3& axis) { return v - axis * v.dot(axis); }

// Subtree of a bone
void collect_subtree(const Skeleton& skel, int node, std::unordered_set<int>& out) {
    out.insert(node);
    if (node >= 0 && node < skel.size()) {
        for (int c : skel[node].children) collect_subtree(skel, c, out);
    }
}

// Forward kinematics update for a node and all its descendants
void update_subtree_globals(const Skeleton& skel, const Shape* shape, const Pose& pose,
                            int node, std::vector<Xform>& globals) {
    if (node < 0 || node >= skel.size()) return;
    const int p = skel[node].parent;
    const Xform parent_xform = p >= 0 ? globals[p] : Xform{};
    globals[node] = parent_xform * skel.local_xform(node, pose, shape);

    for (int child : skel[node].children) {
        update_subtree_globals(skel, shape, pose, child, globals);
    }
}

double bone_radius(const Skeleton& skel, int n) {
    const std::string& name = skel[n].name;
    if (name == "mPelvis") return 0.10;
    if (name == "mTorso") return 0.11;
    if (name == "mChest") return 0.12;
    if (name == "mNeck") return 0.04;
    if (name == "mHead") return 0.08;
    if (name.rfind("mCollar", 0) == 0) return 0.04;
    if (name.rfind("mShoulder", 0) == 0) return 0.05;
    if (name.rfind("mElbow", 0) == 0) return 0.04;
    if (name.rfind("mWrist", 0) == 0) return 0.035;
    if (name.rfind("mHip", 0) == 0) return 0.06;
    if (name.rfind("mKnee", 0) == 0) return 0.05;
    if (name.rfind("mAnkle", 0) == 0) return 0.04;
    if (name.rfind("mFoot", 0) == 0 || name.rfind("mToe", 0) == 0) return 0.035;
    if (name.rfind("mHand", 0) == 0) return 0.02;
    if (name.rfind("mTail", 0) == 0) return 0.035;
    if (name.rfind("mWing", 0) == 0) return 0.03;
    if (name.rfind("mHindLimb", 0) == 0) return 0.05;
    return 0.04;
}

RagdollCapsule make_capsule(const Skeleton& skel, const Shape* shape,
                            const std::vector<Xform>& globals, int node, double rad) {
    RagdollCapsule c;
    c.node = node;
    c.parent = skel[node].parent;
    c.radius = rad;
    Vec3 head = globals[node].pos;
    Vec3 tail = head;
    if (shape && node < static_cast<int>(shape->tails.size()) && shape->tails[node].length() > 1e-4) {
        tail = globals[node].apply(shape->tails[node]);
    } else {
        int joint_child = -1;
        for (int ch : skel[node].children) {
            if (ch < skel.joint_count() && skel[ch].category != Category::AttachmentPoints &&
                skel[ch].category != Category::CollisionVolumes) {
                joint_child = ch;
                break;
            }
        }
        if (joint_child >= 0 && joint_child < static_cast<int>(globals.size())) {
            tail = globals[joint_child].pos;
        } else if (skel[node].end.length() > 1e-4) {
            tail = globals[node].apply(skel[node].end);
        } else {
            tail = globals[node].apply(Vec3{0.05, 0, 0});
        }
    }
    // Offset hip top down by radius so it starts below the hip joint (matching contact_hull in self_contact.cpp)
    const std::string& n = skel[node].name;
    if (n.rfind("mHip", 0) == 0) {
        const double len = (tail - head).length();
        if (len > 2 * c.radius) {
            head += (tail - head) * (c.radius / len);
        }
    }
    c.segments.push_back({head, tail});
    return c;
}

constexpr int kConeStops = 8;  // collision stops per cone, every 45 degrees around the bone
constexpr int kLimbKinds = 5;  // the limb kinds limb_id numbers per side in apply_collision_sweep

}  // namespace

Vec3 rest_bone_dir(const Skeleton& skel, const Shape* shape, int node) {
    for (int c : skel[node].children)
        if (c < skel.joint_count() && skel[c].category != Category::AttachmentPoints &&
            skel[c].category != Category::CollisionVolumes) {
            const Vec3 p = skel.local_xform(c, Pose(skel.size()), shape).pos;
            if (p.length() > 1e-4) return p.normalized();
        }
    if (shape && node < static_cast<int>(shape->tails.size()) && shape->tails[node].length() > 1e-4)
        return shape->tails[node].normalized();
    return skel[node].end.length() > 1e-6 ? skel[node].end.normalized() : Vec3{0, 0, -1};
}

RigConstraints template_limits(const Skeleton& skel, const Shape* shape) {
    RigConstraints rc;

    // Axes come from the skeleton, in SL's frame (X forward, Y left, Z up): a cone around the bone's own direction,
    // and a hinge about (bone direction) x (the way the joint flexes), so a positive angle is the flex. Hard-coded
    // axes got the arms (along +-Y, not X) and the fingers (they curl toward the palm, -Z) wrong.
    auto dir_of = [&](int n, const Vec3&) { return rest_bone_dir(skel, shape, n); };
    auto add_hinge = [&](const std::string& name, const Vec3& flex_sl, double min_deg, double max_deg, double conf) {
        int n = skel.find(name);
        if (n < 0 || skel.reused(n)) return;  // a reused bone is not the part SL named it for
        Vec3 axis = dir_of(n, {0, 0, -1}).cross(flex_sl.normalized());
        if (axis.length() < 1e-6) return;  // a bone pointing the way it flexes has no hinge
        axis = axis.normalized();
        // SL's own hinges (the knee's Y, the elbow's Z, the fingers' X) are axis-aligned; a bone a few degrees off
        // vertical shouldn't tilt the limit off the hinge the solvers bend about, or the clamp fights them.
        for (int k = 0; k < 3; ++k)
            if (std::fabs(axis[k]) > std::cos(10 * kDegToRad)) {
                Vec3 e;
                e[k] = axis[k] > 0 ? 1.0 : -1.0;
                axis = e;
                break;
            }
        JointLimit lim;
        lim.kind = JointLimitKind::Hinge;
        lim.axis = to_frame(shape, n, axis.normalized());
        lim.min_angle = min_deg * kDegToRad;
        lim.max_angle = max_deg * kDegToRad;
        lim.source = JointLimitSource::Template;
        lim.confidence = conf;
        rc.limits[name] = lim;
    };
    auto add_cone = [&](const std::string& name, const Vec3& fallback_dir, double cone_deg, double twist_min_deg,
                        double twist_max_deg, double conf, bool reused = false) {
        int n = skel.find(name);
        if (n < 0 || skel.reused(n) != reused) return;
        JointLimit lim;
        lim.kind = JointLimitKind::Cone;
        lim.bone_axis = to_frame(shape, n, dir_of(n, fallback_dir));
        lim.cone_angle = cone_deg * kDegToRad;
        lim.twist_min = twist_min_deg * kDegToRad;
        lim.twist_max = twist_max_deg * kDegToRad;
        lim.source = JointLimitSource::Template;
        lim.confidence = conf;
        rc.limits[name] = lim;
    };

    const Vec3 forward{1, 0, 0}, back{-1, 0, 0}, down{0, 0, -1};

    // 1. Spine & Head
    add_cone("mTorso", {0, 0, 1}, 35, -30, 30, 0.85);
    add_cone("mChest", {0, 0, 1}, 30, -25, 25, 0.85);
    add_cone("mNeck", {0, 0, 1}, 50, -50, 50, 0.85);
    add_cone("mHead", {0, 0, 1}, 45, -60, 60, 0.85);

    // 2. Limbs (flex directions from the T-pose: elbows bring the forearm forward, knees the shin back, fingers
    // curl toward the palm, which faces down)
    for (const char* side : {"Left", "Right"}) {
        const double sy = std::string_view(side) == "Left" ? 1.0 : -1.0;
        const std::string s(side);
        const Vec3 out{0, sy, 0};

        add_cone("mCollar" + s, out, 25, -15, 15, 0.80);
        add_cone("mShoulder" + s, out, 100, -90, 90, 0.85);
        add_hinge("mElbow" + s, forward, 0.0, 150.0, 0.95);
        add_cone("mWrist" + s, out, 70, -80, 80, 0.85);

        add_cone("mHip" + s, down, 80, -45, 45, 0.85);
        add_hinge("mKnee" + s, back, 0.0, 150.0, 0.95);
        add_cone("mAnkle" + s, down, 45, -25, 25, 0.85);
        add_hinge("mFoot" + s, down, -30, 45, 0.85);
        add_hinge("mToe" + s, down, -10, 50, 0.85);

        // Thumbs curl across the palm, toward the little finger and down
        add_cone("mHandThumb1" + s, out, 50, -30, 30, 0.85);
        const Vec3 thumb_palm = Vec3{-0.7, 0, -1}.normalized();
        add_hinge("mHandThumb2" + s, thumb_palm, 0, 70, 0.85);
        add_hinge("mHandThumb3" + s, thumb_palm, 0, 80, 0.85);
        for (const char* f : {"Index", "Middle", "Ring", "Pinky"}) {
            const std::string fn(f);
            add_cone("mHand" + fn + "1" + s, out, 85, -15, 15, 0.85);
            add_hinge("mHand" + fn + "2" + s, down, 0, 100, 0.85);
            add_hinge("mHand" + fn + "3" + s, down, 0, 85, 0.85);
        }

        // Wings fold back
        add_cone("mWing1" + s, out, 45, -30, 30, 0.65);
        add_hinge("mWing2" + s, back, 0, 120, 0.70);
        add_hinge("mWing3" + s, back, 0, 90, 0.65);
        add_hinge("mWing4" + s, back, 0, 90, 0.65);

        // Hind limbs, digitigrade: the knee folds the shin back, the hock folds the foot forward
        add_cone("mHindLimb1" + s, down, 70, -30, 30, 0.70);
        add_hinge("mHindLimb2" + s, back, 0, 140, 0.75);
        add_hinge("mHindLimb3" + s, forward, 0, 140, 0.75);
        add_cone("mHindLimb4" + s, down, 45, -25, 25, 0.70);
    }

    add_cone("mTail1", back, 30, -20, 20, 0.70);
    for (int k = 2; k <= 6; ++k) add_cone("mTail" + std::to_string(k), back, 35, -20, 20, 0.70);

    // A reused bone (rig_map.h RM-8: a scarf on a wing) swings about its own bone as the shape places it, a cone wide
    // enough for cloth, at a guess's confidence; the collision sweep then fits it to the mesh.
    for (int n = 0; n < skel.joint_count(); ++n)
        if (skel.reused(n)) add_cone(skel[n].name, {0, 0, -1}, 60, -30, 30, 0.5, true);

    return rc;
}

void apply_bind_pose_limits(const Skeleton& skel, const Shape* shape, RigConstraints& constraints) {
    const std::vector<Xform> rest = skel.global_pose(Pose(skel.size()), shape);
    // Only the joints Auto IK bends as hinges (elbows, knees, the hind legs' knee and hock, wing and finger joints),
    // about the hinge it bends them by: a foot, tail root or finger base rigged at an angle is still a ball joint.
    const std::vector<Vec3> hinges = auto_ik_hinges(Rig(skel), shape);
    for (int j = 0; j < skel.size(); ++j) {
        const int up = skel[j].parent;
        if (up < 0 || hinges[j].length() < 1e-9 || skel.reused(j)) continue;  // a reused wing bends as its part does
        const Vec3 h = rest[up].rot.rotate(hinges[j]).normalized();
        const Vec3 a = perp_to_axis(rest[j].pos - rest[up].pos, h);
        const Vec3 b = perp_to_axis(rest[j].rot.rotate(rest_bone_dir(skel, shape, j)), h);
        if (a.length() < 1e-4 || b.length() < 1e-6) continue;
        const double bend = std::atan2(h.dot(a.cross(b)), a.dot(b));
        const double theta = std::fabs(bend);
        // Nearly straight is not rigged bent (spec: 20 degrees); past 150 the joint is folded, not a hinge to open.
        if (theta < 20.0 * kDegToRad || theta > 150.0 * kDegToRad) continue;

        // A positive angle folds further: from straight (-theta) to a 150 degree fold.
        const Vec3 hinge_parent = hinges[j] * (bend < 0 ? -1.0 : 1.0);
        JointLimit lim;
        lim.kind = JointLimitKind::Hinge;
        lim.axis = to_frame(shape, j, skel[j].rest.conj().rotate(hinge_parent)).normalized();
        lim.min_angle = -theta;
        lim.max_angle = 150.0 * kDegToRad - theta;
        lim.source = JointLimitSource::BindPose;
        lim.confidence = 0.95;
        constraints.limits[skel[j].name] = lim;
    }
}

void apply_collision_sweep(const Skeleton& skel, const Shape* shape,
                           const std::vector<const DaeModel*>& models,
                           RigConstraints& constraints, double margin_deg) {
    const double margin = margin_deg * kDegToRad;
    const std::vector<Xform> rest_globals = skel.global_pose(Pose(skel.size()), shape);

    // Compute radii per joint
    std::vector<double> radii(skel.size());
    for (int j = 0; j < skel.size(); ++j) radii[j] = bone_radius(skel, j);

    // Fit from models if weights exist
    if (!models.empty()) {
        std::vector<std::vector<double>> distances(skel.size());
        for (const DaeModel* mdl : models) {
            if (!mdl || !mdl->rigged) continue;
            for (size_t vi = 0; vi < mdl->positions.size() / 3; ++vi) {
                const Vec3 vpos{mdl->positions[vi * 3], mdl->positions[vi * 3 + 1], mdl->positions[vi * 3 + 2]};
                for (int slot = 0; slot < 4; ++slot) {
                    const int idx = mdl->joints[vi * 4 + slot];
                    const float w = mdl->weights[vi * 4 + slot];
                    if (idx >= 0 && idx < skel.size() && w > 0.2f) {
                        Vec3 head = rest_globals[idx].pos;
                        Vec3 tail = head;
                        if (shape && idx < static_cast<int>(shape->tails.size()) && shape->tails[idx].length() > 1e-4) {
                            tail = rest_globals[idx].apply(shape->tails[idx]);
                        } else if (!skel[idx].children.empty() && skel[idx].children.front() < skel.size()) {
                            tail = rest_globals[skel[idx].children.front()].pos;
                        }
                        Vec3 c1, c2;
                        segment_closest_points(head, tail, vpos, vpos, c1, c2);
                        distances[idx].push_back((vpos - c1).length());
                    }
                }
            }
        }
        for (int j = 0; j < skel.size(); ++j) {
            if (distances[j].size() >= 8) {
                std::sort(distances[j].begin(), distances[j].end());
                const double d90 = distances[j][distances[j].size() * 9 / 10];
                radii[j] = std::clamp(d90, 0.02, 0.25);
            }
        }
    }

    std::vector<bool> active_bones(skel.joint_count(), false);
    if (!models.empty()) {
        for (const DaeModel* mdl : models) {
            if (!mdl) continue;
            for (size_t j = 0; j < active_bones.size() && j < mdl->bound.size(); ++j) {
                if (mdl->bound[j]) active_bones[j] = true;
            }
        }
    }
    for (int j = 0; j < skel.joint_count(); ++j) {
        if (skel[j].category == Category::Body || skel[j].category == Category::Hands) {
            active_bones[j] = true;
        }
    }

    // Build static rest capsules for all bone nodes
    std::vector<RagdollCapsule> static_caps(skel.size());
    for (int j = 0; j < skel.joint_count(); ++j) {
        if (!active_bones[j]) continue;
        if (skel[j].category == Category::AttachmentPoints || skel[j].category == Category::CollisionVolumes ||
            skel[j].category == Category::Face) continue;
        static_caps[j] = make_capsule(skel, shape, rest_globals, j, radii[j]);
    }

    std::vector<int> limb_id(skel.size(), -1);
    int next_limb = 0;
    // limb_id = side * kLimbKinds + kind, then the tail.
    for (const auto& side : {"Left", "Right"}) {
        for (PartKind k : {PartKind::Arm, PartKind::Leg, PartKind::HindLeg, PartKind::Wing, PartKind::Hand}) {
            BodyPart p = body_part(skel, k, side);
            for (int b : p.bones) {
                if (b >= 0 && b < skel.size()) limb_id[b] = next_limb;
            }
            ++next_limb;
        }
    }
    {
        BodyPart p = body_part(skel, PartKind::Tail, "");
        for (int b : p.bones) {
            if (b >= 0 && b < skel.size()) limb_id[b] = next_limb;
        }
        ++next_limb;
    }

    // A limb meeting its other side's limb (thigh against thigh, or its collision volume) is no stop: in a pose the
    // other one moves aside, and at rest they nearly touch, which stopped the hips swinging inward at all.
    auto other_side = [&](int a, int b) {
        return a >= 0 && b >= 0 && limb_id[a] >= 0 && limb_id[b] >= 0 && limb_id[a] != limb_id[b] &&
               limb_id[a] < 2 * kLimbKinds && limb_id[b] < 2 * kLimbKinds &&
               limb_id[a] % kLimbKinds == limb_id[b] % kLimbKinds;
    };

    // Pre-select pairs that are separated at rest pose
    std::vector<std::pair<int, int>> bone_pairs;
    for (int i = 0; i < skel.joint_count(); ++i) {
        if (static_caps[i].segments.empty()) continue;
        for (int j = i + 1; j < skel.joint_count(); ++j) {
            if (static_caps[j].segments.empty()) continue;
            int pi = skel[i].parent;
            int pj = skel[j].parent;
            if (i == j || pi == j || pj == i) continue;
            if (limb_id[i] >= 0 && limb_id[i] == limb_id[j]) continue;  // same limb
            if (other_side(i, j)) continue;
            Hit h = capsule_hit(static_caps[i], static_caps[j]);
            if (h.depth <= -0.015) {  // separated by >= 1.5 cm at rest
                bone_pairs.push_back({i, j});
            }
        }
    }

    std::vector<std::pair<int, int>> volume_pairs;
    for (int i = 0; i < skel.joint_count(); ++i) {
        if (static_caps[i].segments.empty()) continue;
        for (int v_idx = 0; v_idx < static_cast<int>(skel.volumes().size()); ++v_idx) {
            const CollisionVolume& v = skel.volumes()[v_idx];
            if (v.joint == i || skel[i].parent == v.joint || other_side(i, v.joint)) continue;
            const Vec3 s = shape ? v.scale.mul(shape->scale[v.joint]) : v.scale;
            if (std::min({s.x, s.y, s.z}) < 1e-4) continue;
            Hit h = volume_hit(static_caps[i], rest_globals[v.node], s);
            if (h.depth <= -0.01) {  // separated at rest
                volume_pairs.push_back({i, v_idx});
            }
        }
    }

    Pose test_pose(skel.size());
    std::vector<Xform> test_globals = rest_globals;

    for (auto& [name, lim] : constraints.limits) {
        const int node = skel.find(name);
        if (node < 0) continue;

        std::unordered_set<int> subtree;
        collect_subtree(skel, node, subtree);

        auto check_collision = [&](const std::vector<Xform>& g) -> bool {
            std::vector<RagdollCapsule> posed_sub(skel.size());
            for (int sub : subtree) {
                if (sub < skel.joint_count() && !static_caps[sub].segments.empty()) {
                    posed_sub[sub] = make_capsule(skel, shape, g, sub, radii[sub]);
                }
            }

            for (const auto& [i, j] : bone_pairs) {
                const bool in_i = subtree.count(i);
                const bool in_j = subtree.count(j);
                if (in_i == in_j) continue;
                const RagdollCapsule& cap_a = in_i ? posed_sub[i] : static_caps[i];
                const RagdollCapsule& cap_b = in_j ? posed_sub[j] : static_caps[j];
                Hit h = capsule_hit(cap_a, cap_b);
                if (h.depth > 0.005) return true;
            }

            for (const auto& [i, v_idx] : volume_pairs) {
                if (!subtree.count(i)) continue;
                const CollisionVolume& v = skel.volumes()[v_idx];
                const Vec3 s = shape ? v.scale.mul(shape->scale[v.joint]) : v.scale;
                Hit h = volume_hit(posed_sub[i], g[v.node], s);
                if (h.depth > 0.005) return true;
            }
            return false;
        };

        if (lim.kind == JointLimitKind::Hinge) {
            const Vec3 axis_local = from_frame(shape, node, lim.axis).normalized();
            // Positive sweep
            for (double deg = 2.0; deg <= lim.max_angle / kDegToRad; deg += 2.0) {
                test_pose.rot[node] = Quat::axis_angle(axis_local, deg * kDegToRad);
                update_subtree_globals(skel, shape, test_pose, node, test_globals);
                if (check_collision(test_globals)) {
                    lim.max_angle = std::max(0.0, (deg * kDegToRad) - margin);
                    lim.source = JointLimitSource::Collision;
                    lim.confidence = std::max(lim.confidence, 0.90);
                    break;
                }
            }
            test_pose.rot[node] = Quat{};
            update_subtree_globals(skel, shape, test_pose, node, test_globals);

            // Negative sweep
            for (double deg = -2.0; deg >= lim.min_angle / kDegToRad; deg -= 2.0) {
                test_pose.rot[node] = Quat::axis_angle(axis_local, deg * kDegToRad);
                update_subtree_globals(skel, shape, test_pose, node, test_globals);
                if (check_collision(test_globals)) {
                    lim.min_angle = std::min(0.0, (deg * kDegToRad) + margin);
                    lim.source = JointLimitSource::Collision;
                    lim.confidence = std::max(lim.confidence, 0.90);
                    break;
                }
            }
            test_pose.rot[node] = Quat{};
            update_subtree_globals(skel, shape, test_pose, node, test_globals);
        } else if (lim.kind == JointLimitKind::Cone) {
            // A stop per direction: a thigh that meets the belly going forward still swings far back and out. One
            // cone at the tightest direction's stop made the SL hips 11 degrees.
            const Vec3 bone_ax = from_frame(shape, node, lim.bone_axis).normalized();
            // The first stop points forward (else up) in SL's frame, so a body with its own bone axes gets the same
            // stops where they are, not turned round the bone with its frame.
            Vec3 fwd = Vec3{1, 0, 0} - bone_ax * bone_ax.x;
            if (fwd.length() < 0.3) fwd = Vec3{0, 0, 1} - bone_ax * bone_ax.z;
            lim.cone_ref = to_frame(shape, node, fwd.normalized());
            std::vector<double> stops(kConeStops, lim.cone_angle);
            bool hit = false;
            for (int k = 0; k < kConeStops; ++k) {
                const Vec3 dir = from_frame(shape, node, cone_stop_dir(lim, k, kConeStops));
                const Vec3 rot_ax = bone_ax.cross(dir).normalized();
                const double cap = cone_stop(lim, cone_stop_dir(lim, k, kConeStops));
                stops[k] = cap;
                for (double deg = 2.0; deg <= cap / kDegToRad; deg += 2.0) {
                    test_pose.rot[node] = Quat::axis_angle(rot_ax, deg * kDegToRad);
                    update_subtree_globals(skel, shape, test_pose, node, test_globals);
                    if (check_collision(test_globals)) {
                        stops[k] = std::clamp(deg * kDegToRad - margin, 0.0, cap);
                        hit = true;
                        break;
                    }
                }
                test_pose.rot[node] = Quat{};
                update_subtree_globals(skel, shape, test_pose, node, test_globals);
            }
            if (hit) {
                lim.cone_stops = stops;
                lim.source = JointLimitSource::Collision;
                lim.confidence = std::max(lim.confidence, 0.90);
            }
        }
        test_globals = rest_globals;
    }
}

void widen_from_animation(const Skeleton& skel, const Shape* shape, const Clip& clip,
                          RigConstraints& constraints, double margin_deg) {
    if (clip.end_frame <= 0) return;
    const double margin = margin_deg * kDegToRad;

    for (int f = 0; f <= clip.end_frame; ++f) {
        Pose p = evaluate_curves(skel, clip, f);
        for (auto& [name, lim] : constraints.limits) {
            const int node = skel.find(name);
            if (node < 0) continue;
            const Quat q_local = p.rot[node];
            const Quat q_frame = to_joint_frame(shape, node, q_local);

            if (lim.kind == JointLimitKind::Hinge) {
                Quat swing, twist;
                decompose_swing_twist(q_frame, lim.axis, swing, twist);
                const Vec3 tv{twist.x, twist.y, twist.z};
                const double sign = tv.dot(lim.axis) >= 0.0 ? 1.0 : -1.0;
                const double ang = sign * twist.angle();
                if (ang < lim.min_angle) {
                    lim.min_angle = ang - margin;
                    lim.source = JointLimitSource::Animation;
                }
                if (ang > lim.max_angle) {
                    lim.max_angle = ang + margin;
                    lim.source = JointLimitSource::Animation;
                }
            } else if (lim.kind == JointLimitKind::Cone) {
                Quat swing, twist;
                decompose_swing_twist(q_frame, lim.bone_axis, swing, twist);
                if (!is_frame_rotation_within_limits(lim, swing, 0)) {
                    widen_cone_swing(lim, swing, margin);
                    lim.source = JointLimitSource::Animation;
                }
                const Vec3 tv{twist.x, twist.y, twist.z};
                const double sign = tv.dot(lim.bone_axis) >= 0.0 ? 1.0 : -1.0;
                const double tw = sign * twist.angle();
                if (tw < lim.twist_min) {
                    lim.twist_min = std::max(-kPi, tw - margin);
                    lim.source = JointLimitSource::Animation;
                }
                if (tw > lim.twist_max) {
                    lim.twist_max = std::min(kPi, tw + margin);
                    lim.source = JointLimitSource::Animation;
                }
            }
        }
    }
}

namespace {

// Left and right get the same limits, the tighter of the two (mirrored): a body a little uneven, or its mesh, made a
// creature's hind knees differ by 20 degrees.
void make_symmetric(const Skeleton& skel, const Shape* shape, RigConstraints& rc) {
    for (auto& [name, left] : rc.limits) {
        if (!name.ends_with("Left")) continue;
        const int l = skel.find(name), r = skel.mirror(l);
        auto it = r >= 0 && r != l ? rc.limits.find(skel[r].name) : rc.limits.end();
        if (it == rc.limits.end() || it->second.kind != left.kind || !left.is_limited()) continue;
        const JointLimit& right = it->second;
        JointLimit m = mirror_joint_limit(skel, shape, l, r, left);
        if (m.kind == JointLimitKind::Hinge) {
            if (m.axis.dot(right.axis) < std::cos(10 * kDegToRad)) continue;  // not the same hinge: leave both
            m.min_angle = std::max(m.min_angle, right.min_angle);
            m.max_angle = std::min(m.max_angle, right.max_angle);
        } else {
            if (m.bone_axis.dot(right.bone_axis) < std::cos(10 * kDegToRad)) continue;
            const int n = static_cast<int>(std::max(m.cone_stops.size(), right.cone_stops.size()));
            std::vector<double> stops(n);
            for (int k = 0; k < n; ++k) {
                const Vec3 d = cone_stop_dir(m, k, n);
                stops[k] = std::min(cone_stop(m, d), cone_stop(right, d));
            }
            m.cone_stops = std::move(stops);
            m.cone_angle = std::min(m.cone_angle, right.cone_angle);
            m.twist_min = std::max(m.twist_min, right.twist_min);
            m.twist_max = std::min(m.twist_max, right.twist_max);
        }
        if (right.source == JointLimitSource::Collision) m.source = right.source;
        m.confidence = std::min(m.confidence, right.confidence);
        it->second = m;
        left = mirror_joint_limit(skel, shape, r, l, m);
    }
}

}  // namespace

std::vector<SuggestedLimitRow> suggest_joint_limits(const Skeleton& skel, const Shape* shape,
                                                    const std::vector<const DaeModel*>& models,
                                                    const Clip* clip,
                                                    const SuggestLimitsOptions& options) {
    RigConstraints rc;
    if (options.use_templates) {
        rc = template_limits(skel, shape);
    }
    if (options.use_bind_pose) {
        apply_bind_pose_limits(skel, shape, rc);
    }
    if (options.use_collision) {
        apply_collision_sweep(skel, shape, models, rc, options.collision_margin_deg);
    }
    make_symmetric(skel, shape, rc);
    if (options.use_animation && clip) {
        widen_from_animation(skel, shape, *clip, rc, options.anim_margin_deg);
    }

    std::vector<SuggestedLimitRow> rows;
    for (auto& [joint, lim] : rc.limits) {
        if (lim.is_limited()) {
            rows.push_back({joint, lim});
        }
    }

    // Sort: lowest confidence rows first, then alphabetically
    std::stable_sort(rows.begin(), rows.end(), [](const SuggestedLimitRow& a, const SuggestedLimitRow& b) {
        if (std::fabs(a.limit.confidence - b.limit.confidence) > 1e-4)
            return a.limit.confidence < b.limit.confidence;
        return a.joint < b.joint;
    });

    return rows;
}

LimitGroup joint_limit_group(std::string_view name) {
    if (name.rfind("mFace", 0) == 0 || name.rfind("mEye", 0) == 0 || name == "mSkull" || name == "mJaw") {
        return LimitGroup::Face;
    }
    if (name.rfind("mTail", 0) == 0) {
        return LimitGroup::Tail;
    }
    if (name.rfind("mWing", 0) == 0) {
        return LimitGroup::Wings;
    }
    if (name.rfind("mHindLimb", 0) == 0) {
        return LimitGroup::HindLegs;
    }
    if (name.rfind("mHand", 0) == 0) {
        if (name.ends_with("Left")) return LimitGroup::LeftHand;
        if (name.ends_with("Right")) return LimitGroup::RightHand;
    }
    if (name.ends_with("Left")) {
        if (name.rfind("mCollar", 0) == 0 || name.rfind("mShoulder", 0) == 0 ||
            name.rfind("mElbow", 0) == 0 || name.rfind("mWrist", 0) == 0) {
            return LimitGroup::LeftArm;
        }
        if (name.rfind("mHip", 0) == 0 || name.rfind("mKnee", 0) == 0 ||
            name.rfind("mAnkle", 0) == 0 || name.rfind("mFoot", 0) == 0 || name.rfind("mToe", 0) == 0) {
            return LimitGroup::LeftLeg;
        }
    }
    if (name.ends_with("Right")) {
        if (name.rfind("mCollar", 0) == 0 || name.rfind("mShoulder", 0) == 0 ||
            name.rfind("mElbow", 0) == 0 || name.rfind("mWrist", 0) == 0) {
            return LimitGroup::RightArm;
        }
        if (name.rfind("mHip", 0) == 0 || name.rfind("mKnee", 0) == 0 ||
            name.rfind("mAnkle", 0) == 0 || name.rfind("mFoot", 0) == 0 || name.rfind("mToe", 0) == 0) {
            return LimitGroup::RightLeg;
        }
    }
    if (name == "mPelvis" || name == "mTorso" || name == "mChest" || name == "mNeck" || name == "mHead" ||
        name.rfind("mSpine", 0) == 0) {
        return LimitGroup::SpineNeckHead;
    }
    return LimitGroup::Other;
}

std::string_view limit_group_name(LimitGroup group) {
    switch (group) {
        case LimitGroup::SpineNeckHead: return "Spine and Neck, Head";
        case LimitGroup::LeftArm: return "Left Arm";
        case LimitGroup::RightArm: return "Right Arm";
        case LimitGroup::LeftHand: return "Left Hand";
        case LimitGroup::RightHand: return "Right Hand";
        case LimitGroup::LeftLeg: return "Left Leg";
        case LimitGroup::RightLeg: return "Right Leg";
        case LimitGroup::HindLegs: return "Hind Legs";
        case LimitGroup::Tail: return "Tail";
        case LimitGroup::Wings: return "Wings";
        case LimitGroup::Face: return "Face";
        case LimitGroup::Other: return "Other";
    }
    return "Other";
}

LimitGroup mirror_limit_group(LimitGroup group) {
    switch (group) {
        case LimitGroup::LeftArm: return LimitGroup::RightArm;
        case LimitGroup::RightArm: return LimitGroup::LeftArm;
        case LimitGroup::LeftHand: return LimitGroup::RightHand;
        case LimitGroup::RightHand: return LimitGroup::LeftHand;
        case LimitGroup::LeftLeg: return LimitGroup::RightLeg;
        case LimitGroup::RightLeg: return LimitGroup::LeftLeg;
        default: return group;
    }
}

static bool limit_changes(const RigConstraints* target, const std::string& joint, const JointLimit& lim) {
    const JointLimit* now = target ? target->find(joint) : nullptr;
    return lim.is_limited() ? !now || !(*now == lim) : now != nullptr;
}

int count_limit_changes(const RigConstraints* target, const RigConstraints& pending,
                        const std::function<bool(const std::string& joint)>& include) {
    int count = 0;
    for (const auto& [j, lim] : pending.limits)
        if ((!include || include(j)) && limit_changes(target, j, lim)) ++count;
    return count;
}

int apply_limits(RigConstraints& target, const RigConstraints& pending,
                 const std::function<bool(const std::string& joint)>& include) {
    int count = 0;
    for (const auto& [j, lim] : pending.limits) {
        if ((include && !include(j)) || !limit_changes(&target, j, lim)) continue;
        if (lim.is_limited()) target.limits[j] = lim;
        else target.remove(j);
        ++count;
    }
    return count;
}

int apply_group_limits(RigConstraints& target, const RigConstraints& pending, LimitGroup group,
                       const std::function<bool(const std::string& joint)>& is_checked) {
    return apply_limits(target, pending,
                        [&](const std::string& j) { return joint_limit_group(j) == group && (!is_checked || is_checked(j)); });
}

int apply_selected_limits(RigConstraints& target, const RigConstraints& pending,
                          const std::vector<std::string>& selected_joints,
                          const std::function<bool(const std::string& joint)>& is_checked) {
    return apply_limits(target, pending, [&](const std::string& j) {
        return std::find(selected_joints.begin(), selected_joints.end(), j) != selected_joints.end() &&
               (!is_checked || is_checked(j));
    });
}

int apply_all_limits(RigConstraints& target, const RigConstraints& pending,
                     const std::function<bool(const std::string& joint)>& is_checked) {
    return apply_limits(target, pending, is_checked);
}

JointLimit widen_limit_to_pose(const Skeleton& skel, const Shape* shape, int node, const JointLimit* existing,
                               const Quat& local_rot) {
    JointLimit lim;
    if (existing && existing->is_limited()) {
        lim = *existing;
    } else if (const RigConstraints tmpl = template_limits(skel, shape); const JointLimit* t = tmpl.find(skel[node].name)) {
        // A fresh 0..0 hinge about a fixed axis froze the joint.
        lim = *t;
    } else {
        lim.kind = JointLimitKind::Cone;
        lim.bone_axis = to_joint_frame(shape, node, rest_bone_dir(skel, shape, node));
        lim.cone_angle = 45 * kDegToRad;
        lim.twist_min = -30 * kDegToRad, lim.twist_max = 30 * kDegToRad;
    }
    // The joint in the limit's frame. Keys are in SL's frame, so they must not go through from_rig_axes first (that
    // read a 90 degree knee as 0.2 degrees on bodies with their own bone axes).
    const Quat frame_rot = to_joint_frame(shape, node, local_rot);
    auto twist_about = [&](const Vec3& axis) {
        Quat swing, twist;
        decompose_swing_twist(frame_rot, axis, swing, twist);
        const Vec3 tv{twist.x, twist.y, twist.z};
        return std::remainder(2.0 * std::atan2(tv.dot(axis), twist.w), 2 * kPi);
    };
    if (lim.kind == JointLimitKind::Hinge) {
        const double a = twist_about(lim.axis.length() > 1e-6 ? lim.axis.normalized() : Vec3{0, 1, 0});
        lim.min_angle = std::min(lim.min_angle, a);
        lim.max_angle = std::max(lim.max_angle, a);
    } else if (lim.kind == JointLimitKind::Cone) {
        const Vec3 bone_axis = lim.bone_axis.length() > 1e-6 ? lim.bone_axis.normalized() : Vec3{0, 1, 0};
        Quat swing, twist;
        decompose_swing_twist(frame_rot, bone_axis, swing, twist);
        const double tw = twist_about(bone_axis);
        widen_cone_swing(lim, swing);  // its per-direction stop too
        lim.twist_min = std::min(lim.twist_min, tw);
        lim.twist_max = std::max(lim.twist_max, tw);
    }
    lim.source = JointLimitSource::Manual;
    lim.confidence = 1.0;
    return lim;
}

void PendingLimits::start(const std::string& body, const std::vector<SuggestedLimitRow>& rows) {
    clear();
    body_id = body;
    for (const auto& r : rows) limits.limits[r.joint] = suggested.limits[r.joint] = r.limit;
}

bool PendingLimits::record(RigConstraints before, size_t depth) {
    if (before == limits) return false;
    undo.push_back({std::move(before), depth});
    redo.clear();
    return true;
}

bool PendingLimits::undo_edit(size_t depth) {
    if (!can_undo(depth)) return false;
    redo.push_back({std::exchange(limits, std::move(undo.back().before)), depth});
    undo.pop_back();
    return true;
}

bool PendingLimits::redo_edit(size_t depth) {
    if (!can_redo(depth)) return false;
    undo.push_back({std::exchange(limits, std::move(redo.back().before)), depth});
    redo.pop_back();
    return true;
}

bool edits_pending_limits(bool panel_open, LimitPreview preview, const PendingLimits& pending,
                          const std::string& body) {
    return panel_open && preview == LimitPreview::Suggested && pending.for_body(body);
}

const RigConstraints* posing_limits(bool respect, bool panel_open, LimitPreview preview, const PendingLimits& pending,
                                    const std::string& body, const RigConstraints* applied) {
    if (!respect || (panel_open && preview == LimitPreview::Off)) return nullptr;
    return edits_pending_limits(panel_open, preview, pending, body) ? &pending.limits : applied;
}

}  // namespace vats
