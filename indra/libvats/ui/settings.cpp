// Viewport Avatar Toolset - application settings, saved as JSON in the user's config folder.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "vats/json.h"

namespace vats {
namespace {

const char* const kPresets[] = {"industry", "blender", "qavimator", "secondlife"};

std::string normalise(const std::string& path) {
    // Absolute and without "." / ".." parts, so the same file is listed once.
    std::string p = path;
    if (!p.empty() && p[0] != '/' && p.find(':') == std::string::npos) {
        std::error_code ec;
        if (auto cwd = std::filesystem::current_path(ec).u8string(); !ec) p = std::string(cwd.begin(), cwd.end()) + "/" + p;
    }
    std::vector<std::string> parts;
    std::stringstream ss(p);
    for (std::string part; std::getline(ss, part, '/');) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(part);
        }
    }
    std::string out;
    for (auto& part : parts) out += "/" + part;
    return p.find(':') != std::string::npos ? path : out;  // leave Windows paths alone
}

}  // namespace

void Settings::load(const std::string& file) {
    std::ifstream f(u8path(file), std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    Json j;
    std::string err;
    if (!parse_json(ss.str(), j, err) || !j.is_object()) return;  // unknown or broken: defaults (06 section 5)
    auto num = [&](const char* k, auto& v, double lo, double hi) {
        if (auto* x = j.find(k); x && x->is_number()) v = static_cast<std::remove_reference_t<decltype(v)>>(std::clamp(x->num, lo, hi));
    };
    auto boolean = [&](const char* k, bool& v) {
        if (auto* x = j.find(k); x && x->is_bool()) v = x->b;
    };
    auto str = [&](const char* k, std::string& v) {
        if (auto* x = j.find(k); x && x->is_string()) v = x->str;
    };
    if (auto* x = j.find("preset"); x && x->is_string()) {
        preset_from_name(x->str, preset);
    } else {
        // An existing settings file with no preset key was saved before presets, when Industry was default.
        // Keep Industry for backwards compatibility so existing configs do not flip unexpectedly.
        preset = Preset::Industry;
    }
    boolean("reduce_motion", reduce_motion);
    boolean("emulate_3_button", emulate_3_button);
    num("interface_size", interface_size, 0.5, 3.0);
    num("gizmo_size", gizmo_size, 50, 220);
    num("view_cube_size", view_cube_size, 60, 260);
    num("snap_degrees", snap_degrees, 1, 90);
    bool local_axes = true;  // before VP-42 only local/world was kept
    boolean("local_axes", local_axes);
    orientation = local_axes ? "local" : "world";
    str("orientation", orientation);
    boolean("bvh_reduce", bvh_reduce);
    boolean("viewer_reset_joints", viewer_reset_joints);
    boolean("viewer_show_others", viewer_show_others);
    boolean("viewer_keep_swap", viewer_keep_swap);
    boolean("mixamo_notice_seen", mixamo_notice_seen);
    boolean("mirror_centre", mirror_centre);
    boolean("auto_ik", auto_ik);
    boolean("follow_through", follow_through);
    boolean("show_weights", show_weights);
    boolean("avatar_physics", avatar_physics);
    if (const Json* v = j.find("physics"); v && v->is_object()) physics = *v;
    // Saved as "show_unused_bones" since hiding became the default; the old key (saved off by default) is ignored.
    bool show_unused = !hide_unused_bones;
    boolean("show_unused_bones", show_unused);
    hide_unused_bones = !show_unused;
    boolean("plain_bone_names", plain_bone_names);
    boolean("respect_joint_limits", respect_joint_limits);
    str("bone_style", bone_style);
    if (bone_style == "glyph") bone_style = "stick";
    if (bone_style != "stick" && bone_style != "hidden") bone_style.clear();
    boolean("scratch_existing_only", scratch_existing_only);
    str("scratch_scrub", scratch_scrub);
    str("picker_style", picker_style);
    boolean("picker_live", picker_live);
    boolean("show_graph", show_graph);
    str("theme", theme);
    boolean("show_welcome", show_welcome);
#ifdef VATS_LEGACY_IMPORT
    boolean("migration_offered", migration_offered);
#endif
    str("body", body);
    str("mesh_body", mesh_body);
    if (auto* cams = j.find("cameras"); cams && cams->is_array())
        for (size_t i = 0; i < cams->arr.size() && i < cameras.size(); ++i) {
            const Json& c = cams->arr[i];
            auto* t = c.find("target");
            if (!c.is_object() || !t || !t->is_array() || t->arr.size() != 3) continue;
            CameraView& v = cameras[i];
            v.set = true;
            v.target = {t->arr[0].num, t->arr[1].num, t->arr[2].num};
            if (auto* x = c.find("yaw")) v.yaw = x->num;
            if (auto* x = c.find("pitch")) v.pitch = x->num;
            if (auto* x = c.find("distance")) v.distance = x->num;
        }
    if (auto* m = j.find("mocap"); m && m->is_object()) mocap = *m;
    if (j.find("workspace")) {
        boolean("workspaces", workspaces);
        str("workspace", workspace);
    } else {
        workspace = "all";  // saved before workspaces: the full layout, as it was
    }
    // 0.2.0: the tabs are on for everyone, once. An install that had them off (the trial's default for existing users)
    // gets them, on All so its layout stays; turned off again after this, they stay off.
    if (!j.find("workspace_tabs_default_on")) workspaces = true;
    if (auto* o = j.find("workspace_layouts"); o && o->is_object())
        for (auto& [id, v] : o->obj)
            if (v.is_string()) workspace_layouts[id] = v.str;
    if (auto* o = j.find("workspace_extra"); o && o->is_object())
        for (auto& [id, v] : o->obj)
            for (size_t i = 0; v.is_array() && i < v.arr.size(); ++i)
                if (v.arr[i].is_string()) workspace_extra[id].push_back(v.arr[i].str);
    if (auto* o = j.find("key_overrides"); o && o->is_object())
        for (auto& [id, v] : o->obj)
            if (v.is_array() && v.arr.size() == 2 && v.arr[0].is_string() && v.arr[1].is_string())
                key_overrides[id] = {v.arr[0].str, v.arr[1].str};
    if (auto* r = j.find("recent"); r && r->is_array())
        for (auto& e : r->arr)
            if (e.is_string() && recent.size() < 10) recent.push_back(e.str);
    for (auto [key, list] : {std::pair{"project_folders", &project_folders}, std::pair{"anim_folders", &anim_folders},
                             std::pair{"check_off", &check_off}, std::pair{"inventory_closed", &inventory_closed}})
        if (auto* r = j.find(key); r && r->is_array())
            for (auto& e : r->arr)
                if (e.is_string()) list->push_back(e.str);
}

void Settings::save(const std::string& file) const {
    Json j = Json::object();
    j.set("preset", kPresets[int(preset)]);
    j.set("reduce_motion", reduce_motion);
    j.set("emulate_3_button", emulate_3_button);
    j.set("interface_size", double(interface_size));
    j.set("gizmo_size", double(gizmo_size));
    j.set("view_cube_size", double(view_cube_size));
    j.set("snap_degrees", double(snap_degrees));
    j.set("orientation", orientation);
    j.set("bvh_reduce", bvh_reduce);
    j.set("viewer_reset_joints", viewer_reset_joints);
    j.set("viewer_show_others", viewer_show_others);
    j.set("viewer_keep_swap", viewer_keep_swap);
    j.set("mixamo_notice_seen", mixamo_notice_seen);
    j.set("mirror_centre", mirror_centre);
    j.set("auto_ik", auto_ik);
    j.set("follow_through", follow_through);
    j.set("show_weights", show_weights);
    j.set("avatar_physics", avatar_physics);
    j.set("physics", physics);
    j.set("show_unused_bones", !hide_unused_bones);
    j.set("plain_bone_names", plain_bone_names);
    j.set("respect_joint_limits", respect_joint_limits);
    if (!bone_style.empty()) j.set("bone_style", bone_style);
    j.set("scratch_existing_only", scratch_existing_only);
    j.set("scratch_scrub", scratch_scrub);
    j.set("picker_style", picker_style);
    j.set("picker_live", picker_live);
    j.set("show_graph", show_graph);
    j.set("theme", theme);
    j.set("show_welcome", show_welcome);
#ifdef VATS_LEGACY_IMPORT
    j.set("migration_offered", migration_offered);
#endif
    j.set("body", body);
    j.set("mesh_body", mesh_body);
    Json cams = Json::array();
    for (auto& v : cameras) {
        if (!v.set) {
            cams.push(Json());
            continue;
        }
        Json c = Json::object(), t = Json::array();
        t.push(v.target.x), t.push(v.target.y), t.push(v.target.z);
        c.set("target", t);
        c.set("yaw", v.yaw), c.set("pitch", v.pitch), c.set("distance", v.distance);
        cams.push(c);
    }
    j.set("cameras", cams);
    Json r = Json::array();
    for (auto& f : recent) r.push(f);
    j.set("recent", r);
    for (auto [key, list] : {std::pair{"project_folders", &project_folders}, std::pair{"anim_folders", &anim_folders},
                             std::pair{"check_off", &check_off}, std::pair{"inventory_closed", &inventory_closed}}) {
        Json a = Json::array();
        for (auto& f : *list) a.push(f);
        j.set(key, a);
    }
    j.set("mocap", mocap);
    j.set("workspaces", workspaces);
    j.set("workspace_tabs_default_on", true);
    j.set("workspace", workspace);
    Json layouts = Json::object(), extra = Json::object();
    for (auto& [id, ini] : workspace_layouts) layouts.set(id, ini);
    for (auto& [id, list] : workspace_extra) {
        Json a = Json::array();
        for (auto& w : list) a.push(w);
        extra.set(id, a);
    }
    j.set("workspace_layouts", layouts);
    j.set("workspace_extra", extra);
    Json keys = Json::object();
    for (auto& [id, k] : key_overrides) {
        Json pair = Json::array();
        pair.push(k[0]), pair.push(k[1]);
        keys.set(id, pair);
    }
    j.set("key_overrides", keys);
    std::string p = file, tmp = p + ".tmp";
    {
        std::ofstream f(u8path(tmp), std::ios::binary | std::ios::trunc);
        f << write_json(j);
        if (!f) return;
    }
    // filesystem::rename replaces the old file on Windows too (std::rename refuses there, so only the first save
    // ever stuck); u8path keeps a non-ASCII %APPDATA% working.
    std::error_code ec;
    std::filesystem::rename(u8path(tmp), u8path(p), ec);
}

void Settings::add_recent(const std::string& file) {
    std::string n = normalise(file);
    recent.erase(std::remove(recent.begin(), recent.end(), n), recent.end());
    recent.insert(recent.begin(), n);
    if (recent.size() > 10) recent.resize(10);
}

bool preset_from_name(const std::string& name, Preset& out) {
    for (int i = 0; i < 4; ++i)
        if (name == kPresets[i]) return out = Preset(i), true;
    return false;
}

const char* preset_label(Preset p) {
    static const char* const labels[] = {"Industry (Maya-style)", "Blender", "QAvimator", "Second Life"};
    return labels[int(p)];
}

std::vector<Preset> offered_presets(bool host_owns_camera) {
    if (host_owns_camera) return {Preset::SecondLife};
    return {Preset::Industry, Preset::Blender, Preset::QAvimator, Preset::SecondLife};
}

}  // namespace vats
