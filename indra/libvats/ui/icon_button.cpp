// Viewport Avatar Toolset - buttons and menu items with icons.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "icon_button.h"

#include <algorithm>
#include <map>
#include <cstring>
#include <string_view>
#include <vector>

#include "icons.h"
#include "imgui.h"
#include "imgui_internal.h"  // MenuItemEx: a menu item with an icon column
#include "theme.h"

namespace vats {
namespace {

std::vector<std::string> g_forced_menus;  // --open-menu

// The menu crawl (start_menu_crawl): on, the item to run, whether it ran, what was found, and each open menu
// window's path (menus at one depth share a window; each records while it is the current one).
struct Crawl {
    bool on = false, ran = false;
    std::string run_path, run_label;
    std::vector<MenuEntry> found;
    std::map<ImGuiID, std::string> paths;
} g_crawl;

std::string menu_name(const char* label) { return std::string(std::string_view(label).substr(0, std::string_view(label).find("##"))); }

bool with_active(bool active, const auto& draw) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    bool pressed = draw();
    if (active) ImGui::PopStyleColor();
    return pressed;
}

// A button pressed in a menu is an action, so it closes the menu, every level, as a menu item does (ImGui closes
// menus only for its own menu items).
bool closes_menu(bool pressed) {
    if (pressed && (ImGui::GetCurrentWindow()->Flags & ImGuiWindowFlags_ChildMenu) &&
        (GImGui->CurrentItemFlags & ImGuiItemFlags_AutoClosePopups))
        ImGui::CloseCurrentPopup();
    return pressed;
}

}  // namespace

float icon_button_width() { return ImGui::GetFontSize() + 2 * ImGui::GetStyle().FramePadding.x; }

