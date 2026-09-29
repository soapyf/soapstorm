// Viewport Avatar Toolset - converting between clips and .anim files.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/anim_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vats/deformer.h"
#include "vats/rig.h"
#include "vats/world_reduce.h"

namespace vats {
namespace {

template <class ErrFn>
std::vector<int> reduce(int n, double tol, int max_gap, const std::vector<char>& anchors, ErrFn err) {
    std::vector<int> keep;
    if (n <= 0) return keep;
    keep.push_back(0);
    if (tol <= 0) {
        for (int i = 1; i < n; ++i) keep.push_back(i);
        return keep;
    }
    int gap = max_gap > 0 ? max_gap : n;
    auto segment_ok = [&](int a, int b) {
        for (int k = a + 1; k < b; ++k)
            if (err(a, b, k) > tol) return false;
        return true;
    };
    for (int a = 0; a < n - 1;) {
        int b = a + 1;
        auto anchored = [&](int f) { return f < static_cast<int>(anchors.size()) && anchors[f]; };
        while (b + 1 < n && !anchored(b) && b + 1 - a <= gap && segment_ok(a, b + 1)) ++b;
        keep.push_back(b);
        a = b;
    }
    return keep;
}

std::uint16_t time_code(int frame, int fps, float duration) {
    float t = static_cast<float>(frame) / static_cast<float>(fps);
    return f32_to_u16(t, 0.f, duration);
}

// Legacy v0.1 rotation keys: mayaQ(x, y, z, ZYX), i.e. Hamilton qx * qy * qz.
Quat legacy_rotation(const std::array<float, 4>& k) {
    return (Quat::axis_angle({1, 0, 0}, k[1] * kDegToRad) * Quat::axis_angle({0, 1, 0}, k[2] * kDegToRad) *
            Quat::axis_angle({0, 0, 1}, k[3] * kDegToRad))
        .normalized();
}

void add_linear_key(FCurve& c, double frame, double value) {
    Key k;
    k.frame = frame;
    k.value = value;
    k.interp = Interp::Linear;
    c.keys.push_back(k);
}

// Sorts keys by frame; of keys on the same frame the last one read wins, as in the viewer.
void finish_curve(FCurve& c) {
    std::stable_sort(c.keys.begin(), c.keys.end(), [](const Key& a, const Key& b) { return a.frame < b.frame; });
    std::vector<Key> out;
    for (auto& k : c.keys) {
        if (!out.empty() && same_frame(out.back().frame, k.frame))
            out.back() = k;
        else
            out.push_back(k);
    }
    c.keys = std::move(out);
    c.recompute_handles();
}

// Frames in [0, n) where any of the track's given channels has a key.
std::vector<char> key_frames(const Track& track, const char* const (&channels)[3], int n) {
    std::vector<char> at(n, 0);
    for (const char* ch : channels) {
        auto c = track.find(ch);
        if (c == track.end()) continue;
        for (auto& k : c->second.keys) {
            long f = std::lround(k.frame);
            if (f >= 0 && f < n) at[f] = 1;
        }
    }
    return at;
}

}  // namespace

std::vector<int> reduce_rotation_keys(const std::vector<Quat>& s, double tol_deg, int max_gap,
                                      const std::vector<char>& anchors) {
    return reduce(static_cast<int>(s.size()), tol_deg, max_gap, anchors, [&](int a, int b, int k) {
        Quat q = nlerp(s[a], s[b], double(k - a) / double(b - a));
        return 2.0 * std::acos(std::min(1.0, std::fabs(q.dot(s[k])))) * kRadToDeg;
    });
}

std::vector<int> reduce_position_keys(const std::vector<Vec3>& s, double tol_m, int max_gap,
                                      const std::vector<char>& anchors) {
    return reduce(static_cast<int>(s.size()), tol_m, max_gap, anchors, [&](int a, int b, int k) {
        double t = double(k - a) / double(b - a);
        return (s[a] + (s[b] - s[a]) * t - s[k]).length();
    });
}

bool static_position(const std::vector<Pose>& frames, int node, double tol) {
    for (const Pose& p : frames)
        if (p.offset[node].length() > tol) return false;
    return true;
}

bool static_rotation(const std::vector<Pose>& frames, int node, double tol_deg) {
    for (const Pose& p : frames)
        if (p.rot[node].angle() > tol_deg * kDegToRad) return false;
    return true;
}

AnimExportResult export_anim(const Skeleton& skel, const Clip& clip, const AnimExportOptions& opt) {
    if (opt.end_at_rest || opt.hold_without_sinking) {  // the deformer tool: on a copy, then as usual
        AnimExportOptions plain = opt;
        plain.end_at_rest = plain.hold_without_sinking = false;
        std::vector<std::string> notes;
        const Clip c = with_deformer_options(skel, clip, opt, &notes);
        AnimExportResult r = export_anim(skel, c, plain);
        r.warnings.insert(r.warnings.end(), notes.begin(), notes.end());
        return r;
    }
    AnimExportResult res;
    AnimFile& f = res.file;
    int fps = std::clamp(clip.fps, 1, 120);
    int last = std::max(clip.end_frame, 1);
    f.duration = static_cast<float>(last) / static_cast<float>(fps);
    if (!(f.duration <= kAnimMaxDuration)) {
        res.errors.push_back("the animation is longer than 60 seconds; SL will not play it");
        return res;  // and do not sample an arbitrarily long clip
    }

    f.base_priority = clip.priority;
    f.emote = clip.emote;
    if (!f.emote.empty() && !is_known_emote(f.emote))
        res.warnings.push_back("SL does not know the expression \"" + f.emote + "\" and will ignore it");
    if (clip.hand_pose < 0 || clip.hand_pose > 13) res.errors.push_back("hand pose must be 0-13");
    f.hand_pose = static_cast<std::uint32_t>(std::clamp(clip.hand_pose, 0, 13));
    f.loop = clip.loop ? 1 : 0;
    if (clip.loop) {
        f.loop_in = std::clamp(static_cast<float>(clip.loop_in) / fps, 0.f, f.duration);
        f.loop_out = std::clamp(static_cast<float>(clip.loop_out) / fps, 0.f, f.duration);
        if (clip.loop_in > clip.loop_out) res.warnings.push_back("loop in is after loop out");
    } else {
        f.loop_in = 0;
        f.loop_out = f.duration;
        if (clip.ease_in + clip.ease_out > f.duration && f.duration > 0) {
            // D9: the viewer's BVH upload scales both down to fit; a .anim keeps them as written.
            double k = f.duration / (clip.ease_in + clip.ease_out);
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "ease in + ease out is longer than the animation (a BVH upload would use %.2f s and %.2f s)",
                          clip.ease_in * k, clip.ease_out * k);
            res.warnings.push_back(buf);
        }
    }
    f.ease_in = static_cast<float>(clip.ease_in);
    f.ease_out = static_cast<float>(clip.ease_out);

