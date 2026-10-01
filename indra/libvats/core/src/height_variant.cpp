// Viewport Avatar Toolset - height-variant exports: the same animation baked for avatars of other heights.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/height_variant.h"

#include <algorithm>
#include <cmath>

namespace vats {

double avatar_height(const Skeleton& skel, const Shape* shape) {
    const Pose rest(skel.size());
    auto z = [&](const char* name) {
        const int i = skel.find(name);
        return i < 0 ? 0.0 : skel.local_xform(i, rest, shape).pos.z;
    };
    const double pelvis_to_foot = z("mHipLeft") - z("mKneeLeft") - z("mAnkleLeft") - z("mFootLeft");
    const double body = pelvis_to_foot + std::sqrt(2.0) * z("mSkull") + z("mHead") + z("mNeck") + z("mChest") + z("mTorso");
    return body + kShapeEditorExtra;
}

BodyShape height_shape(const Skeleton& skel, const AvatarParams& params, bool male, double height_m, double* reached) {
    constexpr int kHeight = 33, kLegLength = 692;  // avatar_lad.xml "Height", "Leg Length"
    // s in [-2, 2]: -1..1 moves Height from its minimum through its default to its maximum; beyond that, Leg Length
    // takes the rest, so each step of s makes a taller avatar.
    auto slider = [&](int id, double u) {  // u in [-1, 1] -> the param's range, 0 at its default
        const auto p = params.find(id);
        if (p == params.end()) return 0.0f;
        const VisualParam& v = p->second;
        return float(u < 0 ? v.def + u * (v.def - v.min) : v.def + u * (v.max - v.def));
    };
    auto make = [&](double s) {
        const double over = std::clamp(std::fabs(s) - 1, 0.0, 1.0) * (s < 0 ? -1 : 1);
        return evaluate_shape(skel, params,
                              {{80, male ? 1.0f : 0.0f}, {kHeight, slider(kHeight, std::clamp(s, -1.0, 1.0))},
                               {kLegLength, slider(kLegLength, over)}});
    };
    double lo = -2, hi = 2;
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) / 2;
        const BodyShape b = make(mid);
        (avatar_height(skel, &b.shape) < height_m ? lo : hi) = mid;
    }
    BodyShape best = make((lo + hi) / 2);
    if (reached) *reached = avatar_height(skel, &best.shape);
    return best;
}

std::vector<double> export_heights(const Json& ex) {
    std::vector<double> out;
    const Json* h = ex.is_object() ? ex.find("heights") : nullptr;
    if (!h || !h->is_array()) return out;
    for (const Json& v : h->arr)
        if (v.is_number() && std::isfinite(v.num) && int(out.size()) < kMaxHeights)
            out.push_back(std::clamp(v.num, kHeightMin, kHeightMax));
    return out;
}

std::string height_tag(double height_m) { return "H" + std::to_string(std::lround(height_m * 100)); }

std::vector<AnimVariant> anim_variants(bool mirrored, bool both, const std::vector<double>& heights) {
    std::vector<AnimVariant> out;
    std::vector<double> all{0};
    all.insert(all.end(), heights.begin(), heights.end());
    for (double h : all) {
        out.push_back({mirrored, h});
        if (both) out.push_back({!mirrored, h});
    }
    return out;
}

std::string variant_file_name(const ExportNaming& naming, const std::string& stem, const AnimVariant& v,
                              const std::string& ext) {
    std::string name = export_file_name(naming, stem, v.mirrored, ext);
    if (v.height > 0) name.insert(name.size() - ext.size() - 1, "_" + height_tag(v.height));
    return name;
}

}  // namespace vats
