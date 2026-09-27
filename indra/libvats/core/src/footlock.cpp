// Viewport Avatar Toolset - foot-contact clean-up (spec 07 RT-9).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/footlock.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace vats {

namespace {

bool is_leg(const LimbInfo& l, bool left, bool right) {
    return (left && l.name == "LegLeft") || (right && l.name == "LegRight");
}

std::vector<int> legs(const Rig& rig, const FootLockOptions& opt) {
    std::vector<int> out;
    for (int i = 0; i < int(rig.limbs().size()); ++i)
        if (is_leg(rig.limbs()[i], opt.left, opt.right)) out.push_back(i);
    return out;
}

int last_frame(const Clip& clip, const FootLockOptions& opt) {
    return opt.to < 0 ? clip.end_frame : std::min(opt.to, clip.end_frame);
}

}  // namespace

std::vector<FootContact> find_foot_contacts(const Rig& rig, const Clip& clip, const FootLockOptions& opt) {
    const int a = std::max(0, opt.from), b = last_frame(clip, opt);
    std::vector<FootContact> out;
    if (b <= a) return out;
    const std::vector<int> ls = legs(rig, opt);
    std::vector<std::vector<Vec3>> pos(ls.size());
    for (int f = a; f <= b; ++f) {
        Evaluation e = evaluate(rig, clip, f, opt.shape);
        for (size_t k = 0; k < ls.size(); ++k) pos[k].push_back(e.globals[rig.limbs()[ls[k]].end].pos);
    }
    const double fps = std::max(clip.fps, 1);
    for (size_t k = 0; k < ls.size(); ++k) {
        const auto& p = pos[k];
        const int n = int(p.size());
        double ground = std::numeric_limits<double>::max();
        for (auto& v : p) ground = std::min(ground, v.z);
        bool on = false;
        int start = 0;
        auto close = [&](int end) {
            if (end - start + 1 >= opt.min_frames) out.push_back({ls[k], a + start, a + end});
        };
        for (int i = 0; i < n; ++i) {
            const int i0 = std::max(i - 1, 0), i1 = std::min(i + 1, n - 1);
            const double speed = i1 > i0 ? (p[i1] - p[i0]).length() / (i1 - i0) * fps : 0;
            const double h = p[i].z - ground;
            if (!on && h <= opt.height && speed <= opt.speed) on = true, start = i;
            else if (on && (h > opt.height * 1.5 || speed > opt.speed * 2)) on = false, close(i - 1);
        }
        if (on) close(n - 1);
    }
    return out;
}

std::vector<std::string> lock_feet(Clip& clip, const Rig& rig, const FootLockOptions& opt) {
    std::vector<std::string> report;
    std::vector<FootContact> contacts = find_foot_contacts(rig, clip, opt);
    // Poles from the pose as animated, before any keys change (the knee keeps pointing where it did).
    std::vector<Evaluation> fk;
    const int b = last_frame(clip, opt);
    for (int f = 0; f <= b; ++f) fk.push_back(evaluate(rig, clip, f, opt.shape));

    for (int limb : legs(rig, opt)) {
        const LimbInfo& l = rig.limbs()[limb];
        std::vector<FootContact> spans;
        for (auto& c : contacts)
            if (c.limb == limb) spans.push_back(c);
        if (spans.empty()) continue;
        const std::string track = "ik." + l.name;
        // IK the user keyed in the range wins; blend keys elsewhere (an earlier take's lock) are fine.
        auto has_blend = [&] {
            auto t = clip.curves.find(track);
            if (t == clip.curves.end() || !t->second.count("blend")) return false;
            for (const Key& k : t->second.at("blend").keys)
                if (k.frame >= opt.from - opt.blend && k.frame <= b + opt.blend) return true;
            return false;
        };
        if (has_blend()) {
            report.push_back(l.label + ": already uses IK here, left as it is");
            continue;
        }
        for (size_t s = 0; s < spans.size(); ++s) {
            const FootContact& c = spans[s];
            // Ramps shrink to fit the gaps between contacts.
            const int gap_before = s ? c.from - spans[s - 1].to - 1 : c.from - std::max(0, opt.from);
            const int gap_after = s + 1 < spans.size() ? spans[s + 1].from - c.to - 1 : b - c.to;
            const int rin = std::clamp(gap_before / 2, 0, opt.blend), rout = std::clamp(gap_after / 2, 0, opt.blend);
            const Xform planted = fk[c.from].globals[l.end];
            for (int f = c.from - rin; f <= c.to + rout; ++f) {
                key_limb_target(clip, rig, f, limb, planted, opt.shape);
                key_limb_pole(clip, rig, f, limb, derive_pole(rig, limb, fk[f].globals), opt.shape);
            }
            FCurve& blend = clip.curves[track]["blend"];
            if (c.from - rin > 0) blend.set_key(c.from - rin - (rin ? 0 : 1), 0, Interp::Linear);
            blend.set_key(c.from, 1, Interp::Linear);
            blend.set_key(c.to, 1, Interp::Linear);
            if (c.to + rout < clip.end_frame) blend.set_key(c.to + rout + (rout ? 0 : 1), 0, Interp::Linear);
        }
        char buf[120];
        std::snprintf(buf, sizeof buf, "%s: %zu foot contact%s held still", l.label.c_str(), spans.size(),
                      spans.size() == 1 ? "" : "s");
        report.push_back(buf);
    }
    if (report.empty()) report.push_back("no foot contacts found");
    return report;
}

}  // namespace vats