    // Which nodes have keyed rotation / position channels; turns = needs a record even without positions.
    std::vector<char> animated(skel.size(), 0), has_pos(skel.size(), 0), turns(skel.size(), 0);
    std::vector<const Track*> tracks(skel.size(), nullptr);
    for (auto& [name, track] : clip.curves) {
        if (name.rfind("ik.", 0) == 0 || name.rfind("pin:", 0) == 0) continue;  // baked in a later stage
        int i = skel.find(name);
        if (i < 0) {
            res.warnings.push_back("track \"" + name + "\" matches no bone and is not exported");
            continue;
        }
        bool any = false;
        for (auto& [ch, curve] : track) {
            any = any || !curve.empty();
            if (!curve.empty() && std::find(std::begin(kPosChannels), std::end(kPosChannels), ch) == std::end(kPosChannels))
                turns[i] = 1;
        }
        animated[i] = animated[i] || any;
        if (any) tracks[i] = &track;
        has_pos[i] = has_pos[i] || clip.has_channels(name, kPosChannels);
    }

    // Bones driven by a limb that uses IK, and pin via bones, are baked from the full evaluation.
    Rig rig(skel);
    rig.external = opt.external;
    bool baked = !clip.pins.empty();
    for (auto& l : rig.limbs()) {
        if (!uses_ik(clip, l)) continue;
        baked = true;
        animated[l.root] = animated[l.mid] = turns[l.root] = turns[l.mid] = 1;
        if (!l.spine) animated[l.end] = turns[l.end] = 1;
    }
    for (auto& p : clip.pins) {
        if (int l = pin_limb(rig, p); l >= 0) {  // held through the limb's IK: rotations only
            const LimbInfo& limb = rig.limbs()[l];
            animated[limb.root] = animated[limb.mid] = animated[limb.end] = 1;
            turns[limb.root] = turns[limb.mid] = turns[limb.end] = 1;
            continue;
        }
        int v = skel.find(p.via);
        if (v >= 0) animated[v] = has_pos[v] = turns[v] = 1;
    }

