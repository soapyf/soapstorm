// Viewport Avatar Toolset - loop assists: the Loop Assist window, loop-aware tangents and the walk treadmill.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 7.4 (LP-5..LP-8). The logic is in the core (loop_assist.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <utility>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "vats/loop_assist.h"

namespace vats {

namespace {

constexpr int kSpeedCount = int(std::size(kSlSpeeds));
constexpr double kTreadmillSpacing = 0.5;  // metres between the lines that scroll

}  // namespace

// LP-7 (every edit recomputes handles the ordinary way, so the seam keys are put back each frame: idempotent and
// cheap) and LP-8 (the ground follows the playhead, round the loop while playing, back and forth while scrubbing).
// ponytail: the seam fix lands a frame after an edit's undo step, so a redo shows the ordinary handles for a frame.
void App::loop_assist_tick() {
    Project& p = doc_.project;
    for (int i = 0; i < std::max(1, int(p.actors.size())); ++i) apply_loop_tangents(actor_clip(p, i));
    const Clip& c = doc_.clip();
    double d = frame_ - treadmill_frame_;
    if (d < 0 && playing_ && c.loop) d += c.loop_out - c.loop_in;  // wrapped round the loop
    treadmill_frame_ = frame_;
    const double v = treadmill_speed_ < kSpeedCount ? kSlSpeeds[treadmill_speed_].mps : treadmill_custom_;
    treadmill_scroll_ = std::fmod(treadmill_scroll_ + d / std::max(c.fps, 1) * v, kTreadmillSpacing);
}

void App::draw_loop_assist_items() {
    ImGui::Separator();
    if (menu_item_icon(icon::kFind, "Find Best Loop Points...")) show_loop_assist_ = true;
    ImGui::SetItemTooltip("Frame pairs whose poses match best, to loop between");
    if (menu_item_icon(icon::kStretch, "Fit Loop to Beats...")) show_loop_assist_ = true;
    ImGui::SetItemTooltip("Stretch the loop to a whole number of the audio track's beats");
    bool on = doc_.clip().loop_tangents;
    if (ImGui::MenuItem("Loop-Aware Tangents", nullptr, &on)) {
        edit(on ? "Loop-Aware Tangents On" : "Loop-Aware Tangents Off", [&](Clip& c) {
            c.loop_tangents = on;
            if (on) return (void)apply_loop_tangents(c);
            for (auto& [name, track] : c.curves)
                for (auto& [ch, curve] : track) curve.recompute_handles();
        });
        sync_actor_timing(doc_.project);
    }
    ImGui::SetItemTooltip("Auto, Spline and Plateau keys at Loop in and Loop out take their slope across the seam, and the "
                          "Graph Editor shows the loop repeated; saved with the project");
}

void App::draw_loop_assist_window() {
    if (!show_loop_assist_) return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 26, em * 26), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Loop Assist", &show_loop_assist_)) return ImGui::End();
    const Clip& clip = doc_.clip();

