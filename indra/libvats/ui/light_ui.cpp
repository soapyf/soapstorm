// Viewport Avatar Toolset - the Light menu: lighting presets and a plain backdrop (spec 08 LT-1, LT-2).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <cmath>
#include <iterator>
#include <vector>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"

namespace vats {

namespace {
// +X is where the avatar faces, +Y its left, Z up. The strengths suit the app's shader; the viewer's sky takes the
// same colours.
const char* const kLightIds[] = {"noon", "key", "rim", "dusk", "night"};
const char* const kLightIcons[] = {icon::kNoon, icon::kKeyLight, icon::kRim, icon::kDusk, icon::kNight};
const LightPreset kLights[] = {
    {"Flat Noon", {0, 0, 1}, {1.0f, 1.0f, 0.96f}, {0.80f, 0.80f, 0.82f}, false},
    {"Three-Quarter Key", {0.60f, 0.55f, 0.58f}, {1.10f, 1.02f, 0.92f}, {0.35f, 0.37f, 0.42f}, false},
    {"Rim / Back", {-0.78f, -0.35f, 0.52f}, {1.20f, 1.20f, 1.25f}, {0.45f, 0.46f, 0.52f}, false},
    {"Dusk", {0.47f, -0.87f, 0.15f}, {1.05f, 0.62f, 0.36f}, {0.32f, 0.28f, 0.38f}, false},
    {"Night", {0.30f, 0.45f, 0.84f}, {0.38f, 0.46f, 0.66f}, {0.18f, 0.20f, 0.30f}, true},
};
}  // namespace

bool App::set_light(const std::string& id) {
    int i = id == "studio" ? -1 : -2;
    for (int k = 0; k < int(std::size(kLightIds)); ++k)
        if (id == kLightIds[k]) i = k;
    if (i < -1) return false;
    light_ = i;
    host_.set_light(i < 0 ? nullptr : &kLights[i]);
    return true;
}

void App::draw_light_menu() {
    if (!begin_top_menu("Light")) return;
    const bool world = host_.world_view();
    auto use = [&](int i) { set_light(i < 0 ? "studio" : kLightIds[i]); };
    if (menu_item_icon(world ? icon::kWorld : icon::kStudio, world ? "The World's Own" : "Studio (Default)", nullptr, light_ < 0))
        use(-1);
    ImGui::SetItemTooltip(world ? "The sky you had before choosing a preset" : "The editor's usual light");
    ImGui::Separator();
    for (int i = 0; i < int(std::size(kLights)); ++i) {
        if (menu_item_icon(kLightIcons[i], kLights[i].name, nullptr, light_ == i)) use(i);
        if (world) ImGui::SetItemTooltip("A sky only you see; your own comes back when the editor closes");
    }
    ImGui::Separator();
    if (menu_item_icon(icon::kBackdrop, "Plain Backdrop", nullptr, backdrop_)) backdrop_ = !backdrop_;
    ImGui::SetItemTooltip("A neutral grey wall and floor behind the actor, turning with the camera");
    ImGui::EndMenu();
}

// A wall 3 m behind the actor as the camera sees it, 12 m wide and 6 m tall, and the floor up to it from 3 m in
// front, at the feet (z = 0). Its own geometry in the scene, so the viewer draws it with the world, behind the avatar.
void App::draw_backdrop() {
    if (!backdrop_ || globals_.empty()) return;
    static std::vector<Vertex> v;
    v.clear();
    const Vec3 at = globals_[0].pos, fwd = camera_.forward();
    double fx = fwd.x, fy = fwd.y;
    const double len = std::hypot(fx, fy);
    if (len < 1e-6) fx = -1, fy = 0;  // looking straight down: behind is -X
    else fx /= len, fy /= len;
    const double rx = -fy, ry = fx;  // along the wall
    constexpr double kBack = 3, kFront = 3, kHalf = 6, kTop = 6, kLift = 0.002;
    const float grey = 0.5f;
    auto quad = [&](const Vec3 (&p)[4], const Vec3& n) {
        for (int i : {0, 1, 2, 0, 2, 3})
            v.push_back({{float(p[i].x), float(p[i].y), float(p[i].z)}, {float(n.x), float(n.y), float(n.z)}, {grey, grey, grey, 1}});
    };
    const double wx = at.x + fx * kBack, wy = at.y + fy * kBack, ex = at.x - fx * kFront, ey = at.y - fy * kFront;
    quad({{wx - rx * kHalf, wy - ry * kHalf, 0}, {wx + rx * kHalf, wy + ry * kHalf, 0},
          {wx + rx * kHalf, wy + ry * kHalf, kTop}, {wx - rx * kHalf, wy - ry * kHalf, kTop}},
         {-fx, -fy, 0});
    quad({{ex - rx * kHalf, ey - ry * kHalf, kLift}, {wx - rx * kHalf, wy - ry * kHalf, kLift},
          {wx + rx * kHalf, wy + ry * kHalf, kLift}, {ex + rx * kHalf, ey + ry * kHalf, kLift}},
         {0, 0, 1});
    host_.scene_triangles(v, {}, true, 0.f);
}

}  // namespace vats
