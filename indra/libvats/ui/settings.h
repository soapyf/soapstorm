// Viewport Avatar Toolset - application settings, saved as JSON in the user's config folder.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/06 section 5. Every change is saved at once (UI-27).
#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "vats/json.h"
#include "vats/math.h"

namespace vats {

enum class Preset { Industry, Blender, QAvimator, SecondLife };
// "industry", "blender", "qavimator" or "secondlife"; false when unknown.
bool preset_from_name(const std::string& name, Preset& out);
// The label Preferences and Keyboard Shortcuts show: "Industry (Maya-style)", "Blender", "QAvimator", "Second Life".
const char* preset_label(Preset p);
// The presets a host offers, in order. A host that owns the camera (the viewer's world view) offers Second Life
// only, the controls it already has in-world, and so shows no preset picker.
std::vector<Preset> offered_presets(bool host_owns_camera);

// A UTF-8 path for std::filesystem: on Windows a plain std::string would be read in the ANSI code page.
inline std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

struct CameraView {
    bool set = false;
    Vec3 target;
    double yaw = 0, pitch = 0, distance = 3.2;
};

struct Settings {
    Preset preset = Preset::Industry;
    bool emulate_3_button = false;
    float interface_size = 1.0f;  // on top of the display scale
    float gizmo_size = 90;
    float view_cube_size = 110;
    float snap_degrees = 5;
    std::string orientation = "local";  // "local", "world" or "gimbal" (VP-42)
    bool show_graph = true;
    std::string theme = "Dusk";
    bool show_welcome = true;
#ifdef VATS_LEGACY_IMPORT
    bool migration_offered = false;  // IO-54: the first-run legacy import is offered once
#endif
    bool bvh_reduce = false;  // IO-35: key reduction after a BVH import
    bool viewer_reset_joints = true;  // in the viewer: a local skeleton reset as the editor opens (spec 09 U4b)
    bool viewer_show_others = false;  // in the viewer: other avatars stay shown while the editor is open (spec 09 U5)
    bool viewer_keep_swap = true;     // in the viewer: View > Body's swapped body stays in the real-avatar modes (build 34)
    bool mirror_centre = false;        // PT-1: live mirror makes centre bones symmetric in place
    bool scratch_existing_only = false;  // PT-2: a scratch pose keys only channels that already have keys
    std::string scratch_scrub = "ask";   // PT-2: scrubbing off a scratch pose: "ask", "keep" or "discard"
    std::string picker_style = "silhouette";  // 08 PK-3: the Picker's backdrop, "silhouette" or "avatar"
    bool picker_live = true;                  // 08 PK-3: the avatar backdrop follows the pose (else the rest pose)
    std::string body = "sl-default";
    std::string mesh_body;  // id of a library mesh body shown instead of the Linden mesh ("" = none)
    std::array<CameraView, 4> cameras;
    std::vector<std::string> recent;
    std::vector<std::string> project_folders, anim_folders;  // Inventory folders added with Add Folder... (spec 08 FL)
    std::vector<std::string> check_off;  // Animation Check rules switched off, by id (spec 08 CK)
    std::vector<std::string> inventory_closed;  // the Inventory's collapsed sections, by name
    bool mixamo_notice_seen = false;     // 07 RT-14: Batch Retarget showed the Mixamo licence notice once
    // The user's own shortcuts over the preset (ui/keymap.h): action id -> both slots, "" for none.
    std::map<std::string, std::array<std::string, 2>> key_overrides;
    Json mocap = Json::object();  // Motion Capture and face-tracking choices, read and written by mocap_ui.cpp

    // file: the host's settings.json (ui::Paths::settings; --data-dir puts it there too, IO-53).
    void load(const std::string& file);
    void save(const std::string& file) const;
    void add_recent(const std::string& file);  // most recent first, at most 10, no duplicates; not saved
};

}  // namespace vats
