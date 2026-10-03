// Viewport Avatar Toolset - Edit > Undo History: the named steps undo can take back, and a click to go to any of them.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.

#include "app.h"
#include "imgui.h"
#include "widgets.h"

namespace vats {

// Steps back and forward through the same Undo and Redo the menu runs, so every sidecar (weight strokes, limit
// suggestions, share drags) unwinds in order too. A step that can't run (an edit still open) ends the walk.
void App::go_to_history_step(size_t undo_count) {
    for (int guard = 0; guard < 2000 && doc_.history.undo_steps().size() > undo_count; ++guard) {
        if (doc_.history.is_open() || scene_busy()) break;  // Undo's own "finish the current edit first"
        run_action("undo");
    }
    for (int guard = 0; guard < 2000 && doc_.history.undo_steps().size() < undo_count; ++guard) {
        if (!doc_.history.can_redo()) break;
        run_action("redo");
    }
}

void App::draw_undo_history() {
    if (!show_undo_history_) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 18, ImGui::GetFontSize() * 24), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Undo History", &show_undo_history_)) return ImGui::End();
    const auto& done = doc_.history.undo_steps();
    const auto& undone = doc_.history.redo_steps();  // the next redo last
    if (done.empty() && undone.empty()) empty_state("Nothing to undo yet: each edit appears here by name.");
    size_t go = SIZE_MAX;
    // The open project as it was, then each step, the current one highlighted; steps undone (redo) below, dimmed.
    if (ImGui::Selectable("(start)", done.empty())) go = 0;
    for (size_t i = 0; i < done.size(); ++i) {
        ImGui::PushID(int(i));
        if (ImGui::Selectable(done[i].label.c_str(), i + 1 == done.size())) go = i + 1;
        if (i + 1 == done.size() && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    for (size_t k = undone.size(); k-- > 0;) {
        ImGui::PushID(int(10000 + k));
        if (ImGui::Selectable(undone[k].label.c_str(), false)) go = done.size() + (undone.size() - k);
        ImGui::SetItemTooltip("Undone: click to redo up to here");
        ImGui::PopID();
    }
    ImGui::PopStyleColor();
    if (go != SIZE_MAX && go != done.size()) go_to_history_step(go);
    ImGui::End();
}

}  // namespace vats
