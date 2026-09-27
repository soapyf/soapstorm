// Viewport Avatar Toolset - buttons and menu items with icons.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "icon_button.h"

#include <algorithm>
#include <cstring>

#include "icons.h"
#include "imgui.h"
#include "imgui_internal.h"  // MenuItemEx: a menu item with an icon column
#include "theme.h"

namespace vats {
namespace {

bool with_active(bool active, const auto& draw) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    bool pressed = draw();
    if (active) ImGui::PopStyleColor();
    return pressed;
}

}  // namespace

float icon_button_width() { return ImGui::GetFontSize() + 2 * ImGui::GetStyle().FramePadding.x; }

bool icon_button(const char* id, const char* icon, const std::string& tooltip, bool active) {
    bool pressed = with_active(active, [&] {
        return ImGui::Button((std::string(icon) + "##" + id).c_str(), ImVec2(icon_button_width(), 0));
    });
    ImGui::SetItemTooltip("%s", tooltip.c_str());
    return pressed;
}

bool icon_small_button(const char* id, const char* icon, const std::string& tooltip, bool active) {
    ImGui::PushStyleVarY(ImGuiStyleVar_FramePadding, 0);
    bool pressed = icon_button(id, icon, tooltip, active);
    ImGui::PopStyleVar();
    return pressed;
}

bool icon_label_button(const char* icon, const char* label, const std::string& tooltip, bool active) {
    bool pressed = with_active(active, [&] { return ImGui::Button((std::string(icon) + " " + label).c_str()); });
    if (!tooltip.empty()) ImGui::SetItemTooltip("%s", tooltip.c_str());
    return pressed;
}

bool curve_icon_button(const char* id, CurveIcon shape, const std::string& tooltip) {
    bool pressed = ImGui::Button((std::string("##") + id).c_str(), ImVec2(icon_button_width(), 0));
    ImGui::SetItemTooltip("%s", tooltip.c_str());

    // A square the size of a glyph in the middle of the button; (u, v) run 0..1 left to right, bottom to top.
    const float s = ImGui::GetFontSize() * 0.9f;
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    const ImVec2 c((lo.x + hi.x) / 2, (lo.y + hi.y) / 2);
    auto at = [&](float u, float v) { return ImVec2(c.x + (u - 0.5f) * s, c.y + (0.5f - v) * s); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 line = ImGui::GetColorU32(ImGuiCol_Text), key = ImGui::GetColorU32(ui::kKey);  // both dim when disabled
    const float w = std::max(1.f, ImGui::GetFontSize() / 11), dot = w * 1.6f;
    auto curve = [&](float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3) {
        dl->AddBezierCubic(at(u0, v0), at(u1, v1), at(u2, v2), at(u3, v3), line, w);
    };
    auto handles = [&](float u0, float v0, float u1, float v1) {  // a key's two handles, from (u0, v0) to (u1, v1)
        dl->AddLine(at(u0, v0), at(0.5f, 0.5f), line, w);
        dl->AddLine(at(0.5f, 0.5f), at(u1, v1), line, w);
        dl->AddCircleFilled(at(u0, v0), dot * 0.8f, line);
        dl->AddCircleFilled(at(u1, v1), dot * 0.8f, line);
        dl->AddCircleFilled(at(0.5f, 0.5f), dot * 1.3f, key);
    };
    switch (shape) {
        case CurveIcon::Auto:  // a hill, flat on top
            curve(0, 0.1f, 0.25f, 0.1f, 0.25f, 0.85f, 0.5f, 0.85f);
            curve(0.5f, 0.85f, 0.75f, 0.85f, 0.75f, 0.1f, 1, 0.1f);
            dl->AddCircleFilled(at(0.5f, 0.85f), dot, key);
            break;
        case CurveIcon::Spline:  // rising through the key, overshooting the next
            curve(0, 0.1f, 0.2f, 0.1f, 0.35f, 0.35f, 0.5f, 0.5f);
            curve(0.5f, 0.5f, 0.65f, 0.65f, 0.75f, 1.05f, 1, 0.7f);
            dl->AddCircleFilled(at(0.5f, 0.5f), dot, key);
            break;
        case CurveIcon::Plateau:  // rising to a level, settling on it
            curve(0, 0.1f, 0.25f, 0.1f, 0.25f, 0.75f, 0.5f, 0.75f);
            dl->AddLine(at(0.5f, 0.75f), at(1, 0.75f), line, w);
            dl->AddCircleFilled(at(0.5f, 0.75f), dot, key);
            break;
        case CurveIcon::Linear: {
            const ImVec2 p[] = {at(0, 0.1f), at(0.5f, 0.85f), at(1, 0.35f)};
            dl->AddPolyline(p, 3, line, 0, w);
            dl->AddCircleFilled(p[1], dot, key);
            break;
        }
        case CurveIcon::Stepped: {
            const ImVec2 p[] = {at(0, 0.15f), at(0.35f, 0.15f), at(0.35f, 0.5f), at(0.7f, 0.5f), at(0.7f, 0.85f), at(1, 0.85f)};
            dl->AddPolyline(p, 6, line, 0, w);
            dl->AddCircleFilled(p[1], dot, key);
            dl->AddCircleFilled(p[3], dot, key);
            break;
        }
        case CurveIcon::Flat: handles(0.05f, 0.5f, 0.95f, 0.5f); break;     // level handles
        case CurveIcon::Break: handles(0.1f, 0.95f, 0.95f, 0.75f); break;   // at their own angles
        case CurveIcon::Unify: handles(0.08f, 0.15f, 0.92f, 0.85f); break;  // in one line
    }
    return pressed;
}

bool menu_item_icon(const char* icon, const char* label, const char* shortcut, bool selected, bool enabled) {
    return ImGui::MenuItemEx(label, icon, shortcut, selected, enabled);
}

const char* action_icon(const char* action_id) {
    static const struct {
        const char *id, *icon;
    } icons[] = {{"new", icon::kNew},     {"open", icon::kOpen},          {"save", icon::kSave},
                 {"undo", icon::kUndo},   {"redo", icon::kRedo},          {"export_anim", icon::kExport},
                 {"upload", icon::kUpload}, {"import_prop", icon::kImport}};
    for (const auto& i : icons)
        if (std::strcmp(i.id, action_id) == 0) return i.icon;
    return nullptr;
}

}  // namespace vats
