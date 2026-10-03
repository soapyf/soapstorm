// Viewport Avatar Toolset - the Ragdoll window: let the body or the selected limbs fall limp (spec 08 RD).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "icons.h"
#include "icon_button.h"
#include "theme.h"
#include "imgui.h"
#include "widgets.h"
#include "vats/ragdoll.h"

namespace vats {

// The simulated preview replaces the evaluated pose while it still matches the clip; any edit to the
// animation or the ragdoll settings drops it.
void App::apply_ragdoll_preview(Evaluation& e) {
    if (rd_frames_.empty()) return;
    const Clip& clip = doc_.clip();
    if (clip.ragdoll != rd_for_ || clip.curves != rd_curves_for_ || int(rd_frames_.size()) != clip.end_frame + 1) {
        rd_frames_.clear();
        return;
    }
    const int a = std::clamp(int(std::floor(frame_)), 0, clip.end_frame), b = std::min(a + 1, clip.end_frame);
    const double t = std::clamp(frame_ - a, 0.0, 1.0);
    for (int n = 0; n < skel_.size(); ++n) {
        e.pose.rot[n] = nlerp(rd_frames_[a].rot[n], rd_frames_[b].rot[n], t);
        e.pose.offset[n] = rd_frames_[a].offset[n] + (rd_frames_[b].offset[n] - rd_frames_[a].offset[n]) * t;
    }
    e.globals = skel_.global_pose(e.pose, shape());
}

void App::draw_ragdoll_panel() {
    if (!show_ragdoll_) return;
    place_tool_window("Ragdoll", 24, 30);
    if (!ImGui::Begin("Ragdoll", &show_ragdoll_)) return ImGui::End();
    help_button("ragdoll");
    Clip& clip = doc_.clip();
    hint("Let the body, or the selected limbs, fall limp from a frame.");

    if (!clip.ragdoll) {
        if (primary_button("Set Up Ragdoll", "", 0, icon::kRagdoll)) {
            Ragdoll r;
            r.start = std::clamp(int(std::lround(frame_)), 0, clip.end_frame);
            r.frames = std::max(1, std::min(60, clip.end_frame - r.start));
            edit("Ragdoll", [&](Clip& c) { c.ragdoll = r; });
        }
        ImGui::End();
        return;
    }
    Ragdoll& rd = *clip.ragdoll;
    // Drags are one undo step each: opened on activation, committed on release. A slider jumps to the click on
    // its first frame, so the snapshot is taken with the value put back to what it was before the widget.
    auto track = [&](const char* step, auto& value, auto before) {
        if (ImGui::IsItemActivated()) {
            auto now = value;
            value = before;
            doc_.history.begin(clip);
            value = now;
        }
        if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit(step, clip)) mark_dirty();
    };

