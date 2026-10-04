// Viewport Avatar Toolset - Find a Tool (F3): every menu command and tool window, found by typing part of its name.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <string>
#include <vector>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "widgets.h"

namespace vats {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The tool windows the menus open by name (show_window), as the menus label them.
struct ToolWindow {
    const char* label;
    const char* window;
};
constexpr ToolWindow kWindows[] = {
    {"Graph Editor", "graph"},           {"Dope Sheet", "dope-sheet"},           {"Motion Path", "motion-path"},
    {"Motion Capture", "mocap"},         {"Face", "face"},                       {"Face Cam", "face-cam"},
    {"Hand Poser", "hands"},             {"Dynamics", "dynamics"},               {"Ragdoll", "ragdoll"},
    {"Idle Layer", "idle"},              {"Overlap", "overlap"},                 {"Auto-Balance", "auto-balance"},
    {"Jump Arc", "jump-arc"},            {"Clean Up Foot Sliding", "foot-lock"},             {"Loop Assist", "loop-assist"},
    {"Make Transition", "transition"},   {"Simplify Curves", "simplify"},        {"Reference", "reference"},
    {"Actors (Couples and Groups)", "actors"}, {"Clips (AO Sets)", "clips"},     {"Split Dance at Beats", "split-dance"},
    {"Priority Planner", "planner"},     {"Batch Retarget", "batch-retarget"},   {"Animation Check", "check"},
    {"Motion Quality", "quality"},       {"Export", "export"},                   {"Export Listing Media", "listing-media"},
    {"Export Rigged Mesh for SL", "rig-export"}, {"Joint Offset Inspector", "joint-inspector"},
    {"Map Rig to Second Life", "map-rig"}, {"Suggest Joint Limits", "suggest-limits"},
    {"Rig a Model from Scratch", "rig-scratch"}, {"Paint Weights", "paint-weights"}, {"Undo History", "undo-history"},
    {"Preview as SL Plays It", "sl-preview"}, {"Treadmill", "treadmill"},        {"Preferences", "preferences"},
    {"Keyboard Shortcuts", "keys"},      {"Help", "help"},                       {"Properties", "properties"},
    {"Timeline", "timeline"},            {"Bones", "bones"},                     {"Inventory", "inventory"},
    {"Picker", "picker"},
};

}  // namespace

// Every action and tool window whose name holds each typed word; names starting with the first word come first.
std::vector<App::ToolHit> App::tool_hits(const std::string& query) const {
    std::vector<ToolHit> entries;
    auto seen = [&](const std::string& l) {
        const std::string k = lower(l.substr(0, l.find("...")));
        return std::any_of(entries.begin(), entries.end(), [&](const ToolHit& e) { return lower(e.label.substr(0, e.label.find("..."))) == k; });
    };
    for (size_t i = 0; i < actions_.size(); ++i) {
        const Action& a = actions_[i].second;
        if (!a.label || !*a.label || actions_[i].first == "find_tool" || seen(a.label)) continue;
        const char* why = a.unavailable ? a.unavailable() : nullptr;
        entries.push_back({a.label, a.key ? key_label(a.key) : "", why ? why : "", int(i), nullptr, "", false});
    }
    for (const ToolWindow& w : kWindows)
        if (!seen(w.label)) entries.push_back({w.label, "", "", -1, w.window, "", false});
    // Every menu item the crawl found (user test: "seam" found nothing though Tools > Loop Tools > Make Loop Seamless
    // exists): an action or window gets its menu path; any other item comes in, run through its menu.
    for (const MenuEntry& m : menu_index_) {
        if (m.label.empty() || m.path.empty()) continue;
        const std::string k = lower(m.label.substr(0, m.label.find("...")));
        auto same = std::find_if(entries.begin(), entries.end(), [&](const ToolHit& e) {
            return lower(e.label.substr(0, e.label.find("..."))) == k && (!e.menu || e.path == m.path);
        });
        if (same == entries.end()) entries.push_back({m.label, "", "", -1, nullptr, m.path, true});
        else if (same->path.empty()) same->path = m.path;
    }

    const std::string q = lower(query);
    std::vector<std::string> words;
    for (size_t s = 0, e; s < q.size(); s = e + 1) {
        e = q.find(' ', s);
        if (e == std::string::npos) e = q.size();
        if (e > s) words.push_back(q.substr(s, e - s));
    }
    std::vector<ToolHit> hits;
    for (ToolHit& e : entries) {
        const std::string l = lower(e.label + " " + e.path);
        if (std::all_of(words.begin(), words.end(), [&](const std::string& w) { return l.find(w) != std::string::npos; }))
            hits.push_back(std::move(e));
    }
    if (!words.empty())
        std::stable_sort(hits.begin(), hits.end(), [&](const ToolHit& a, const ToolHit& b) {
            return (lower(a.label).rfind(words[0], 0) == 0) > (lower(b.label).rfind(words[0], 0) == 0);
        });
    return hits;
}

