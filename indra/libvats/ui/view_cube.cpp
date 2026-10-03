// Viewport Avatar Toolset - the view cube: click a face, edge or corner to look from there.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/04 VP-60..VP-65.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "theme.h"

namespace vats {
namespace {

constexpr float kBevel = 0.2f;

struct Region {
    Vec3 n;                  // direction, components in {-1, 0, 1}
    std::vector<Vec3> verts;  // on the bevelled unit cube
    const char* label = nullptr;
};

const std::vector<Region>& regions() {
    static std::vector<Region> out = [] {
        std::vector<Region> r;
        const double in = 1 - kBevel;
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z) {
                    if (!x && !y && !z) continue;
                    int s[3] = {x, y, z};
                    int k = !!x + !!y + !!z;
                    Region reg;
                    reg.n = Vec3{double(x), double(y), double(z)};
                    std::vector<Vec3> v;
                    if (k == 1) {  // face: its inner square
                        int a = x ? 0 : y ? 1 : 2, b = (a + 1) % 3, c = (a + 2) % 3;
                        for (int i = 0; i < 4; ++i) {
                            Vec3 p;
                            p[a] = s[a];
                            p[b] = (i == 0 || i == 3 ? -in : in);
                            p[c] = (i < 2 ? -in : in);
                            v.push_back(p);
                        }
                    } else if (k == 2) {  // edge bevel between two faces
                        int f = !x ? 0 : !y ? 1 : 2, a = (f + 1) % 3, c = (f + 2) % 3;
                        for (double fv : {-in, in}) {
                            Vec3 p1, p2;
                            p1[f] = p2[f] = fv;
                            p1[a] = s[a], p1[c] = s[c] * in;
                            p2[a] = s[a] * in, p2[c] = s[c];
                            v.push_back(p1), v.push_back(p2);
                        }
                    } else {  // corner facet
                        for (int i = 0; i < 3; ++i) {
                            Vec3 p{s[0] * in, s[1] * in, s[2] * in};
                            p[i] = s[i];
                            v.push_back(p);
                        }
                    }
                    // Order the polygon around its normal so it is convex and consistently wound.
                    Vec3 n = reg.n.normalized(), c;
                    for (auto& p : v) c += p;
                    c = c * (1.0 / v.size());
                    Vec3 u = (v[0] - c).normalized(), w = n.cross(u);
                    std::sort(v.begin(), v.end(), [&](const Vec3& a, const Vec3& b) {
                        return std::atan2((a - c).dot(w), (a - c).dot(u)) < std::atan2((b - c).dot(w), (b - c).dot(u));
                    });
                    reg.verts = v;
                    if (k == 1)
                        reg.label = x > 0 ? "FRONT" : x < 0 ? "BACK" : y > 0 ? "LEFT" : y < 0 ? "RIGHT" : z > 0 ? "TOP" : "BOTTOM";
                    r.push_back(reg);
                }
        return r;
    }();
    return out;
}

bool inside(const std::vector<ImVec2>& poly, ImVec2 m) {
    int pos = 0, neg = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        ImVec2 a = poly[i], b = poly[(i + 1) % poly.size()];
        float cross = (b.x - a.x) * (m.y - a.y) - (b.y - a.y) * (m.x - a.x);
        (cross > 0 ? pos : neg)++;
    }
    return pos == 0 || neg == 0;
}

ImU32 shade(float r, float g, float b, float k, float a) {
    return IM_COL32(int(r * k * 255), int(g * k * 255), int(b * k * 255), int(a * 255));
}

}  // namespace

// Turns the camera to look from a direction, animated over 0.3 s (VP-63).
void App::look_from(const Vec3& dir) {
    cam_glide_.look_from(camera_, dir);
}

// Second Life's focus swing (LLAgentCamera::setFocusGlobal, then startCameraAnimation and updateCamera): the camera
// stays where it is and the focus slides from the old point to the new over ZoomTime (~0.4 s), exponential
// smoothed with blended inputs matching LLAgentCamera.
void App::focus_camera_on(const Vec3& point) {
    cam_glide_.focus_on(camera_, point);
}

void App::update_camera_animation(double dt) {
    cam_glide_.update(camera_, dt, settings_.reduce_motion);
}

