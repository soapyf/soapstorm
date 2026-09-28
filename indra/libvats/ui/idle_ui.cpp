// Viewport Avatar Toolset - the Idle Layer window: breathing and sway layers, live preview and bake (spec 08 IL).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "theme.h"
#include "imgui.h"
#include "vats/idle.h"
#include "vats/loop_tools.h"

namespace vats {

// IL-4: while playing, the unbaked layers ride on top of the evaluated pose. Bones in a limb that uses IK are
// left out, since IK overrides their keys once baked.
void App::apply_idle_preview(Evaluation& e) {
    const Clip& clip = doc_.clip();
    if (!idle_preview_ || !playing_) return;
    std::vector<int> ik;
    for (int n = 0; n < int(skel_.size()); ++n)
        if (int l = rig_->limb_of_bone(n); l >= 0 && l < int(e.limbs.size()) && e.limbs[l].uses_ik) ik.push_back(n);
    bool any = false;
    for (const IdleLayer& l : clip.idle)
        if (!l.baked) apply_idle(skel_, clip, l, frame_, e.pose, ik), any = true;
    if (any) e.globals = skel_.global_pose(e.pose, shape());
}

void App::draw_idle_panel() {
    if (!show_idle_) return;
    place_tool_window(4, 24, 36);
    if (!ImGui::Begin("Idle Layer", &show_idle_)) return ImGui::End();
    help_button("idle-layer");
    Clip& clip = doc_.clip();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("A slow breath and a faint sway on top of the animation, made to loop. Add a layer, "
                       "preview it while playing, then bake it to keys.");
    ImGui::PopStyleColor();

    auto add = [&](const char* label, const char* kind) {
        if (ImGui::Button(label)) {
            IdleLayer l = idle_preset(kind);
            edit("Add Idle Layer", [&](Clip& c) { c.idle.push_back(l); });
            idle_selected_ = int(clip.idle.size()) - 1;
        }
    };
    add("Add Breath", "breath");
    ImGui::SameLine();
    add("Add Sway", "sway");
    ImGui::Checkbox("Preview while playing", &idle_preview_);
    ImGui::SetItemTooltip("Play layers that are not baked yet on top of the animation");

    if (ImGui::BeginListBox("##idle_layers", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 4))) {
        for (int i = 0; i < int(clip.idle.size()); ++i) {
            const IdleLayer& l = clip.idle[i];
            std::string label = std::string(l.kind == "breath" ? "Breath" : "Sway") + ", " +
                                std::to_string(idle_nodes(skel_, l).size()) + " bones" + (l.baked ? "  (baked)" : "") +
                                "##" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), idle_selected_ == i)) idle_selected_ = i;
        }
        ImGui::EndListBox();
    }
    if (idle_selected_ >= int(clip.idle.size())) idle_selected_ = int(clip.idle.size()) - 1;

    if (idle_selected_ >= 0) {
        const int i = idle_selected_;
        IdleLayer& l = clip.idle[i];
        const bool breath = l.kind == "breath";
        ImGui::SeparatorText(breath ? "Breath" : "Sway");
        const float label_w = ImGui::GetFontSize() * 5.5f;
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(-1);
        };
        // One undo step per drag, as in the Dynamics window.
        auto track = [&](const char* step, auto& value, auto before) {
            if (ImGui::IsItemActivated()) {
                auto now = value;
                value = before;
                doc_.history.begin(clip);
                value = now;
            }
            if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit(step, clip)) mark_dirty();
        };
        auto slider = [&](const char* name, double& v, float lo, float hi, const char* fmt, const char* tip) {
            const double v0 = v;
            float f = float(v);
            label(name);
            if (ImGui::SliderFloat((std::string("##idle_") + name).c_str(), &f, lo, hi, fmt)) v = f;
            ImGui::SetItemTooltip("%s", tip);
            track("Idle Layer Settings", v, v0);
        };
        slider("Amplitude", l.amplitude, 0.f, 5.f, "%.2f deg",
               breath ? "How far the chest tilts back on each breath; mTorso rises this many millimetres"
                      : "The largest turn any bone makes, in degrees");
        slider("Period", l.period, 0.5f, 20.f, "%.1f s",
               breath ? "Seconds per breath, before snapping to the loop" : "Seconds between sway changes, before snapping");
        const LoopRange r = loop_range(clip);
        const double frames = idle_period_frames(clip, l), fps = std::max(clip.fps, 1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Text("Snapped: %.2f s, %d per %s", frames / fps, int(std::lround((r.out - r.in) / frames)),
                    clip.loop ? "loop" : "clip");
        ImGui::PopStyleColor();
        if (!breath) {
            int s = l.seed;
            label("Seed");
            if (ImGui::InputInt("##idle_seed", &s)) edit("Idle Layer Settings", [&](Clip& c) { c.idle[i].seed = s; });
            ImGui::SetItemTooltip("Another seed gives another sway; the same seed always gives the same keys");
        }

        label("Bones");
        std::string names;
        for (int n : idle_nodes(skel_, l)) names += (names.empty() ? "" : ", ") + skel_[n].name;
        ImGui::TextWrapped("%s", names.empty() ? "(none)" : names.c_str());
        ImGui::BeginDisabled(l.baked);
        std::vector<std::string> picked;
        for (int n : selection_)
            if (n >= 0 && n < int(skel_.size()) && idle_bone_allowed(skel_[n])) picked.push_back(skel_[n].name);
        ImGui::BeginDisabled(picked.empty());
        if (ImGui::Button("Use Selected Bones")) edit("Idle Layer Bones", [&](Clip& c) { c.idle[i].bones = picked; });
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Face and eye bones, attachment points and collision volumes are skipped");
        ImGui::SameLine();
        if (ImGui::Button("Default Bones"))
            edit("Idle Layer Bones", [&](Clip& c) { c.idle[i].bones = idle_preset(l.kind).bones; });
        ImGui::EndDisabled();
        if (l.baked) ImGui::SetItemTooltip("Unbake before changing the bones");

        if (ImGui::Button(l.baked ? "Re-bake" : "Bake")) {
            edit("Bake Idle Layer", [&](Clip& c) { bake_idle(c, skel_, i); });
            status("Baked the idle layer to keys");
        }
        ImGui::SetItemTooltip("Write the layer onto its bones' keys (one undo step)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!l.baked);
        if (ImGui::Button("Unbake")) edit("Unbake Idle Layer", [&](Clip& c) { unbake_idle(c, skel_, i); });
        ImGui::SetItemTooltip("Put back the keys the bones had before baking");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            edit("Remove Idle Layer", [&](Clip& c) {
                unbake_idle(c, skel_, i);
                c.idle.erase(c.idle.begin() + i);
            });
            idle_selected_ = -1;
        }
        ImGui::SetItemTooltip("Remove the layer and put back its pre-bake keys");
    }
    if (clip.idle.size() > 1 && ImGui::Button("Bake All")) {
        edit("Bake Idle Layer", [&](Clip& c) { bake_idle(c, skel_); });
        status("Baked " + std::to_string(clip.idle.size()) + " idle layers to keys");
    }
    ImGui::End();
}

}  // namespace vats
