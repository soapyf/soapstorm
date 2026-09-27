// Viewport Avatar Toolset - BVH export and import.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/bvh.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "vats/anim_convert.h"
#include "vats/rig.h"
#include "guard.h"

namespace vats {
namespace {

// BVH axes: X = SL Y, Y = SL Z, Z = SL X. The mapping is a proper rotation.
Vec3 sl_to_bvh(const Vec3& v) { return {v.y, v.z, v.x}; }
Vec3 bvh_to_sl(const Vec3& v) { return {v.z, v.x, v.y}; }
Quat sl_to_bvh(const Quat& q) { return {q.w, q.y, q.z, q.x}; }
Quat bvh_to_sl(const Quat& q) { return {q.w, q.z, q.x, q.y}; }

// Degrees (z, x, y) with R = Rz(z) * Rx(x) * Ry(y), for channels "Zrotation Xrotation Yrotation".
Vec3 decompose_zxy(const Quat& qin) {
    Quat q = qin.normalized();
    double r01 = 2 * (q.x * q.y - q.w * q.z);
    double r11 = 1 - 2 * (q.x * q.x + q.z * q.z);
    double r10 = 2 * (q.x * q.y + q.w * q.z);
    double r00 = 1 - 2 * (q.y * q.y + q.z * q.z);
    double r20 = 2 * (q.x * q.z - q.w * q.y);
    double r21 = 2 * (q.y * q.z + q.w * q.x);
    double r22 = 1 - 2 * (q.x * q.x + q.y * q.y);
    double sx = std::clamp(r21, -1.0, 1.0);
    if (std::fabs(sx) < 0.999999)
        return {std::atan2(-r01, r11) * kRadToDeg, std::asin(sx) * kRadToDeg, std::atan2(-r20, r22) * kRadToDeg};
    return {std::atan2(r10, r00) * kRadToDeg, std::asin(sx) * kRadToDeg, 0};
}

void appendf(std::string& s, const char* fmt, double a, double b, double c) {
    auto clean = [](double v) { return std::fabs(v) < 5e-7 ? 0.0 : v; };  // no "-0.000000"
    a = clean(a);
    b = clean(b);
    c = clean(c);
    char buf[128];
    std::snprintf(buf, sizeof buf, fmt, a, b, c);
    s += buf;
}

}  // namespace

BvhExportResult export_bvh(const Skeleton& skel, const Clip& clip, const BvhExportOptions& opt) {
    BvhExportResult res;
    const int jc = skel.joint_count();
    std::vector<char> animated(skel.size(), 0), has_pos(skel.size(), 0), include(jc, 0);
    for (auto& [name, track] : clip.curves) {
        int i = skel.find(name);
        if (i < 0) continue;
        bool any = false;
        for (auto& [ch, c] : track) any = any || !c.empty();
        animated[i] = any;
        has_pos[i] = clip.has_channels(name, kPosChannels);
    }
    Rig rig(skel);
    rig.external = opt.external;
    bool baked = !clip.pins.empty();
    for (auto& l : rig.limbs()) {
        if (!uses_ik(clip, l)) continue;
        baked = true;
        animated[l.root] = animated[l.mid] = 1;
        if (!l.spine) animated[l.end] = 1;
    }
    for (auto& p : clip.pins) {
        if (int l = pin_limb(rig, p); l >= 0) {  // held through the limb's IK: rotations only
            const LimbInfo& limb = rig.limbs()[l];
            animated[limb.root] = animated[limb.mid] = animated[limb.end] = 1;
            continue;
        }
        int v = skel.find(p.via);
        if (v >= 0) animated[v] = has_pos[v] = 1;
    }
    for (int i = 0; i < skel.size(); ++i) {
        if (!animated[i]) continue;
        if (skel[i].attachment) {
            res.lost.push_back("attachment point motion (" + skel[i].name + ")");
            continue;
        }
        if (has_pos[i] && i != 0 && !opt.joint_positions)
            res.lost.push_back("position keys on " + skel[i].name);
        for (int p = i; p >= 0; p = skel[p].parent) include[p] = 1;
    }
    include[0] = 1;
    if (opt.all_bones) std::fill(include.begin(), include.end(), char(1));
    if (!clip.joint_priority.empty()) res.lost.push_back("per-joint priorities");
    if (!clip.constraints.empty()) res.lost.push_back("constraints");
    if (!clip.orphans.empty()) res.lost.push_back("imported joints that are not in the skeleton");

    auto six = [&](int i) { return i == 0 || (opt.joint_positions && has_pos[i]); };
    std::vector<int> order;  // joints in the order their channels appear
    std::string& s = res.text;
    s = "HIERARCHY\n";
    std::function<void(int, int)> write = [&](int i, int depth) {
        std::string ind(depth, '\t');
        s += ind + (i == 0 ? "ROOT " : "JOINT ") + skel[i].name + "\n" + ind + "{\n";
        Vec3 off = sl_to_bvh(skel[i].pos) * kInchesPerMetre;
        appendf(s, (ind + "\tOFFSET %.6f %.6f %.6f\n").c_str(), off.x, off.y, off.z);
        s += ind + (six(i) ? "\tCHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation\n"
                           : "\tCHANNELS 3 Zrotation Xrotation Yrotation\n");
        order.push_back(i);
        bool leaf = true;
        for (int c : skel[i].children)
            if (c < jc && include[c]) {
                leaf = false;
                write(c, depth + 1);
            }
        if (leaf) {
            Vec3 e = sl_to_bvh(skel[i].end) * kInchesPerMetre;
            s += ind + "\tEnd Site\n" + ind + "\t{\n";
            appendf(s, (ind + "\t\tOFFSET %.6f %.6f %.6f\n").c_str(), e.x, e.y, e.z);
            s += ind + "\t}\n";
        }
        s += ind + "}\n";
    };
    write(0, 0);

    const int last = std::max(clip.end_frame, 1);
    const int fps = std::clamp(clip.fps, 1, 120);
    char buf[64];
    std::snprintf(buf, sizeof buf, "MOTION\nFrames: %d\nFrame Time: %.6f\n", last + 2, 1.0 / fps);
    s += buf;

    // Frame 0 is the reference: rest position, zero rotation. Then clip frames 0..last.
    for (int fr = -1; fr <= last; ++fr) {
        Pose pose = fr < 0 ? Pose(skel.size())
                    : baked ? evaluate(rig, clip, fr, opt.shape).pose
                            : evaluate_curves(skel, clip, fr);
        std::string line;
        for (int i : order) {
            if (six(i)) {
                Vec3 p = sl_to_bvh(skel[i].pos + pose.offset[i]) * kInchesPerMetre;
                appendf(line, "%.6f %.6f %.6f ", p.x, p.y, p.z);
            }
            Vec3 r = decompose_zxy(sl_to_bvh(pose.rot[i]));
            appendf(line, "%.6f %.6f %.6f ", r.x, r.y, r.z);
        }
        line.back() = '\n';
        s += line;
    }
    return res;
}

static BvhImportResult import_bvh_text(const Skeleton& skel, std::string_view text, const BvhImportOptions& opt) {
    BvhImportResult res;
    std::vector<std::string> tok;
    for (size_t i = 0; i < text.size();) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        size_t b = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i > b) tok.emplace_back(text.substr(b, i - b));
    }
    size_t t = 0;
    auto fail = [&](const std::string& why) {
        res.ok = false;
        res.error = why;
        return res;
    };
    auto next = [&]() -> const std::string& {
        static const std::string empty;
        return t < tok.size() ? tok[t++] : empty;
    };
    auto number = [&](double& v) {
        const std::string& s = next();
        char* end = nullptr;
        v = std::strtod(s.c_str(), &end);
        return !s.empty() && end && *end == 0 && std::isfinite(v);
    };

    struct J {
        std::string name;
        int node = -1;
        std::vector<std::string> channels;
    };
    std::vector<J> joints;
    if (next() != "HIERARCHY") return fail("not a BVH file (no HIERARCHY)");

    // Parses ROOT/JOINT blocks; "End Site" blocks carry no channels.
    std::function<bool(bool, int)> block = [&](bool end_site, int depth) -> bool {
        if (depth > 256 || next() != "{") return false;
        J j;
        if (!end_site) j.name = tok[t - 2];
        size_t me = joints.size();
        if (!end_site) joints.push_back(j);
        for (;;) {
            if (t >= tok.size()) return false;
            const std::string& k = next();
            if (k == "}") return true;
            if (k == "OFFSET") {
                double d;
                for (int a = 0; a < 3; ++a)
                    if (!number(d)) return false;
            } else if (k == "CHANNELS" && !end_site) {
                double n;
                if (!number(n) || n < 0 || n > 64) return false;
                for (int c = 0; c < int(n); ++c) joints[me].channels.push_back(next());
            } else if (k == "JOINT") {
                next();
                if (!block(false, depth + 1)) return false;
            } else if (k == "End") {
                if (next() != "Site" || !block(true, depth + 1)) return false;
            } else {
                return false;
            }
        }
    };
    if (next() != "ROOT") return fail("no ROOT joint");
    next();
    if (!block(false, 0)) return fail("malformed HIERARCHY");
    if (next() != "MOTION" || next() != "Frames:") return fail("malformed MOTION header");
    double nframes, frame_time;
    if (!number(nframes) || nframes < 1 || nframes > 1e6) return fail("bad frame count");
    if (next() != "Frame" || next() != "Time:" || !number(frame_time) || frame_time <= 0)
        return fail("bad frame time");

    size_t per_frame = 0;
    for (auto& j : joints) per_frame += j.channels.size();
    const int N = static_cast<int>(nframes);
    if (per_frame == 0) return fail("the file has no motion channels");
    if (tok.size() - t < per_frame * size_t(N)) return fail("not enough motion data");
    if (double(N) * double(joints.size()) > kMaxSamples) return fail("the animation is too large");

    for (auto& j : joints) {
        j.node = skel.find_viewer(j.name);
        if (j.node < 0) {
            j.node = skel.find(j.name);
            if (j.node >= 0) res.report.push_back("\"" + j.name + "\" matched " + skel[j.node].name + " ignoring case");
            else res.report.push_back("\"" + j.name + "\" is not an SL joint; skipped");
        }
    }
    if (joints.empty() || joints[0].node != 0) return fail("the root joint is not the hip (mPelvis)");

    Clip& clip = res.clip;
    clip.fps = std::clamp(static_cast<int>(std::lround(1.0 / frame_time)), 1, 120);
    clip.end_frame = std::max(N - 2, 1);
    clip.loop_out = clip.end_frame;
    clip.priority = 2;
    clip.ease_in = clip.ease_out = 0.3;

    // Read all values: per frame, per joint: rotation (BVH axes) and optional position (inches).
    struct Sample {
        Quat rot;
        Vec3 pos;
        bool has_pos = false;
    };
    std::vector<std::vector<Sample>> data(N, std::vector<Sample>(joints.size()));
    for (int f = 0; f < N; ++f)
        for (size_t ji = 0; ji < joints.size(); ++ji) {
            Sample& smp = data[f][ji];
            for (auto& ch : joints[ji].channels) {
                double v = std::strtod(tok[t++].c_str(), nullptr);
                if (!std::isfinite(v)) v = 0;
                if (ch.size() == 9 && ch.substr(1) == "position") {
                    smp.pos[ch[0] - 'X'] = v;
                    smp.has_pos = true;
                } else if (ch.size() == 9 && ch.substr(1) == "rotation") {
                    static const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
                    int a = ch[0] - 'X';
                    if (a >= 0 && a < 3) smp.rot = smp.rot * Quat::axis_angle(axes[a], v * kDegToRad);
                }
            }
        }

    const bool has_ref = N > 1;
    const int first = has_ref ? 1 : 0;
    for (size_t ji = 0; ji < joints.size(); ++ji) {
        int node = joints[ji].node;
        if (node < 0) continue;
        Track& track = clip.curves[skel[node].name];
        Quat ref_inv = has_ref && node == 0 ? data[0][ji].rot.conj() : Quat{};
        Vec3 ref_pos = has_ref && node == 0 ? data[0][ji].pos : Vec3{};
        Vec3 prev;
        std::vector<Quat> rots;
        std::vector<Vec3> offsets;
        for (int f = first; f < N; ++f) {
            const Sample& smp = data[f][ji];
            double fr = f - first;
            Quat q = bvh_to_sl(node == 0 ? ref_inv * smp.rot : smp.rot);
            rots.push_back(skel[node].rest.conj() * q);
            prev = nearest_euler(rots.back(), prev);
            for (int a = 0; a < 3; ++a) {
                Key k;
                k.frame = fr;
                k.value = prev[a];
                k.interp = Interp::Linear;
                track[kRotChannels[a]].keys.push_back(k);
            }
            if (!smp.has_pos) continue;
            Vec3 p;
            if (node == 0 && has_ref) {
                p = bvh_to_sl(ref_inv.rotate(smp.pos - ref_pos)) * kMetresPerInch;  // relative to frame 0
            } else {
                p = bvh_to_sl(smp.pos) * kMetresPerInch - skel[node].pos;
            }
            offsets.push_back(p);
            for (int a = 0; a < 3; ++a) {
                Key k;
                k.frame = fr;
                k.value = p[a];
                k.interp = Interp::Linear;
                track[kPosChannels[a]].keys.push_back(k);
            }
        }
        // Keys sit at index = frame, so keep the reducer's indices (IO-35).
        auto keep_only = [&](const char* const* channels, const std::vector<int>& kept) {
            for (int a = 0; a < 3; ++a) {
                auto it = track.find(channels[a]);
                if (it == track.end()) continue;
                std::vector<Key> out;
                for (int i : kept) out.push_back(it->second.keys[i]);
                it->second.keys = std::move(out);
            }
        };
        if (opt.reduce_rot_deg > 0 && rots.size() > 2)
            keep_only(kRotChannels, reduce_rotation_keys(rots, opt.reduce_rot_deg, 60));
        if (opt.reduce_pos_m > 0 && offsets.size() == rots.size() && offsets.size() > 2)
            keep_only(kPosChannels, reduce_position_keys(offsets, opt.reduce_pos_m, 60));
        for (auto& [ch, c] : track) c.recompute_handles();
    }
    res.ok = true;
    return res;
}

BvhImportResult import_bvh(const Skeleton& skel, std::string_view text, const BvhImportOptions& opt) {
    BvhImportResult res;
    std::string err;
    if (!guarded(err, [&] {
            res = import_bvh_text(skel, text, opt);
            return true;
        })) {
        res = BvhImportResult{};
        res.error = err;
    }
    return res;
}

}  // namespace vats