void App::draw_view_cube(ImDrawList* dl, ImVec2 vp_min, bool viewport_hovered) {
    ImGuiIO& io = ImGui::GetIO();
    const float size = settings_.view_cube_size;
    ImVec2 box_min(vp_min.x + 8, vp_min.y + 8), box_max(vp_min.x + 8 + size, vp_min.y + 8 + size);  // top left (VP-60)
    ImVec2 centre((box_min.x + box_max.x) / 2, (box_min.y + box_max.y) / 2);
    const ImVec2 m = io.MousePos;
    bool over_box = m.x >= box_min.x && m.x <= box_max.x && m.y >= box_min.y && m.y <= box_max.y;
    bool over = viewport_hovered && over_box;
    cube_hover_ = over || cube_drag_ != 0;

    // See-through until hovered (VP-62).
    float target = cube_hover_ ? 1.f : 0.5f, rate = cube_hover_ ? 1 / 0.12f : 1 / 0.25f;
    cube_alpha_ += std::clamp(target - cube_alpha_, -rate * io.DeltaTime, rate * io.DeltaTime);

    const Vec3 right = camera_.right(), up = camera_.up(), toward = -camera_.forward();
    const float half = size * 0.36f;
    auto project = [&](const Vec3& p) {
        double depth = p.dot(toward);                  // +1 nearest the viewer
        double k = half / (1 - 0.14 * depth / 1.7);    // mild false perspective
        return ImVec2(centre.x + float(p.dot(right) * k), centre.y - float(p.dot(up) * k));
    };

    // Soft shadow and ring.
    for (int i = 0; i < 4; ++i)
        dl->AddEllipseFilled(ImVec2(centre.x, centre.y + half * 1.35f), ImVec2(half * (0.9f - 0.12f * i), half * 0.18f),
                             IM_COL32(0, 0, 0, int(18 * cube_alpha_)));
    dl->AddCircle(centre, half * 1.75f, IM_COL32(255, 255, 255, int(15 * cube_alpha_)), 48, 1.f);

    // Facing regions, back to front.
    std::vector<std::pair<double, int>> order;
    const auto& regs = regions();
    for (int i = 0; i < int(regs.size()); ++i) {
        double facing = regs[i].n.normalized().dot(toward);
        if (facing > 1e-3) order.emplace_back(facing, i);
    }
    std::sort(order.begin(), order.end());
    int hot = -1;
    std::vector<std::vector<ImVec2>> polys(regs.size());
    for (auto& [f, i] : order) {
        for (auto& v : regs[i].verts) polys[i].push_back(project(v));
        if (over && cube_drag_ == 0 && inside(polys[i], m)) hot = i;  // the frontmost wins (drawn last)
    }
    const Vec3 light = (toward * 0.7 + up * 0.5 - right * 0.5).normalized();
    for (auto& [facing, i] : order) {
        const Region& r = regs[i];
        float k = 0.42f + 0.58f * float(std::max(0.0, r.n.normalized().dot(light)));
        ImU32 fill = i == hot ? shade(0.69f, 0.345f, 0.478f, k, 0.97f * cube_alpha_)
                              : shade(0.60f, 0.60f, 0.64f, k, 0.97f * cube_alpha_);
        dl->AddConvexPolyFilled(polys[i].data(), int(polys[i].size()), fill);
        if (r.label) {
            dl->AddPolyline(polys[i].data(), int(polys[i].size()), IM_COL32(0, 0, 0, int(72 * cube_alpha_)),
                            ImDrawFlags_Closed, 1.f);
            float fade = std::clamp(float((facing - 0.26) / 0.5), 0.f, 1.f);  // hidden when turned away
            if (fade > 0) {
                ImVec2 c(0, 0);
                for (auto& p : polys[i]) c.x += p.x / 4, c.y += p.y / 4;
                float scale = std::clamp(float(facing) * size / 150.f, 0.45f, 1.f);
                ImFont* font = ImGui::GetFont();
                float fs = ImGui::GetFontSize() * scale;
                ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, r.label);
                // Light on the faces, with a dark edge under it, so it reads on the see-through cube and on the
                // light faces of a hovered one.
                const ImVec2 at(c.x - ts.x / 2, c.y - ts.y / 2);
                dl->AddText(font, fs, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, int(150 * fade * cube_alpha_)), r.label);
                dl->AddText(font, fs, at, IM_COL32(245, 245, 245, int(255 * fade * cube_alpha_)), r.label);
            }
        }
    }

    // Resize grip, bottom-right of the widget, away from the corner it is anchored to (VP-65).
    ImVec2 g0(box_max.x, box_max.y), g1(box_max.x - 14, box_max.y), g2(box_max.x, box_max.y - 14);
    bool over_grip = over && m.x >= box_max.x - 14 && m.y >= box_max.y - 14 && (box_max.x - m.x) + (box_max.y - m.y) <= 14;
    dl->AddTriangleFilled(g0, g2, g1, IM_COL32(255, 255, 255, int((over_grip || cube_drag_ == 2 ? 140 : 64) * cube_alpha_)));
    if (over_grip || cube_drag_ == 2) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    if (over && cube_drag_ == 0 && !over_grip) ImGui::SetTooltip("Click a side to look from it, drag to orbit");

    // Presses and drags.
    if (over && ImGui::IsMouseClicked(0)) {
        cube_drag_ = over_grip ? 2 : 1;
        cube_press_ = m;
        cube_press_size_ = size;
        cube_moved_ = false;
        cube_press_region_ = hot;
    }
    if (cube_drag_ != 0) {
        ImVec2 d(m.x - cube_press_.x, m.y - cube_press_.y);
        if (cube_drag_ == 2) {
            settings_.view_cube_size = std::clamp(cube_press_size_ + std::max(d.x, d.y), 60.f, 260.f);
        } else {
            cube_moved_ = cube_moved_ || std::hypot(d.x, d.y) > 3;
            if (cube_moved_) {
                cam_glide_.apply_input(camera_, [&](Camera& c) {
                    c.yaw -= io.MouseDelta.x * 0.012;
                    c.pitch = std::clamp(c.pitch + io.MouseDelta.y * 0.012, -1.5, 1.5);
                });
            }
        }
        if (!ImGui::IsMouseDown(0)) {
            if (cube_drag_ == 2) save_settings();
            else if (!cube_moved_ && cube_press_region_ >= 0) look_from(regs[cube_press_region_].n);
            cube_drag_ = 0;
        }
    }
}

}  // namespace vats
