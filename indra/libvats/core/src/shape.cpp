// Viewport Avatar Toolset - body shapes from the viewer's visual-param system (avatar_lad.xml).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/shape.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "vats/xml.h"

namespace vats {
namespace {

Vec3 parse_vec(const std::string& s) {
    // strtod rather than sscanf: MSVC's /WX build (the viewer's) rejects sscanf as deprecated.
    const char* p = s.c_str();
    double v[3];
    for (double& x : v) {
        char* end = nullptr;
        x = std::strtod(p, &end);
        if (end == p) return {};
        p = end;
    }
    return {v[0], v[1], v[2]};
}

float attr_f(const XmlNode& n, const char* key, float fallback) {
    auto* a = n.attr(key);
    return a ? std::strtof(a->c_str(), nullptr) : fallback;
}

int mesh_index(const XmlNode& mesh) {
    if (mesh.attr_or("lod") != "0") return -1;
    static const char* types[ShapeMeshCount] = {"headMesh", "upperBodyMesh", "lowerBodyMesh", "eyelashMesh"};
    for (int i = 0; i < ShapeMeshCount; ++i)
        if (mesh.attr_or("type") == types[i]) return i;
    return -1;
}

void read_param(const XmlNode& x, int mesh, AvatarParams& out) {
    int id = std::atoi(x.attr_or("id").c_str());
    // The same id appears once per mesh a morph spans (head and eyelashes): one weight, several morphs.
    bool seen = out.count(id) != 0;
    VisualParam& p = out[id];
    if (!seen) {
        p.name = x.attr_or("name");
        p.min = attr_f(x, "value_min", 0);
        p.max = attr_f(x, "value_max", 1);
        // Viewer: an explicit default is clamped to the range; a missing one is 0, unclamped.
        p.def = x.attr("value_default") ? std::clamp(attr_f(x, "value_default", 0), p.min, p.max) : 0.0f;
        std::string sex = x.attr_or("sex", "both");
        p.sex = sex == "male" ? 2 : sex == "female" ? 1 : 3;
    }
    for (auto& c : x.children) {
        if (c.name == "param_driver") {
            for (auto& d : c.children) {
                if (d.name != "driven") continue;
                VisualParam::Driven e;
                e.id = std::atoi(d.attr_or("id").c_str());
                e.min1 = attr_f(d, "min1", p.min);
                e.max1 = attr_f(d, "max1", p.max);
                e.max2 = attr_f(d, "max2", e.max1);
                e.min2 = attr_f(d, "min2", e.max1);
                p.driven.push_back(e);
            }
        } else if (c.name == "param_skeleton") {
            for (auto& b : c.children)
                if (b.name == "bone")
                    p.bones.push_back({b.attr_or("name"), parse_vec(b.attr_or("scale")), parse_vec(b.attr_or("offset"))});
        } else if (c.name == "param_morph") {
            if (mesh >= 0) p.morphs.push_back({mesh, p.name});
            // Its collision-volume morphs, once (the same param is repeated for each mesh it spans).
            for (auto& v : c.children)
                if (v.name == "volume_morph" && std::none_of(p.volumes.begin(), p.volumes.end(),
                                                             [&](const VisualParam::Bone& b) { return b.name == v.attr_or("name"); }))
                    p.volumes.push_back({v.attr_or("name"), parse_vec(v.attr_or("scale")), parse_vec(v.attr_or("pos"))});
        }
    }
}

void walk(const XmlNode& n, int mesh, AvatarParams& out) {
    if (n.name == "mesh") mesh = mesh_index(n);
    for (auto& c : n.children) {
        if (c.name == "param")
            read_param(c, mesh, out);
        else
            walk(c, mesh, out);
    }
}

// LLDriverParam::getDrivenWeight.
float driven_weight(const VisualParam& driver, const VisualParam::Driven& e, const VisualParam& driven, float w) {
    if (w <= e.min1) return e.min1 == e.max1 && e.min1 <= driver.min ? driven.max : driven.min;
    if (w <= e.max1) return driven.min + (w - e.min1) / (e.max1 - e.min1) * (driven.max - driven.min);
    if (w <= e.max2) return driven.max;
    if (w <= e.min2) return driven.max + (w - e.max2) / (e.min2 - e.max2) * (driven.min - driven.max);
    return e.max2 >= driver.max ? driven.max : driven.min;
}

}  // namespace

bool parse_avatar_params(std::string_view lad_xml, AvatarParams& out, std::string& err) {
    out.clear();
    XmlNode root;
    if (!parse_xml(lad_xml, root, err)) {
        err = "avatar_lad.xml: " + err;
        return false;
    }
    walk(root, -1, out);
    if (!out.count(80)) {
        err = "avatar_lad.xml: no \"male\" param (id 80)";
        return false;
    }
    return true;
}

BodyShape evaluate_shape(const Skeleton& skel, const AvatarParams& params, const std::map<int, float>& weights) {
    std::map<int, float> w;
    for (auto& [id, p] : params) {
        auto o = weights.find(id);
        w[id] = o != weights.end() ? std::clamp(o->second, p.min, p.max) : std::clamp(p.def, p.min, p.max);
    }

    // Drivers not driven by anything push their weight down the chain (LLDriverParam::setWeight).
    std::map<int, bool> is_driven;
    for (auto& [id, p] : params)
        for (auto& e : p.driven) is_driven[e.id] = true;
    std::function<void(int, float, int)> set = [&](int id, float v, int depth) {
        const VisualParam& p = params.at(id);
        w[id] = std::clamp(v, p.min, p.max);
        if (depth > 16) return;  // a cycle in a hand-edited file
        for (auto& e : p.driven) {
            auto d = params.find(e.id);
            if (d != params.end()) set(e.id, driven_weight(p, e, d->second, w[id]), depth + 1);
        }
    };
    for (auto& [id, p] : params)
        if (!p.driven.empty() && !is_driven.count(id)) set(id, w[id], 0);

    BodyShape out;
    out.shape.scale.assign(skel.size(), {1, 1, 1});
    out.shape.offset.assign(skel.size(), {});
    const int avatar_sex = w[80] > 0.5f ? 2 : 1;
    for (auto& [id, p] : params) {
        // LLPolySkeletalDistortion / LLPolyMorphTarget::apply: a param for the other sex stays at its default.
        float eff = (p.sex & avatar_sex) ? w[id] : p.def;
        if (eff == 0) continue;
        for (auto& b : p.bones) {
            int i = skel.find(b.name);
            if (i < 0 || skel[i].name != b.name) continue;  // collision volumes (BELLY, ...): not in Shape
            out.shape.scale[i] += b.scale * eff;
            out.shape.offset[i] += b.offset * eff;
        }
        for (auto& [mesh, name] : p.morphs) out.morphs[mesh].push_back({name, eff});
        // LLPolyMorphTarget::apply: a volume morph adds to the volume's scale and to its position against its joint.
        for (auto& b : p.volumes)
            if (const int v = skel.find_volume(b.name); v >= 0) {
                const CollisionVolume& cv = skel.volumes()[size_t(v)];
                out.shape.scale[cv.node] += Vec3{b.scale.x / cv.scale.x, b.scale.y / cv.scale.y, b.scale.z / cv.scale.z} * eff;
                out.shape.offset[cv.node] += b.offset * eff;
            }
    }

    // Keep the feet on the ground (SK-28).
    int foot = skel.find("mFootLeft");
    if (foot >= 0) {
        Pose zero(skel.size());
        out.shape.offset[0].z += skel.global_pose(zero)[foot].pos.z - skel.global_pose(zero, &out.shape)[foot].pos.z;
    }
    return out;
}

}  // namespace vats
