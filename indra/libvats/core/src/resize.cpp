// Viewport Avatar Toolset - resizing the skeleton (spec 08 RS).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/resize.h"

#include <algorithm>
#include <cmath>

namespace vats {

double body_resize(const Json& ex) {
    const Json* v = ex.is_object() ? ex.find("size") : nullptr;
    if (!v || !v->is_number() || !std::isfinite(v->num) || v->num <= 0) return 1;
    return std::clamp(v->num, kResizeMin, kResizeMax);
}

Shape resized_shape(const Skeleton& skel, const Shape* base, double s, double ground_z) {
    Shape out;
    if (base) out = *base;
    const size_t n = size_t(skel.size());
    out.scale.resize(n, {1, 1, 1});
    out.offset.resize(n, {});
    for (size_t i = 0; i < n; ++i) {
        out.scale[i] = out.scale[i] * s;
        // A root-level node's translation meets no parent scale: move its rest out to s times as far. Every other
        // node's translation is scaled by its parent's scale, now s times the base's.
        if (skel[int(i)].parent < 0) {
            const Vec3 g{0, 0, ground_z};
            out.offset[i] = g + (skel[int(i)].pos + out.offset[i] - g) * s - skel[int(i)].pos;
        }
    }
    for (Vec3& t : out.tails) t = t * s;
    return out;
}

ResizedKeys scale_position_keys(const Skeleton& skel, Clip& clip, double r) {
    ResizedKeys out;
    for_each_track_map(clip, [&](std::map<std::string, Track>& tracks) {
        for (auto& [name, track] : tracks) {
            // An IK controller ("ik.<limb>") holds its target and pole in avatar space, or a finger's in its
            // parent bone's frame, and a pin's offset track ("pin:<joint>") moves the held point in the pin's frame:
            // all grow with the body.
            const bool controller = name.rfind("ik.", 0) == 0 || name.rfind("pin:", 0) == 0;
            const int node = controller ? -1 : skel.find(name);
            if (!controller && (node < 0 || skel[node].parent >= 0)) continue;
            for (const char* ch : {"pos_x", "pos_y", "pos_z", "pole_x", "pole_y", "pole_z"}) {
                auto c = track.find(ch);
                if (c == track.end() || c->second.empty()) continue;
                for (Key& k : c->second.keys) k.value *= r, k.ly *= r, k.ry *= r;
                ++out.channels;
            }
        }
    });
    for (Pin& p : clip.pins) {
        if (!p.target_actor.empty()) continue;  // the partner's bone: its size is the partner's
        p.pos = p.pos * r;
        ++out.pins;
    }
    return out;
}

}  // namespace vats
