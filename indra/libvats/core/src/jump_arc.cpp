// Viewport Avatar Toolset - Jump Arc (spec 08 section 21, JA-1).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/jump_arc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "vats/curve_ops.h"
#include "vats/edit.h"

namespace vats {

double jump_apex(double t, double dz, double g) {
    if (t <= 0 || g <= 0) return std::max(0.0, dz);
    const double v = dz / t + 0.5 * g * t, top = v / g;  // take-off speed, and when it reaches the top
    return top > 0 && top < t ? v * v / (2 * g) : std::max(0.0, dz);
}

bool jump_arc(Clip& clip, const JumpArcOptions& opt, std::string& msg) {
    const int a = opt.takeoff, b = opt.landing;
    if (a < 0 || b > clip.end_frame || b - a < 2) return msg = "Pick a takeoff frame at least 2 frames before the landing", false;
    if (!(opt.gravity > 0)) return msg = "Gravity must be above 0", false;
    const Vec3 p0 = curve_offset(clip, "mPelvis", a), p1 = curve_offset(clip, "mPelvis", b);
    std::vector<Vec3> old;
    for (int f = a; f <= b; ++f) old.push_back(curve_offset(clip, "mPelvis", f));
    const double fps = std::max(clip.fps, 1), t = (b - a) / fps, g = opt.gravity;
    const double v = (p1.z - p0.z) / t + 0.5 * g * t;
    // Takeoff and landing keep their values, and the curves before and after them their shape.
    for (const char* ch : kPosChannels)
        for (int f : {a, b}) insert_on_curve(clip.curves["mPelvis"][ch], f);
    const Clip ends = clip;
    for (int f = a + 1; f < b; ++f) {
        const double s = (f - a) / fps, u = (f - a) / double(b - a);
        Vec3 p = old[f - a];
        p.z = p0.z + v * s - 0.5 * g * s * s;
        if (opt.forward) p.x = p0.x + (p1.x - p0.x) * u;
        if (!opt.keep_lateral) p.y = p0.y + (p1.y - p0.y) * u;
        key_offset(clip, "mPelvis", f, p);
    }
    keep_outer_handles(clip, ends, "mPelvis", a, b);
    char buf[120];
    std::snprintf(buf, sizeof buf, "Jump Arc: %.2f s in the air, the hips rise %.1f cm", t, jump_apex(t, p1.z - p0.z, g) * 100);
    msg = buf;
    return true;
}

}  // namespace vats
