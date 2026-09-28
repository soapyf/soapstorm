// Viewport Avatar Toolset - the Dynamics window: dynamic chains, live preview and bake (spec 08 DY).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "theme.h"
#include "imgui.h"
#include "vats/dynamics.h"

namespace vats {

namespace {

// The preset a bone most likely wants, from its name and kind.
const char* guess_kind(const Node& n) {
    if (n.volume) return "jiggle";
    if (n.name.find("Tail") != std::string::npos) return "tail";
    if (n.name.find("Ear") != std::string::npos) return "ears";
    return "tail";
}

// Joints from node down the first-child path, the node included.
int chain_depth(const Skeleton& skel, int node) {
    int depth = 1;
    for (;;) {
        int next = -1;
        for (int c : skel[node].children)
            if (!skel[c].attachment) {
                next = c;
                break;
            }
        if (next < 0) return depth;
        node = next;
        ++depth;
    }
}

}  // namespace

// DY-4: while playing, the unbaked chains swing live on top of the evaluated pose. A jump (scrubbing, a new
// clip, changed settings) restarts the simulation at rest.
void App::apply_dynamics_preview(Evaluation& e) {
    const Clip& clip = doc_.clip();
    std::vector<DynChain> live;
    for (const DynChain& d : clip.dynamics)
        if (!d.baked) live.push_back(d);
    if (!dyn_preview_ || !playing_ || live.empty()) {
        dyn_sim_.reset();
        return;
    }
    const double fps = std::max(clip.fps, 1);
    double span = frame_ - dyn_last_frame_;
    if (span < 0 && clip.loop) span += clip.loop_out - clip.loop_in;  // wrapped round the loop
    if (!dyn_sim_ || live != dyn_chains_ || span < 0 || span > fps) {
        dyn_sim_ = std::make_unique<DynSim>(skel_, live);
        dyn_sim_->reset(e.globals);
        dyn_chains_ = live;
    } else if (span > 0) {
        const int steps = std::max(1, int(std::lround(span / fps * DynSim::kStepsPerSecond)));
        const double dt = span / fps / steps;
        for (int s = 1; s <= steps; ++s) {
            double t = dyn_last_frame_ + span * s / steps;
            if (clip.loop && t > clip.loop_out) t -= clip.loop_out - clip.loop_in;
            dyn_sim_->step(s == steps ? e.globals : vats::evaluate(*rig_, clip, t, shape()).globals, dt);
        }
    }
    dyn_last_frame_ = frame_;
    dyn_sim_->apply(e.globals, e.pose);
    e.globals = skel_.global_pose(e.pose, shape());
}

void App::draw_dynamics_panel() {
    if (!show_dynamics_) return;
    place_tool_window(0, 24, 40);
    if (!ImGui::Begin("Dynamics", &show_dynamics_)) return ImGui::End();
    help_button("dynamics");
    Clip& clip = doc_.clip();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Tails, ears, hair and soft parts swing behind the animation. Select the first bone of a "
                       "chain (or a collision volume such as BELLY) and add it; bake to write the motion as keys.");
    ImGui::PopStyleColor();

    const int p = primary();
    const bool can_add = p >= 0 && !(skel_[p].attachment && !skel_[p].volume);
    ImGui::BeginDisabled(!can_add);
    if (ImGui::Button("Add Chain from Selected Bone")) {
        const Node& n = skel_[p];
        DynChain d = dyn_preset(guess_kind(n), n.name, n.volume ? 1 : chain_depth(skel_, p));
        edit("Add Dynamic Chain", [&](Clip& c) { c.dynamics.push_back(d); });
        dyn_selected_ = int(clip.dynamics.size()) - 1;
    }
    ImGui::EndDisabled();
    if (!can_add) ImGui::SetItemTooltip("Select a joint or a collision volume first");
    ImGui::Checkbox("Preview while playing", &dyn_preview_);
    ImGui::SetItemTooltip("Simulate chains that are not baked yet while the clip plays");

