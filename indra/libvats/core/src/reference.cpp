// Viewport Avatar Toolset - reference pictures (see vats/reference.h).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/reference.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <utility>

namespace vats {

namespace fs = std::filesystem;

namespace {

constexpr double kBehind = 1.5;  // metres from the actor to the scene plane
constexpr const char* kViewNames[] = {"any", "front", "back", "right", "left", "top"};

// The name split around its last run of digits: {prefix, digits, suffix}; digits empty when there are none.
struct Numbered {
    std::string prefix, digits, suffix;
};
Numbered split_number(const std::string& name) {
    size_t end = name.size();
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(name[end - 1]))) --end;
    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(name[begin - 1]))) --begin;
    return {name.substr(0, begin), name.substr(begin, end - begin), name.substr(end)};
}

}  // namespace

Vec3 reference_view_dir(ReferenceView v) {
    switch (v) {
        case ReferenceView::Back: return {-1, 0, 0};
        case ReferenceView::Right: return {0, -1, 0};
        case ReferenceView::Left: return {0, 1, 0};
        case ReferenceView::Top: return {0, 0, 1};
        default: return {1, 0, 0};
    }
}

bool reference_shows(const Reference& r, const Vec3& eye_dir) {
    if (r.hidden) return false;
    if (r.view == ReferenceView::Any) return true;
    return eye_dir.normalized().dot(reference_view_dir(r.view)) >= std::cos(15 * kDegToRad);
}

std::vector<std::string> sequence_files(const std::string& any_picture) {
    const fs::path p(any_picture);
    const Numbered me = split_number(p.filename().string());
    if (me.digits.empty()) return {any_picture};
    std::vector<std::pair<unsigned long long, std::string>> found;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(p.parent_path().empty() ? "." : p.parent_path(), ec)) {
        const std::string name = e.path().filename().string();
        const Numbered n = split_number(name);
        if (n.digits.empty() || n.digits.size() > 18 || n.prefix != me.prefix || n.suffix != me.suffix) continue;
        found.emplace_back(std::stoull(n.digits), (p.parent_path() / name).string());
    }
    if (found.empty()) return {any_picture};  // unreadable folder
    std::sort(found.begin(), found.end());
    std::vector<std::string> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

int sequence_index(const Reference& r, double frame, int clip_fps, int count) {
    if (count <= 0) return -1;
    const double seconds = (frame - r.frame_offset) / std::max(clip_fps, 1);
    const double i = std::floor(seconds * std::max(r.fps, 0.001) + 1e-6);  // a whole frame lands on its picture
    return int(std::clamp(i, 0.0, double(count - 1)));
}

namespace {

std::array<Vec3, 4> flipped(const Reference& r, std::array<Vec3, 4> q) {
    if (r.flip_x) std::swap(q[0], q[1]), std::swap(q[2], q[3]);
    if (r.flip_y) std::swap(q[0], q[3]), std::swap(q[1], q[2]);
    return q;
}

}  // namespace

std::array<Vec3, 4> reference_backdrop_quad(const Reference& r, double view_aspect, double picture_aspect) {
    view_aspect = std::max(view_aspect, 1e-6);
    const double hh = r.scale, hw = r.scale * picture_aspect / view_aspect;  // half sizes; clip space is 2 across
    const double cx = 2 * r.offset_x / view_aspect, cy = 2 * r.offset_y;
    return flipped(r, {{{cx - hw, cy - hh, 0}, {cx + hw, cy - hh, 0}, {cx + hw, cy + hh, 0}, {cx - hw, cy + hh, 0}}});
}