    // LP-5
    ImGui::SeparatorText("Best Loop Points");
    ImGui::SetNextItemWidth(em * 7);
    ImGui::InputInt("Shortest Loop (frames)", &loop_min_length_);
    loop_min_length_ = std::clamp(loop_min_length_, 2, 3600);
    if (icon_label_button(icon::kFind, "Find") || std::exchange(loop_find_now_, false)) {
        loop_candidates_ = find_loop_points(*rig_, clip, loop_min_length_, 8, shape());
        status(loop_candidates_.empty() ? "The animation is shorter than the shortest loop"
                                        : "Found " + std::to_string(loop_candidates_.size()) + " loop candidates");
    }
    ImGui::SetItemTooltip("Pose and motion compared; hips and legs count most, fingers and face least");
    int use = -1;
    if (!loop_candidates_.empty() &&
        ImGui::BeginTable("##loops", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Frames");
        ImGui::TableSetupColumn("Length");
        ImGui::TableSetupColumn("Distance");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(loop_candidates_.size()); ++i) {
            const LoopCandidate& k = loop_candidates_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d to %d", k.in, k.out);
            ImGui::TableNextColumn();
            ImGui::Text("%d (%.2f s)", k.length(), k.length() / double(std::max(clip.fps, 1)));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", k.distance);
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (icon_label_small_button(icon::kApply, "Use")) use = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        ImGui::TextDisabled("Distance: how far apart the two poses are, a weighted average in degrees (0 = the same)");
    }
    if (use >= 0) {
        const LoopCandidate k = loop_candidates_[use];
        edit("Set Loop Points", [&](Clip& c) { c.loop = true, c.loop_in = k.in, c.loop_out = k.out; });
        sync_actor_timing(doc_.project);  // GR-3
        const std::string range = "frames " + std::to_string(k.in) + " to " + std::to_string(k.out);
        status("The loop is now " + range);
        host_.ask("Loop Points", "The loop is now " + range + ". Make it seamless too (Tools > Loop Tools > Make Loop Seamless)?",
                  {"Make Seamless", "Not Now"}, [this](int answer) {
                      if (answer != 0) return;
                      int n = 0;
                      edit("Make Loop Seamless", [&](Clip& c) { n = make_loop_seamless(c, loop_blend_); });
                      status(n ? "Loop made seamless on " + std::to_string(n) + (n == 1 ? " channel" : " channels")
                               : "The loop was already seamless");
                  });
    }

    // LP-6
    ImGui::SeparatorText("Fit Loop to Beats");
    const double bpm = clip.audio ? clip.audio->bpm : 0;
    if (bpm <= 0) {
        ImGui::TextWrapped("Needs a BPM: load audio (File > Load Audio...) and set its BPM in the timeline's right-click menu.");
    } else {
        ImGui::SetNextItemWidth(em * 7);
        ImGui::InputInt("Beats", &loop_beats_);
        loop_beats_ = std::clamp(loop_beats_, 1, 256);
        const BeatFit fit = fit_to_beats(bpm, clip.fps, loop_beats_);
        const LoopRange r = loop_range(clip);
        ImGui::Text("%d beats at %.1f BPM: %.3f s, %d frames at %d fps", loop_beats_, bpm, fit.seconds, fit.frames, clip.fps);
        ImGui::TextDisabled("The %s is %d frames now", clip.loop ? "loop" : "whole animation", r.out - r.in);
        if (fit.loops_to_drift == 0)
            ImGui::TextWrapped("At %d frames every loop ends on the beat.", fit.frames);
        else
            ImGui::TextWrapped("Each loop ends %.1f ms %s the beat: a whole frame off after %ld loops.", std::fabs(fit.residual_ms),
                               fit.residual_ms > 0 ? "after" : "before", fit.loops_to_drift);
        if (fit.suggested_fps == clip.fps)
            ImGui::TextWrapped("Every beat falls on a whole frame at %d fps.", clip.fps);
        else if (fit.suggested_fps)
            ImGui::TextWrapped("At %d fps every beat falls on a whole frame (Properties, Frame rate).", fit.suggested_fps);
        else
            ImGui::TextWrapped("No frame rate from 10 to 60 fps puts every beat on a whole frame.");
        ImGui::BeginDisabled(fit.frames == r.out - r.in);
        if (icon_label_button(icon::kStretch, ("Stretch to " + std::to_string(fit.frames) + " Frames").c_str())) {
            edit("Fit Loop to Beats", [&](Clip& c) { stretch_loop(c, fit.frames); });
            sync_actor_timing(doc_.project);
            status("The " + std::string(clip.loop ? "loop" : "animation") + " is now " + std::to_string(loop_beats_) + " beats long");
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Stretches or squashes the keys in the range; later keys move with its end");
    }
    ImGui::End();
}

// LP-8: View > Treadmill.
void App::draw_treadmill_menu() {
    if (menu_item_icon(icon::kTreadmill, "Show Treadmill", nullptr, treadmill_on_)) treadmill_on_ = !treadmill_on_;
    ImGui::SetItemTooltip("A ground grid that scrolls under the avatar at the chosen speed, for walking in place");
    ImGui::SeparatorText("Speed");
    char label[64];
    for (int i = 0; i < kSpeedCount; ++i) {
        std::snprintf(label, sizeof label, "SL %s (%.2f m/s)", kSlSpeeds[i].name, kSlSpeeds[i].mps);
        if (ImGui::RadioButton(label, treadmill_speed_ == i)) treadmill_speed_ = i;
    }
    if (ImGui::RadioButton("Custom", treadmill_speed_ == kSpeedCount)) treadmill_speed_ = kSpeedCount;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    if (ImGui::DragFloat("##custom", &treadmill_custom_, 0.01f, 0.f, 30.f, "%.2f m/s")) treadmill_speed_ = kSpeedCount;
    const double target = treadmill_speed_ < kSpeedCount ? kSlSpeeds[treadmill_speed_].mps : treadmill_custom_;

    ImGui::SeparatorText("The cycle");
    // ponytail: measured every frame the menu is open (one pose per contact end plus the contact search).
    const Gait g = measure_gait(*rig_, doc_.clip(), shape());
    if (g.speed <= 0) {
        ImGui::TextDisabled("No foot contacts found in the %s", doc_.clip().loop ? "loop" : "animation");
    } else {
        ImGui::Text("Stride %.2f m, cycle %.2f s", g.stride, g.cycle);
        ImGui::Text("Implied speed %.2f m/s (%.0f%% of %.2f)", g.speed, target > 0 ? g.speed / target * 100 : 0.0, target);
    }
    ImGui::SetItemTooltip("From the foot contacts: how fast the body moves over a planted foot");

    ImGui::SeparatorText("Match Cycle to Speed");
    ImGui::BeginDisabled(g.speed <= 0 || target <= 0);
    if (menu_item_icon(icon::kStretch, "Stretch Time")) {
        int frames = 0;
        edit("Match Cycle to Speed", [&](Clip& c) { frames = match_speed_by_time(c, g, target); });
        sync_actor_timing(doc_.project);
        status("The cycle is now " + std::to_string(frames) + " frames long");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Stretches or squashes the loop so its implied speed is the treadmill's; the stride stays");
    Clip probe = doc_.clip();
    const bool travels = remove_travel(probe).speed() >= 1e-3;
    ImGui::BeginDisabled(!travels || target <= 0);
    if (ImGui::MenuItem("Scale Hip Travel")) {
        edit("Match Hip Travel to Speed", [&](Clip& c) { match_speed_by_travel(c, target); });
        char buf[96];
        std::snprintf(buf, sizeof buf, "The hips now travel at %.2f m/s", target);
        status(buf);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(travels ? "The hips travel at the treadmill's speed; the timing stays, so planted feet slide by the "
                                    "difference (Foot Clean-up plants them again)"
                                  : "The hips do not travel: the cycle is in place already");
}

// The grid: lines across the walking direction (+X) that scroll backwards, and two along it, as thin quads on the ground
// (z = 0) around the hips. The app rasterises them with its scene; the viewer draws them in its world (spec 09 U5).
void App::draw_treadmill() {
    if (!treadmill_on_ || globals_.empty()) return;
    static std::vector<Vertex> v;
    v.clear();
    const Vec3 at = globals_[0].pos;
    const Rgb col{0.43f, 0.75f, 1.f};  // the loop's blue (theme kLoopHandle), so it stands out from the ground grid
    constexpr double kReach = 3, kHalfWidth = 0.8, kLine = 0.008, kLift = 0.002;
    auto quad = [&](double x0, double y0, double x1, double y1) {
        const float a = float(0.7 * (1 - std::min(std::fabs((x0 + x1) / 2 - at.x) / kReach, 1.0)));
        const float x[4] = {float(x0), float(x1), float(x1), float(x0)}, y[4] = {float(y0), float(y0), float(y1), float(y1)};
        for (int i : {0, 1, 2, 0, 2, 3}) v.push_back({{x[i], y[i], float(kLift)}, {0, 0, 1}, {col.r, col.g, col.b, a}});
    };
    const double first = std::ceil((at.x - kReach + treadmill_scroll_) / kTreadmillSpacing) * kTreadmillSpacing - treadmill_scroll_;
    for (double x = first; x <= at.x + kReach; x += kTreadmillSpacing)
        quad(x - kLine, at.y - kHalfWidth, x + kLine, at.y + kHalfWidth);
    for (double y : {at.y - kHalfWidth, at.y + kHalfWidth})
        for (double x = at.x - kReach; x < at.x + kReach; x += kTreadmillSpacing)  // pieces, so they fade out too
            quad(x, y - kLine, x + kTreadmillSpacing, y + kLine);
    host_.scene_triangles(v, {}, true, 0.f, true);
}

}  // namespace vats