    // Sample every integer frame once.
    const int n = last + 1;
    std::vector<Pose> frames;
    frames.reserve(n);
    for (int fr = 0; fr < n; ++fr)
        frames.push_back(baked ? evaluate(rig, clip, fr, opt.shape).pose : evaluate_curves(skel, clip, fr));

    // IO-11a: position channels that move nothing are left out (the pelvis keeps its own); a joint keyed only
    // by them gets no record at all.
    for (int i = 1; i < skel.size(); ++i) {
        if (!has_pos[i] || !static_position(frames, i, opt.reduce_pos_m)) continue;
        has_pos[i] = 0;
        ++res.static_positions;
        if (!turns[i]) animated[i] = 0;
    }
    // IO-11b, when asked: rotations that never leave rest are left out too (the pelvis keeps its own).
    std::vector<char> rot_out(skel.size(), 1);
    if (opt.leave_out_static_rotations)
        for (int i = 1; i < skel.size(); ++i) {
            if (!animated[i] || !static_rotation(frames, i, opt.reduce_rot_deg)) continue;
            rot_out[i] = 0;
            ++res.static_rotations;
            if (!has_pos[i]) animated[i] = 0;
        }

    // IO-14w: world-space reduction. Each frame's global pose is computed once; the reach and the budgets come from it.
    const bool world = opt.reduce_world_m > 0;
    std::vector<std::vector<double>> reach;
    std::vector<double> budget;
    if (world) {
        std::vector<std::vector<Xform>> globals(n);
        for (int fr = 0; fr < n; ++fr) globals[fr] = skel.global_pose(frames[fr], opt.shape);
        reach = world_reach(skel, globals);
        std::vector<char> rot_keyed(skel.size(), 0), pos_keyed(skel.size(), 0);
        for (int i = 0; i < skel.size(); ++i)
            rot_keyed[i] = animated[i] && rot_out[i], pos_keyed[i] = animated[i] && has_pos[i];
        budget = world_budgets(skel, rot_keyed, pos_keyed, opt.reduce_world_m);
    }

