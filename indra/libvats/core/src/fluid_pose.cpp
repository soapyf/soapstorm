// Viewport Avatar Toolset - posing by dragging the body.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/fluid_pose.h"
#include "vats/edit.h"
#include "vats/rig_constraints.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>

namespace vats {

SurfaceHit ray_surface(const Vec3& origin, const Vec3& dir, const std::vector<float>& positions,
                       const std::vector<std::uint32_t>& indices) {
    SurfaceHit best;
    auto vtx = [&](std::uint32_t i) {
        return Vec3{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
    };
    const size_t num_tris = indices.size() / 3;
    for (size_t t = 0; t < num_tris; ++t) {
        const std::uint32_t i0 = indices[t * 3];
        const std::uint32_t i1 = indices[t * 3 + 1];
        const std::uint32_t i2 = indices[t * 3 + 2];
        if (i0 * 3 + 2 >= positions.size() || i1 * 3 + 2 >= positions.size() || i2 * 3 + 2 >= positions.size()) continue;
        const Vec3 a = vtx(i0);
        const Vec3 e1 = vtx(i1) - a;
        const Vec3 e2 = vtx(i2) - a;
        const Vec3 pv = dir.cross(e2);
        const double det = e1.dot(pv);
        if (std::fabs(det) < 1e-12) continue;
        const Vec3 tv = origin - a;
        const double u = tv.dot(pv) / det;
        if (u < 0.0 || u > 1.0) continue;
        const Vec3 qv = tv.cross(e1);
        const double v = dir.dot(qv) / det;
        if (v < 0.0 || u + v > 1.0) continue;
        const double dist = e2.dot(qv) / det;
        if (dist <= 1e-6 || dist >= best.t) continue;
        best.t = dist;
        best.triangle = static_cast<int>(t);
        best.u = u;
        best.v = v;
    }
    return best;
}

int surface_joint(const Skeleton& skel, const DaeModel& model, const SurfaceHit& hit) {
    if (hit.triangle < 0 || hit.t >= 1e30 || !model.rigged) return -1;
    const size_t tri = static_cast<size_t>(hit.triangle);
    if (tri * 3 + 2 >= model.indices.size()) return -1;

    double u = std::clamp(hit.u, 0.0, 1.0);
    double v = std::clamp(hit.v, 0.0, 1.0);
    if (u + v > 1.0) {
        double s = u + v;
        u /= s;
        v /= s;
    }
    const double w[3] = {1.0 - u - v, u, v};
    const std::uint32_t vi[3] = {
        model.indices[tri * 3],
        model.indices[tri * 3 + 1],
        model.indices[tri * 3 + 2]
    };

    const int root = dae_root(skel);
    const int pelvis = skel.find("mPelvis");

    auto map_joint = [&](int j) -> int {
        if (j < 0) return -1;
        if (j < root) {
            if (j < skel.joint_count()) return j;
            if (j < skel.size() && skel[j].volume) {
                for (const CollisionVolume& cv : skel.volumes())
                    if (cv.node == j) return cv.joint;
            }
            return -1;
        }
        if (j == root) return pelvis >= 0 ? pelvis : 0;
        const int vol_idx = j - root - 1;
        if (vol_idx >= 0 && vol_idx < static_cast<int>(skel.volumes().size()))
            return skel.volumes()[vol_idx].joint;
        return -1;
    };

    std::vector<double> joint_weights(static_cast<size_t>(skel.joint_count()), 0.0);
    for (int c = 0; c < 3; ++c) {
        if (w[c] <= 0.0) continue;
        const size_t vidx = vi[c];
        if (vidx * 4 + 3 >= model.joints.size() || vidx * 4 + 3 >= model.weights.size()) continue;
        for (int k = 0; k < 4; ++k) {
            const float wt = model.weights[vidx * 4 + k];
            if (wt <= 0.0f) continue;
            const int mapped = map_joint(model.joints[vidx * 4 + k]);
            if (mapped >= 0 && mapped < skel.joint_count())
                joint_weights[mapped] += wt * w[c];
        }
    }

    int best_joint = -1;
    double best_weight = 0.0;
    for (int j = 0; j < skel.joint_count(); ++j) {
        if (joint_weights[j] > best_weight) {
            best_weight = joint_weights[j];
            best_joint = j;
        }
    }
    return best_joint;
}

int surface_joint(const Skeleton& skel, const AvatarMesh& mesh, const SurfaceHit& hit) {
    if (hit.triangle < 0 || hit.t >= 1e30) return -1;
    const size_t tri = static_cast<size_t>(hit.triangle);
    if (tri * 3 + 2 >= mesh.indices().size()) return -1;

    double u = std::clamp(hit.u, 0.0, 1.0);
    double v = std::clamp(hit.v, 0.0, 1.0);
    if (u + v > 1.0) {
        double s = u + v;
        u /= s;
        v /= s;
    }
    const double w[3] = {1.0 - u - v, u, v};
    const std::uint32_t vi[3] = {
        mesh.indices()[tri * 3],
        mesh.indices()[tri * 3 + 1],
        mesh.indices()[tri * 3 + 2]
    };

    const int pelvis = skel.find("mPelvis");
    auto map_node = [&](int n) -> int {
        if (n < 0) return pelvis >= 0 ? pelvis : 0;
        if (n < skel.joint_count()) return n;
        if (n < skel.size() && skel[n].volume) {
            for (const CollisionVolume& cv : skel.volumes())
                if (cv.node == n) return cv.joint;
        }
        return -1;
    };

    std::vector<double> joint_weights(static_cast<size_t>(skel.joint_count()), 0.0);
    const auto& infs = mesh.influences();
    for (int c = 0; c < 3; ++c) {
        if (w[c] <= 0.0) continue;
        const size_t vidx = vi[c];
        if (vidx >= infs.size()) continue;
        const Influence& inf = infs[vidx];
        const float wa = (1.0f - inf.blend);
        const float wb = inf.blend;
        if (wa > 0.0f) {
            const int ja = map_node(inf.a);
            if (ja >= 0 && ja < skel.joint_count())
                joint_weights[ja] += wa * w[c];
        }
        if (wb > 0.0f) {
            const int jb = map_node(inf.b);
            if (jb >= 0 && jb < skel.joint_count())
                joint_weights[jb] += wb * w[c];
        }
    }

    int best_joint = -1;
    double best_weight = 0.0;
    for (int j = 0; j < skel.joint_count(); ++j) {
        if (joint_weights[j] > best_weight) {
            best_weight = joint_weights[j];
            best_joint = j;
        }
    }
    return best_joint;
}

bool body_drag_joint(const Skeleton& skel, int node) {
    if (node < 0 || node >= skel.size()) return false;
    const std::string& name = skel[node].name;
    return name == "mPelvis" || name == "mTorso" || name == "mChest" ||
           (name.rfind("mSpine", 0) == 0);
}

namespace {

// Whether limb li is a leg (or hind leg) the shown body weights.
bool body_leg(const Rig& rig, size_t li, const BodyGround& body) {
    const LimbInfo& l = rig.limbs()[li];
    if (l.name.find("Leg") == std::string::npos || l.end < 0 || l.end >= rig.skeleton().joint_count()) return false;
    return !body.weighted || body.weighted(l.root) || body.weighted(l.mid) || body.weighted(l.end);
}

// Each leg's lowest point (z) in globals, 1e30 for a limb that is no leg of the body: the lowest of its end joint and
// the joints under it, or with a mesh the lowest vertex those joints carry.
std::vector<double> leg_lows(const Rig& rig, const std::vector<Xform>& globals, const Shape* shape,
                             const BodyGround& body) {
    const Skeleton& skel = rig.skeleton();
    std::vector<double> low(rig.limbs().size(), 1e30);
    std::vector<int> leg_of(static_cast<size_t>(skel.joint_count()), -1);
    for (size_t li = 0; li < rig.limbs().size(); ++li) {
        if (!body_leg(rig, li, body)) continue;
        std::vector<int> stack = {rig.limbs()[li].end};
        while (!stack.empty()) {
            const int cur = stack.back();
            stack.pop_back();
            if (cur < 0 || cur >= skel.joint_count()) continue;
            leg_of[cur] = static_cast<int>(li);
            if (body.mesh.empty()) low[li] = std::min(low[li], globals[cur].pos.z);
            for (int c : skel[cur].children) stack.push_back(c);
        }
    }
    std::vector<float> pos, nrm;
    for (const DaeModel* m : body.mesh) {
        if (!m || !m->rigged) continue;
        skin_prop(*m, skel, globals, shape, pos, nrm);
        for (size_t v = 0; v * 3 + 2 < pos.size(); ++v)
            for (size_t i = v * 4; i < v * 4 + 4 && i < m->joints.size() && i < m->weights.size(); ++i) {
                const int j = m->joints[i];  // below the joint count, a skeleton joint (SK-40)
                if (m->weights[i] > 0 && j >= 0 && j < skel.joint_count() && leg_of[j] >= 0)
                    low[leg_of[j]] = std::min(low[leg_of[j]], double(pos[v * 3 + 2]));
            }
    }
    return low;
}

// lim stretched to take in local_rot: a planted leg's joint keyed past its limit stays as keyed instead of the limit
// pulling it in and the foot off its spot. It can turn back towards the range, not further out. (A hinge still drops
// a turn off its axis, as clamp_frame_rotation does.)
void take_in(JointLimit& lim, const Quat& local_rot, const Shape* shape, int node) {
    const Quat f = to_joint_frame(shape, node, local_rot);
    auto about = [&](const Vec3& axis_in, Quat& swing, double ref) {
        const Vec3 axis = axis_in.length() > 1e-6 ? axis_in.normalized() : Vec3{0, 1, 0};
        Quat twist;
        decompose_swing_twist(f, axis, swing, twist);
        return wrap_near_pi(2.0 * std::atan2(Vec3{twist.x, twist.y, twist.z}.dot(axis), twist.w), ref);
    };
    Quat swing;
    if (lim.kind == JointLimitKind::Hinge) {
        const double a = about(lim.axis, swing, 0.5 * (lim.min_angle + lim.max_angle));
        lim.min_angle = std::min(lim.min_angle, a);
        lim.max_angle = std::max(lim.max_angle, a);
    } else if (lim.kind == JointLimitKind::Cone) {
        const double t = about(lim.bone_axis, swing, 0.5 * (lim.twist_min + lim.twist_max));
        lim.twist_min = std::min(lim.twist_min, t);
        lim.twist_max = std::max(lim.twist_max, t);
        lim.cone_angle = std::max(lim.cone_angle, 2.0 * std::atan2(Vec3{swing.x, swing.y, swing.z}.length(), swing.w));
    }
}

// Clamps node's rotation in pose to its limit, noting a clamp in report.
void clamp_noted(const Skeleton& skel, const RigConstraints* constraints, int node, const Shape* shape, Pose& pose,
                 ClampReport* report) {
    const JointLimit* lim = constraints ? constraints->find(skel[node].name) : nullptr;
    if (!lim) return;
    ClampedJoint c;
    if (report && check_joint_clamp(skel[node].name, node, *lim, pose.rot[node], shape, c) &&
        !report->contains(skel[node].name))
        report->clamped.push_back(std::move(c));
    pose.rot[node] = clamp_joint_rotation(*lim, pose.rot[node], shape, node);
}

}  // namespace

double ground_height(const Rig& rig, const Shape* shape, const BodyGround& body) {
    const Skeleton& skel = rig.skeleton();
    const std::vector<double> low = leg_lows(rig, skel.global_pose(Pose(skel.size()), shape), shape, body);
    const double lowest = low.empty() ? 1e30 : *std::min_element(low.begin(), low.end());
    return lowest < 1e29 ? lowest : 0.0;
}

BodyDrag begin_body_drag(const Rig& rig, const Clip& clip, double frame, int node, const Shape* shape,
                         const BodyGround& body) {
    const Skeleton& skel = rig.skeleton();
    BodyDrag drag;
    drag.node = node;
    drag.start = evaluate(rig, clip, frame, shape);

    const int chest = skel.find("mChest");
    const int pelvis = skel.find("mPelvis");

    if (node == chest && chest >= 0) {
        drag.spine.end = chest;
        for (int i = skel[chest].parent; i > 0 && i != pelvis; i = skel[i].parent) {
            drag.spine.bones.insert(drag.spine.bones.begin(), i);
            drag.spine.longest += (skel[i].name.rfind("mSpine", 0) != 0);
            drag.spine.turning += (skel[i].name.rfind("mSpine", 0) != 0);
        }
        drag.head = skel.find("mHead");
    }

    const double ground = ground_height(rig, shape, body);
    const std::vector<double> low = leg_lows(rig, drag.start.globals, shape, body);

    for (size_t li = 0; li < rig.limbs().size(); ++li) {
        if (!body_leg(rig, li, body)) continue;
        const LimbInfo& l = rig.limbs()[li];

        bool is_held = false;
        if (li < drag.start.limbs.size() && drag.start.limbs[li].blend > 0) {
            is_held = true;
        } else {
            std::vector<int> stack = {l.end};
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                if (cur < 0 || cur >= skel.joint_count()) continue;
                if (pin_at(clip, rig, cur, frame) >= 0) {
                    is_held = true;
                    break;
                }
                for (int c : skel[cur].children)
                    if (c >= 0 && c < skel.joint_count()) stack.push_back(c);
            }
        }

        if (is_held || low[li] - ground <= 0.05) {
            PlantedFoot foot;
            foot.limb = static_cast<int>(li);
            foot.node = l.end;
            foot.at = drag.start.globals[l.end];
            foot.held = is_held;
            if (!is_held) {
                foot.chain = auto_ik_chain(rig, clip, frame, l.end);
                if (foot.chain.bones.empty()) {
                    foot.chain.end = l.end;
                    foot.chain.bones = {l.root, l.mid};
                    foot.chain.turning = foot.chain.longest = 2;
                }
            }
            drag.feet.push_back(std::move(foot));
        }
    }

    return drag;
}

std::vector<std::string> key_body_drag(Clip& clip, const Rig& rig, double frame, const BodyDrag& drag,
                                       const Vec3& target, bool plant, const Shape* shape,
                                       const RigConstraints* constraints, ClampReport* report) {
    std::vector<std::string> keyed;
    const Skeleton& skel = rig.skeleton();
    const int pelvis = skel.find("mPelvis");
    if (pelvis < 0 || drag.node < 0) return keyed;

    Pose pose = drag.start.pose;
    const auto hinges = auto_ik_hinges(rig, shape);

    if (!drag.spine.bones.empty()) {
        const Vec3 chest_start = drag.start.globals[drag.node].pos;
        const Vec3 halfway = chest_start + (target - chest_start) * 0.5;
        solve_auto_ik(skel, shape, drag.spine, hinges, halfway, pose, constraints, report);

        std::vector<Xform> cur_g = skel.global_pose(pose, shape);
        const Vec3 remaining = target - cur_g[drag.node].pos;
        pose.offset[pelvis] += remaining;

        if (drag.head >= 0) {
            cur_g = skel.global_pose(pose, shape);
            const int head_parent = skel[drag.head].parent;
            const Quat pr = head_parent >= 0 ? cur_g[head_parent].rot : Quat{};
            pose.rot[drag.head] = (skel[drag.head].rest.conj() * pr.conj() * drag.start.globals[drag.head].rot).normalized();
            clamp_noted(skel, constraints, drag.head, shape, pose, report);
        }
    } else {
        const Vec3 node_start = drag.start.globals[drag.node].pos;
        const Vec3 delta = target - node_start;
        pose.offset[pelvis] += delta;
    }

    if (plant) {
        // The legs' limits, each taking in the leg's pose at the press.
        std::optional<RigConstraints> legs;
        if (constraints) {
            legs = *constraints;
            for (const PlantedFoot& f : drag.feet) {
                if (f.held) continue;
                for (int b : f.chain.bones)
                    if (JointLimit* lim = legs->find(skel[b].name)) take_in(*lim, drag.start.pose.rot[b], shape, b);
                if (JointLimit* lim = legs->find(skel[f.node].name))
                    take_in(*lim, drag.start.pose.rot[f.node], shape, f.node);
            }
        }
        const RigConstraints* leg_limits = legs ? &*legs : nullptr;
        for (const PlantedFoot& f : drag.feet) {
            if (f.held || f.chain.bones.empty()) continue;
            solve_auto_ik(skel, shape, f.chain, hinges, f.at.pos, pose, leg_limits, report);
            const std::vector<Xform> cur_g = skel.global_pose(pose, shape);
            const int foot_parent = skel[f.node].parent;
            const Quat pr = foot_parent >= 0 ? cur_g[foot_parent].rot : Quat{};
            pose.rot[f.node] = (skel[f.node].rest.conj() * pr.conj() * f.at.rot).normalized();
            clamp_noted(skel, leg_limits, f.node, shape, pose, report);
        }
    }

    key_offset(clip, skel[pelvis].name, frame, pose.offset[pelvis]);
    keyed.push_back(skel[pelvis].name);

    if (!drag.spine.bones.empty()) {
        for (int b : drag.spine.bones) {
            if (skel[b].name.rfind("mSpine", 0) == 0) continue;
            key_rotation(clip, skel[b].name, frame, pose.rot[b]);
            keyed.push_back(skel[b].name);
        }
        if (drag.head >= 0) {
            key_rotation(clip, skel[drag.head].name, frame, pose.rot[drag.head]);
            keyed.push_back(skel[drag.head].name);
        }
    }

    // Planted, the legs key their solve. Not (Alt let go of the floor mid-drag), a leg this drag keyed goes back to
    // the press: a bone with a key at the frame gets its press rotation again (the press's own key, or one this drag
    // added); one without was never keyed here, so it is at the press's already.
    for (const PlantedFoot& f : drag.feet) {
        if (f.held) continue;
        std::vector<int> bones = f.chain.bones;
        bones.push_back(f.node);
        for (int b : bones) {
            if (skel[b].name.rfind("mSpine", 0) == 0) continue;
            if (!plant && !has_key_at(clip, skel[b].name, frame)) continue;
            key_rotation(clip, skel[b].name, frame, pose.rot[b]);
            keyed.push_back(skel[b].name);
        }
    }

    return keyed;
}

namespace {

int chain_depth(const Skeleton& skel, int node) {
    int depth = 1;
    for (int steps = 0; steps < skel.size(); ++steps) {
        int next = -1;
        for (int c : skel[node].children) {
            if (!skel[c].attachment) {
                next = c;
                break;
            }
        }
        if (next < 0) return depth;
        node = next;
        ++depth;
    }
    return depth;
}

}  // namespace

std::vector<DynChain> follow_through_chains(const Skeleton& skel, const Clip& clip, const std::vector<int>& moving,
                                            const std::function<bool(int)>& weighted) {
    std::vector<DynChain> result;
    std::unordered_set<int> covered;

    auto takes_moving = [&](const std::vector<int>& nodes) {
        for (int n : nodes) {
            if (std::find(moving.begin(), moving.end(), n) != moving.end()) return true;
        }
        return false;
    };

    // 1. Clip's own unbaked Dynamics chains
    for (const DynChain& d : clip.dynamics) {
        if (d.baked) continue;
        std::vector<int> nodes = dyn_nodes(skel, d, true);
        if (nodes.empty() || takes_moving(nodes)) continue;
        DynChain c = d;
        c.gravity = 0.0;
        result.push_back(c);
        for (int n : nodes) covered.insert(n);
    }

    // 2. Default chains for loose parts not covered
    struct LoosePart {
        const char* name;
        const char* preset;
    };
    static const LoosePart kLooseParts[] = {
        {"mTail1", "tail"},
        {"mWing1Left", "tail"},
        {"mWing1Right", "tail"},
        {"mFaceEar1Left", "ears"},
        {"mFaceEar1Right", "ears"},
        {"BELLY", "jiggle"},
        {"BUTT", "jiggle"},
        {"LEFT_PEC", "jiggle"},
        {"RIGHT_PEC", "jiggle"},
    };

    for (const auto& part : kLooseParts) {
        int root = skel.find(part.name);
        if (root < 0) continue;
        if (weighted && !weighted(root)) continue;

        int len = skel[root].volume ? 1 : chain_depth(skel, root);
        DynChain c = dyn_preset(part.preset, part.name, len);
        c.gravity = 0.0;
        std::vector<int> nodes = dyn_nodes(skel, c, true);
        if (nodes.empty() || takes_moving(nodes)) continue;

        bool is_covered = false;
        for (int n : nodes) {
            if (covered.count(n)) {
                is_covered = true;
                break;
            }
        }
        if (is_covered) continue;

        result.push_back(c);
        for (int n : nodes) covered.insert(n);
    }

    return result;
}

FollowThrough::FollowThrough(const Skeleton& skel, const std::vector<DynChain>& chains,
                             const std::vector<Xform>& animated)
    : chains_(chains), sim_(skel, chains_), settled_(chains.empty()) {
    if (!chains_.empty()) {
        sim_.reset(animated);
    }
}

bool FollowThrough::step(const std::vector<Xform>& animated, double dt, Pose& pose) {
    if (chains_.empty()) {
        settled_ = true;
        return false;
    }
    if (dt <= 0) return !settled_;

    const int steps = std::max(1, int(std::ceil(dt * DynSim::kStepsPerSecond - 1e-6)));
    const double sub_dt = dt / steps;
    for (int s = 0; s < steps; ++s) {
        sim_.step(animated, sub_dt);
    }

    constexpr double kRestSpeed = 0.005;  // 5 mm/s
    if (sim_.max_speed() < kRestSpeed) {
        still_ += dt;
        if (still_ >= 0.15) {
            settled_ = true;
            sim_.reset(animated);
        }
    } else {
        still_ = 0.0;
        settled_ = false;
    }

    if (!settled_) {
        sim_.apply(animated, pose);
    }
    return !settled_;
}

}  // namespace vats