void App::run_tool_hit(const ToolHit& e) {
    status(e.label);
    if (e.menu) menu_run_ = {e.path, e.label};  // next frame, once the menu it was picked in has closed
    else if (e.action >= 0) run_action(actions_[size_t(e.action)].first.c_str());
    else show_window(e.window);
}

// The menu bar. When no menu or field is busy, a crawl runs with it (icon_button.h): once at the start, to index the
// menus for the searches, and for a picked search hit that is a plain menu item, to run that item.
void App::draw_menu_bar() {
    const bool idle = ImGui::GetFrameCount() > 2 && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive();
    const bool crawl = idle && (!menu_indexed_ || menu_run_);
    if (crawl) {
        start_menu_crawl(menu_run_ ? menu_run_->first : "", menu_run_ ? menu_run_->second : "");
        for (ImGuiCol c : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive})  // no open-menu highlight
            ImGui::PushStyleColor(c, ImVec4(0, 0, 0, 0));
    }
    draw_menus();
    if (!crawl) return;
    ImGui::PopStyleColor(3);
    bool ran = false;
    std::vector<MenuEntry> found = finish_menu_crawl(ran);
    if (!menu_indexed_) menu_index_ = std::move(found), menu_indexed_ = true;
    if (menu_run_ && !ran) status(menu_run_->second + " is not available now");
    menu_run_.reset();
}

// The search at the top of each menu: typing replaces the menu with the matching tools from every menu. True while
// it shows them, so the menu's own items are skipped.
bool App::menu_search(const char* menu) {
    if (ImGui::IsWindowAppearing() || menu_search_menu_ != menu) {
        menu_search_menu_ = menu, menu_search_.clear(), menu_search_sel_ = 0;
        ImGui::SetKeyboardFocusHere();
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", menu_search_.c_str());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    const std::string hint = std::string(icon::kFind) + "  Search";
    if (filter_input("##menu_search", hint.c_str(), buf, sizeof(buf))) menu_search_ = buf, menu_search_sel_ = 0;
    if (ImGui::IsItemActive())  // the arrows pick a match here instead of moving ImGui's menu focus
        ImGui::SetItemKeyOwner(ImGuiKey_UpArrow), ImGui::SetItemKeyOwner(ImGuiKey_DownArrow);
    if (menu_search_.empty()) {
        ImGui::Separator();
        return false;
    }
    const std::vector<ToolHit> hits = tool_hits(menu_search_);
    const int shown = std::min(int(hits.size()), 16);
    menu_search_sel_ = std::clamp(menu_search_sel_, 0, std::max(0, shown - 1));
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) menu_search_sel_ = std::min(menu_search_sel_ + 1, shown - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) menu_search_sel_ = std::max(menu_search_sel_ - 1, 0);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && shown > 0) {
        const ToolHit& e = hits[size_t(menu_search_sel_)];
        if (!e.why.empty()) return status(e.label + ": " + e.why), true;
        ImGui::CloseCurrentPopup();
        run_tool_hit(e);
        return true;
    }
    ImGui::Separator();
    float names_w = 0;  // the keys line up in a column after the longest name
    for (int i = 0; i < shown; ++i) names_w = std::max(names_w, ImGui::CalcTextSize(hits[size_t(i)].label.c_str()).x);
    const float key_x = ImGui::GetCursorPosX() + names_w + ImGui::GetFontSize() * 2;
    for (int i = 0; i < shown; ++i) {
        const ToolHit& e = hits[size_t(i)];
        ImGui::PushID(i);
        // Enter runs the highlighted one; it starts on the best match.
        const ImGuiSelectableFlags off = e.why.empty() ? 0 : ImGuiSelectableFlags_Disabled;
        if (ImGui::Selectable(e.label.c_str(), i == menu_search_sel_, off | ImGuiSelectableFlags_SpanAllColumns)) run_tool_hit(e);
        if (!e.why.empty()) ImGui::SetItemTooltip("%s", e.why.c_str());
        if (!e.key.empty() || !e.path.empty()) {
            ImGui::SameLine(key_x);
            ImGui::TextDisabled("%s%s%s", e.key.c_str(), e.key.empty() || e.path.empty() ? "" : "   ", e.path.c_str());
        }
        ImGui::PopID();
    }
    if (hits.empty()) ImGui::TextDisabled("Nothing by that name");
    else if (int(hits.size()) > shown) ImGui::TextDisabled("%d more: type more of the name", int(hits.size()) - shown);
    return true;
}

