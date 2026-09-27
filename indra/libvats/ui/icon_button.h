// Viewport Avatar Toolset - buttons and menu items with icons.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The icons are glyphs of the UI font (ui/icons.h), so they follow the theme's text colour, the disabled
// dimming and the UI scale like any label. Every icon-only button has a tooltip naming it.
#pragma once

#include <string>

namespace vats {

// An icon-only button: id keeps it unique ("play"), tooltip names the action and its key. active paints it
// in the selection colour, as the active tool is.
bool icon_button(const char* id, const char* icon, const std::string& tooltip, bool active = false);
// The same without vertical padding, for rows of small buttons (ImGui::SmallButton).
bool icon_small_button(const char* id, const char* icon, const std::string& tooltip, bool active = false);
// The width icon_button() takes, for toolbars that wrap.
float icon_button_width();

// An icon before a text label. The label may carry "###id" to keep the ID when the text changes; tooltip
// may be empty.
bool icon_label_button(const char* icon, const char* label, const std::string& tooltip = "", bool active = false);

// The graph editor's tangent shapes, drawn rather than taken from the font (no icon font has them).
enum class CurveIcon { Auto, Spline, Plateau, Linear, Flat, Stepped, Break, Unify };
bool curve_icon_button(const char* id, CurveIcon shape, const std::string& tooltip);

// A menu item with an icon in a column of its own (ImGui keeps the labels of the menu aligned); icon may
// be null.
bool menu_item_icon(const char* icon, const char* label, const char* shortcut = nullptr, bool selected = false,
                    bool enabled = true);
// The icon of an editor action shown in the menus ("open", "undo"...), or null.
const char* action_icon(const char* action_id);

}  // namespace vats
