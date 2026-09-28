// Viewport Avatar Toolset - the .anim as Second Life plays it, and what its bytes go on.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/sl_preview.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "vats/json.h"
#include "vats/pose_ops.h"

namespace vats {
namespace {

using Keys = std::vector<std::array<std::uint16_t, 4>>;

// The value of a key list at time t: the bracketing keys mixed, the first or last key outside them.
template <class T, class Decode, class Mix>
T sample(const Keys& keys, float duration, double t, Decode decode, Mix mix) {
    auto time_of = [&](const std::array<std::uint16_t, 4>& k) { return double(u16_to_f32(k[0], 0.f, duration)); };
    auto after = std::upper_bound(keys.begin(), keys.end(), t, [&](double v, const auto& k) { return v < time_of(k); });
    if (after == keys.begin()) return decode(keys.front());
    if (after == keys.end()) return decode(keys.back());
    const auto& a = *(after - 1);
    const double ta = time_of(a), tb = time_of(*after);
    return mix(decode(a), decode(*after), tb > ta ? (t - ta) / (tb - ta) : 1.0);
}

// FNV-1a over the fields that reach the .anim.
struct Hasher {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(p)[i]) * 1099511628211ull;
    }
    template <class T>
    void pod(T v) { bytes(&v, sizeof v); }
    void str(const std::string& s) { pod(s.size()), bytes(s.data(), s.size()); }
    void vec(const Vec3& v) { pod(v.x), pod(v.y), pod(v.z); }
    void quat(const Quat& q) { pod(q.w), pod(q.x), pod(q.y), pod(q.z); }
    void shape(const Shape* s) {
        pod(s != nullptr);
        if (!s) return;
        for (const Vec3& v : s->scale) vec(v);
        for (const Vec3& v : s->offset) vec(v);
    }
};

}  // namespace

Pose anim_pose(const Skeleton& skel, const AnimFile& f, double seconds, const Shape* positions) {
    Pose pose(skel.size());
    if (f.legacy()) return pose;  // ponytail: v0.1 files play at rest here; export only writes v1.0
    for (const AnimJoint& j : f.joints) {
        const int i = skel.find_viewer(j.name);
        if (i < 0) continue;
        const Node& node = skel[i];
        if (!j.rot.empty()) {
            const Quat q = sample<Quat>(j.rot, f.duration, seconds, decode_rotation,
                                        [](const Quat& a, const Quat& b, double u) { return nlerp(a, b, u); });
            pose.rot[i] = (node.rest.conj() * q).normalized();
        }
        if (!j.pos.empty()) {
            Vec3 p = sample<Vec3>(j.pos, f.duration, seconds, decode_position,
                                  [](const Vec3& a, const Vec3& b, double u) { return a + (b - a) * u; });
            if (i != 0) {  // the file holds absolute local positions except for the pelvis (export_anim)
                p = p - node.pos;
                if (positions && i < int(positions->offset.size())) p = p - positions->offset[i];
            }
            pose.offset[i] = p;
        }
    }
    return pose;
}

std::vector<BoneDeviation> anim_deviation(const Rig& rig, const Clip& clip, const AnimFile& f, const Shape* shape,
                                          const Shape* positions) {
    const Skeleton& skel = rig.skeleton();
    const int fps = std::clamp(clip.fps, 1, 120), last = std::max(clip.end_frame, 1);
    std::vector<BoneDeviation> out(skel.volume_start());
    for (int i = 0; i < int(out.size()); ++i) out[i].node = i;
    for (int fr = 0; fr <= last; ++fr) {
        const std::vector<Xform> a = evaluate(rig, clip, fr, shape).globals;
        const std::vector<Xform> b = skel.global_pose(anim_pose(skel, f, double(fr) / fps, positions), shape);
        for (BoneDeviation& d : out) {
            const double mm = (a[d.node].pos - b[d.node].pos).length() * 1000;
            const double deg = (a[d.node].rot.conj() * b[d.node].rot).angle() * kRadToDeg;
            if (mm > d.mm) d.mm = mm, d.frame_mm = fr;
            if (deg > d.deg) d.deg = deg, d.frame_deg = fr;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const BoneDeviation& x, const BoneDeviation& y) {
        return x.mm != y.mm ? x.mm > y.mm : x.deg > y.deg;
    });
    return out;
}