bool App::begin_top_menu(const char* label) {
    if (!begin_menu_icon(nullptr, label)) return false;
    if (menu_crawling() || !menu_search(label)) return true;
    ImGui::EndMenu();
    return false;
}

void App::draw_tool_search() {
    if (!show_tool_search_) return;
    const std::vector<ToolHit> hit_list = tool_hits(tool_search_);
    std::vector<const ToolHit*> hits;
    for (const ToolHit& h : hit_list) hits.push_back(&h);
    using Entry = ToolHit;
    tool_search_sel_ = std::clamp(tool_search_sel_, 0, std::max(0, int(hits.size()) - 1));

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(ImGui::GetFontSize() * 30, vp->WorkSize.x - 32);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - w) / 2, vp->WorkPos.y + vp->WorkSize.y * 0.12f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    bool open = true;
    if (!ImGui::Begin("Find a Tool", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    auto run = [&](const Entry& e) {
        show_tool_search_ = false;
        run_tool_hit(e);
    };
    if (tool_search_focus_) ImGui::SetKeyboardFocusHere(), tool_search_focus_ = false;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", tool_search_.c_str());
    ImGui::SetNextItemWidth(-1);
    if (filter_input("##tool_search", "Type part of a tool's name", buf, sizeof(buf))) {
        tool_search_ = buf;
        tool_search_sel_ = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) tool_search_sel_ = std::min(tool_search_sel_ + 1, int(hits.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) tool_search_sel_ = std::max(tool_search_sel_ - 1, 0);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !hits.empty()) run(*hits[size_t(tool_search_sel_)]);

    ImGui::BeginChild("##tool_hits", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12), false);
    for (size_t i = 0; i < hits.size() && show_tool_search_; ++i) {
        const Entry& e = *hits[i];
        ImGui::PushID(int(i));
        if (!e.why.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(e.label.c_str(), int(i) == tool_search_sel_)) run(e);
        if (int(i) == tool_search_sel_ && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
            ImGui::SetScrollHereY();
        if (!e.why.empty()) {
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", e.why.c_str());
        }
        if (const std::string side = e.key + (e.key.empty() || e.path.empty() ? "" : "   ") + e.path; !side.empty()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(side.c_str()).x + ImGui::GetCursorPosX() - 4);
            ImGui::TextDisabled("%s", side.c_str());
        }
        ImGui::PopID();
    }
    if (hits.empty()) ImGui::TextDisabled("Nothing by that name");
    ImGui::EndChild();
    ImGui::End();
    if (!open) show_tool_search_ = false;
}

}  // namespace vats
