// Viewport Avatar Toolset - the Animation Check (spec 08 section 9).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/lint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>

#include "vats/curve_ops.h"
#include "vats/dynamics.h"
#include "vats/edit.h"
#include "vats/loop_tools.h"
#include "vats/ragdoll.h"
#include "vats/rig.h"
#include "vats/world_reduce.h"
#include "vats/self_contact.h"

namespace vats {
namespace {

constexpr double kGroundTol = 0.02;    // m: the lowest foot this far off its rest height is off the ground
constexpr double kLimitTol = 5;        // degrees past a ragdoll limit before it counts
constexpr double kArcMax = 90;         // degrees between kept keys before nlerp speed goes visibly uneven
constexpr double kArcStep = 45;        // the fix's key spacing: any two frames inside a step are <= 90 apart
constexpr double kSubFrame = 0.1;      // frames off a whole frame (reverse's slivers sit closer, on purpose)
constexpr double kEaseFixed = 0.3;     // seconds the ease fix gives a zero ease
constexpr int kAoPriority = 4;         // AO walks and stands play at up to this

const std::vector<LintRule> kRules = {
    {"loop_seam", "Loop seam jumps"},
    {"hip_drift", "Hips travel over the loop"},
    {"loop_range", "Loop range reversed or shorter than the eases"},
    {"ease_long", "Ease longer than the animation"},
    {"ease_zero", "Zero ease pops"},
    {"subframe", "Keys between frames"},
    {"nlerp_arc", "Turns over 90 degrees between keys"},
    {"ao_priority", "Whole body below priority 4"},
    {"hover_pop", "Hip and leg position keys"},
    {"frozen_bones", "Bones that never move"},
    {"face_positions", "Face position keys on the default face"},
    {"eyes", "Eyes keyed"},
    {"expression", "Expression with face bones"},
    {"hand_pose", "Hand pose with finger bones"},
    {"joint_limits", "Joints past their limits"},
    {"ground", "Feet off the ground"},
    {"self_contact", "Body parts pass through each other"},
    {"upload_size", "Upload size"},
    {"duration", "Duration"},
};

std::string fmt(const char* f, double a, double b = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

bool is_eye(const std::string& n) {
    return n == "mEyeLeft" || n == "mEyeRight" || n == "mFaceEyeAltLeft" || n == "mFaceEyeAltRight";
}

bool keyed(const Track& t) {
    for (auto& [ch, c] : t)
        if (!c.empty()) return true;
    return false;
}

// The hips (mHipLeft, mHipRight) and every joint below them.
bool in_leg(const Skeleton& skel, int n) {
    for (; n >= 0; n = skel[n].parent)
        if (skel[n].name == "mHipLeft" || skel[n].name == "mHipRight") return true;
    return false;
}

bool in_upper_body(const std::string& n) {
    for (const char* b : {"mTorso", "mChest", "mNeck", "mHead"})
        if (n == b) return true;
    for (const char* b : {"mCollar", "mShoulder", "mElbow", "mWrist"})
        if (n.rfind(b, 0) == 0) return true;
    return false;
}

// Every key of the tracks' channels (all of them, or the position channels only).
std::vector<KeyRef> keys_of(const Clip& c, const std::vector<std::string>& tracks, bool positions_only) {
    std::vector<KeyRef> sel;
    for (const std::string& t : tracks) {
        auto it = c.curves.find(t);
        if (it == c.curves.end()) continue;
        for (auto& [ch, curve] : it->second) {
            if (positions_only && ch.rfind("pos_", 0) != 0) continue;
            for (int i = 0; i < int(curve.keys.size()); ++i) sel.push_back({t, ch, i});
        }
    }
    return sel;
}

std::function<void(Clip&)> delete_all(std::vector<std::string> tracks, bool positions_only) {
    return [tracks = std::move(tracks), positions_only](Clip& c) { delete_keys(c, keys_of(c, tracks, positions_only)); };
}

// Moves a track's pos_z by dz: every key (the graph's Move), or one held key when it has none.
void shift_z(Clip& c, const std::string& track, double dz, bool add_if_missing) {
    const FCurve* z = nullptr;
    if (auto t = c.curves.find(track); t != c.curves.end())
        if (auto ch = t->second.find("pos_z"); ch != t->second.end() && !ch->second.empty()) z = &ch->second;
    if (!z) {
        if (add_if_missing) key_offset(c, track, 0, curve_offset(c, track, 0) + Vec3{0, 0, dz});
        return;
    }
    std::vector<KeyRef> sel;
    for (int i = 0; i < int(z->keys.size()); ++i) sel.push_back({track, "pos_z", i});
    const Clip before = c;
    move_keys(c, before, sel, 0, dz, false);
    finish_transform(c, sel);
}

// Fits both eases into `seconds`, scaling them down together (as the viewer's BVH upload does, D9).
void fit_eases(Clip& c, double seconds) {
    const double sum = c.ease_in + c.ease_out;
    if (sum <= seconds || sum <= 0) return;
    c.ease_in *= seconds / sum, c.ease_out *= seconds / sum;
}

// The clip's own export settings over opt (the app reads them the same way when it exports).
AnimExportOptions with_clip_settings(const Clip& clip, AnimExportOptions o) {
    const Json& ex = clip.export_settings;
    if (const Json* v = ex.find("leave_static"); v && v->is_bool()) o.leave_out_static_rotations = v->b;
    if (const Json* r = ex.find("reduce"); r && r->is_array() && r->arr.size() == 2 && r->arr[0].is_number() &&
                                           r->arr[1].is_number())
        o.reduce_rot_deg = r->arr[0].num, o.reduce_pos_m = r->arr[1].num;
    if (const Json* m = ex.find("reduce_mode"); m && m->is_string() && m->str == "world") {  // 08 WR
        const Json* w = ex.find("reduce_world");
        o.reduce_world_m = w && w->is_number() && w->num > 0 ? w->num : kReduceWorldDefault;
    }
    return o;
}

std::string list(const std::vector<std::string>& names) {
    std::string s;
    for (size_t i = 0; i < names.size() && i < 4; ++i) s += (i ? ", " : "") + names[i];
    if (names.size() > 4) s += " and " + std::to_string(names.size() - 4) + " more";
    return s;
}

}  // namespace

const std::vector<LintRule>& lint_rules() { return kRules; }

std::vector<LintFinding> lint_clip(const Skeleton& skel, const Clip& clip, const AnimExportOptions& opt,
                                   const std::vector<std::string>& off, const Shape* mesh_body) {
    std::vector<LintFinding> out;
    auto on = [&](const char* rule) { return std::find(off.begin(), off.end(), rule) == off.end(); };
    auto add = [&](const char* rule, LintSeverity sev, std::vector<std::string> bones, std::vector<int> frames,
                   std::string msg, LintFix fix) {
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        out.push_back({rule, sev, std::move(bones), std::move(frames), std::move(msg), std::move(fix)});
    };
    const int fps = std::clamp(clip.fps, 1, 120), last = std::max(clip.end_frame, 1);
    const double seconds = double(last) / fps;

    // Settings: the loop and the eases.
    const bool reversed = clip.loop && clip.loop_in >= clip.loop_out;
    if (clip.loop && !reversed) {
        const LoopRange r = loop_range(clip);
        std::vector<std::string> seam;
        double worst = 0, travel = 0;
        for (const SeamJump& j : loop_seam_jumps(clip)) {
            if (j.track == "mPelvis" && (j.channel == "pos_x" || j.channel == "pos_y")) {
                travel = std::hypot(travel, j.jump);
                continue;
            }
            if (std::find(seam.begin(), seam.end(), j.track) == seam.end()) seam.push_back(j.track);
            worst = std::max(worst, std::fabs(j.jump));
        }
        if (on("loop_seam") && !seam.empty())
            add("loop_seam", LintSeverity::Warning, seam, {r.in, r.out},
                "The loop jumps where it repeats on " + list(seam) + " (up to " + fmt("%.3g", worst) + ")",
                {"Make Loop Seamless", [](Clip& c) { make_loop_seamless(c, 0); }});
        if (on("hip_drift") && travel > 0)
            add("hip_drift", LintSeverity::Warning, {"mPelvis"}, {r.in, r.out},
                fmt("The hips travel %.2f m over the loop and jump back each time it repeats", travel),
                {"Remove Hip Travel (In Place)", [](Clip& c) { remove_travel(c); }});
    }
    if (on("loop_range") && reversed)
        add("loop_range", LintSeverity::Error, {}, {clip.loop_in, clip.loop_out},
            clip.loop_in == clip.loop_out ? "The loop is empty: loop in and loop out are the same frame"
                                          : "Loop in is after loop out",
            {"Swap Loop In and Loop Out", [](Clip& c) {
                 if (c.loop_in == c.loop_out) c.loop_in = 0, c.loop_out = std::max(c.end_frame, 1);
                 else std::swap(c.loop_in, c.loop_out);
             }});
    const LoopRange lr = loop_range(clip);
    const double loop_s = double(lr.out - lr.in) / fps;
    if (on("loop_range") && clip.loop && !reversed && clip.ease_in + clip.ease_out > loop_s + 1e-6)
        add("loop_range", LintSeverity::Warning, {}, {clip.loop_in, clip.loop_out},
            fmt("The loop (%.2f s) is shorter than ease in plus ease out (%.2f s)", loop_s, clip.ease_in + clip.ease_out),
            {"Shorten the Eases to Fit", [loop_s](Clip& c) { fit_eases(c, loop_s); }});
    if (on("ease_long") && !clip.loop && clip.ease_in + clip.ease_out > seconds + 1e-6)
        add("ease_long", LintSeverity::Warning, {}, {},
            fmt("Ease in plus ease out (%.2f s) is longer than the animation (%.2f s)", clip.ease_in + clip.ease_out,
                seconds),
            {"Shorten the Eases to Fit", [seconds](Clip& c) { fit_eases(c, seconds); }});
    if (on("ease_zero") && (clip.ease_in <= 0 || clip.ease_out <= 0)) {
        const double room = clip.loop && !reversed ? loop_s : seconds;
        add("ease_zero", LintSeverity::Info, {}, {},
            clip.ease_in <= 0 && clip.ease_out <= 0 ? "Ease in and ease out are 0: the avatar snaps into and out of the pose"
            : clip.ease_in <= 0                      ? "Ease in is 0: the avatar snaps into the first pose"
                                                     : "Ease out is 0: the avatar snaps back when the animation stops",
            {fmt("Set the Zero Ease to %.2f s", kEaseFixed), [room](Clip& c) {
                 for (double* e : {&c.ease_in, &c.ease_out})  // within what is left, so the other ease rules stay clean
                     if (*e <= 0) *e = std::min(kEaseFixed, std::max(room - (e == &c.ease_in ? c.ease_out : c.ease_in), 0.0));
             }});
    }

    // Keys between whole frames: export samples whole frames only.
    if (on("subframe")) {
        std::vector<std::string> bones;
        std::vector<int> frames;
        std::vector<KeyRef> sel;
        for (auto& [name, track] : clip.curves)
            for (auto& [ch, c] : track)
                for (int i = 0; i < int(c.keys.size()); ++i)
                    if (std::fabs(c.keys[i].frame - std::round(c.keys[i].frame)) > kSubFrame) {
                        sel.push_back({name, ch, i});
                        frames.push_back(int(std::lround(c.keys[i].frame)));
                        if (std::find(bones.begin(), bones.end(), name) == bones.end()) bones.push_back(name);
                    }
        if (!sel.empty())
            add("subframe", LintSeverity::Warning, bones, frames,
                std::to_string(sel.size()) + (sel.size() == 1 ? " key sits" : " keys sit") +
                    " between whole frames; SL plays whole frames only and never shows them",
                {"Snap Keys to Whole Frames", [sel](Clip& c) {
                     std::vector<KeyRef> s = sel;
                     const Clip before = c;
                     scale_keys(c, before, s, 0, 0, 1, 1, true);
                     finish_transform(c, s);
                 }});
    }

    // Header fields against what is keyed.
    auto keyed_in = [&](Category cat) {
        std::vector<std::string> names;
        for (auto& [name, track] : clip.curves) {
            const int n = skel.find(name);
            if (n >= 0 && skel[n].category == cat && keyed(track)) names.push_back(name);
        }
        return names;
    };
    if (on("eyes")) {
        std::vector<std::string> eyes;
        for (auto& [name, track] : clip.curves)
            if (is_eye(name) && keyed(track)) eyes.push_back(name);
        if (!eyes.empty())
            add("eyes", LintSeverity::Warning, eyes, {},
                "The eyes are keyed (" + list(eyes) + "); they fight the viewer's look-at and jitter in world",
                {"Remove the Eye Keys", delete_all(eyes, false)});
    }
    if (on("expression") && !clip.emote.empty())
        if (auto face = keyed_in(Category::Face); !face.empty())
            add("expression", LintSeverity::Warning, face, {},
                "The expression " + clip.emote + " is set while face bones are keyed; the two fight over the face",
                {"Set Expression to None", [](Clip& c) { c.emote.clear(); }});
    if (on("hand_pose") && clip.hand_pose != 1)
        if (auto fingers = keyed_in(Category::Hands); !fingers.empty())
            add("hand_pose", LintSeverity::Warning, fingers, {},
                "A hand pose is set while finger bones are keyed; the pose morphs the hand under the fingers",
                {"Set Hand Pose to Relaxed", [](Clip& c) { c.hand_pose = 1; }});

    if (seconds > kAnimMaxDuration) {  // nothing exports: the other checks wait until it fits
        if (on("duration"))
            add("duration", LintSeverity::Error, {}, {fps * int(kAnimMaxDuration), last},
                fmt("The animation is %.1f s; SL plays at most %.0f s", seconds, kAnimMaxDuration),
                {fmt("Trim to %.0f s", kAnimMaxDuration), [fps](Clip& c) {
                     c.end_frame = fps * int(kAnimMaxDuration);  // AM-4
                     c.loop_out = std::min(c.loop_out, c.end_frame);
                     c.loop_in = std::min(c.loop_in, c.loop_out);
                 }});
        return out;
    }

    // Sample every whole frame as export does (IK, pins and the bake shape included).
    Rig rig(skel);
    rig.external = opt.external;
    std::vector<Pose> poses;
    poses.reserve(last + 1);
    const int feet[] = {skel.find("mAnkleLeft"), skel.find("mAnkleRight"), skel.find("mFootLeft"),
                        skel.find("mFootRight"), skel.find("mToeLeft"),    skel.find("mToeRight")};
    auto lowest = [&](const std::vector<Xform>& g) {
        double z = 1e9;
        for (int n : feet)
            if (n >= 0) z = std::min(z, g[n].pos.z);
        return z;
    };
    const double ground = lowest(skel.global_pose(Pose(skel.size()), opt.shape));
    std::vector<double> low;
    for (int f = 0; f <= last; ++f) {
        Evaluation e = evaluate(rig, clip, f, opt.shape);
        low.push_back(lowest(e.globals));
        poses.push_back(std::move(e.pose));
    }

    if (on("joint_limits")) {
        std::map<int, std::pair<double, std::vector<int>>> over;  // node -> worst, frames
        for (int f = 0; f <= last; ++f)
            for (const LimitExcess& e : ragdoll_limit_excesses(skel, poses[f], kLimitTol)) {
                auto& o = over[e.node];
                o.first = std::max(o.first, e.deg);
                o.second.push_back(f);
            }
        for (auto& [n, o] : over) {
            const std::string name = skel[n].name;
            add("joint_limits", LintSeverity::Info, {name}, o.second,
                name + fmt(" goes %.0f degrees past what a body can do", o.first),
                {"Key the Joint Inside Its Limits", [&skel, n, name](Clip& c) {
                     // FK only: a joint IK drives keeps its pose. Keying moves the curve around the key, so repeat.
                     for (int pass = 0; pass < 4; ++pass) {
                         bool keyed_any = false;
                         for (int f = 0; f <= std::max(c.end_frame, 1); ++f)
                             for (const LimitExcess& e : ragdoll_limit_excesses(skel, evaluate_curves(skel, c, f), 0.5))
                                 if (e.node == n) key_rotation(c, name, f, e.inside), keyed_any = true;
                         if (!keyed_any) break;
                     }
                 }});
        }
    }

    // Self-penetration (08 SX): the ragdoll's capsules on every frame; on the mesh body, with its volumes, when there
    // is one.
    if (on("self_contact")) {
        const Shape* body = mesh_body ? mesh_body : opt.shape;
        std::map<std::pair<int, int>, std::pair<double, std::vector<int>>> hits;  // pair -> deepest, frames
        const SelfContactCheck check(skel, body, mesh_body != nullptr);
        for (int f = 0; f <= last; ++f)
            for (const SelfContact& s : check.find(skel.global_pose(poses[f], body))) {
                auto& h = hits[{s.a, s.b}];
                h.first = std::max(h.first, s.depth);
                h.second.push_back(f);
            }
        // The fix outlives this call: it keeps its own copies of the shapes.
        const std::optional<Shape> sh = opt.shape ? std::optional<Shape>(*opt.shape) : std::nullopt;
        const std::optional<Shape> mb = mesh_body ? std::optional<Shape>(*mesh_body) : std::nullopt;
        for (auto& [ab, h] : hits) {
            const auto [a, b] = ab;
            LintFix fix;
            if (push_out_moves(skel[a].name) || push_out_moves(skel[b].name))
                fix = {"Push Out", [&skel, ext = opt.external, a, b, frames = h.second, sh, mb](Clip& c) {
                           Rig r(skel);
                           r.external = ext;
                           push_out(c, r, a, b, frames, sh ? &*sh : nullptr, mb ? &*mb : nullptr);
                       }};
            add("self_contact", LintSeverity::Info, {skel[a].name, skel[b].name}, h.second,
                skel[a].name + " and " + skel[b].name +
                    fmt(" pass %.1f cm into each other (their capsules: a hint, not the mesh)", h.first * 100),
                std::move(fix));
        }
    }

    // ponytail: rest height is the ground, so sits and flights are flagged too (Info when above); a sit-aware ground
    // would need to know what the avatar sits on.
    if (on("ground") && feet[0] >= 0) {
        const auto lo = std::min_element(low.begin(), low.end());
        const double d = *lo - ground;
        std::vector<int> frames;
        if (d < -kGroundTol) {
            for (int f = 0; f <= last; ++f)
                if (low[f] - ground < -kGroundTol) frames.push_back(f);
        } else {
            frames.push_back(int(lo - low.begin()));
        }
        if (std::fabs(d) > kGroundTol) {
            std::vector<std::string> ik_legs;
            for (const char* limb : {"LegLeft", "LegRight"})
                if (int l = rig.find_limb(limb); l >= 0 && uses_ik(clip, rig.limbs()[l])) ik_legs.push_back("ik." + std::string(limb));
            const double cm = std::fabs(d) * 100;
            add("ground", d < 0 ? LintSeverity::Warning : LintSeverity::Info, {"mPelvis"}, frames,
                d < 0 ? fmt("The feet go %.1f cm below the ground", cm)
                      : fmt("The feet never reach the ground: the lowest is %.1f cm above it", cm),
                {fmt(d < 0 ? "Raise the Hips by %.1f cm" : "Drop the Hips by %.1f cm", cm), [d, ik_legs](Clip& c) {
                     shift_z(c, "mPelvis", -d, true);
                     for (const std::string& t : ik_legs) shift_z(c, t, -d, false);  // targets keyed in avatar space
                 }});
        }
    }

    // What the export writes.
    const AnimExportOptions eo = with_clip_settings(clip, opt);
    const AnimExportResult r = export_anim(skel, clip, eo);
    const AnimFile& file = r.file;
    if (file.joints.empty()) return out;

    if (on("upload_size")) {
        const std::size_t bytes = write_anim(file).size();
        if (bytes >= kAnimMaxUploadBytes) {
            // Every keyed bone's curves re-keyed from their own samples at a coarser tolerance (the Dynamics bake): the
            // export's own reduction keeps every keyed frame, so a clip keyed on every frame (mocap) only shrinks this way.
            // The tolerance is the smallest doubling whose key count, counted the way export reduces, fits 90% of the
            // limit; the bake runs once (again, doubled, if the file still does not fit). It runs when the fix is applied.
            // ponytail: one tolerance for every bone; a per-joint byte budget would keep more detail.
            std::vector<int> nodes, with_pos;
            for (auto& [name, track] : clip.curves)
                if (const int n = skel.find(name); n >= 0 && keyed(track)) {
                    nodes.push_back(n);
                    if (clip.has_channels(name, kPosChannels)) with_pos.push_back(n);
                }
            LintFix fix{"Thin Out Keys", [&skel, eo, nodes, with_pos, bytes](Clip& c) {
                            std::vector<Pose> fk;
                            for (int f = 0; f <= std::max(c.end_frame, 1); ++f) fk.push_back(evaluate_curves(skel, c, f));
                            const int gap = std::max(c.fps, 1) * 2;  // bake_samples' own
                            auto keys = [&](double tol) {
                                std::size_t k = 0;
                                for (int n : nodes) {
                                    std::vector<Quat> r;
                                    std::vector<Vec3> p;
                                    for (const Pose& f : fk) r.push_back(f.rot[n]), p.push_back(f.offset[n]);
                                    k += reduce_rotation_keys(r, tol, gap).size();
                                    if (std::find(with_pos.begin(), with_pos.end(), n) != with_pos.end())
                                        k += reduce_position_keys(p, tol / 100, gap).size();
                                }
                                return double(k);
                            };
                            const double now = keys(0);
                            double tol = std::max(eo.reduce_rot_deg, 0.05) * 2;
                            while (tol < 8 && double(bytes) * keys(tol) / now >= 0.9 * kAnimMaxUploadBytes) tol *= 2;
                            for (Clip trial = c;; trial = c, tol *= 2) {
                                bake_samples(trial, skel, nodes, fk, tol, tol / 100, with_pos);  // 1 degree ~ 1 cm
                                if (tol >= 8 || write_anim(export_anim(skel, trial, eo).file).size() < kAnimMaxUploadBytes) {
                                    c = std::move(trial);
                                    return;
                                }
                            }
                        }};
            add("upload_size", LintSeverity::Error, {}, {},
                fmt("The file is %.0f bytes; SL refuses %.0f bytes or more", double(bytes), double(kAnimMaxUploadBytes)),
                std::move(fix));
        }
    }

    // Per exported joint.
    std::vector<std::string> low_priority, hover, frozen, face_pos;
    bool upper = false;
    for (const AnimJoint& j : file.joints) {
        const int n = skel.find_viewer(j.name);
        if (n < 0) continue;  // an orphan written back as it came
        const std::string& name = skel[n].name;
        const Category cat = skel[n].category;
        const int prio = j.priority < 0 ? file.base_priority : j.priority;
        const bool leg = n == 0 || in_leg(skel, n);
        upper = upper || in_upper_body(name);
        if (leg && prio < kAoPriority) low_priority.push_back(name);
        if (!j.pos.empty() && !skel[n].attachment && (name == "mSpine1" || in_leg(skel, n))) hover.push_back(name);
        if (!j.pos.empty() && cat == Category::Face && !is_eye(name) && !eo.positions) face_pos.push_back(name);
        if (j.pos.empty() && !j.rot.empty() && !is_eye(name) &&
            (cat == Category::Tail || cat == Category::Wings || cat == Category::Face || cat == Category::Hands) &&
            static_rotation(poses, n, eo.reduce_rot_deg))
            frozen.push_back(name);

        if (on("nlerp_arc") && j.rot.size() > 1) {
            auto frame_of = [&](const std::array<std::uint16_t, 4>& k) {
                return int(std::lround(u16_to_f32(k[0], 0.f, file.duration) * fps));
            };
            std::vector<int> frames, add_at;
            double worst = 0;
            for (size_t k = 1; k < j.rot.size(); ++k) {
                const double a = 2 * std::acos(std::min(1.0, std::fabs(decode_rotation(j.rot[k - 1]).dot(decode_rotation(j.rot[k]))))) * kRadToDeg;
                if (a <= kArcMax) continue;
                worst = std::max(worst, a);
                const int fa = frame_of(j.rot[k - 1]), fb = frame_of(j.rot[k]);
                frames.push_back(fa), frames.push_back(fb);
                // Keys where the turn from the last one passes kArcStep; export keeps every keyed frame (anchors).
                for (int s = fa, f = fa + 1; f < fb; ++f)
                    if (2 * std::acos(std::min(1.0, std::fabs(poses[s].rot[n].dot(poses[f + 1].rot[n])))) * kRadToDeg > kArcStep)
                        add_at.push_back(f), s = f;
            }
            if (frames.empty()) continue;
            LintFix fix;  // none when the turn happens within one frame, or IK turns the joint
            if (!add_at.empty() && clip.has_channels(name, kRotChannels))
                fix = {"Add Keys Along the Turn", [name, add_at](Clip& c) {
                           for (auto& [ch, curve] : c.curves[name])
                               if (ch.rfind("rot_", 0) == 0 && !curve.empty())
                                   for (int f : add_at) insert_on_curve(curve, f);
                       }};
            add("nlerp_arc", LintSeverity::Warning, {name}, frames,
                name + fmt(" turns %.0f degrees between two kept keys; SL blends that at an uneven speed", worst),
                std::move(fix));
        }
    }
    if (on("ao_priority") && upper && !low_priority.empty())
        add("ao_priority", LintSeverity::Warning, low_priority, {},
            "The whole body is animated but the hips and legs play below priority 4; a walking or standing AO wins them",
            {"Set Priority 4", [low_priority](Clip& c) {
                 c.priority = std::max(c.priority, kAoPriority);
                 for (const std::string& b : low_priority)
                     if (auto p = c.joint_priority.find(b); p != c.joint_priority.end() && p->second < kAoPriority)
                         c.joint_priority.erase(p);
             }});
    if (on("hover_pop") && !hover.empty())
        add("hover_pop", LintSeverity::Warning, hover, {},
            "Position keys on " + list(hover) + " change the leg length; the avatar pops up or down as it starts",
            {"Remove Their Position Keys", delete_all(hover, true)});
    if (on("frozen_bones") && !frozen.empty())
        add("frozen_bones", LintSeverity::Warning, frozen, {},
            list(frozen) + " never move but are exported; they freeze the wearer's own tail, wings, face or hands",
            {"Leave Out Bones That Don't Move", [](Clip& c) {
                 if (!c.export_settings.is_object()) c.export_settings = Json::object();
                 c.export_settings.set("leave_static", true);  // IO-11b
             }});
    if (on("face_positions") && !face_pos.empty())
        add("face_positions", LintSeverity::Warning, face_pos, {},
            "Face bones carry position keys written from the SL Default face; they pull a mesh head towards it",
            {"Remove the Face Position Keys", delete_all(face_pos, true)});
    return out;
}

}  // namespace vats
