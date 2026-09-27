// Viewport Avatar Toolset - application settings, saved as JSON in the user's config folder.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/06 section 5. Every change is saved at once (UI-27).
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "vats/json.h"
#include "vats/math.h"

namespace vats {

enum class Preset { Industry, Blender, QAvimator, SecondLife };
// "industry", "blender", "qavimator" or "secondlife"; false when unknown.
bool preset_from_name(const std::string& name, Preset& out);

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
    bool migration_offered = false;  // IO-54: the reference-app import is offered once
    bool bvh_reduce = false;  // IO-35: key reduction after a BVH import
    std::string body = "sl-default";
    std::string mesh_body;  // id of a library mesh body shown instead of the Linden mesh ("" = none)
    std::array<CameraView, 4> cameras;
    std::vector<std::string> recent;
    Json mocap = Json::object();  // Motion Capture and face-tracking choices, read and written by mocap_ui.cpp

    // file: the host's settings.json (ui::Paths::settings; --data-dir puts it there too, IO-53).
    void load(const std::string& file);
    void save(const std::string& file) const;
    void add_recent(const std::string& file);  // most recent first, at most 10, no duplicates; not saved
};

}  // namespace vats
