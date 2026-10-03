// Viewport Avatar Toolset - Tools > Motion Quality...: the numbers each clean-up tool lowers, before and after.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 15 (MQ). The numbers are in the core (motion_quality.h). The before and after come
// from the undo history, so no tool needs a hook: the last step a clean-up tool recorded is the one shown.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "widgets.h"
#include "imgui.h"
#include "theme.h"

namespace vats {
namespace {

// Undo labels of the tools the numbers are for.
bool quality_tool(const std::string& label) {
    for (const char* p : {"Clean Up", "Bake ", "Filter Curves", "Simplify Curves", "Fit ", "Make Loop Seamless", "Remove Hip Travel"})
        if (label.rfind(p, 0) == 0) return true;
    return false;
}

}  // namespace

// ponytail: measuring exports the clip and evaluates every frame, so "now" is measured again after every edit while
// the window is open and has no clean-up step to show; measure on demand if long clips make edits stutter.
void App::draw_quality_panel() {
    if (!show_quality_) return;
    place_tool_window("Motion Quality", 26, 17);
    if (!ImGui::Begin("Motion Quality", &show_quality_)) return ImGui::End();
    help_button("motion-quality");
    const History& h = doc_.history;
    if (h.serial() != quality_serial_ && !h.is_open()) {
        quality_serial_ = h.serial();
        const History::Step* step = nullptr;
        for (auto it = h.undo_steps().rbegin(); it != h.undo_steps().rend() && !step; ++it)
            if (!it->scene_before && it->actor == doc_.project.active && quality_tool(it->label)) step = &*it;
        const AnimExportOptions opt = anim_export_options();
        if (!step) {
            quality_label_.clear();
            quality_[1] = measure_quality(*rig_, doc_.clip(), opt);
        } else if (step->label != quality_label_ || !(step->after == quality_after_)) {
            quality_label_ = step->label, quality_after_ = step->after;
            quality_[0] = measure_quality(*rig_, step->before, opt);
            quality_[1] = measure_quality(*rig_, step->after, opt);
        }
    }
    const bool compare = !quality_label_.empty();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (compare) ImGui::TextWrapped("Before and after the last clean-up: %s.", quality_label_.c_str());
    else ImGui::TextWrapped("The animation now; a clean-up adds its before and after here.");
    ImGui::PopStyleColor();

    const MotionQuality &b = quality_[0], &a = quality_[1];
    if (ImGui::BeginTable("##quality", compare ? 4 : 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Measure", ImGuiTableColumnFlags_WidthStretch);
        if (compare) ImGui::TableSetupColumn("Before");
        ImGui::TableSetupColumn(compare ? "After" : "Now");
        if (compare) ImGui::TableSetupColumn("Change");
        ImGui::TableHeadersRow();
        auto row = [&](const char* name, const char* tip, double before, double after, const char* fmt) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(), ImGui::TextUnformatted(name);
            ImGui::SetItemTooltip("%s", tip);
            if (compare) ImGui::TableNextColumn(), ImGui::Text(fmt, before);
            ImGui::TableNextColumn(), ImGui::Text(fmt, after);
            if (!compare) return;
            ImGui::TableNextColumn();
            if (std::fabs(after - before) < 1e-9) ImGui::TextDisabled("same");
            else if (before > 0) ImGui::Text("%+.0f%%", (after - before) / before * 100);
            else ImGui::Text("new");
        };
        row("Keys", "Keys in the animation's curves", b.keys, a.keys, "%.0f");
        row("Size", "Bytes of the .anim export writes", double(b.bytes), double(a.bytes), "%.0f bytes");
        row("Jitter", "How shaky the rotation curves are: the bones' average jerk (how fast their acceleration changes), "
            "in degrees per second cubed. Lower is smoother; filtering or simplifying lowers it", b.jerk, a.jerk,
            "%.0f deg/s\xC2\xB3");
        row("Foot slide", "How far planted feet move along the ground, summed over every foot contact", b.foot_slide * 1000,
            a.foot_slide * 1000, "%.1f mm");
        row("Hip drift", "How far the hips travel along the ground per loop, or over the whole animation when it does not loop (what Remove Hip Travel takes away)",
            b.hip_drift * 1000, a.hip_drift * 1000, "%.1f mm");
        if (a.loops) {
            row("Seam jump", "The largest rotation jump from loop out back to loop in", b.seam_deg, a.seam_deg, "%.2f deg");
            row("Seam jump (position)", "The largest position jump from loop out back to loop in", b.seam_mm, a.seam_mm,
                "%.1f mm");
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s%s", count_noun(size_t(std::max(a.contacts, 0)), "foot contact").c_str(), a.loops ? "" : "; the animation does not loop");

    if (section_header("Jitter per Bone", false)) {
        if (ImGui::BeginTable("##shake_bones", compare ? 3 : 2,
                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                              ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 9))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthStretch);
            if (compare) ImGui::TableSetupColumn("Before");
            ImGui::TableSetupColumn(compare ? "After" : "Now");
            ImGui::TableHeadersRow();
            for (const JointShake& s : a.shake) {
                if (s.rot <= 0) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(), ImGui::TextUnformatted(s.track.c_str());
                if (compare) {
                    ImGui::TableNextColumn();
                    auto it = std::find_if(b.shake.begin(), b.shake.end(), [&](const JointShake& o) { return o.track == s.track; });
                    it == b.shake.end() ? ImGui::TextDisabled("-") : ImGui::Text("%.0f", it->rot);
                }
                ImGui::TableNextColumn(), ImGui::Text("%.0f", s.rot);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

}  // namespace vats