bool icon_button(const char* id, const char* icon, const std::string& tooltip, bool active) {
    bool pressed = with_active(active, [&] {
        return closes_menu(ImGui::Button((std::string(icon) + "##" + id).c_str(), ImVec2(icon_button_width(), 0)));
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
    bool pressed = with_active(active, [&] { return closes_menu(ImGui::Button((std::string(icon) + " " + label).c_str())); });
    if (!tooltip.empty()) ImGui::SetItemTooltip("%s", tooltip.c_str());
    return pressed;
}

bool primary_button(const char* label, const std::string& tooltip, float width, const char* icon) {
    const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(accent_colour());
    auto shade = [&](float k) { return ImVec4(accent.x * k, accent.y * k, accent.z * k, 1); };
    ImGui::PushStyleColor(ImGuiCol_Button, accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(1.f, accent.x * 1.1f), std::min(1.f, accent.y * 1.1f),
                                                         std::min(1.f, accent.z * 1.1f), 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, shade(0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(accent_text_colour()));
    const std::string text = icon ? std::string(icon) + " " + label : std::string(label);
    const bool pressed = closes_menu(ImGui::Button(text.c_str(), ImVec2(width, 0)));
    ImGui::PopStyleColor(4);
    if (!tooltip.empty()) ImGui::SetItemTooltip("%s", tooltip.c_str());
    return pressed;
}

bool icon_label_small_button(const char* icon, const char* label, const std::string& tooltip) {
    bool pressed = closes_menu(ImGui::SmallButton((std::string(icon) + " " + label).c_str()));
    if (!tooltip.empty()) ImGui::SetItemTooltip("%s", tooltip.c_str());
    return pressed;
}

bool curve_icon_button(const char* id, CurveIcon shape, const std::string& tooltip, const char* label) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float label_w = label ? ImGui::CalcTextSize(label).x + st.FramePadding.x : 0;
    bool pressed = ImGui::Button((std::string("##") + id).c_str(), ImVec2(icon_button_width() + label_w, 0));
    ImGui::SetItemTooltip("%s", tooltip.c_str());

    // A square the size of a glyph in the middle of the button's icon part; (u, v) run 0..1 left to right, bottom to top.
    const float s = ImGui::GetFontSize() * 0.9f;
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    const ImVec2 c(lo.x + icon_button_width() / 2, (lo.y + hi.y) / 2);
    if (label) ImGui::GetWindowDrawList()->AddText(ImVec2(lo.x + icon_button_width() - st.FramePadding.x * 0.5f, lo.y + st.FramePadding.y),
                                                   ImGui::GetColorU32(ImGuiCol_Text), label);
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
    if (g_crawl.on) {
        const std::string path = g_crawl.paths[ImGui::GetCurrentWindow()->ID], name = menu_name(label);
        g_crawl.found.push_back({path, name, enabled});
        if (!g_crawl.ran && enabled && path == g_crawl.run_path && name == g_crawl.run_label) {
            g_crawl.ran = true;
            ImGui::ClosePopupToLevel(0, true);  // as a click closes the menus, before the item's code opens anything
            return true;
        }
    }
    return ImGui::MenuItemEx(label, icon, shortcut, selected, enabled);
}

void force_open_menus(const std::string& path) {
    for (size_t at = 0; at <= path.size();) {
        const size_t end = std::min(path.find('/', at), path.size());
        g_forced_menus.push_back(path.substr(at, end - at));
        at = end + 1;
    }
}

bool begin_menu_icon(const char* icon, const char* label, bool enabled) {
    const std::string name = menu_name(label);
    const bool crawl = g_crawl.on && !g_crawl.ran && enabled;
    if ((crawl || std::find(g_forced_menus.begin(), g_forced_menus.end(), name) != g_forced_menus.end()) && !ImGui::IsPopupOpen(label))
        ImGui::OpenPopup(label);
    const ImGuiID parent = ImGui::GetCurrentWindow()->ID;
    if (!ImGui::BeginMenuEx(label, icon, enabled)) return false;
    if (crawl) {
        const std::string& up = g_crawl.paths[parent];
        g_crawl.paths[ImGui::GetCurrentWindow()->ID] = up.empty() ? name : up + " > " + name;
    }
    return true;
}

void start_menu_crawl(const std::string& run_path, const std::string& run_label) {
    g_crawl = {};
    g_crawl.on = true, g_crawl.run_path = run_path, g_crawl.run_label = run_label;
}

bool menu_crawling() { return g_crawl.on; }

std::vector<MenuEntry> finish_menu_crawl(bool& ran) {
    // The menus it opened were drawn this frame: hidden, they never reach the screen, and closed they are gone.
    for (ImGuiWindow* w : GImGui->Windows)
        if (w->Active && (w->Flags & ImGuiWindowFlags_ChildMenu)) w->Hidden = true;
    if (!g_crawl.ran && !GImGui->OpenPopupStack.empty()) ImGui::ClosePopupToLevel(0, true);
    ran = g_crawl.ran;
    std::vector<MenuEntry> found = std::move(g_crawl.found);
    g_crawl = {};
    return found;
}

const char* action_icon(const char* action_id) {
    static const struct {
        const char *id, *icon;
    } icons[] = {{"new", icon::kNew},     {"open", icon::kOpen},          {"save", icon::kSave},
                 {"save_as", icon::kSaveAs}, {"save_to_library", icon::kAddToLibrary}, {"import_bvh", icon::kImportBvh},
                 {"import_anim", icon::kImportAnim}, {"import_retarget", icon::kRetarget}, {"load_audio", icon::kAudio},
                 {"export_bvh", icon::kText}, {"export_bvh_all", icon::kFiles}, {"export_all_clips", icon::kClips},
                 {"upload_all_clips", icon::kUploadAll}, {"edit_limits", icon::kEditLimits},
                 {"undo", icon::kUndo},   {"redo", icon::kRedo},          {"export_anim", icon::kExport},
                 {"upload", icon::kUpload}, {"import_prop", icon::kImport}, {"tween", icon::kTween},
                 {"batch_retarget", icon::kBatch}, {"graph", icon::kEase}, {"dope_sheet", icon::kDopeSheet}, {"reset_layout", icon::kRefresh}, {"foot_lock", icon::kFootLock},
                 {"tool_select", icon::kSelect}, {"tool_move", icon::kMove}, {"tool_rotate", icon::kRotate},
                 {"tool_scale", icon::kScale}, {"orientation", icon::kGimbal}, {"ik_toggle", icon::kIkFk}, {"auto_ik", icon::kPull},
                 {"follow_target", icon::kFollow}, {"pin_world", icon::kPin}, {"pin_bone", icon::kBind},
                 {"bind_to", icon::kBind}, {"sit_on_seat", icon::kSeat},
                 {"unpin", icon::kRelease}, {"delete_pin", icon::kDelete}, {"hands", icon::kHand},
                 {"target_show", icon::kShown}, {"target_load", icon::kOpen}, {"target_clear", icon::kClear}};
    for (const auto& i : icons)
        if (std::strcmp(i.id, action_id) == 0) return i.icon;
    return nullptr;
}

}  // namespace vats
