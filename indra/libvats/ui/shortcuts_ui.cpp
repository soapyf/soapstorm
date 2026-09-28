// Viewport Avatar Toolset - Edit > Keyboard Shortcuts...: the user's own keys on top of the control preset.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Every action of the key dispatcher (App::actions_), grouped by the menu that holds it, with a search over the
// names, menus and keys. A changed key is an override in settings.json (keymap.h); apply_preset() lays the
// overrides over the preset, so the menus, tooltips and Help > Controls show them. Host calls only: the viewer
// has it too, saved in its own settings.json.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"

namespace vats {
namespace {

// The menus, in menu-bar order, and the actions each holds (App::draw_menus). Actions in none go under Other.
struct Group {
    const char* menu;
    std::vector<const char*> ids;
};
const std::vector<Group>& groups() {
    static const std::vector<Group> g = {
        {"File",
         {"new", "open", "save", "save_as", "save_to_library", "import_bvh", "import_anim", "import_retarget",
          "batch_retarget", "import_prop", "load_audio", "export_anim", "export_bvh", "export_bvh_all", "export_all_clips",
          "upload", "upload_all_clips", "quit"}},
        {"Edit",
         {"undo", "redo", "key", "key_all", "tween", "delete_key", "delete_frame", "reset_bone", "reset_hip", "reset_pose",
          "copy", "paste", "save_clip", "mirror_l2r", "mirror_r2l", "flip_pose", "mirror_bone", "reverse", "shortcuts",
          "prefs"}},
        {"Edit > Time",
         {"insert_frames", "remove_range", "stretch_range", "copy_range", "paste_range", "paste_range_insert",
          "paste_range_mirrored"}},
        {"Playback", {"play", "next_frame", "prev_frame", "next_key", "prev_key", "start", "end"}},
        {"View",
         {"view_front", "view_back", "view_right", "view_left", "view_top", "view_ortho", "frame_selected", "frame_all",
          "zoom_in", "zoom_out", "reset_camera", "graph", "dope_sheet"}},
        {"View > Camera > Camera Views",
         {"cam_1", "cam_2", "cam_3", "cam_4", "store_cam_1", "store_cam_2", "store_cam_3", "store_cam_4"}},
        {"View > Target Ghost", {"target_show", "target_load", "target_clear"}},
        {"Select",
         {"select_all", "select_keyed_frame", "select_all_keyed", "select_none", "select_parent", "select_child",
          "next_sibling", "prev_sibling"}},
        {"Tools",
         {"tool_select", "tool_move", "tool_rotate", "tool_scale", "orientation", "snap_toggle", "ik_toggle",
          "follow_target", "pin_world", "pin_bone", "unpin", "delete_pin", "foot_lock", "hands"}},
        {"Timeline (right-click)", {"remove_audio", "tap_beat", "clear_beats"}},
        {"Help", {"help_contents", "tutorials", "help", "welcome", "about"}},
    };
    return g;
}

// Where an action sits: its menu's place in the menu bar and its own in the menu; Other comes last.
std::pair<size_t, size_t> menu_place(const std::string& id) {
    for (size_t g = 0; g < groups().size(); ++g)
        for (size_t i = 0; i < groups()[g].ids.size(); ++i)
            if (id == groups()[g].ids[i]) return {g, i};
    return {groups().size(), 0};
}

const char* menu_of(const std::string& id) {
    const size_t g = menu_place(id).first;
    return g < groups().size() ? groups()[g].menu : "Other";
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Every word of the search is somewhere in the text.
bool matches(const std::string& text, const std::string& search) {
    const std::string hay = lower(text), words = lower(search);
    for (size_t at = 0; at < words.size();) {
        const size_t end = std::min(words.find(' ', at), words.size());
        if (end > at && hay.find(words.substr(at, end - at)) == std::string::npos) return false;
        at = end + 1;
    }
    return true;
}

std::string keys_text(ImGuiKeyChord k) {
    if (!k) return "";
    return key_label(k) + (needs_numpad(k) ? " numpad keypad number pad" : "");
}

}  // namespace

void App::set_action_keys(const std::string& id, const KeyPair& keys) {
    set_keys(settings_.key_overrides, id, keys, preset_keys_[id]);
    apply_preset();
    save_settings();
}

void App::draw_shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    static bool nav_was_on = false;  // keyboard navigation is off while a key is captured (arrows, Space, Enter)
    auto end_capture = [&] {
        if (std::exchange(nav_was_on, false)) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        capture_slot_ = -1;
        capture_id_.clear();
        skip_shortcuts_ = true;  // the key that ended it runs nothing
    };
    if (!show_shortcuts_) {
        if (capture_slot_ != -1) end_capture();
        return;
    }
    const bool was_capturing = capture_slot_ != -1;  // the Esc that ends a capture does not close the window
    ImGui::SetNextWindowSize(window_size(46, 46), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Keyboard Shortcuts", &show_shortcuts_, ImGuiWindowFlags_NoDocking)) return ImGui::End();

    const float fs = ImGui::GetFontSize();
    const ImU32 accent = accent_colour();
    const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    auto find_action = [&](const std::string& id) -> Action* {
        for (auto& [aid, a] : actions_)
            if (aid == id) return &a;
        return nullptr;
    };

    // A captured key: Esc cancels, Backspace clears, anything else (with its modifiers) binds or asks first.
    if (capture_slot_ >= 0) {
        skip_shortcuts_ = true;
        if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) end_capture();  // clicking elsewhere cancels
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && capture_slot_ >= 0; ++k) {
            if (!bindable_key(ImGuiKey(k)) || !ImGui::IsKeyPressed(ImGuiKey(k), false)) continue;
            const ImGuiKeyChord mods = io.KeyMods & (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super);
            Action* a = find_action(capture_id_);
            if (!a || (k == ImGuiKey_Escape && !mods)) {
                end_capture();
                break;
            }
            KeyPair keys = {a->key, a->key2};
            const ImGuiKeyChord chord = k == ImGuiKey_Backspace && !mods ? 0 : mods | k;
            keys[capture_slot_] = chord;
            std::vector<std::pair<std::string, KeyPair>> bindings;
            for (auto& [aid, b] : actions_) bindings.push_back({aid, {b.key, b.key2}});
            auto [other, slot] = find_conflict(bindings, capture_id_, chord);
            if (!other.empty()) {
                conflict_with_ = other, conflict_slot_ = slot, conflict_chord_ = chord;
                ImGui::OpenPopup("Shortcut in use");
                if (std::exchange(nav_was_on, false)) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                capture_slot_ = -(capture_slot_ + 2);  // -2 / -3: waiting on the popup, the slot kept
            } else {
                set_action_keys(capture_id_, keys);
                status(std::string(a->label) + ": " + (chord ? key_label(chord) : "no key"));
                end_capture();
            }
        }
    }

