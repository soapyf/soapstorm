// Viewport Avatar Toolset - the deformer tool (spec 09 section 0l).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/deformer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vats/curve_ops.h"
#include "vats/dynamics.h"
#include "vats/edit.h"
#include "vats/position_reset.h"

namespace vats {
namespace {
constexpr double kBakeTol = 0.0002;  // m: mSkull's counter-keys within this of every frame's (linear keys)
}  // namespace

bool counts_for_sl_height(const std::string& bone) {
    for (const char* b : {"mTorso", "mChest", "mNeck", "mHead", "mSkull", "mHipLeft", "mKneeLeft", "mAnkleLeft", "mFootLeft"})
        if (bone == b) return true;
    return false;
}

std::vector<std::string> end_at_rest(Clip& clip, const Skeleton& skel) {
    std::vector<std::string> keyed;
    const int end = std::max(clip.end_frame, 1);
    const std::vector<std::string> moved = joints_ending_moved_by_position(skel, clip);
    for (const std::string& name : moved) {
        auto it = clip.curves.find(name);
        if (it == clip.curves.end()) continue;
        for (const char* ch : kPosChannels) {
            auto cit = it->second.find(ch);
            if (cit == it->second.end() || cit->second.empty()) continue;
            FCurve& c = cit->second;
            // A key on the last frame that keeps the curve's shape up to it, then a straight line to rest.
            Key& k = c.keys[size_t(insert_on_curve(c, end))];
            k.left = Handle::Free;
            k.interp = Interp::Linear;
            c.set_key(end + 1, 0.0, Interp::Linear);
        }
        keyed.push_back(name);
    }
    if (!keyed.empty()) clip.end_frame = end + 1;
    return keyed;
}

HoldResult hold_without_sinking(Clip& clip, const Skeleton& skel, const Shape* shape) {
    HoldResult r;
    const int skull = skel.find("mSkull"), head = skel.find("mHead");
    if (skull < 0 || skel[skull].name != "mSkull" || head < 0) {
        r.refused = "the skeleton has no mSkull";
        return r;
    }
    if (shape && size_t(head) < shape->scale.size()) r.head_scale = shape->scale[size_t(head)].z, r.head_scale_known = true;
    if (!(std::fabs(r.head_scale) > 1e-6)) {
        r.refused = "the bake shape's head has no height";
        return r;
    }
    r.combined = clip.has_channels("mSkull", kPosChannels);
    bool others = false;
    for (auto& [name, track] : clip.curves)
        others = others || (name != "mSkull" && counts_for_sl_height(name) && clip.has_channels(name, kPosChannels));
    if (!others) {
        if (r.combined)
            r.refused = "only mSkull's own position keys change the height, and countering them on mSkull would undo them. "
                        "Stretch mNeck or mHead instead, or the spine (mSpine1 to mSpine4), which the height ignores";
        return r;
    }
    // Per frame: the skull's Z that puts sl_body_size's height back at rest, its own keys included; within 5 m.
    const double rest = sl_body_size(skel, nullptr, shape).height, k = std::sqrt(2.0) * r.head_scale;
    const double base = skel[skull].pos.z, lim = double(kAnimMaxOffset);
    std::vector<Pose> frames;
    bool moves = false;
    for (int f = 0; f <= std::max(clip.end_frame, 1); ++f) {
        Pose p = evaluate_curves(skel, clip, f);
        const double own = p.offset[size_t(skull)].z;
        const double want = own - (sl_body_size(skel, &p, shape).height - rest) / k;
        const double z = std::clamp(base + want, -lim, lim) - base;
        if (std::fabs(z - want) > 1e-9) {
            r.shortfall = std::max(r.shortfall, std::fabs(z - want) * std::fabs(k));
            r.short_frames.push_back(f);
        }
        moves = moves || std::fabs(z - own) > kBakeTol;  // what the bake leaves counts as done
        p.offset[size_t(skull)].z = z;
        frames.push_back(std::move(p));
    }
    if (!moves) return r;  // the other bones' keys change no height
    bake_samples(clip, skel, {skull}, frames, 0.1, kBakeTol, {skull}, true);
    r.keyed = true;
    return r;
}

Clip with_deformer_options(const Skeleton& skel, const Clip& clip, const AnimExportOptions& opt,
                           std::vector<std::string>* notes) {
    Clip c = clip;
    auto note = [&](const std::string& s) {
        if (notes) notes->push_back(s);
    };
    if (opt.end_at_rest && !end_at_rest(c, skel).empty() && c.loop)
        note("End at rest: this animation loops, so it stops wherever it is when it is stopped and the bones keep those "
             "positions; the rest key plays only if it runs to its last frame. Export an undeformer to put them back");
    if (opt.hold_without_sinking) {
        const HoldResult h = hold_without_sinking(c, skel, deformer_body(opt));
        if (!h.refused.empty()) note("Hold without sinking: nothing done: " + h.refused);
        if (!h.short_frames.empty()) {
            char buf[300];
            std::snprintf(buf, sizeof buf,
                          "Hold without sinking: mSkull reaches the 5 m position limit on %zu frames; there the avatar "
                          "still sinks or rises up to %.1f cm",
                          h.short_frames.size(), h.shortfall * 50);
            note(buf);
        }
    }
    return c;
}

AnimFile make_undeformer(const Skeleton& skel, const AnimFile& d, const Shape* positions) {
    AnimFile u;
    u.base_priority = d.base_priority;
    u.duration = kUndeformSeconds;
    u.loop_in = 0, u.loop_out = u.duration;
    u.ease_in = u.ease_out = 0;
    for (const AnimJoint& j : d.joints) {
        if (j.pos.empty() && j.pos_legacy.empty()) continue;
        const int n = skel.find_viewer(j.name);
        if (n <= 0) continue;  // the hip (pelvis_fix takes it back), or a joint this skeleton does not know
        u.joints.push_back(make_rest_joint(skel, j.name, j.priority, positions));
    }
    return u;
}

}  // namespace vats
