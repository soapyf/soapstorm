// Viewport Avatar Toolset - props in a project, and the prop library.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/03 sections 3.4.2 (props[]), 3.6 (prop library), IO-42 (relative paths), IO-43
// (unknown fields are kept).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "vats/json.h"
#include "vats/math.h"

namespace vats {

struct Prop {
    std::string path;   // as stored: relative to the project (see prop_path_*) or absolute
    std::string name;   // defaults to the file stem of path
    std::string bone;   // parent bone, "" = world
    std::string point;  // attachment point; wins over bone for placement
    Vec3 pos;           // metres, SL space, in the parent's frame
    Vec3 rot;           // project Euler, degrees
    Vec3 scale{1, 1, 1};
    bool visible = true;
    bool rigged = false;
    std::string lib_id;  // prop-library item, "" = none
    Json extra = Json::object();  // unknown fields, written back in their order

    bool operator==(const Prop&) const = default;
};

// props[] of a project. A number for scale is uniform scale (older files). On failure returns false,
// sets err ("props[2].pos: ...") and leaves out unchanged.
bool props_from_json(const Json& arr, std::vector<Prop>& out, std::string& err);
Json props_to_json(const std::vector<Prop>& props);

// IO-42: the stored form of a prop path, relative to project_dir when the two share a root, absolute
// otherwise (and always when project_dir is empty, e.g. an untitled project). '/' separators.
std::string prop_path_to_stored(const std::string& absolute, const std::string& project_dir);
// The absolute path of a stored prop path; absolute paths pass through.
std::string prop_path_from_stored(const std::string& stored, const std::string& project_dir);

// Prop library item (library.json, 03 section 3.6). prop.visible and prop.lib_id are not stored.
struct PropLibraryItem {
    std::string id;
    Prop prop;  // path (absolute), name, rigged, bone, point, pos, rot, scale
};

// Reads "vats-prop-library" (and, with VATS_LEGACY_IMPORT, a legacy one). On failure returns false and sets err.
bool load_prop_library(std::string_view text, std::vector<PropLibraryItem>& out, std::string& err);
// Writes "vats-prop-library", tab-indented.
std::string save_prop_library(const std::vector<PropLibraryItem>& items);
// A new random item id (UUID v4 text).
std::string new_library_id();

// VP-85: exactly "<x, y, z>" (whitespace anywhere between parts, signed decimals and exponents).
bool parse_sl_vector(std::string_view text, Vec3& out);

}  // namespace vats
