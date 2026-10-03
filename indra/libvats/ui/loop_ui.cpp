// Viewport Avatar Toolset - onion-skin settings and the loop tools menu.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 sections 7.1 (ON) and 7.4 (LP). The logic is in the core (onion.h, loop_tools.h).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "widgets.h"
#include "vats/loop_tools.h"

namespace vats {

// Onion settings are remembered per project, in its "onion" field (a view setting: no undo step).
App::OnionView App::onion_view() const {
    OnionView v;
    const Json* o = doc_.project.extra.find("onion");
    if (!o || !o->is_object()) return v;
    auto num = [&](const char* k, int& out, int lo, int hi) {
        if (const Json* x = o->find(k); x && x->is_number()) out = std::clamp(int(x->num), lo, hi);
    };
    auto flag = [&](const char* k, bool& out) {
        if (const Json* x = o->find(k); x && x->is_bool()) out = x->b;
    };
    flag("on", v.on);
    flag("bones_only", v.bones_only);
    flag("keyed_only", v.s.keyed_only);
    num("before", v.s.before, 0, 5);
    num("after", v.s.after, 0, 5);
    num("step", v.s.step, 1, 10);
    return v;
}

void App::set_onion_view(const OnionView& v) {
    Json o = Json::object();
    o.set("on", v.on);
    o.set("bones_only", v.bones_only);
    o.set("keyed_only", v.s.keyed_only);
    o.set("before", v.s.before);
    o.set("after", v.s.after);
    o.set("step", v.s.step);
    doc_.project.extra.set("onion", std::move(o));
}

void App::draw_onion_settings() {
    OnionView v = onion_view();
    bool changed = ImGui::Checkbox("Show Ghosts", &v.on);
    ImGui::SetItemTooltip("Faint copies of the pose before (cool) and after (warm) the current frame");
    ImGui::BeginDisabled(!v.on);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    changed |= slider_int("Before", &v.s.before, 0, 5);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    changed |= slider_int("After", &v.s.after, 0, 5);
    changed |= ImGui::Checkbox("Keyed Frames Only", &v.s.keyed_only);
    ImGui::BeginDisabled(v.s.keyed_only);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    changed |= slider_int("Every", &v.s.step, 1, 10, v.s.step == 1 ? "%d frame" : "%d frames");
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Bones Only", &v.bones_only);
    ImGui::SetItemTooltip("Draw the ghosts as bones instead of the body");
    ImGui::EndDisabled();
    if (changed) set_onion_view(v);
}

void App::draw_loop_tools_menu() {
    if (host_.world_view()) {  // spec 09 build 20, item 48: the viewer only
        const bool keyed = walk_test_ || !actor_clip(doc_.project, 0).curves.empty();
        const char* nothing = "Nothing to play as your walk: key a walk cycle first (Help > Loop Tools has an example walk)";
        if (menu_item_icon(icon::kWalkTest, "Test as My Walk", nullptr, walk_test_ == 1, keyed))
            start_walk_test(walk_test_ == 1 ? 0 : 1);
        ImGui::SetItemTooltip("%s", keyed ? "Walk for real: your animation plays as your walk on your screen, with your "
                                            "speed against its stride" : nothing);
        if (menu_item_icon(icon::kWalkTest, "Test as My Run", nullptr, walk_test_ == 2, keyed))
            start_walk_test(walk_test_ == 2 ? 0 : 2);
        if (!keyed) ImGui::SetItemTooltip("%s", nothing);
        ImGui::Separator();
    }
    const LoopRange r = loop_range(doc_.clip());
    ImGui::TextDisabled("Frames %d to %d%s", r.in, r.out, doc_.clip().loop ? " (loop)" : " (whole clip)");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    slider_int("Blend", &loop_blend_, 0, 15, loop_blend_ ? "%d frames" : "end key only");
    ImGui::SetItemTooltip("Ease the last frames into the start pose instead of only changing the end key");
    if (menu_item_icon(icon::kSeamless, "Make Loop Seamless")) {
        int n = 0;
        graph_.snapshot_curves(doc_.clip());  // PT-4
        edit("Make Loop Seamless", [&](Clip& c) { n = make_loop_seamless(c, loop_blend_); });
        status(n ? "Loop made seamless on " + std::to_string(n) + (n == 1 ? " channel" : " channels")
                 : "The loop was already seamless");
    }
    ImGui::SetItemTooltip("Every channel ends where it starts, with the same slope");
    ImGui::Separator();
    if (menu_item_icon(icon::kInPlace, "Remove Hip Travel (In Place)")) {
        Travel t;
        graph_.snapshot_curves(doc_.clip());  // PT-4
        edit("Remove Hip Travel", [&](Clip& c) { t = remove_travel(c); });
        char buf[160];
        auto shown = [](double v) { return std::fabs(v) < 0.005 ? 0.0 : v; };  // never "-0.00"
        std::snprintf(buf, sizeof buf, "Removed hip travel: %.2f m/s forward, %.2f m/s sideways (%.2f m/s)", shown(t.vx),
                      shown(t.vy), t.speed());
        status(buf);
    }
    ImGui::SetItemTooltip("Keeps the hips' sway and height; the walk speed is shown so an AO can match it");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::DragFloat("##travel", &loop_travel_, 0.01f, -5.f, 5.f, "%.2f m/s");
    ImGui::SameLine();
    if (ImGui::Button("Add Travel Forward")) {
        const float v = loop_travel_;
        graph_.snapshot_curves(doc_.clip());  // PT-4
        edit("Add Hip Travel", [&](Clip& c) { add_travel(c, {v, 0}); });
        status("Hips now travel forward at " + std::to_string(v).substr(0, 4) + " m/s");
    }
    ImGui::Separator();
    const int here = int(std::lround(frame_));
    ImGui::BeginDisabled(here <= r.in || here >= r.out);
    if (menu_item_icon(icon::kCycleStart, ("Start Cycle at Frame " + std::to_string(here)).c_str())) {
        graph_.snapshot_curves(doc_.clip());  // PT-4
        edit("Start Cycle Here", [&](Clip& c) { cycle_offset(c, here); });
        status("The cycle now starts with what was frame " + std::to_string(here));
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Rotates the loop in time so it starts on this pose (make it seamless first)");
    draw_loop_assist_items();  // loop_assist_ui.cpp (LP-5..LP-7)
}

// LP-2: a red tick at loop-out when channels jump at the seam, listed on hover.
void App::draw_loop_seam_mark(ImDrawList* dl, float x_out, float y, bool hovered) {
    if (!doc_.clip().loop) return;
    const auto jumps = loop_seam_jumps(doc_.clip());
    if (jumps.empty()) return;
    dl->AddRectFilled(ImVec2(x_out - 1.5f, y), ImVec2(x_out + 1.5f, y + 12), IM_COL32(235, 80, 70, 255));
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (!hovered || std::fabs(m.x - x_out) > 5 || m.y < y || m.y > y + 14) return;
    std::string tip = "The loop jumps at the seam on:";
    for (size_t i = 0; i < jumps.size() && i < 8; ++i) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "\n  %s %s (%+.3g)", jumps[i].track.c_str(), jumps[i].channel.c_str(), jumps[i].jump);
        tip += buf;
    }
    if (jumps.size() > 8) tip += "\n  and " + std::to_string(jumps.size() - 8) + " more";
    tip += "\nTools > Loop Tools > Make Loop Seamless fixes them";
    ImGui::SetTooltip("%s", tip.c_str());
}

}  // namespace vats
