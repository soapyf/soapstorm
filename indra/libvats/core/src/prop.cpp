// Viewport Avatar Toolset - props in a project, and the prop library.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/prop.h"
#ifdef VATS_LEGACY_IMPORT
#include "vats/legacy_import.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <random>

namespace vats {
namespace {

namespace fs = std::filesystem;

constexpr const char* kLibraryFormat = "vats-prop-library";

Json vec(const Vec3& v) {
    Json a = Json::array();
    a.arr = {v.x, v.y, v.z};
    return a;
}

std::string file_stem(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return fs::path(path).stem().string();
}

// One props[] or library item object. id is read only for library items.
bool read_prop(const Json& o, Prop& p, const std::string& where, std::string& err, std::string* id) {
    auto fail = [&](const std::string& field, const char* what) {
        err = where + "." + field + ": expected " + what;
        return false;
    };
    if (!o.is_object()) {
        err = where + ": expected an object";
        return false;
    }
    bool has_name = false;
    for (auto& [key, v] : o.obj) {
        std::string* s = key == "path" ? &p.path : key == "name" ? &p.name : key == "bone" ? &p.bone
                       : key == "point" ? &p.point : key == "lib_id" ? &p.lib_id : key == "id" && id ? id : nullptr;
        bool* b = key == "visible" ? &p.visible : key == "rigged" ? &p.rigged : nullptr;
        Vec3* xyz = key == "pos" ? &p.pos : key == "rot" ? &p.rot : key == "scale" ? &p.scale : nullptr;
        if (s) {
            if (!v.is_string()) return fail(key, "a string");
            *s = v.str;
            has_name |= key == "name";
        } else if (b) {
            if (!v.is_bool()) return fail(key, "a bool");
            *b = v.b;
        } else if (xyz) {
            if (key == "scale" && v.is_number()) {  // uniform scale (older files)
                *xyz = {v.num, v.num, v.num};
                continue;
            }
            if (!v.is_array() || v.arr.size() != 3 || !std::all_of(v.arr.begin(), v.arr.end(), [](auto& e) { return e.is_number(); }))
                return fail(key, "3 numbers");
            *xyz = {v.arr[0].num, v.arr[1].num, v.arr[2].num};
        } else {
            p.extra.obj.emplace_back(key, v);
        }
    }
    if (!has_name) p.name = file_stem(p.path);
    return true;
}

void write_prop(Json& o, const Prop& p, bool library) {
    o.set("path", p.path);
    o.set("name", p.name);
    o.set("bone", p.bone);
    o.set("point", p.point);
    o.set("pos", vec(p.pos));
    o.set("rot", vec(p.rot));
    o.set("scale", vec(p.scale));
    if (!library) o.set("visible", p.visible);
    o.set("rigged", p.rigged);
    if (!library) o.set("lib_id", p.lib_id);
    for (auto& [k, v] : p.extra.obj)
        if (!o.find(k)) o.obj.emplace_back(k, v);
}

}  // namespace

bool props_from_json(const Json& arr, std::vector<Prop>& out, std::string& err) {
    if (!arr.is_array()) {
        err = "props: expected an array";
        return false;
    }
    std::vector<Prop> props(arr.arr.size());
    for (size_t i = 0; i < props.size(); ++i)
        if (!read_prop(arr.arr[i], props[i], "props[" + std::to_string(i) + "]", err, nullptr)) return false;
    out = std::move(props);
    return true;
}

Json props_to_json(const std::vector<Prop>& props) {
    Json a = Json::array();
    for (auto& p : props) write_prop(a.push(Json::object()), p, false);
    return a;
}

std::string prop_path_to_stored(const std::string& absolute, const std::string& project_dir) {
    fs::path a = fs::path(absolute).lexically_normal();
    if (project_dir.empty() || !a.is_absolute()) return a.generic_string();
    fs::path d = fs::path(project_dir).lexically_normal();
    if (a.root_name() != d.root_name()) return a.generic_string();  // another drive
    fs::path r = a.lexically_relative(d);
    return r.empty() ? a.generic_string() : r.generic_string();
}

std::string prop_path_from_stored(const std::string& stored, const std::string& project_dir) {
    fs::path s(stored);
    if (s.is_absolute() || project_dir.empty()) return s.lexically_normal().string();
    return (fs::path(project_dir) / s).lexically_normal().string();
}

bool load_prop_library(std::string_view text, std::vector<PropLibraryItem>& out, std::string& err) {
    Json doc;
    if (!parse_json(text, doc, err)) return false;
    const Json* format = doc.find("format");
    bool ours = format && format->is_string() && format->str == kLibraryFormat;
#ifdef VATS_LEGACY_IMPORT
    ours = ours || (format && format->is_string() && format->str == legacy_import::kPropLibraryFormat);
#endif
    if (!ours) {
        err = "not a prop library (format is missing or unknown)";
        return false;
    }
    std::vector<PropLibraryItem> items;
    if (const Json* a = doc.find("items")) {
        if (!a->is_array()) {
            err = "items: expected an array";
            return false;
        }
        items.resize(a->arr.size());
        for (size_t i = 0; i < items.size(); ++i)
            if (!read_prop(a->arr[i], items[i].prop, "items[" + std::to_string(i) + "]", err, &items[i].id))
                return false;
    }
    out = std::move(items);
    return true;
}

std::string save_prop_library(const std::vector<PropLibraryItem>& items) {
    Json doc = Json::object();
    doc.set("format", kLibraryFormat);
    Json& a = doc.set("items", Json::array());
    for (auto& it : items) {
        Json& o = a.push(Json::object());
        o.set("id", it.id);
        write_prop(o, it.prop, true);
    }
    return write_json(doc);
}

std::string new_library_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t hi = rng(), lo = rng();
    hi = (hi & ~0xF000ull) | 0x4000ull;              // version 4
    lo = (lo & ~(3ull << 62)) | (2ull << 62);        // RFC 4122 variant
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", unsigned(hi >> 32), unsigned(hi >> 16 & 0xFFFF),
                  unsigned(hi & 0xFFFF), unsigned(lo >> 48), static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
    return buf;
}

bool parse_sl_vector(std::string_view text, Vec3& out) {
    auto trim = [](std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return s;
    };
    text = trim(text);
    if (text.size() < 2 || text.front() != '<' || text.back() != '>') return false;
    text = text.substr(1, text.size() - 2);
    double v[3];
    for (int i = 0; i < 3; ++i) {
        size_t comma = text.find(',');
        if ((i < 2) != (comma != std::string_view::npos)) return false;
        std::string part(trim(text.substr(0, comma)));
        if (part.empty() || part.find_first_not_of("+-.0123456789eE") != std::string::npos) return false;
        char* end = nullptr;
        v[i] = std::strtod(part.c_str(), &end);
        if (*end || !std::isfinite(v[i])) return false;
        text = i < 2 ? text.substr(comma + 1) : std::string_view{};
    }
    out = {v[0], v[1], v[2]};
    return true;
}

}  // namespace vats