    // Search, Reset All.
    const char* reset_all = "Reset All";
    const float reset_w = ImGui::CalcTextSize(reset_all).x + fs * 1.6f + ImGui::GetStyle().FramePadding.x * 2 + 8;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(dim, "%s", icon::kFind);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reset_w - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##search", "Search commands, menus or keys, e.g. numpad", shortcut_search_, sizeof shortcut_search_);
    ImGui::SameLine();
    ImGui::BeginDisabled(settings_.key_overrides.empty());
    if (icon_label_button(icon::kUnbake, reset_all, "Every command back to the preset's keys")) ImGui::OpenPopup("Reset all shortcuts?");
    ImGui::EndDisabled();

    // The preset, what is changed, and a way to the keys a keyboard without a number pad cannot press.
    int numpad = 0;
    for (auto& [id, a] : actions_) numpad += needs_numpad(a.key) + needs_numpad(a.key2);
    if (capture_slot_ >= 0) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "Press a key\xE2\x80\xA6 (Esc cancels, Backspace clears)");
    } else {
        const size_t n = settings_.key_overrides.size();
        ImGui::TextColored(dim, "Preset: %s  \xC2\xB7  %s", preset_label(settings_.preset),
                           n ? (std::to_string(n) + (n == 1 ? " command changed" : " commands changed")).c_str()
                             : "no changes");
        if (numpad > 0) {
            ImGui::SameLine();
            ImGui::TextColored(dim, " \xC2\xB7 ");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kKey);
            const std::string show = std::to_string(numpad) + (numpad == 1 ? " key needs" : " keys need") + " a number pad";
            if (ImGui::SmallButton(show.c_str())) std::snprintf(shortcut_search_, sizeof shortcut_search_, "numpad");
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Show them, to give them keys your keyboard has");
        }
    }
    ImGui::Spacing();

    // The table, one section per menu.
    const float key_w = fs * 8.5f, reset_col = ImGui::GetFrameHeight();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuterH |
                                  ImGuiTableFlags_PadOuterX;
    const float table_h = -ImGui::GetTextLineHeightWithSpacing() * 1.6f;
    if (ImGui::BeginTable("##shortcuts", 5, flags, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed, fs * 1.3f);
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, key_w);
        ImGui::TableSetupColumn("Alternate", ImGuiTableColumnFlags_WidthFixed, key_w);
        ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, reset_col);
        ImGui::TableHeadersRow();

        std::vector<size_t> order(actions_.size());  // the actions in menu order
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t x, size_t y) { return menu_place(actions_[x].first) < menu_place(actions_[y].first); });
        int shown = 0;
        const char* last_menu = nullptr;
        {
            for (size_t row : order) {
                auto& [id, a] = actions_[row];
                const char* menu = menu_of(id);
                const std::string text = std::string(a.label) + " " + menu + " " + keys_text(a.key) + " " + keys_text(a.key2);
                if (!matches(text, shortcut_search_)) continue;
                if (menu != last_menu) {  // the menu's name, once it has a row to show
                    last_menu = menu;
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
                    ImGui::TableSetColumnIndex(1);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "%s", menu);
                }
                ++shown;
                const KeyPair preset = preset_keys_[id];
                const bool changed = settings_.key_overrides.count(id) && KeyPair{a.key, a.key2} != preset;
                ImGui::PushID(id.c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                if (changed) {  // a bar in the accent colour marks a key of your own
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float h = ImGui::GetFrameHeight();
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y + 3), ImVec2(p.x - 1, p.y + h - 3), accent, 1.5f);
                }
                if (const char* ic = action_icon(id.c_str())) ImGui::TextColored(dim, "%s", ic);
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(a.label);
                if (changed)
                    ImGui::SetItemTooltip("Changed from the preset (%s)",
                                          preset[0] ? (key_label(preset[0]) + (preset[1] ? ", " + key_label(preset[1]) : "")).c_str()
                                                    : "no key");
                for (int s = 0; s < 2; ++s) {
                    ImGui::TableSetColumnIndex(2 + s);
                    const ImGuiKeyChord k = s ? a.key2 : a.key;
                    const bool capturing = capture_slot_ == s && capture_id_ == id;
                    const bool numpad_key = needs_numpad(k);
                    std::string label = capturing ? "Press a key\xE2\x80\xA6" : k ? key_label(k) : "\xE2\x80\x94";  // an em dash: none
                    // A key sits on a key cap; an empty slot shows only on hover, so the keys stand out.
                    ImGui::PushStyleColor(ImGuiCol_Button, capturing ? (accent & 0x00FFFFFF) | 0x50000000
                                                           : k       ? ImGui::GetColorU32(ImGuiCol_FrameBg)
                                                                     : 0u);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetColorU32(ImGuiCol_FrameBgActive));
                    ImGui::PushStyleColor(ImGuiCol_Text, capturing ? ImGui::GetColorU32(ImGuiCol_Text)
                                                         : numpad_key ? ui::kKey
                                                         : k ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled));
                    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
                    if (ImGui::Button((label + "###k" + char('0' + s)).c_str(), ImVec2(-FLT_MIN, 0)) && capture_slot_ < 0 &&
                        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
                        capture_id_ = id, capture_slot_ = s;
                        nav_was_on = io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard;
                        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
                    }
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(4);
                    if (capturing) ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), accent,
                                                                       ImGui::GetStyle().FrameRounding, 0, 1.5f);
                    else if (numpad_key)
                        ImGui::SetItemTooltip("A number pad key: keyboards without a number pad cannot press it. Click to "
                                              "change it.");
                    else
                        ImGui::SetItemTooltip("Click, then press the new key");
                }
                ImGui::TableSetColumnIndex(4);
                if (changed && icon_small_button("reset", icon::kUnbake, "Back to the preset's keys")) {
                    settings_.key_overrides.erase(id);
                    apply_preset();
                    save_settings();
                }
                ImGui::PopID();
            }
        }
        if (!shown) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(dim, "No command matches \"%s\"", shortcut_search_);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (!host_.world_view())
        hint("Mouse controls follow the preset (Edit > Preferences...). Your keys stay when you change the preset.");

    // The key is another action's: take it from that one, or leave both as they were.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Shortcut in use", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Action *a = find_action(capture_id_), *other = find_action(conflict_with_);
        const int slot = -capture_slot_ - 2;
        bool closed = !a || !other || slot < 0 || slot > 1;
        if (!closed) {
            ImGui::Text("%s is already the key of", key_label(conflict_chord_).c_str());
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "%s  (%s)", other->label, menu_of(conflict_with_));
            ImGui::TextColored(dim, "Replace takes it for %s; %s loses it.", a->label, other->label);
            ImGui::Spacing();
            const float w = fs * 7;
            const bool keys = !ImGui::IsWindowAppearing();  // not the key that was just captured
            if (ImGui::Button("Replace", ImVec2(w, 0)) || (keys && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
                KeyPair theirs = {other->key, other->key2}, mine = {a->key, a->key2};
                theirs[conflict_slot_] = 0;
                mine[slot] = conflict_chord_;
                const std::string label = a->label;
                set_action_keys(conflict_with_, theirs);
                set_action_keys(capture_id_, mine);
                status(label + ": " + key_label(conflict_chord_));
                closed = true;
            }
            ImGui::SameLine();
            closed |= ImGui::Button("Cancel", ImVec2(w, 0)) || (keys && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
        }
        skip_shortcuts_ = true;  // Enter and Esc answer the question and nothing else
        if (closed) ImGui::CloseCurrentPopup(), capture_slot_ = -1, capture_id_.clear();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Reset all shortcuts?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Every command goes back to the %s keys.", preset_label(settings_.preset));
        ImGui::Spacing();
        const float w = fs * 7;
        if (ImGui::Button("Reset All", ImVec2(w, 0))) {
            settings_.key_overrides.clear();
            apply_preset();
            save_settings();
            status("Keyboard shortcuts: the preset's keys");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(w, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (!was_capturing && capture_slot_ == -1 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        show_shortcuts_ = false, skip_shortcuts_ = true;  // Esc closes it and clears nothing
    ImGui::End();
}

}  // namespace vats
