// Viewport Avatar Toolset - buttons and menu items with icons.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The icons are glyphs of the UI font (ui/icons.h), so they follow the theme's text colour, the disabled
// dimming and the UI scale like any label. Every icon-only button has a tooltip naming it.
//
// Buttons, the rule (docs/wiki/STYLE.md, UI text):
// - Icons only on toolbar buttons and on a window's primary action; other buttons are words.
// - Each window has at most one primary action (the one it is for): primary_button(), filled with the accent.
// - Small buttons (SmallButton, *_small_button) only inside table and list rows; everything else is the one
//   default height.
// - A disabled button keeps a tooltip saying why (tooltips show on disabled items, theme.cpp).
// - Labels in Title Case, "..." when the button opens a window or asks something first.
#pragma once

#include <string>
#include <vector>

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
// A window's primary action: the accent fill and a label that reads on it. width 0 fits the label, -1 fills the
// row; icon may be null.
bool primary_button(const char* label, const std::string& tooltip = "", float width = 0, const char* icon = nullptr);
// The same as a small button (ImGui::SmallButton), for rows inside lists.
bool icon_label_small_button(const char* icon, const char* label, const std::string& tooltip = "");

// The graph editor's tangent shapes, drawn rather than taken from the font (no icon font has them).
// label: its name after the drawing (a toolbar with room for names), or null for the drawing alone.
enum class CurveIcon { Auto, Spline, Plateau, Linear, Flat, Stepped, Break, Unify };
bool curve_icon_button(const char* id, CurveIcon shape, const std::string& tooltip, const char* label = nullptr);

// A menu item with an icon in a column of its own (ImGui keeps the labels of the menu aligned); icon may
// be null.
bool menu_item_icon(const char* icon, const char* label, const char* shortcut = nullptr, bool selected = false,
                    bool enabled = true);
// A sub-menu with an icon in the same column (ImGui::BeginMenu otherwise); EndMenu() as usual.
bool begin_menu_icon(const char* icon, const char* label, bool enabled = true);
// --open-menu (screenshots): the menus on this path ("Tools", "Tools/Loop Tools") open by themselves in every
// begin_menu_icon() call; a menu's name is its label up to any "##".
void force_open_menus(const std::string& path);
// The icon of an editor action shown in the menus ("open", "undo"...), or null.
const char* action_icon(const char* action_id);

// Find a Tool's index of the menus (tool_search.cpp). For one frame, between start_menu_crawl() and
// finish_menu_crawl() around the menu bar, every begin_menu_icon() opens and every menu_item_icon() is recorded with
// its path ("Tools > Loop Tools"); finish hides and closes what the crawl opened, so nothing shows. With run_label,
// that item (on run_path) reports a click instead, so its own code runs as from the menu.
struct MenuEntry {
    std::string path, label;
    bool enabled = true;
};
void start_menu_crawl(const std::string& run_path = "", const std::string& run_label = "");
bool menu_crawling();
std::vector<MenuEntry> finish_menu_crawl(bool& ran);

}  // namespace vats