    bool clamped = false;
    for (int i = 0; i < skel.size(); ++i) {
        if (!animated[i]) continue;
        const Node& node = skel[i];
        AnimJoint j;
        j.name = node.name;
        auto pr = clip.joint_priority.find(node.name);
        j.priority = pr != clip.joint_priority.end() ? pr->second : clip.priority;

        std::vector<Quat> rot(n);
        for (int fr = 0; fr < n; ++fr) rot[fr] = (node.rest * frames[fr].rot[i]).normalized();
        static const Track kNoTrack;
        const Track& track = tracks[i] ? *tracks[i] : kNoTrack;
        std::vector<int> rot_keys;
        if (rot_out[i]) {
            const std::vector<char> anchors = key_frames(track, kRotChannels, n);
            rot_keys = world ? reduce_rotation_keys_world(rot, reach[i], budget[i], opt.max_gap, anchors)
                             : reduce_rotation_keys(rot, opt.reduce_rot_deg, opt.max_gap, anchors);
        }
        for (int k : rot_keys) {
            auto c = encode_rotation(rot[k]);
            j.rot.push_back({time_code(k, fps, f.duration), c[0], c[1], c[2]});
        }
        if (has_pos[i]) {
            Vec3 base = node.pos;
            if (opt.positions && i < int(opt.positions->offset.size())) base += opt.positions->offset[i];
            std::vector<Vec3> pos(n);
            for (int fr = 0; fr < n; ++fr) {
                Vec3 p = frames[fr].offset[i] + (i == 0 ? Vec3{} : base);  // pelvis: offset only
                for (int a = 0; a < 3; ++a) {
                    if (std::fabs(p[a]) > kAnimMaxOffset) clamped = true;
                    p[a] = std::clamp(p[a], -double(kAnimMaxOffset), double(kAnimMaxOffset));
                }
                pos[fr] = p;
            }
            const std::vector<char> anchors = key_frames(track, kPosChannels, n);
            std::vector<int> pos_keys;
            if (world) {
                const int p = node.parent;
                const Vec3 s = opt.shape && p >= 0 && p < int(opt.shape->scale.size()) ? opt.shape->scale[p] : Vec3{1, 1, 1};
                const double scale = std::max({std::fabs(s.x), std::fabs(s.y), std::fabs(s.z)});
                pos_keys = reduce_position_keys_world(pos, scale, budget[i], opt.max_gap, anchors);
            } else {
                pos_keys = reduce_position_keys(pos, opt.reduce_pos_m, opt.max_gap, anchors);
            }
            for (int k : pos_keys) {
                auto c = encode_position(pos[k]);
                j.pos.push_back({time_code(k, fps, f.duration), c[0], c[1], c[2]});
            }
        }
        f.joints.push_back(std::move(j));
    }
    if (clamped) res.warnings.push_back("some positions are further than 5 m and were clamped");
    if (!opt.positions && !opt.worn_overrides.empty()) {
        int pinned = 0;
        for (const AnimJoint& j : f.joints) {
            const int i = skel.find(j.name);
            pinned += i >= 0 && skel[i].category == Category::Face && !j.pos.empty() &&
                      std::find(opt.worn_overrides.begin(), opt.worn_overrides.end(), skel[i].name) != opt.worn_overrides.end();
        }
        if (pinned)
            res.warnings.push_back(std::to_string(pinned) + " face bones carry position keys while a mesh head is worn; "
                                   "they will pull it towards the default face (set Bake shape to Your avatar)");
    }

    for (auto& o : clip.orphans) {
        AnimJoint j;
        j.name = o.name;
        j.priority = o.priority;
        bool dropped = false;
        for (auto& [t, q] : o.rot) {
            if (t > f.duration) { dropped = true; continue; }
            auto c = encode_rotation(q);
            j.rot.push_back({f32_to_u16(static_cast<float>(t), 0.f, f.duration), c[0], c[1], c[2]});
        }
        for (auto& [t, p] : o.pos) {
            if (t > f.duration) { dropped = true; continue; }
            auto c = encode_position(p);
            j.pos.push_back({f32_to_u16(static_cast<float>(t), 0.f, f.duration), c[0], c[1], c[2]});
        }
        if (dropped) res.warnings.push_back("keys of \"" + o.name + "\" after the end were dropped");
        f.joints.push_back(std::move(j));
    }

    f.constraints = clip.constraints;
    f.num_constraints = static_cast<std::int32_t>(f.constraints.size());

