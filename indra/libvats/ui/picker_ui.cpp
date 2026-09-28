// Viewport Avatar Toolset - the Picker tab: a clickable avatar outline, hand and face sub-pickers, and selection sets.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 24 (PK-1, PK-2, SS-1..SS-3). The layout and the set logic are in the core (picker.h,
// selection_sets.h); everything here is drawn with ImGui draw lists, so the viewer has it too.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "app.h"
#include "imgui_internal.h"
#include "theme.h"
#include "vats/edit.h"
#include "vats/picker.h"
#include "vats/selection_sets.h"

namespace vats {

namespace {

// Region states, as the Bones list colours its rows (spec 06 4.1 item 5); IK has its own violet.
constexpr ImU32 kPinned = IM_COL32(140, 217, 255, 255), kIk = IM_COL32(224, 130, 230, 255),
                kKeyHere = IM_COL32(255, 217, 77, 255), kKeyed = IM_COL32(242, 191, 128, 255),
                kPlain = IM_COL32(150, 156, 168, 255);

ImU32 with_alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(std::clamp(a, 0.f, 1.f) * 255) << 24); }

std::string sets_path(const std::string& dir) { return dir + "selection_sets.json"; }

}  // namespace

void App::load_library_sets() {
    if (library_sets_loaded_) return;
    library_sets_loaded_ = true;
    const std::string path = sets_path(library_dir());
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string err;
    if (!load_selection_sets(ss.str(), library_sets_, err)) {
        // Kept aside rather than overwritten by the next save (03 P10), as the pose library does.
        const std::string aside = path + ".corrupt-" + std::to_string(host_.ticks_ns() / 1000000);
        std::rename(path.c_str(), aside.c_str());
        message("Selection sets damaged", "They could not be read (" + err + ") and were renamed to\n" + aside);
    }
}

void App::save_library_sets() {
    std::string why;
    if (!write_text(sets_path(library_dir()), save_selection_sets(library_sets_), false, why))
        message("Could not save the selection sets", why);
}

