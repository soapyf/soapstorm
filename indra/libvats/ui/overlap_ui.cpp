// Viewport Avatar Toolset - the Overlap window: follow-through down a chain of keyed bones (spec 08 OV).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "theme.h"
#include "imgui.h"
#include "widgets.h"
#include "vats/dynamics.h"
#include "vats/overlap.h"

namespace vats {

void App::draw_overlap_panel() {
    if (!show_overlap_) return;
    place_tool_window(24, 26);
    if (!ImGui::Begin("Overlap", &show_overlap_)) return ImGui::End();
    help_button("overlap");
    const Clip& clip = doc_.clip();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Select the first bone of a keyed chain (upper arm, tail, spine, finger). Each bone after "
                       "it plays its keys a little later than the one before.");
    ImGui::PopStyleColor();

    // The chain runs down the first-child path, like a dynamics chain.
    const int p = primary();
    DynChain whole;
    if (p >= 0) whole.root = skel_[p].name, whole.length = 64;
    const int depth = p >= 0 ? int(dyn_nodes(skel_, whole, false).size()) : 0;
    std::vector<int> chain;
    if (depth >= 2) {
        overlap_length_ = std::clamp(overlap_length_, 2, depth);
        DynChain d = whole;
        d.length = overlap_length_;
        chain = dyn_nodes(skel_, d, false);
    }

    const float label_w = ImGui::GetFontSize() * 5.5f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
    };
    label("Chain");
    std::string names;
    for (int n : chain) names += (names.empty() ? "" : ", ") + skel_[n].name;
    ImGui::TextWrapped("%s", names.empty() ? "(select a bone with a child)" : names.c_str());
    if (depth >= 2) {
        label("Bones");
        slider_int("##overlap_bones", &overlap_length_, 2, depth);
    }
    float shift = float(overlap_.shift), falloff = float(overlap_.falloff);
    label("Delay");
    if (slider_float("##overlap_delay", &shift, 0.5f, 3.f, "%.1f frames", ImGui::GetFontSize() * 10)) overlap_.shift = shift;
    ImGui::SetItemTooltip("How many frames later each bone moves than the bone before it");
    label("Falloff");
    if (slider_float("##overlap_falloff", &falloff, 0.25f, 1.5f, "%.2f")) overlap_.falloff = falloff;
    ImGui::SetItemTooltip("Each bone swings this many times as far as the one before (1 = unchanged)");
    ImGui::BeginDisabled(clip.loop);
    ImGui::Checkbox("Settle at the end", &overlap_.settle);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(clip.loop ? "A looping clip wraps the delay round the loop instead"
                                    : "Ease back over the last frames so the chain ends in its keyed end pose");

    const std::string why = chain.empty() ? "Select a bone with a child" : overlap_refusal(clip, *rig_, chain);
    ImGui::BeginDisabled(!why.empty());
    if (icon_label_button(icon::kApply, "Apply Overlap")) {
        edit("Overlap", [&](Clip& c) { apply_overlap(c, skel_, chain, overlap_); });
        status("Overlap applied down " + count_noun(chain.size(), "bone"));
    }
    ImGui::EndDisabled();
    if (!why.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", why.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

}  // namespace vats
