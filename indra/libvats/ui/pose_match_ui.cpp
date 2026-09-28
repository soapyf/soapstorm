// Viewport Avatar Toolset - Match Poses (pose-matched insertion) and Make Transition.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 23 (PM). The logic is in the core (pose_match.h).
#include <algorithm>
#include <cmath>

#include "app.h"
#include "imgui.h"
#include "vats/pose_presets.h"

namespace vats {

namespace {

constexpr const char* kSlBlendNote =
    "Second Life blends whole animations itself, through each one's Ease in and Ease out. This builds one file "
    "from several pieces.";

// Linear, then TW-3's shapes that stay inside the two poses (In-Out).
bool ease_combo(const char* id, std::optional<EaseShape>& e) {
    static const char* names[] = {"Linear", "Quad", "Cubic", "Sine"};
    int i = e ? int(*e) + 1 : 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (!ImGui::Combo(id, &i, names, 4)) return false;
    e = i ? std::optional<EaseShape>(EaseShape(i - 1)) : std::nullopt;
    return true;
}

void note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

}  // namespace

Clip App::library_clip(const LibraryItem& it, bool mirrored) const {
    Clip c;
    c.fps = doc_.clip().fps;
    c.end_frame = std::max(1, int(std::ceil(it.length)));
    paste_clip(c, skel_, it, 0, mirrored, nullptr);
    return c;
}

void App::open_match_poses(Clip incoming, const std::string& name) {
    match_.incoming = std::move(incoming);
    match_.name = name;
    match_.stale = true;
    show_match_ = true;
}

void App::draw_match_poses_window() {
    if (!show_match_) return;
    place_tool_window(5, 26, 26);
    if (!ImGui::Begin("Match Poses", &show_match_)) return ImGui::End();
    help_button("project-library");
    const float em = ImGui::GetFontSize();
    ImGui::TextWrapped("Joins %s onto the end of the clip where the poses match best.", match_.name.c_str());
    MatchOptions& o = match_.o;
    bool changed = false;
    ImGui::SetNextItemWidth(em * 8);
    changed |= ImGui::SliderInt("Search", &o.search, 2, 60, "%d frames");
    ImGui::SetItemTooltip("How many frames at the end of the clip and at the start of %s are compared", match_.name.c_str());
    ImGui::SetNextItemWidth(em * 8);
    changed |= ImGui::SliderInt("Blend", &o.blend, 0, 30, o.blend ? "%d frames" : "a straight cut");
    ImGui::SetItemTooltip("Frames over which the clip's motion eases into the new one's");
    changed |= ease_combo("Ease", o.ease);
    changed |= ImGui::Checkbox("Align the hips", &o.align);
    ImGui::SetItemTooltip("Turn and move the new clip so its hips carry on where the clip's are (height kept)");
    if (changed || match_.stale) {
        match_.m = match_poses(*rig_, doc_.clip(), match_.incoming, o, shape());
        match_.stale = false;
    }
    const PoseMatch& m = match_.m;
    ImGui::Separator();
    ImGui::Text("Cut at frame %d, where %s's frame %d lands.", m.cut, match_.name.c_str(), m.into);
    ImGui::Text("Pose difference %.1f%s", m.distance, m.distance < 5 ? " (close)" : m.distance < 15 ? "" : " (far apart)");
    if (o.align) ImGui::Text("Turned %.0f degrees, moved %.2f m", m.yaw, std::hypot(m.shift.x, m.shift.y));
    const int end = m.cut + match_.incoming.end_frame - m.into;
    ImGui::Text("The clip becomes %d frames long.", end);
    note(kSlBlendNote);
    if (ImGui::Button("Insert")) {
        match_.m = match_poses(*rig_, doc_.clip(), match_.incoming, o, shape());  // the clip may have changed since
        edit("Insert, Matching Poses", [&](Clip& c) { join_matched(c, match_.incoming, match_.m, o); });
        sync_actor_timing(doc_.project);
        status("Joined " + match_.name + " at frame " + std::to_string(match_.m.cut));
        show_match_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) show_match_ = false;
    ImGui::End();
}

void App::draw_transition_window() {
    if (!show_transition_) return;
    place_tool_window(5, 26, 26);
    if (!ImGui::Begin("Make Transition", &show_transition_)) return ImGui::End();
    help_button("pose-library");
    const float em = ImGui::GetFontSize();
    const int last = doc_.clip().end_frame;
    // The library's poses (not clips), then the starter body poses.
    std::vector<const LibraryItem*> poses;
    for (const LibraryItem& it : library_.items)
        if (!it.clip) poses.push_back(&it);
    for (const LibraryItem& it : builtin_poses(skel_))
        if (it.kind != "hand") poses.push_back(&it);
    TransitionState& t = transition_;
    ImGui::TextUnformatted("From");
    ImGui::SameLine(em * 5);
    ImGui::RadioButton("Frame", &t.from_pose, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Library pose", &t.from_pose, 1);
    ImGui::SetNextItemWidth(em * 12);
    if (t.from_pose) {
        t.pose = std::clamp(t.pose, 0, std::max(int(poses.size()) - 1, 0));
        if (ImGui::BeginCombo("##pose", poses.empty() ? "(no poses saved)" : poses[t.pose]->name.c_str())) {
            for (int i = 0; i < int(poses.size()); ++i)
                if (ImGui::Selectable((poses[i]->name + "##" + std::to_string(i)).c_str(), i == t.pose)) t.pose = i;
            ImGui::EndCombo();
        }
    } else {
        ImGui::InputInt("##from", &t.from);
    }
    ImGui::SetNextItemWidth(em * 12);
    ImGui::InputInt("To frame", &t.to);
    ImGui::SetNextItemWidth(em * 12);
    ImGui::InputInt("Frames", &t.frames);
    t.to = std::clamp(t.to, 0, last);
    t.from = std::clamp(t.from, 0, last);
    t.frames = std::clamp(t.frames, 1, std::max(t.to, 1));
    if (!t.from_pose && ImGui::SmallButton("Span From to To")) t.frames = std::max(t.to - t.from, 1);
    ease_combo("Ease", t.ease);
    const int at = t.to - t.frames;
    ImGui::Text("Writes frames %d to %d, every frame keyed.", at, t.to);
    note(kSlBlendNote);
    const bool can = at >= 0 && (!t.from_pose || !poses.empty());
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Make Transition")) {
        int n = 0;
        edit("Make Transition", [&](Clip& c) {
            if (t.from_pose) {
                Clip posed = c;
                apply_pose(posed, skel_, *poses[t.pose], at, false);
                n = make_transition(c, posed, at, c, t.to, at, t.frames, t.ease);
            } else {
                n = make_transition(c, c, t.from, c, t.to, at, t.frames, t.ease);
            }
        });
        status("Transition over frames " + std::to_string(at) + " to " + std::to_string(t.to) + " on " +
               std::to_string(n) + (n == 1 ? " track" : " tracks"));
    }
    ImGui::EndDisabled();
    ImGui::End();
}

}  // namespace vats
