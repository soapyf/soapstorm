// Viewport Avatar Toolset - height-variant exports (spec 08 section 20, HV; idea 43).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Properties > Export, "Also export for heights": two or three heights (presets or custom), kept in the export
// settings as "heights" (metres). Export and Upload write every file once more per height, baked on that body
// (height_shape_ replaces the bake shape in anim_export_options); the placement note lists each actor's hip lift.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "theme.h"
#include "vats/height_variant.h"

namespace vats {

void App::set_export_height(double height_m) {
    height_shape_ = nullptr;
    if (height_m <= 0) return;
    const bool male = bake_shape_key(doc_.clip().export_settings, exporting_yours()) == "sl-default-male";
    height_body_ = height_shape(skel_, mesh_.params(), male, height_m);
    height_shape_ = &height_body_.shape;
}

double App::export_hip_lift() const {
    if (!height_shape_) return 0;
    const Pose rest(skel_.size());
    return skel_.global_pose(rest, height_shape_)[0].pos.z - skel_.global_pose(rest, export_shape())[0].pos.z;
}

void App::draw_height_variants(float label_w) {
    const std::vector<double> heights = export_heights(doc_.clip().export_settings);
    auto store = [&](const std::vector<double>& hs) {
        Json a = Json::array();
        for (double h : hs) a.push(h);
        edit("Export Settings", [&](Clip& c) { c.export_settings.set("heights", a); });
    };
    bool on = !heights.empty();
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Checkbox("Also export for heights", &on))
        store(on ? std::vector<double>(std::begin(kHeightPresets), std::end(kHeightPresets)) : std::vector<double>{});
    ImGui::SetItemTooltip("Writes the animation again for each height, baked on SL Default made that tall (the male one "
                          "when Bake shape is SL Default (Male)): IK and pins are solved again on that body, so hands "
                          "and feet stay on their targets. The files end in _H175, _H195 and so on.");
    if (!on) return;
    std::vector<double> hs = heights;
    bool changed = false;
    int remove = -1;
    const float combo_w = ImGui::GetFontSize() * 5.5f, field_w = ImGui::GetFontSize() * 4;
    for (int i = 0; i < int(hs.size()); ++i) {
        ImGui::PushID(i);
        ImGui::SetCursorPosX(label_w);
        int preset = -1;
        for (int k = 0; k < int(std::size(kHeightPresets)); ++k)
            if (std::fabs(hs[i] - kHeightPresets[k]) < 1e-6) preset = k;
        char text[32];
        std::snprintf(text, sizeof text, "%.2f m", hs[i]);
        ImGui::SetNextItemWidth(combo_w);
        if (ImGui::BeginCombo("##h", preset >= 0 ? text : "Custom")) {
            for (int k = 0; k < int(std::size(kHeightPresets)); ++k) {
                std::snprintf(text, sizeof text, "%.2f m", kHeightPresets[k]);
                if (ImGui::Selectable(text, k == preset) && k != preset) hs[i] = kHeightPresets[k], changed = true;
            }
            if (ImGui::Selectable("Custom", preset < 0) && preset >= 0)
                hs[i] = std::clamp(hs[i] + 0.05, kHeightMin, kHeightMax), changed = true;
            ImGui::EndCombo();
        }
        if (preset < 0) {
            ImGui::SameLine();
            float v = float(hs[i]);
            ImGui::SetNextItemWidth(field_w);
            if (ImGui::DragFloat("##hv", &v, 0.005f, float(kHeightMin), float(kHeightMax), "%.2f m",
                                 ImGuiSliderFlags_AlwaysClamp))
                hs[i] = v, changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("_%s", height_tag(hs[i]).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(hs.size() <= 2);
        if (icon_small_button("remove", icon::kDelete, "Remove this height")) remove = i;
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (remove >= 0) hs.erase(hs.begin() + remove), changed = true;
    ImGui::SetCursorPosX(label_w);
    ImGui::BeginDisabled(int(hs.size()) >= kMaxHeights);
    if (ImGui::Button("Add Height")) {
        double h = kHeightPresets[0];
        for (double p : kHeightPresets)
            if (std::none_of(hs.begin(), hs.end(), [&](double x) { return std::fabs(x - p) < 1e-6; })) {
                h = p;
                break;
            }
        hs.push_back(h), changed = true;
    }
    ImGui::EndDisabled();
    if (changed) store(hs);
}

}  // namespace vats
