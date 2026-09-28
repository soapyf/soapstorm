// Viewport Avatar Toolset - pinned ghosts: a frame, another actor's frame or a library pose, drawn until removed.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 ON-5. They go through draw_ghost (the app) and draw_world_extras (the viewer), as the onion
// ghosts do. A view aid like the onion settings: not saved and not undone; each is evaluated every frame, so it
// always shows the clip as it is now (after an undo too).
#include <algorithm>
#include <cmath>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"

namespace vats {

void App::pin_ghost(int actor, double frame) {
    const Project& p = doc_.project;
    const std::string name = p.actors.empty() ? "" : p.actors[actor].name;
    const std::string at = std::to_string(int(std::round(frame)));
    pinned_ghosts_.push_back({actor == p.active ? "Frame " + at : name + ", frame " + at, name, std::round(frame), std::nullopt});
}

void App::pin_pose_ghost(const LibraryItem& pose) { pinned_ghosts_.push_back({"Pose " + pose.name, "", 0, pose}); }

// Each pinned ghost's globals in the active actor's space; one that names a removed actor is skipped.
std::vector<std::vector<Xform>> App::pinned_ghost_poses() {
    std::vector<std::vector<Xform>> out;
    if (!rig_) return out;
    const Project& p = doc_.project;
    for (const PinnedGhost& g : pinned_ghosts_) {
        if (g.pose) {
            out.push_back(pose_ghost(*rig_, doc_.clip(), frame_, shape(), *g.pose));
            continue;
        }
        int i = p.active;
        if (!g.actor.empty()) {
            i = -1;
            for (int k = 0; k < int(p.actors.size()); ++k)
                if (p.actors[k].name == g.actor) i = k;
            if (i < 0) continue;
        }
        if (i == p.active) {
            out.push_back(vats::evaluate(*rig_, doc_.clip(), g.frame, shape()).globals);
        } else {
            Evaluation e = evaluate_actor(i, g.frame);
            const Xform rel = actor_rel(i);
            for (Xform& x : e.globals) x = rel * x;
            out.push_back(std::move(e.globals));
        }
    }
    return out;
}

// The Onion Skin menu's part: pin, the other actors, and the pinned list with a remove button each.
void App::draw_pinned_ghost_menu() {
    const Project& p = doc_.project;
    ImGui::SeparatorText("Pinned Ghosts");
    if (menu_item_icon(icon::kPin, "Pin Ghost at This Frame")) pin_ghost(p.active, frame_);
    ImGui::SetItemTooltip("Keep a violet ghost of this frame's pose in the view while you work elsewhere");
    if (p.actors.size() > 1 && ImGui::BeginMenu("Ghost Other Actor at Frame")) {
        for (int i = 0; i < int(p.actors.size()); ++i)
            if (i != p.active && ImGui::MenuItem(p.actors[i].name.c_str())) pin_ghost(i, frame_);
        ImGui::EndMenu();
    }
    int remove = -1;
    for (int i = 0; i < int(pinned_ghosts_.size()); ++i) {
        ImGui::PushID(i);
        if (icon_small_button("remove", icon::kDelete, "Remove this ghost")) remove = i;
        ImGui::SameLine();
        ImGui::TextUnformatted(pinned_ghosts_[i].label.c_str());
        ImGui::PopID();
    }
    if (remove >= 0) pinned_ghosts_.erase(pinned_ghosts_.begin() + remove);
    if (pinned_ghosts_.size() > 1 && menu_item_icon(icon::kDelete, "Remove All Pinned Ghosts")) pinned_ghosts_.clear();
}

}  // namespace vats