    // Chain list.
    if (ImGui::BeginListBox("##chains", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 5))) {
        for (int i = 0; i < int(clip.dynamics.size()); ++i) {
            const DynChain& d = clip.dynamics[i];
            std::string label = d.root + (d.length > 1 ? " +" + std::to_string(d.length - 1) : "") +
                                (d.baked ? "  (baked)" : "") + "##" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), dyn_selected_ == i)) {
                dyn_selected_ = i;
                if (int n = skel_.find(d.root); n >= 0) select(n, false);
            }
        }
        ImGui::EndListBox();
    }
    if (dyn_selected_ >= int(clip.dynamics.size())) dyn_selected_ = int(clip.dynamics.size()) - 1;

    if (dyn_selected_ >= 0) {
        const int i = dyn_selected_;
        DynChain& d = clip.dynamics[i];
        ImGui::SeparatorText(d.root.c_str());
        // Label on the left, like the rest of the app (spec 06 section 1.1).
        const float label_w = ImGui::GetFontSize() * 5.5f;
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(-1);
        };
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
        auto preset = [&](const char* label, const char* kind) {
            if (ImGui::Button(label)) {
                DynChain q = dyn_preset(kind, d.root, d.length);
                edit("Dynamics Preset", [&](Clip& c) {
                    DynChain& t = c.dynamics[i];
                    t.stiffness = q.stiffness, t.damping = q.damping, t.drag = q.drag, t.gravity = q.gravity,
                    t.radius = q.radius;
                });
            }
        };
        preset("Tail", "tail");
        ImGui::SameLine();
        preset("Ears", "ears");
        ImGui::SameLine();
        preset("Jiggle", "jiggle");
        ImGui::SameLine();
        preset("Overlap", "overlap");
        const int root = skel_.find(d.root);
        if (root >= 0 && !skel_[root].volume) {
            const int len0 = d.length;
            int len = d.length;
            label("Bones");
            if (ImGui::SliderInt("##bones", &len, 1, chain_depth(skel_, root))) d.length = len;
            track("Chain Length", d.length, len0);
        }
        auto slider = [&](const char* name, double& v, float lo, float hi, const char* fmt, const char* tip) {
            const double v0 = v;
            float f = float(v);
            label(name);
            if (ImGui::SliderFloat((std::string("##") + name).c_str(), &f, lo, hi, fmt)) v = f;
            ImGui::SetItemTooltip("%s", tip);
            track("Dynamics Settings", v, v0);
        };
        slider("Stiffness", d.stiffness, 0.f, 1.f, "%.3f", "How hard the chain pulls back to the animated pose");
        slider("Damping", d.damping, 0.f, 1.f, "%.3f", "How quickly swinging calms down");
        slider("Drag", d.drag, 0.f, 0.5f, "%.3f", "Air resistance: slows all motion, not just the swing");
        slider("Gravity", d.gravity, 0.f, 3.f, "%.2f g", "Pull downwards, in multiples of Earth's gravity");
        slider("Radius", d.radius, 0.f, 0.15f, "%.3f m", "How far the chain keeps from the body's collision volumes");

        if (ImGui::Button(d.baked ? "Re-bake" : "Bake")) {
            graph_.snapshot_curves(doc_.clip());  // PT-4
            edit("Bake Dynamics", [&](Clip& c) { bake_dynamics(c, *rig_, export_shape(), i); });
            status("Baked " + d.root + " to keys");
        }
        ImGui::SetItemTooltip("Simulate the whole clip and write the motion as keys (one undo step)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!d.baked);
        if (ImGui::Button("Unbake")) edit("Unbake Dynamics", [&](Clip& c) { unbake_dynamics(c, skel_, i); });
        ImGui::SetItemTooltip("Put back the keys the chain had before baking");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            edit("Remove Dynamic Chain", [&](Clip& c) {
                unbake_dynamics(c, skel_, i);
                c.dynamics.erase(c.dynamics.begin() + i);
            });
            dyn_selected_ = -1;
        }
        ImGui::SetItemTooltip("Remove the chain and put back its pre-bake keys");
    }
    if (clip.dynamics.size() > 1 && ImGui::Button("Bake All")) {
        edit("Bake Dynamics", [&](Clip& c) { bake_dynamics(c, *rig_, export_shape()); });
        status("Baked " + std::to_string(clip.dynamics.size()) + " chains to keys");
    }
    ImGui::End();
}

}  // namespace vats