AnimCost anim_cost(const Skeleton& skel, const AnimFile& f) {
    AnimCost c;
    c.total = write_anim(f).size();
    const std::size_t per_key = f.legacy() ? 16 : 8;  // time + x, y, z: four u16, or four floats in v0.1
    std::map<std::string, AnimCost::Part> parts;
    for (const AnimJoint& j : f.joints) {
        const std::size_t record = j.name.size() + 1 + 3 * 4;  // name, priority, the two key counts
        const std::size_t rot = per_key * (f.legacy() ? j.rot_legacy.size() : j.rot.size());
        const std::size_t pos = per_key * (f.legacy() ? j.pos_legacy.size() : j.pos.size());
        const int i = skel.find_viewer(j.name);
        const std::string name = i < 0                                  ? "Other"
                                 : skel[i].attachment                   ? "Attachment points"
                                 : skel[i].category == Category::Face ? "Face"
                                                                        : body_part_of(skel, i).label;
        AnimCost::Part& p = parts[name];
        p.name = name;
        p.bytes += record + rot + pos, p.rot += rot, p.pos += pos, ++p.joints;
        c.records += record, c.rot += rot, c.pos += pos;
    }
    c.header = c.total - c.records - c.rot - c.pos;
    for (auto& [name, p] : parts) c.parts.push_back(p);
    std::stable_sort(c.parts.begin(), c.parts.end(), [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
    return c;
}

BudgetFit fit_anim_budget(const Rig& rig, const Clip& clip, AnimExportOptions opt) {
    BudgetFit fit;
    const Skeleton& skel = rig.skeleton();
    if (double(std::max(clip.end_frame, 1)) / std::clamp(clip.fps, 1, 120) > kAnimMaxDuration) {
        fit.too_long = true;
        return fit;
    }
    constexpr double kMaxRot = 5, kMaxPos = 0.05;  // the Reduce keys fields' upper ends
    AnimExportResult r = export_anim(skel, clip, opt);
    fit.bytes = write_anim(r.file).size();
    while (fit.bytes >= kAnimMaxUploadBytes && (opt.reduce_rot_deg < kMaxRot || opt.reduce_pos_m < kMaxPos)) {
        opt.reduce_rot_deg = std::min(kMaxRot, std::max(opt.reduce_rot_deg, 0.05) * 1.5);
        opt.reduce_pos_m = std::min(kMaxPos, std::max(opt.reduce_pos_m, 0.0005) * 1.5);
        ++fit.steps;
        r = export_anim(skel, clip, opt);
        fit.bytes = write_anim(r.file).size();
    }
    fit.rot_deg = opt.reduce_rot_deg, fit.pos_m = opt.reduce_pos_m;
    fit.fits = fit.bytes < kAnimMaxUploadBytes && !r.file.joints.empty();
    if (!fit.fits) return fit;
    for (const BoneDeviation& d : anim_deviation(rig, clip, r.file, opt.shape, opt.positions)) {
        if (d.mm > fit.max_mm) fit.max_mm = d.mm, fit.worst_mm = skel[d.node].name;
        if (d.deg > fit.max_deg) fit.max_deg = d.deg, fit.worst_deg = skel[d.node].name;
    }
    return fit;
}

std::uint64_t anim_hash(const Clip& c, const Shape* shape, const Shape* positions) {
    Hasher h;
    h.pod(c.fps), h.pod(c.end_frame), h.pod(c.loop), h.pod(c.loop_in), h.pod(c.loop_out), h.pod(c.priority);
    h.pod(c.ease_in), h.pod(c.ease_out), h.pod(c.hand_pose), h.str(c.emote), h.pod(int(c.ik_solve));
    h.pod(c.mirror_export);
    h.str(write_json(c.export_settings));
    for (const auto& [name, track] : c.curves) {
        h.str(name);
        for (const auto& [ch, curve] : track) {
            h.str(ch);
            h.pod(curve.keys.size());
            for (const Key& k : curve.keys) {
                h.pod(k.frame), h.pod(k.value), h.pod(k.interp), h.pod(k.left), h.pod(k.right);
                h.pod(k.lx), h.pod(k.ly), h.pod(k.rx), h.pod(k.ry);
            }
        }
    }
    for (const auto& [name, p] : c.joint_priority) h.str(name), h.pod(p);
    for (const AnimConstraint& k : c.constraints) h.bytes(k.data(), k.size());
    for (const OrphanJoint& o : c.orphans) {
        h.str(o.name), h.pod(o.priority);
        for (const auto& [t, q] : o.rot) h.pod(t), h.quat(q);
        for (const auto& [t, p] : o.pos) h.pod(t), h.vec(p);
    }
    for (const Pin& p : c.pins) {
        h.str(p.joint), h.str(p.via), h.str(p.target), h.str(p.target_actor);
        h.pod(p.from), h.pod(p.to), h.vec(p.pos), h.quat(p.rot), h.pod(p.start_key), h.pod(p.release_key);
    }
    h.shape(shape);
    h.shape(positions);
    return h.h;
}

}  // namespace vats
