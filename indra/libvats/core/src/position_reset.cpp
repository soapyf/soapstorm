// Viewport Avatar Toolset - joint position reset and unended position detection.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/position_reset.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "vats/anim_convert.h"
#include "vats/curve_ops.h"
#include "vats/edit.h"
#include "vats/rig.h"

namespace vats {
namespace {

std::string json_str(const Json& obj, const char* key, const std::string& fallback = "") {
    const Json* v = obj.find(key);
    return v && v->is_string() ? v->str : fallback;
}

bool json_bool(const Json& obj, const char* key) {
    const Json* v = obj.find(key);
    return v && v->is_bool() && v->b;
}

}  // namespace

std::vector<std::string> joints_ending_moved_by_position(const Skeleton& skel, const Clip& clip) {
    std::vector<std::string> moved;
    const int last = std::max(clip.end_frame, 1);
    for (const auto& [name, track] : clip.curves) {
        const int n = skel.find(name);
        if (n <= 0 || skel[n].name != name) continue;
        if (!clip.has_channels(name, kPosChannels)) continue;
        if (curve_offset(clip, name, last).length() >= kAnimPositionStep) {
            moved.push_back(name);
        }
    }
    std::sort(moved.begin(), moved.end());
    return moved;
}

std::vector<std::string> joints_moved_by_position(const Skeleton& skel, const Clip& clip) {
    std::vector<std::string> moved;
    for (const auto& [name, track] : clip.curves) {
        const int n = skel.find(name);
        if (n <= 0 || skel[n].name != name) continue;
        if (writes_position(clip, name, kAnimPositionStep)) moved.push_back(name);
    }
    std::sort(moved.begin(), moved.end());
    return moved;
}

bool writes_position(const Clip& clip, const std::string& name, double tol) {
    if (!clip.has_channels(name, kPosChannels)) return false;
    for (int f = 0; f <= std::max(clip.end_frame, 1); ++f)
        if (curve_offset(clip, name, f).length() > tol) return true;
    return false;
}

double position_tolerance(const Json& export_settings) {
    if (const Json* r = export_settings.find("reduce"); r && r->is_array() && r->arr.size() == 2 && r->arr[1].is_number())
        return r->arr[1].num;
    return AnimExportOptions{}.reduce_pos_m;
}

std::vector<std::string> clip_rotated_joints(const Skeleton& skel, const Clip& clip) {
    std::set<std::string> names;
    for (const auto& [name, track] : clip.curves) {
        const int n = skel.find(name);
        if (n <= 0 || skel[n].name != name) continue;
        if (clip.has_channels(name, kRotChannels)) names.insert(name);
    }
    Rig rig(skel);
    for (const auto& l : rig.limbs()) {
        if (!uses_ik(clip, l)) continue;
        if (l.root > 0) names.insert(skel[l.root].name);
        if (l.mid > 0) names.insert(skel[l.mid].name);
        if (!l.spine && l.end > 0) names.insert(skel[l.end].name);
    }
    return std::vector<std::string>(names.begin(), names.end());
}

std::vector<std::string> other_clips_position_joints(const Skeleton& skel, const std::vector<const Clip*>& other_clips) {
    std::set<std::string> names;
    for (const Clip* oc : other_clips) {
        if (!oc) continue;
        for (const auto& [name, track] : oc->curves) {
            const int n = skel.find(name);
            if (n <= 0 || skel[n].name != name) continue;
            if (writes_position(*oc, name, position_tolerance(oc->export_settings))) names.insert(name);
        }
    }
    return std::vector<std::string>(names.begin(), names.end());
}

Vec3 rest_joint_position(const Skeleton& skel, int node, const Shape* shape) {
    if (node <= 0 || node >= skel.size()) return {};
    Vec3 rest = skel[node].pos;
    if (shape && size_t(node) < shape->offset.size()) rest += shape->offset[node];
    for (int a = 0; a < 3; ++a) {
        rest[a] = std::clamp(rest[a], -double(kAnimMaxOffset), double(kAnimMaxOffset));
    }
    return rest;
}

std::vector<std::array<std::uint16_t, 4>> rest_position_keys(const Skeleton& skel, int node, const Shape* shape) {
    const Vec3 rest = rest_joint_position(skel, node, shape);
    const auto c = encode_position(rest);
    return {{0, c[0], c[1], c[2]}, {65535, c[0], c[1], c[2]}};
}

AnimJoint make_rest_joint(const Skeleton& skel, const std::string& name, int priority, const Shape* shape) {
    AnimJoint r;
    r.name = name;
    r.priority = priority;
    const int n = skel.find_viewer(name);
    if (n > 0) {
        r.pos = rest_position_keys(skel, n, shape);
    }
    return r;
}

std::vector<std::string> resolve_reset_position_joints(const Skeleton& skel, const Clip& clip,
                                                       const std::vector<const Clip*>& other_clips,
                                                       const Json& export_settings) {
    if (!json_bool(export_settings, "reset_positions")) return {};
    const std::string mode = json_str(export_settings, "reset_positions_mode", "rotated");
    std::vector<std::string> raw;
    if (mode == "rotated") {
        raw = clip_rotated_joints(skel, clip);
    } else if (mode == "other_clips") {
        raw = other_clips_position_joints(skel, other_clips);
    } else if (mode == "pick") {
        if (const Json* arr = export_settings.find("reset_positions_joints"); arr && arr->is_array()) {
            for (const auto& v : arr->arr) {
                // Joints only: attachment points and collision volumes have no position an animation moves.
                if (const int n = v.is_string() ? skel.find(v.str) : -1; n > 0 && n < skel.joint_count())
                    raw.push_back(v.str);
            }
        }
    } else {
        raw = clip_rotated_joints(skel, clip);
    }
    const double tol = position_tolerance(export_settings);
    std::vector<std::string> out;
    for (const std::string& name : raw) {
        if (!writes_position(clip, name, tol)) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace vats
