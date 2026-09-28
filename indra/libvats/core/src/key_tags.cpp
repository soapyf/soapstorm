// Viewport Avatar Toolset - key tags (Extreme, Breakdown, Hold) and blocking mode.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/key_tags.h"

#include <algorithm>
#include <cmath>

namespace vats {

const char* key_tag_name(KeyTag tag) {
    switch (tag) {
        case KeyTag::Extreme: return "Extreme";
        case KeyTag::Breakdown: return "Breakdown";
        case KeyTag::Hold: return "Hold";
        case KeyTag::None: break;
    }
    return "";
}

int tag_keys(Clip& clip, const std::vector<KeyRef>& sel, KeyTag tag) {
    int n = 0;
    for (const KeyRef& r : sel) {
        auto t = clip.curves.find(r.track);
        if (t == clip.curves.end()) continue;
        auto c = t->second.find(r.channel);
        if (c == t->second.end() || r.index < 0 || r.index >= int(c->second.keys.size())) continue;
        Key& k = c->second.keys[r.index];
        n += k.tag != tag;
        k.tag = tag;
    }
    return n;
}

int tag_keys_at(Clip& clip, const std::vector<std::string>& tracks, double frame, KeyTag tag) {
    int n = 0;
    for (const std::string& name : tracks) {
        auto t = clip.curves.find(name);
        if (t == clip.curves.end()) continue;
        for (auto& [ch, curve] : t->second)
            if (int i = curve.find(frame); i >= 0) {
                n += curve.keys[i].tag != tag;
                curve.keys[i].tag = tag;
            }
    }
    return n;
}

int step_new_keys(Clip& clip, const Clip& before) {
    int n = 0;
    for (auto& [name, track] : clip.curves) {
        auto bt = before.curves.find(name);
        for (auto& [ch, curve] : track) {
            const FCurve* old = nullptr;
            if (bt != before.curves.end())
                if (auto c = bt->second.find(ch); c != bt->second.end()) old = &c->second;
            if (old && curve.keys.size() <= old->keys.size()) continue;  // moved or deleted keys are not new ones
            // Both key lists are sorted: walk them together.
            size_t j = 0;
            for (Key& k : curve.keys) {
                while (old && j < old->keys.size() && old->keys[j].frame < k.frame && !same_frame(old->keys[j].frame, k.frame)) ++j;
                const bool existed = old && j < old->keys.size() && same_frame(old->keys[j].frame, k.frame);
                if (existed || k.interp == Interp::Constant) continue;
                k.interp = Interp::Constant;
                ++n;
            }
        }
    }
    return n;
}

int blocking_to_spline(Clip& clip, double drift) {
    int holds = 0;
    for (auto& [name, track] : clip.curves)
        for (auto& [ch, curve] : track) {
            if (ch == "blend") continue;  // IK switches stay stepped
            auto& ks = curve.keys;
            for (size_t i = 0; i < ks.size(); ++i) {
                ks[i].interp = Interp::Bezier;
                ks[i].left = ks[i].right = Handle::AutoClamped;
            }
            if (ch.rfind("rot_", 0) == 0)
                for (size_t i = 0; i + 1 < ks.size(); ++i) {
                    Key &a = ks[i], &b = ks[i + 1];
                    if (a.tag != KeyTag::Hold || b.tag != KeyTag::Hold || std::fabs(b.value - a.value) >= drift) continue;
                    // Towards the key after the pair, never past it; with none (or level), onwards from the key before.
                    double dir = 0, room = drift;
                    if (i + 2 < ks.size()) dir = ks[i + 2].value - a.value, room = std::min(drift, std::fabs(dir));
                    if (std::fabs(dir) < 1e-9 && i > 0) dir = a.value - ks[i - 1].value, room = drift;
                    if (std::fabs(dir) < 1e-9) continue;
                    b.value = a.value + std::copysign(room, dir);
                    a.right = b.left = Handle::Plateau;  // the drift stays between the two values
                    ++holds;
                }
            curve.recompute_handles();
        }
    return holds;
}

}  // namespace vats