    // RD-1: what falls.
    int who = rd.whole_body ? 0 : 1;
    bool changed = ImGui::RadioButton("Whole body", &who, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Selected bones", &who, 1);
    if (changed) edit("Ragdoll Bones", [&](Clip& c) { c.ragdoll->whole_body = who == 0; });
    if (!rd.whole_body) {
        std::string list;
        for (auto& b : rd.bones) list += (list.empty() ? "" : ", ") + b;
        ImGui::TextWrapped("%s", list.empty() ? "(none yet)" : list.c_str());
        ImGui::BeginDisabled(selection_.empty());
        if (ImGui::Button("Use the Selected Bones")) {
            std::vector<std::string> names;
            for (int n : selection_) names.push_back(skel_[n].name);
            edit("Ragdoll Bones", [&](Clip& c) { c.ragdoll->bones = names; });
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("These bones and every limb joint below them fall limp; the rest keeps its animation");
    }

    // Label on the left, like the rest of the app (spec 06 section 1.1).
    auto label = [&](const char* text, float w = -1) {
        labelled_row(text);
        if (w > 0) ImGui::SetNextItemWidth(w);
    };
    subheading("Frames");
    auto int_field = [&](const char* name, int& v, int lo, int hi, const char* tip, float w = -1) {
        const int x0 = v;
        int x = v;
        label(name, w);
        if (ImGui::DragInt((std::string("##") + name).c_str(), &x, 0.2f, lo, hi)) v = std::clamp(x, lo, hi);
        ImGui::SetItemTooltip("%s", tip);
        track("Ragdoll Settings", v, x0);
    };
    int_field("Start", rd.start, 0, clip.end_frame, "The frame the ragdoll takes over from the animation",
              ImGui::GetFontSize() * 6);
    ImGui::SameLine();
    if (ImGui::Button("Current")) edit("Ragdoll Settings", [&](Clip& c) {
        c.ragdoll->start = std::clamp(int(std::lround(frame_)), 0, c.end_frame);
    });
    int_field("Length", rd.frames, 1, std::max(1, clip.end_frame), "How many frames it falls for");
    int_field("Blend in", rd.blend_in, 0, 60, "Frames to ease from the animation into the fall");
    int_field("Blend out", rd.blend_out, 0, 60, "Frames to ease back to the animation at the end (0 = stay down)");

    subheading("Body");
    auto slider = [&](const char* name, double& v, float lo, float hi, const char* fmt, const char* tip) {
        const double v0 = v;
        float f = float(v);
        label(name);
        if (slider_float((std::string("##") + name).c_str(), &f, lo, hi, fmt)) v = f;
        ImGui::SetItemTooltip("%s", tip);
        track("Ragdoll Settings", v, v0);
    };
    slider("Gravity", rd.gravity, 0.f, 2.f, "%.2f g", "Pull downwards, in multiples of Earth's gravity");
    slider("Stiffness", rd.stiffness, 0.f, 1.f, "%.2f", "Muscle: how hard joints pull towards the animated pose (0 = limp)");
    slider("Friction", rd.friction, 0.f, 1.f, "%.2f", "Grip on the ground and on props");
    {
        static const char* const dirs[] = {"forward", "back", "left", "right", "random", "none"};
        const Json* cur = rd.extra.find("fall_direction");
        int d = 0;
        for (int i = 0; i < 6; ++i)
            if (cur && cur->is_string() && cur->str == dirs[i]) d = i;
        label("Fall direction");
        if (ImGui::Combo("##fall", &d, "Forward\0Back\0Left\0Right\0Random\0None\0")) {
            std::string pick = dirs[d];
            edit("Ragdoll Settings", [&](Clip& c) { c.ragdoll->extra.set("fall_direction", pick); });
        }
        ImGui::SetItemTooltip("Which way a standing body topples when it goes limp (random is repeatable)");
    }

    // Limbs in IK or pinned would keep to their targets; the bake switches them to FK over the fall.
    std::vector<std::string> ik_limbs;
    {
        RagdollSolver probe(skel_, rd, {});
        const auto& nodes = probe.nodes();
        const int stop = std::min(clip.end_frame, rd.start + rd.frames);
        for (int l = 0; l < int(rig_->limbs().size()); ++l) {
            const LimbInfo& li = rig_->limbs()[l];
            if (std::find(nodes.begin(), nodes.end(), li.root) == nodes.end()) continue;
            bool held = false;
            for (const Pin& p : clip.pins)
                held |= pin_limb(*rig_, p) == l && p.from <= stop && (p.to < 0 || p.to >= rd.start);
            for (int f = rd.start; !held && f <= stop; f += 5) held = vats::evaluate(*rig_, clip, f, export_shape()).limbs[l].ik_on;
            if (held) ik_limbs.push_back(li.label);
        }
    }
    if (!ik_limbs.empty()) {
        std::string list;
        for (auto& l : ik_limbs) list += (list.empty() ? "" : ", ") + l;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("In IK or pinned in this range: %s. The bake switches them to FK over the fall and back "
                           "after; Clear puts the IK and pins back.", list.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    // Static props are boxes the ragdoll lands on (RD-2); rigged props move with the avatar, so not those.
    std::vector<RagdollBox> boxes;
    for (const Prop& p : clip.props)
        if (const DaeModel* m = !p.rigged && p.visible ? prop_model(p.path) : nullptr)
            boxes.push_back({prop_frame(p), (m->bounds_max - m->bounds_min).mul(p.scale) * 0.5});  // drawn box-centred (VP-81)
    if (primary_button("Simulate", "", 0, icon::kRagdoll)) {
        rd_frames_ = simulate_ragdoll(*rig_, clip, export_shape(), boxes);
        rd_for_ = clip.ragdoll;
        rd_curves_for_ = clip.curves;
        set_frame(rd.start);
        status("Simulated frames " + std::to_string(rd.start) + " to " +
               std::to_string(std::min(clip.end_frame, rd.start + rd.frames)) + ": scrub or play to see it");
    }
    ImGui::SetItemTooltip("Run the fall and show it in the view without changing any keys");
    ImGui::SameLine();
    if (ImGui::Button(rd.baked ? "Re-bake" : "Bake")) {
        edit("Bake Ragdoll", [&](Clip& c) { bake_ragdoll(c, *rig_, export_shape(), boxes); });
        rd_frames_.clear();
        status("Baked the ragdoll to keys");
    }
    ImGui::SetItemTooltip("Write the fall as keys (one undo step); re-baking starts again from the original keys");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        edit("Clear Ragdoll", [&](Clip& c) {
            unbake_ragdoll(c, skel_);
            c.ragdoll.reset();
        });
        rd_frames_.clear();
    }
    ImGui::SetItemTooltip("Put back the keys from before the bake and remove the ragdoll");
    if (!rd_frames_.empty()) ImGui::TextDisabled("Showing the simulated preview");
    ImGui::End();
}

}  // namespace vats
