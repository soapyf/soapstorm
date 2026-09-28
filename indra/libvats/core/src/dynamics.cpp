// Viewport Avatar Toolset - bakeable dynamics ("dynabones").
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/dynamics.h"

#include <algorithm>
#include <cmath>

#include "vats/anim_convert.h"
#include "vats/edit.h"

namespace vats {
namespace {

Quat arc(const Vec3& u, const Vec3& v) {
    double d = u.dot(v);
    if (d < -0.999999) {
        Vec3 a = std::fabs(u.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        return Quat::axis_angle(u.cross(a).normalized(), kPi);
    }
    Vec3 c = u.cross(v);
    return Quat{1 + d, c.x, c.y, c.z}.normalized();
}

Xform parent_global(const Skeleton& skel, const std::vector<Xform>& g, int node) {
    int p = skel[node].parent;
    return p >= 0 ? g[p] : Xform{};
}

// The bone's tail in its own frame: the next chain node, else the display tail, else the first joint child.
Vec3 tail_local(const Skeleton& skel, const std::vector<int>& nodes, size_t i, const std::vector<Xform>& g) {
    int n = nodes[i];
    if (i + 1 < nodes.size()) return (g[n].inverse() * g[nodes[i + 1]]).pos;
    if (skel[n].end.length() > 1e-4) return skel[n].end;
    for (int c : skel[n].children)
        if (!skel[c].attachment) return (g[n].inverse() * g[c]).pos;
    return {};
}

}  // namespace

DynChain dyn_preset(const std::string& kind, const std::string& root, int length) {
    DynChain c;
    c.root = root;
    c.length = length;
    if (kind == "tail") c.stiffness = 0.08, c.damping = 0.12, c.drag = 0.03, c.gravity = 0.3, c.radius = 0.03;
    else if (kind == "ears") c.stiffness = 0.2, c.damping = 0.25, c.drag = 0.02, c.gravity = 0.1, c.radius = 0.01;
    // Follow-through on a limb or spine: lags and settles with little swing back, no droop.
    else if (kind == "overlap") c.stiffness = 0.3, c.damping = 0.35, c.drag = 0.02, c.gravity = 0.0, c.radius = 0.02;
    else if (kind == "jiggle") c.stiffness = 0.12, c.damping = 0.08, c.drag = 0.0, c.gravity = 0.0, c.radius = 0, c.length = 1;
    return c;
}

std::vector<int> dyn_nodes(const Skeleton& skel, const DynChain& chain) {
    std::vector<int> out;
    int n = skel.find(chain.root);
    if (n < 0) return out;
    if (skel[n].attachment && !skel[n].volume) return out;  // attachment points do not swing
    out.push_back(n);
    if (skel[n].volume) return out;
    for (int k = 1; k < chain.length; ++k) {
        int next = -1;
        for (int c : skel[n].children)
            if (!skel[c].attachment) {
                next = c;
                break;
            }
        if (next < 0) break;
        out.push_back(n = next);
    }
    return out;
}

DynSim::DynSim(const Skeleton& skel, const std::vector<DynChain>& chains) : skel_(skel) {
    for (const DynChain& d : chains) {
        Chain c{d, dyn_nodes(skel, d), {}, {}, {}};
        if (c.nodes.empty()) continue;
        c.p.resize(c.nodes.size());
        c.prev = c.target_prev = c.p;
        chains_.push_back(std::move(c));
    }
}

void DynSim::reset(const std::vector<Xform>& g) {
    for (Chain& c : chains_)
        for (size_t i = 0; i < c.nodes.size(); ++i) {
            const Xform& x = g[c.nodes[i]];
            c.p[i] = skel_[c.nodes[i]].volume ? x.pos : x.apply(tail_local(skel_, c.nodes, i, g));
            c.prev[i] = c.target_prev[i] = c.p[i];
        }
}

// Pushes p out of every collision volume, inflated by radius. A volume that already holds the point's
// place in the animated pose is skipped: the rest pose sits inside it (ears in the head), so it is not an obstacle.
Vec3 DynSim::collide(const Vec3& p, const Vec3& target, double radius, const std::vector<Xform>& g) const {
    Vec3 out = p;
    for (const CollisionVolume& v : skel_.volumes()) {
        if (v.node < 0) continue;
        const Xform& x = g[v.node];
        Vec3 s = v.scale + Vec3{radius, radius, radius};
        auto unit = [&](const Vec3& w) {
            Vec3 q = x.inverse().apply(w);
            return Vec3{q.x / s.x, q.y / s.y, q.z / s.z};
        };
        if (unit(target).length() < 1) continue;
        Vec3 u = unit(out);
        double len = u.length();
        if (len >= 1 || len < 1e-9) continue;
        u = u * (1 / len);
        out = x.apply(Vec3{u.x * s.x, u.y * s.y, u.z * s.z});
    }
    return out;
}

void DynSim::step(const std::vector<Xform>& g, double dt) {
    const Vec3 gravity_step{0, 0, -9.81 * dt * dt};
    for (Chain& c : chains_) {
        const DynChain& d = c.def;
        Xform parent = parent_global(skel_, g, c.nodes[0]);
        for (size_t i = 0; i < c.nodes.size(); ++i) {
            const int n = c.nodes[i];
            // Animated transform of this node under its (simulated) parent.
            Xform a = parent * (parent_global(skel_, g, n).inverse() * g[n]);
            const bool volume = skel_[n].volume;
            const Vec3 t = volume ? Vec3{} : tail_local(skel_, c.nodes, i, g);
            const Vec3 target = a.apply(t);
            Vec3 v = c.p[i] - c.prev[i], vt = target - c.target_prev[i];
            Vec3 next = c.p[i] + v * (1 - d.drag) - (v - vt) * d.damping + (target - c.p[i]) * d.stiffness +
                        gravity_step * d.gravity;
            const double len = t.length();
            if (!volume && len > 1e-6) {
                Vec3 dir = next - a.pos;
                next = a.pos + (dir.length() > 1e-9 ? dir.normalized() : (target - a.pos).normalized()) * len;
            }
            // Pushing out and keeping the bone length fight near a surface; a few rounds settle it. Obstacles
            // are judged by the purely animated pose. ponytail: radial push-out, so a tip wedged at a grazing
            // angle can stay a few millimetres inside; push along the bone's tangent if that shows.
            for (int round = 0; round < 4; ++round) {
                next = collide(next, g[n].apply(t), d.radius, g);
                if (!volume && len > 1e-6) next = a.pos + (next - a.pos).normalized() * len;
            }
            c.prev[i] = c.p[i];
            c.p[i] = next;
            c.target_prev[i] = target;
            if (!volume && len > 1e-6) a.rot = arc((target - a.pos).normalized(), (next - a.pos).normalized()) * a.rot;
            parent = a;
        }
    }
}

void DynSim::apply(const std::vector<Xform>& g, Pose& pose) const {
    for (const Chain& c : chains_) {
        Xform parent = parent_global(skel_, g, c.nodes[0]);
        for (size_t i = 0; i < c.nodes.size(); ++i) {
            const int n = c.nodes[i];
            Xform a = parent * (parent_global(skel_, g, n).inverse() * g[n]);
            if (skel_[n].volume) {
                // ponytail: the offset ignores the parent's shape scale; jiggle offsets are millimetres.
                pose.offset[n] += parent.rot.conj().rotate(c.p[i] - a.pos);
                continue;
            }
            const Vec3 t = tail_local(skel_, c.nodes, i, g);
            if (t.length() > 1e-6)
                a.rot = arc(a.rot.rotate(t).normalized(), (c.p[i] - a.pos).normalized()) * a.rot;
            pose.rot[n] = (skel_[n].rest.conj() * (parent.rot.conj() * a.rot)).normalized();
            parent = a;
        }
    }
}

std::vector<Pose> simulate_frames(const Rig& rig, const Clip& clip, const Shape* shape, DynSim& sim) {
    const int end = std::max(clip.end_frame, 0), fps = std::max(clip.fps, 1);
    const int sub = std::max(1, int(std::lround(double(DynSim::kStepsPerSecond) / fps)));
    const double dt = 1.0 / (double(fps) * sub);
    std::vector<Pose> frames(end + 1);
    auto record = [&](int f) {
        Evaluation e = evaluate(rig, clip, f, shape);
        frames[f] = e.pose;
        sim.apply(e.globals, frames[f]);
    };
    auto advance = [&](int f) {  // from frame f to f + 1
        for (int s = 1; s <= sub; ++s) sim.step(evaluate(rig, clip, f + double(s) / sub, shape).globals, dt);
    };
    sim.reset(evaluate(rig, clip, 0, shape).globals);
    record(0);
    const bool loop = clip.loop && clip.loop_out > clip.loop_in && clip.loop_out <= end;
    const int straight_to = loop ? clip.loop_in : end;
    for (int f = 0; f < straight_to; ++f) advance(f), record(f + 1);
    if (!loop) return frames;
    for (int pass = 0; pass < 2; ++pass)  // pre-roll: two cycles settle the loop (DY-2)
        for (int f = clip.loop_in; f < clip.loop_out; ++f) advance(f);
    record(clip.loop_in);
    for (int f = clip.loop_in; f < end; ++f) advance(f), record(f + 1);
    return frames;
}

void bake_samples(Clip& clip, const Skeleton& skel, const std::vector<int>& nodes, const std::vector<Pose>& frames,
                  double tol_deg, double tol_m, const std::vector<int>& with_position, bool position_only) {
    const int max_gap = std::max(clip.fps, 1) * 2;
    for (int n : nodes) {
        const std::string& track = skel[n].name;
        const bool rot = !skel[n].volume && !position_only;
        const bool pos = skel[n].volume || std::find(with_position.begin(), with_position.end(), n) != with_position.end();
        if (pos) {
            for (const char* ch : kPosChannels) clip.curves[track].erase(ch);
            std::vector<Vec3> s;
            for (const Pose& p : frames) s.push_back(p.offset[n]);
            for (int k : reduce_position_keys(s, tol_m, max_gap)) key_offset(clip, track, k, s[k]);
        }
        if (rot) {
            for (const char* ch : kRotChannels) clip.curves[track].erase(ch);
            std::vector<Quat> s;
            for (const Pose& p : frames) s.push_back(p.rot[n]);
            for (int k : reduce_rotation_keys(s, tol_deg, max_gap)) key_rotation(clip, track, k, s[k]);
        }
        for (auto& [ch, c] : clip.curves[track]) {
            const bool is_pos = ch.rfind("pos_", 0) == 0;
            if (is_pos ? !pos : !rot) continue;  // only the channels baked here
            for (Key& k : c.keys) k.interp = Interp::Linear;
            c.recompute_handles();
        }
    }
}

void bake_dynamics(Clip& clip, const Rig& rig, const Shape* shape, int which) {
    const Skeleton& skel = rig.skeleton();
    // Drive from the pre-bake tracks of the chains being baked.
    Clip drive = clip;
    std::vector<DynChain> chosen;
    std::vector<int> index;
    for (int i = 0; i < int(clip.dynamics.size()); ++i) {
        if (which >= 0 && i != which) continue;
        const DynChain& d = clip.dynamics[i];
        if (d.baked)
            for (int n : dyn_nodes(skel, d)) {
                auto it = d.source.find(skel[n].name);
                if (it == d.source.end()) drive.curves.erase(skel[n].name);
                else drive.curves[skel[n].name] = it->second;
            }
        chosen.push_back(d);
        index.push_back(i);
    }
    if (chosen.empty()) return;
    DynSim sim(skel, chosen);
    std::vector<Pose> frames = simulate_frames(rig, drive, shape, sim);
    for (int i : index) {
        DynChain& d = clip.dynamics[i];
        std::vector<int> nodes = dyn_nodes(skel, d);
        if (!d.baked) {
            d.source.clear();
            // From the pre-bake tracks: an overlapping chain baked earlier in this call already wrote clip.curves.
            for (int n : nodes)
                if (auto it = drive.curves.find(skel[n].name); it != drive.curves.end()) d.source[it->first] = it->second;
        }
        for (int n : nodes) {
            if (auto it = drive.curves.find(skel[n].name); it != drive.curves.end()) clip.curves[it->first] = it->second;
            else clip.curves.erase(skel[n].name);
        }
        bake_samples(clip, skel, nodes, frames);
        d.baked = true;
    }
}

void unbake_dynamics(Clip& clip, const Skeleton& skel, int which) {
    if (which < 0 || which >= int(clip.dynamics.size())) return;
    DynChain& d = clip.dynamics[which];
    if (!d.baked) return;
    for (int n : dyn_nodes(skel, d)) {
        auto it = d.source.find(skel[n].name);
        if (it == d.source.end()) clip.curves.erase(skel[n].name);
        else clip.curves[it->first] = it->second;
    }
    d.source.clear();
    d.baked = false;
}

}  // namespace vats