    if (f.joints.empty()) res.errors.push_back("nothing is keyed");
    std::size_t bytes = write_anim(f).size();
    if (bytes >= kAnimMaxUploadBytes)
        res.errors.push_back("the file is " + std::to_string(bytes) + " bytes; SL accepts animations under " +
                             std::to_string(kAnimMaxUploadBytes) + " bytes (use fewer keys or bones)");
    auto structural = validate_anim(f, skel, false);
    res.errors.insert(res.errors.end(), structural.begin(), structural.end());
    for (auto& s : validate_anim(f, skel, true))
        if (std::find(structural.begin(), structural.end(), s) == structural.end())
            res.warnings.push_back("SL's upload will refuse this file: " + s);
    return res;
}

AnimImportResult import_anim(const Skeleton& skel, const AnimFile& file, int fps_override) {
    AnimImportResult res;
    Clip& clip = res.clip;
    const bool legacy = file.legacy();
    if (legacy) res.report.push_back("old v0.1 .anim format");

    // Gather every key time in seconds.
    auto rot_time = [&](const AnimJoint& j, size_t k) {
        return legacy ? double(j.rot_legacy[k][0]) : double(u16_to_f32(j.rot[k][0], 0.f, file.duration));
    };
    auto pos_time = [&](const AnimJoint& j, size_t k) {
        return legacy ? double(j.pos_legacy[k][0]) : double(u16_to_f32(j.pos[k][0], 0.f, file.duration));
    };
    std::vector<double> times;
    for (auto& j : file.joints) {
        size_t nr = legacy ? j.rot_legacy.size() : j.rot.size();
        size_t np = legacy ? j.pos_legacy.size() : j.pos.size();
        for (size_t k = 0; k < nr; ++k) times.push_back(rot_time(j, k));
        for (size_t k = 0; k < np; ++k) times.push_back(pos_time(j, k));
    }

    // Guess the frame rate: the first rate on which every key lands on a whole frame.
    int fps = fps_override;
    bool snapped = true;
    if (fps <= 0) {
        std::vector<int> candidates = {30, 24, 25, 60, 50, 48, 15, 12, 10};
        for (int r = 1; r <= 120; ++r)
            if (std::find(candidates.begin(), candidates.end(), r) == candidates.end()) candidates.push_back(r);
        double lsb = legacy ? 1e-4 : double(file.duration) / 65535.0;
        fps = 0;
        for (int r : candidates) {
            double tol = lsb * r + 1e-6;  // the time code is floored: up to one LSB early
            bool fits = true;
            for (double t : times)
                if (std::fabs(t * r - std::round(t * r)) > tol) { fits = false; break; }
            if (fits) { fps = r; break; }
        }
        if (fps == 0) {
            fps = 30;
            snapped = false;
            res.report.push_back("key times fit no frame rate; imported at 30 fps on fractional frames");
        }
    }
    auto to_frame = [&](double t) { return snapped ? std::round(t * fps) : t * fps; };

    // A damaged file can claim any duration; keep the clip within the editor's 0-3600 frames.
    double duration = std::isfinite(file.duration) ? std::clamp(double(file.duration), 0.0, 3600.0 / fps) : 0.0;
    clip.fps = fps;
    clip.end_frame = std::max(1, static_cast<int>(std::lround(duration * fps)));
    clip.loop = file.loop != 0;
    auto frame_of = [&](float t) {
        return std::isfinite(t) ? static_cast<int>(std::lround(std::clamp(double(t), 0.0, duration) * fps)) : 0;
    };
    clip.loop_in = frame_of(file.loop_in);
    clip.loop_out = frame_of(file.loop_out);
    if (!clip.loop) {
        clip.loop_in = 0;
        clip.loop_out = clip.end_frame;
    }
    clip.priority = std::clamp(file.base_priority, 0, 6);
    clip.ease_in = std::isfinite(file.ease_in) ? std::clamp(double(file.ease_in), 0.0, 10.0) : 0.0;
    clip.ease_out = std::isfinite(file.ease_out) ? std::clamp(double(file.ease_out), 0.0, 10.0) : 0.0;
    clip.hand_pose = static_cast<int>(std::min<std::uint32_t>(file.hand_pose, 13));
    clip.emote = file.emote;
    clip.constraints = file.constraints;

    for (auto& j : file.joints) {
        int idx = skel.find_viewer(j.name);
        if (idx < 0) {
            idx = skel.find(j.name);
            if (idx >= 0) res.report.push_back("\"" + j.name + "\" matched " + skel[idx].name + " ignoring case");
        }
        size_t nr = legacy ? j.rot_legacy.size() : j.rot.size();
        size_t np = legacy ? j.pos_legacy.size() : j.pos.size();
        auto rot_at = [&](size_t k) { return legacy ? legacy_rotation(j.rot_legacy[k]) : stable_rotation(j.rot[k]); };
        auto pos_at = [&](size_t k) {
            if (!legacy) return stable_position(j.pos[k]);
            Vec3 p{j.pos_legacy[k][1], j.pos_legacy[k][2], j.pos_legacy[k][3]};
            for (int a = 0; a < 3; ++a) p[a] = std::clamp(p[a], -double(kAnimMaxOffset), double(kAnimMaxOffset));
            return p;
        };

        if (idx < 0) {
            res.report.push_back("\"" + j.name + "\" is not in the skeleton; kept as-is");
            OrphanJoint o;
            o.name = j.name;
            o.priority = j.priority;
            // Times at the middle of their code so re-export writes the same code.
            auto mid = [&](double t, std::uint16_t code) { return legacy ? t : (code + 0.5) / 65535.0 * file.duration; };
            for (size_t k = 0; k < nr; ++k) o.rot.emplace_back(mid(rot_time(j, k), legacy ? 0 : j.rot[k][0]), rot_at(k));
            for (size_t k = 0; k < np; ++k) o.pos.emplace_back(mid(pos_time(j, k), legacy ? 0 : j.pos[k][0]), pos_at(k));
            clip.orphans.push_back(std::move(o));
            continue;
        }
        const Node& node = skel[idx];
        if (j.priority != file.base_priority) clip.joint_priority[node.name] = j.priority;

        Track& track = clip.curves[node.name];
        // Keys in time order so each Euler triple is unwrapped against the previous one.
        std::vector<size_t> order(nr);
        for (size_t k = 0; k < nr; ++k) order[k] = k;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rot_time(j, a) < rot_time(j, b); });
        Vec3 prev;
        auto finite = [](const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        for (size_t k : order) {
            double fr = to_frame(rot_time(j, k));
            Quat q = node.rest.conj() * rot_at(k);
            Vec3 e = nearest_euler(q, prev);
            if (!std::isfinite(fr) || !finite(e)) continue;  // damaged key (the viewer refuses these files)
            prev = e;
            for (int a = 0; a < 3; ++a) add_linear_key(track[kRotChannels[a]], fr, prev[a]);
        }
        for (size_t k = 0; k < np; ++k) {
            Vec3 p = pos_at(k);
            if (idx != 0) p = p - node.pos;  // file holds absolute local positions except for the pelvis
            double fr = to_frame(pos_time(j, k));
            if (!std::isfinite(fr) || !finite(p)) continue;
            for (int a = 0; a < 3; ++a) add_linear_key(track[kPosChannels[a]], fr, p[a]);
        }
        for (auto& [ch, curve] : track) finish_curve(curve);
    }
    return res;
}

const AnimFile* raw_reexport(const RawAnim& raw, const Clip& clip) {
    // Props and export choices do not change the animation bytes.
    Clip a = clip, b = raw.clip;
    a.props.clear(), b.props.clear();
    a.mirror_export = b.mirror_export, a.export_settings = b.export_settings;
    return a == b ? &raw.file : nullptr;
}

}  // namespace vats
