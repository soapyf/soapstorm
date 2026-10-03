// Viewport Avatar Toolset - the UI themes.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#pragma once

#include <string>

#include <algorithm>

#include "imgui.h"
#include "scene.h"

namespace vats {

// A host's own colour roles (the viewer's skin, spec 09 U4). Given to apply_theme, they replace the theme's.
struct HostColours {
    ImVec4 bg, panel, frame, frame_hi, frame_active, text, text_dim, border, accent;
};

void load_fonts(const char* assets_dir);
ImFont* bold_font();  // Inter SemiBold (help pages), the regular font when missing
// Styles ImGui with a theme at a UI scale (display scale x interface size); host colours, when given, replace
// the theme's colours.
void apply_theme(int theme, float scale, const HostColours* host = nullptr);
int theme_count();
const char* theme_name(int theme);
int find_theme(const std::string& name);  // 0 when unknown

const SceneColours& scene_colours();
ImU32 accent_colour();

// Timeline and graph colours that follow the theme (TG-113).
struct TimelineColours {
    ImU32 bg, graph_bg, ruler, grid, grid_major, zero, text_dim;
};
const TimelineColours& timeline_colours();
inline ImU32 timeline_background() { return timeline_colours().bg; }
// The loop's band behind the keys: the theme's accent, faint, so it is no cool hue apart from the palette.
inline ImU32 loop_band() { return (accent_colour() & ~IM_COL32_A_MASK) | (IM_COL32(0, 0, 0, 22) & IM_COL32_A_MASK); }

// The two kinds of heading (docs/wiki/STYLE.md, UI text). Never filled with the accent, which means "selected".
// A collapsible group: a subtle bar the panel's width, a small chevron and the bold font. Its open state is ImGui's
// CollapsingHeader's (the same ID), so ImGui::SetNextItemOpen() works on it.
bool section_header(const char* name, bool open_by_default = true);
// A static subheading inside a group or window: a dim label on a rule.
inline void subheading(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}

// The colour of a missing required input or a warning, which must not be the least visible text on screen.
inline ImVec4 warn_colour() { return ImVec4(1, 0.75f, 0.35f, 1); }
inline void warn_text(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn_colour());
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// The text colour that reads best on the accent (a primary button's label): dark or light, by contrast.
ImU32 accent_text_colour();

// Floating windows and dialogs: a 1 px edge and a soft shadow, so one never reads as part of the panel under it.
// Once a frame, after every window (it draws into each window's own list, outside its rectangle).
void decorate_floating_windows();

// A first-open window size in font units, never taller or wider than 90% of the screen.
inline ImVec2 window_size(float w_em, float h_em) {
    const ImVec2 ws = ImGui::GetMainViewport()->WorkSize;
    const float fs = ImGui::GetFontSize();
    return ImVec2(std::min(w_em * fs, 0.9f * ws.x), std::min(h_em * fs, 0.9f * ws.y));
}

// Dimmed explanatory text that wraps to the panel instead of running off its edge.
inline void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Colours that stay the same in every theme.
namespace ui {
inline constexpr ImU32 kKey = IM_COL32(240, 196, 92, 255);        // key diamonds
inline constexpr ImU32 kKeyDim = IM_COL32(170, 150, 110, 150);    // keys of non-primary selection
// Rose, so it reads apart from pins (light blue, TG-100), keys (amber) and the X/Y/Z curves.
inline constexpr ImU32 kPlayhead = IM_COL32(236, 112, 160, 255);  // current frame
inline constexpr ImU32 kLoopHandle = IM_COL32(110, 190, 255, 255);
inline constexpr ImU32 kAttachment = IM_COL32(120, 210, 140, 255);
}  // namespace ui

}  // namespace vats