void App::draw_picker_panel() {
    // A layout saved before the Picker existed: open it as a tab beside Bones.
    if (ImGuiWindow* bones = ImGui::FindWindowByName("Bones"); bones && bones->DockId)
        ImGui::SetNextWindowDockID(bones->DockId, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Picker")) return ImGui::End();
    load_library_sets();
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& st = ImGui::GetStyle();

    // View buttons, flowing onto a second row when the panel is narrow.
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    for (int v = 0; v < kPickerViewCount; ++v) {
        const char* name = picker_view_name(PickerView(v));
        const float w = ImGui::CalcTextSize(name).x + 2 * st.FramePadding.x;
        if (v && ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= right) ImGui::SameLine();
        const bool on = picker_view_ == v;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(name)) picker_view_ = v;
        if (on) ImGui::PopStyleColor();
    }
    const PickerView view = PickerView(picker_view_);

    // The canvas: as wide as the panel, but leaving room for the sets below.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float h_units = picker_height(view);
    const float scale = std::max(1.f, std::min(avail.x / kPickerWidth, std::max(avail.y * 0.6f, 160.f) / h_units));
    const ImVec2 size(avail.x, scale * h_units);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float ox = o.x + (avail.x - scale * kPickerWidth) / 2;
    ImGui::InvisibleButton("##picker", size);
    const bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), timeline_background(), 6);
    auto at = [&](float x, float y) { return ImVec2(ox + x * scale, o.y + y * scale); };
    if (view == PickerView::Face)  // the head's outline around the face regions
        dl->AddEllipse(at(0.5f, 0.47f), ImVec2(0.36f * scale, 0.5f * scale), IM_COL32(255, 255, 255, 40), 0, 0, 1.5f);

    const Clip& clip = doc_.clip();
    const std::vector<PickerRegion>& regions = picker_regions(view);
    auto inside = [&](const PickerRegion& r, ImVec2 m) {
        const ImVec2 a = at(r.x, r.y), b = at(r.x + r.w, r.y + r.h);
        if (!r.round) return m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y;
        const float dx = (m.x - (a.x + b.x) / 2) / ((b.x - a.x) / 2), dy = (m.y - (a.y + b.y) / 2) / ((b.y - a.y) / 2);
        return dx * dx + dy * dy <= 1;
    };
    int hot = -1;  // a later region wins where they overlap
    if (hovered)
        for (int i = int(regions.size()) - 1; i >= 0 && hot < 0; --i)
            if (inside(regions[i], io.MousePos)) hot = i;

    for (int i = 0; i < int(regions.size()); ++i) {
        const PickerRegion& r = regions[i];
        const std::vector<int> nodes = region_nodes(skel_, r);
        bool pinned = false, ik = false, key_here = false, keyed = false, selected = false;
        for (int n : nodes) {
            pinned = pinned || pin_at(clip, *rig_, n, frame_) >= 0;
            const int l = rig_->limb_of_bone(n);
            ik = ik || (l >= 0 && l < int(limb_states_.size()) && limb_states_[l].ik_on);
            key_here = key_here || has_key_at(clip, skel_[n].name, frame_);
            keyed = keyed || clip.curves.count(skel_[n].name);
            selected = selected || std::find(selection_.begin(), selection_.end(), n) != selection_.end();
        }
        const ImU32 c = nodes.empty() ? with_alpha(kPlain, 0.3f) : pinned ? kPinned : ik ? kIk : key_here ? kKeyHere
                                                                  : keyed ? kKeyed : kPlain;
        const ImU32 fill = selected ? with_alpha(accent_colour(), 0.75f) : with_alpha(c, i == hot ? 0.55f : 0.3f);
        const ImU32 edge = selected ? accent_colour() : c;
        const ImVec2 a = at(r.x, r.y), b = at(r.x + r.w, r.y + r.h);
        if (r.round) {
            const ImVec2 mid((a.x + b.x) / 2, (a.y + b.y) / 2), rad((b.x - a.x) / 2, (b.y - a.y) / 2);
            dl->AddEllipseFilled(mid, rad, fill);
            dl->AddEllipse(mid, rad, edge, 0, 0, selected ? 2.f : 1.f);
        } else {
            const float round = std::min(b.x - a.x, b.y - a.y) * 0.3f;
            dl->AddRectFilled(a, b, fill, round);
            dl->AddRect(a, b, edge, round, 0, selected ? 2.f : 1.f);
        }
        if (i != hot) continue;
        std::string bones;
        for (const std::string& n : r.bones) bones += (bones.empty() ? "" : ", ") + n;
        ImGui::SetTooltip("%s\n%s%s", r.label.c_str(), bones.c_str(),
                          nodes.empty() ? "\n(not in this skeleton)" : "\nClick to select, Shift+click to add");
        if (clicked && !nodes.empty()) {
            if (!io.KeyShift) clear_selection();
            selected_prop_ = -1, handle_primary_ = false;
            for (int n : nodes)
                if (std::find(selection_.begin(), selection_.end(), n) == selection_.end()) selection_.push_back(n);
            status(r.label + (io.KeyShift ? " added: " : ": ") + std::to_string(selection_.size()) + " bone(s) selected");
        }
    }

    // Selection sets (SS-1..SS-3).
    ImGui::SeparatorText("Selection Sets");
    std::vector<std::string> names;
    for (int n : selection_) names.push_back(skel_[n].name);
    auto recall = [&](const SelectionSet& s) {
        const std::vector<int> nodes = recall_selection_set(skel_, s);
        if (nodes.empty()) return status("None of " + s.name + "'s bones are in this skeleton");
        if (!io.KeyShift) clear_selection();
        selected_prop_ = -1, handle_primary_ = false;
        for (int n : nodes)
            if (std::find(selection_.begin(), selection_.end(), n) == selection_.end()) selection_.push_back(n);
        status(s.name + ": " + std::to_string(selection_.size()) + " bone(s) selected");
    };
    auto bone_list = [](const SelectionSet& s) {
        std::string t;
        for (const std::string& b : s.bones) t += (t.empty() ? "" : ", ") + b;
        return t.empty() ? std::string("(no bones)") : t;
    };
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s", set_name_.c_str());
    const float save_w = ImGui::CalcTextSize("Save Set").x + 2 * st.FramePadding.x;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - save_w - st.ItemSpacing.x, 40.f));
    if (ImGui::InputTextWithHint("##set_name", "Set name", buf, sizeof buf)) set_name_ = buf;
    ImGui::SameLine();
    ImGui::BeginDisabled(names.empty() || set_name_.empty());
    if (ImGui::Button("Save Set")) {
        edit("Save Selection Set", [&](Clip& c) { store_selection_set(c.selection_sets, set_name_, names); });
        if (set_to_library_) store_selection_set(library_sets_, set_name_, names), save_library_sets();
        status("Saved " + set_name_ + ": " + std::to_string(names.size()) + " bone(s)");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", names.empty() ? "Select bones first" : "Save the selected bones under this name; "
                                                                        "a set of the same name is replaced");
    ImGui::Checkbox("Also save to the library", &set_to_library_);
    ImGui::SetItemTooltip("Library sets are kept beside the pose library, for every project");

    int remove = -1;
    const std::vector<SelectionSet>& sets = clip.selection_sets;
    if (sets.empty()) hint("No sets in this project.");
    for (int i = 0; i < int(sets.size()); ++i) {
        ImGui::PushID(i);
        const std::string label = sets[i].name + "  (" + std::to_string(sets[i].bones.size()) + ")";
        if (ImGui::Selectable(label.c_str())) recall(sets[i]);
        ImGui::SetItemTooltip("%s\nClick to select, Shift+click to add, right-click to edit", bone_list(sets[i]).c_str());
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Add Selected Bones", nullptr, false, !names.empty()))
                edit("Add to Selection Set", [&](Clip& c) { edit_selection_set(c.selection_sets[i], names, true); });
            if (ImGui::MenuItem("Remove Selected Bones", nullptr, false, !names.empty()))
                edit("Remove from Selection Set", [&](Clip& c) { edit_selection_set(c.selection_sets[i], names, false); });
            if (ImGui::MenuItem("Save to Library")) {
                store_selection_set(library_sets_, sets[i].name, sets[i].bones);
                save_library_sets();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Set")) remove = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (remove >= 0) edit("Delete Selection Set", [&](Clip& c) { c.selection_sets.erase(c.selection_sets.begin() + remove); });

    if (!library_sets_.empty()) {
        ImGui::SeparatorText("Library");
        int drop = -1;
        for (int i = 0; i < int(library_sets_.size()); ++i) {
            ImGui::PushID(1000 + i);
            const SelectionSet& s = library_sets_[i];
            if (ImGui::Selectable((s.name + "  (" + std::to_string(s.bones.size()) + ")").c_str())) recall(s);
            ImGui::SetItemTooltip("%s\nClick to select, Shift+click to add, right-click for more", bone_list(s).c_str());
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Add to Project"))
                    edit("Save Selection Set", [&](Clip& c) { store_selection_set(c.selection_sets, s.name, s.bones); });
                if (ImGui::MenuItem("Delete from Library")) drop = i;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (drop >= 0) {
            library_sets_.erase(library_sets_.begin() + drop);
            save_library_sets();
        }
    }
    ImGui::End();
}

}  // namespace vats
