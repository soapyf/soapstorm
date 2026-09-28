// Viewport Avatar Toolset - live mirror, scratch pose and propagate pose (Edit menu, timeline bar).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section PT. The logic is in the core (pose_tools.h); the graph's curve buffer is in graph_editor.cpp.
#include <cmath>

#include "app.h"
#include "imgui.h"
#include "vats/pose_tools.h"

namespace vats {

void App::mirror_edit(const std::vector<std::string>& tracks) {
    if (mirror_live_) mirror_live(doc_.clip(), skel_, frame_, tracks, settings_.mirror_centre);
}

// ponytail: compares the whole clip every frame while a session runs; keep a dirty flag from edit() if clips get large.
void App::scratch_tick() {
    if (doc_.history.is_open() || scratch_prompt_) return;
    if (scratch_on_ && !scratch_) {
        Scratch s{doc_.clip(), History{}, frame_};
        s.history.set_actor(doc_.project.active);
        std::swap(doc_.history, s.history);
        scratch_ = std::move(s);
    }
    scratch_marks_.clear();
    if (!scratch_) return;
    const bool changed = !(scratch_->base == doc_.clip());
    if (changed) scratch_marks_ = scratch_tracks(scratch_->base, doc_.clip());
    const bool moved = std::fabs(frame_ - scratch_->frame) > 1e-9;
    if (scratch_on_ && !moved) return;
    if (!changed) {  // nothing to keep: follow the playhead, or end
        if (scratch_on_) scratch_->frame = frame_;
        else scratch_end(false);
        return;
    }
    scratch_target_ = frame_;  // back to the scratch frame until the question is answered
    frame_ = scratch_->frame;
    playing_ = false;
    if (settings_.scratch_scrub == "keep" || settings_.scratch_scrub == "discard") {
        scratch_end(settings_.scratch_scrub == "keep");
        frame_ = scratch_target_;
    } else {
        scratch_prompt_ = true;
        scratch_dont_ask_ = false;
    }
}

void App::scratch_end(bool keep) {
    if (!scratch_) return;
    Scratch s = std::move(*scratch_);
    scratch_.reset();
    scratch_marks_.clear();
    Clip working = std::move(doc_.clip());
    std::swap(doc_.history, s.history);  // the document's own history again
    doc_.clip() = s.base;
    graph_.clip_replaced();              // its key selection may name keys only the scratch pose had
    if (working == s.base) return;
    if (!keep) return status("Scratch pose discarded");
    edit("Key Scratch Pose", [&](Clip& c) { c = scratch_commit(s.base, working, s.frame, settings_.scratch_existing_only); });
    status("Scratch pose keyed at frame " + std::to_string(int(std::lround(s.frame))));
}

void App::draw_scratch_prompt() {
    if (!scratch_prompt_) return;
    const char* title = "Scratch Pose";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Keep scratch pose as keys?");
    ImGui::TextDisabled("%zu track(s) changed at frame %d", scratch_marks_.size(), int(std::lround(frame_)));
    ImGui::Spacing();
    ImGui::Checkbox("Don't ask again", &scratch_dont_ask_);
    ImGui::SetItemTooltip("Remember this answer; Preferences > Posing asks again");
    auto answer = [&](int a) {  // 0 keep, 1 discard, 2 cancel
        scratch_prompt_ = false;
        ImGui::CloseCurrentPopup();
        if (a == 2) {
            scratch_on_ = true;  // Cancel also undoes turning Scratch Pose off
            return;
        }
        if (scratch_dont_ask_) settings_.scratch_scrub = a == 0 ? "keep" : "discard", save_settings();
        scratch_end(a == 0);
        set_frame(scratch_target_);
    };
    if (ImGui::Button("Keep")) answer(0);
    ImGui::SameLine();
    if (ImGui::Button("Discard")) answer(1);
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) answer(2);
    ImGui::EndPopup();
}

void App::draw_pose_tool_menu_items() {
    if (ImGui::MenuItem("Scratch Pose", nullptr, scratch_on_)) {
        scratch_on_ = !scratch_on_;
        if (scratch_on_) status("Scratch Pose: edits show but key nothing until Set Key");
    }
    ImGui::SetItemTooltip("Pose freely without writing keys; Set Key keeps the pose, leaving the frame asks");
    const std::vector<std::string> tracks = selected_tracks();
    double a = 0, b = 0;
    const bool range = clip_range(a, b);
    ImGui::BeginDisabled(tracks.empty());
    if (ImGui::BeginMenu("Propagate Pose")) {
        auto item = [&](const char* label, PropagateTo to, bool enabled) {
            if (!ImGui::MenuItem(label, nullptr, false, enabled)) return;
            scratch_end(true);  // a scratch pose is the pose to propagate: its keys first, then the propagation
            int n = 0;
            edit("Propagate Pose", [&](Clip& c) { n = propagate_pose(c, tracks, frame_, to, a, b); });
            status(n ? "Propagated the pose to " + std::to_string(n) + " key(s)" : "No later keys to change");
        };
        item("To Next Key", PropagateTo::NextKey, true);
        item("To Selected Range", PropagateTo::Range, range);
        if (!range) ImGui::SetItemTooltip("Shift-drag on the timeline, or select keys in the graph, to set a range");
        item("To End", PropagateTo::End, true);
        ImGui::EndMenu();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Write the selected bones' pose at this frame onto their later keys");
}

}  // namespace vats