std::array<Vec3, 4> reference_scene_quad(const Reference& r, const Vec3& at, double picture_aspect) {
    const bool top = r.view == ReferenceView::Top;
    const Vec3 n = reference_view_dir(r.view), up = top ? Vec3{1, 0, 0} : Vec3{0, 0, 1}, right = up.cross(n);
    const double h = 2 * r.scale, w = h * picture_aspect;
    Vec3 c = top ? Vec3{at.x, at.y, -0.01} : Vec3{at.x, at.y, h / 2} - n * kBehind;
    c += right * r.offset_x + up * r.offset_y;
    const Vec3 dx = right * (w / 2), dy = up * (h / 2);
    return flipped(r, {c - dx - dy, c + dx - dy, c + dx + dy, c - dx + dy});
}

Json reference_to_json(const Reference& r) {
    Json e = Json::object();
    e.set("path", r.path);
    e.set("sequence", r.sequence);
    e.set("in_scene", r.in_scene);
    e.set("hidden", r.hidden);
    e.set("view", kViewNames[int(r.view)]);
    e.set("opacity", r.opacity);
    e.set("scale", r.scale);
    Json& off = e.set("offset", Json::array());
    off.push(r.offset_x), off.push(r.offset_y);
    e.set("flip_x", r.flip_x);
    e.set("flip_y", r.flip_y);
    e.set("frame_offset", r.frame_offset);
    e.set("fps", r.fps);
    for (auto& [k, x] : r.extra.obj)
        if (!e.find(k)) e.obj.emplace_back(k, x);
    return e;
}

bool reference_from_json(const Json& j, Reference& out, std::string& err) {
    static constexpr const char* known[] = {"path",   "sequence", "in_scene", "hidden", "view",         "opacity", "scale",
                                            "offset", "flip_x",   "flip_y",   "frame_offset", "fps"};
    auto fail = [&](const std::string& what) { return err = "reference: " + what, false; };
    if (!j.is_object()) return fail("not an object");
    Reference r;
    for (auto& [k, v] : j.obj) {
        auto number = [&](double& to) {
            if (!v.is_number() || !std::isfinite(v.num)) return fail(k + " is not a number");
            return to = v.num, true;
        };
        auto flag = [&](bool& to) { return v.is_bool() ? (to = v.b, true) : fail(k + " is not true or false"); };
        bool ok = true;
        if (k == "path") ok = v.is_string() ? (r.path = v.str, true) : fail("path is not a string");
        else if (k == "sequence") ok = flag(r.sequence);
        else if (k == "in_scene") ok = flag(r.in_scene);
        else if (k == "hidden") ok = flag(r.hidden);
        else if (k == "flip_x") ok = flag(r.flip_x);
        else if (k == "flip_y") ok = flag(r.flip_y);
        else if (k == "opacity") ok = number(r.opacity);
        else if (k == "scale") ok = number(r.scale);
        else if (k == "fps") ok = number(r.fps);
        else if (k == "frame_offset") {
            double f = 0;
            ok = number(f);
            r.frame_offset = int(std::clamp(std::round(f), -100000.0, 100000.0));
        } else if (k == "offset") {
            ok = v.is_array() && v.arr.size() == 2 && v.arr[0].is_number() && v.arr[1].is_number() &&
                 std::isfinite(v.arr[0].num) && std::isfinite(v.arr[1].num);
            if (!ok) return fail("offset is not two numbers");
            r.offset_x = v.arr[0].num, r.offset_y = v.arr[1].num;
        } else if (k == "view") {
            const auto* it = v.is_string() ? std::find_if(std::begin(kViewNames), std::end(kViewNames),
                                                          [&](const char* n) { return v.str == n; })
                                           : std::end(kViewNames);
            if (it == std::end(kViewNames)) return fail("unknown view");
            r.view = ReferenceView(it - std::begin(kViewNames));
        } else if (std::find(std::begin(known), std::end(known), k) == std::end(known)) {
            r.extra.obj.emplace_back(k, v);
        }
        if (!ok) return false;
    }
    r.opacity = std::clamp(r.opacity, 0.0, 1.0);
    r.scale = std::clamp(r.scale, 0.01, 100.0);
    r.fps = std::clamp(r.fps, 0.1, 240.0);
    out = std::move(r);
    return true;
}

}  // namespace vats
