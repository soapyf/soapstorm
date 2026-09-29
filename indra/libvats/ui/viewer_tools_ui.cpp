// Viewport Avatar Toolset - the viewer's world tools: your avatar as the world plays it, walking the clip for real, and
// animating on the furniture you sit on.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/09 section 0i (build 20), items 47 (View > As It Plays In-World), 48 (Tools > Loop Tools > Test as
// My Walk / Run), 4 and 46 (the seat). They need a world, so only a host with one offers them (Host::play_in_world,
// test_walk, seat); the app's Priority Planner covers the file-based case of 47. The rules are the core's
// (priority_plan.h, loop_assist.h, rig.h, sit_export.h); the host only binds joints, lets the avatar go and casts
// single rays.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "widgets.h"
#include "theme.h"
#include "vats/loop_assist.h"
#include "vats/priority_plan.h"
#include "vats/sit_export.h"

namespace vats {

struct InWorldUi {
    Clip seen;                     // the clip the claims were made from
    bool sent = false;
    PlanClip own;                  // what your animation claims as it exports
    std::vector<PlanClip> running; // the host's running motions, refreshed twice a second
    double running_at = -1;
    bool only_yours = false;
    // The walk test.
    bool was_moving = false;
    double speed = 0;              // the ground speed, smoothed while moving
    Clip gait_seen;
    Gait gait;
};

namespace {

constexpr Rgb kOwn{0.35f, 0.78f, 1.0f};  // the Priority Planner's colour for your clip

}  // namespace

// The clip your avatar plays: your actor's (the first), also while another is edited.
static const Clip& avatar_clip(const Project& p) { return actor_clip(p, 0); }

void App::set_in_world(bool on) {
    if (!in_world_ui_) in_world_ui_ = std::make_shared<InWorldUi>();
    InWorldUi& ui = *in_world_ui_;
    if (!on && walk_test_) {  // the walk test plays with these claims: it ends too, and with it the in-world play
        walk_was_in_world_ = false;
        return start_walk_test(0);
    }
    if (!on) {
        host_.play_in_world(nullptr);
        in_world_ = false;
        ui.sent = false;
        return status("Your avatar shows the editor's pose alone again");
    }
    ui.own = plan_clip_from_clip(skel_, avatar_clip(doc_.project), anim_export_options(), "This project");
    ui.own.own = true;
    if (!host_.play_in_world(&ui.own)) return status("This program has no world to play the animation in");
    ui.seen = avatar_clip(doc_.project);
    ui.sent = true;
    ui.running_at = -1;
    in_world_ = true;
    status(std::string("As it plays in-world: your AO, the default motions and avatar physics run, your animation at its priorities") +
           real_mode_swap_note());
}

void App::start_walk_test(int state) {
    if (!in_world_ui_) in_world_ui_ = std::make_shared<InWorldUi>();
    InWorldUi& ui = *in_world_ui_;
    if (state == 0) {
        if (!walk_test_) return;
        host_.test_walk(0);
        walk_test_ = 0;
        if (!walk_was_in_world_) set_in_world(false);  // the hold returns: sat down, every other motion stopped
        return status("Walk test stopped: the editor holds your avatar again");
    }
    walk_was_in_world_ = in_world_;
    if (!in_world_) set_in_world(true);  // the claims the walk plays with
    if (in_world_ && ui.own.joints.empty()) {  // nothing keyed: the test would only stand the avatar up
        if (!walk_was_in_world_) set_in_world(false);
        return status("Nothing to play as your walk: key a walk cycle first (Help > Loop Tools has an example walk)");
    }
    if (!in_world_ || !host_.test_walk(state)) {
        if (!walk_was_in_world_) set_in_world(false);
        return status("This program cannot walk your avatar");
    }
    walk_test_ = state;
    ui.was_moving = false;
    ui.speed = 0;
    playing_ = true;
    status(std::string(state == 1 ? "Walk with your usual keys: your animation plays as your walk"
                                  : "Run with your usual keys: your animation plays as your run") +
           real_mode_swap_note());
}

// Every frame, before evaluate(): the claims follow the project once nothing is being dragged (as the Priority Planner's
// own entry does); the walk test starts the loop when the walk starts, as SL starts the animation; Esc cancels the
// armed furniture click.
void App::viewer_tools_tick() {
    if (seat_pick_ && (ImGui::IsKeyPressed(ImGuiKey_Escape) || !host_.seat().seated)) {
        seat_pick_ = false;
        status("Place on Furniture Point cancelled");
    }
    if (!in_world_ || !in_world_ui_) return;
    InWorldUi& ui = *in_world_ui_;
    const Clip& c = avatar_clip(doc_.project);
    if (!doc_.history.is_open() && !dragging_gizmo_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !(c == ui.seen)) {
        ui.seen = c;
        ui.own = plan_clip_from_clip(skel_, c, anim_export_options(), "This project");
        ui.own.own = true;
        host_.play_in_world(&ui.own);
    }
    if (walk_test_) {
        const ui::Host::Locomotion l = host_.locomotion();
        if (l.moving && !ui.was_moving) {  // SL starts the walk animation from its first frame
            const double lo = c.loop ? c.loop_in : 0;
            frame_ = lo;
            playing_ = true;
        }
        if (l.moving) ui.speed = ui.speed <= 0 ? l.speed : ui.speed * 0.95 + l.speed * 0.05;
        ui.was_moving = l.moving;
    }
}

// Who wins each joint while your avatar plays as it will in-world: the Priority Planner's rule (plan_winners) over the
// motions running on your avatar (Host::running_motions, started first) and your animation (started last).
void App::draw_in_world_window() {
    if (!in_world_ || !in_world_ui_) return;
    InWorldUi& ui = *in_world_ui_;
    const double now = double(host_.ticks_ns()) * 1e-9;
    if (ui.running_at < 0 || now - ui.running_at > 0.5) {
        ui.running = host_.running_motions();
        ui.running_at = now;
    }
    std::vector<PlanClip> clips = ui.running;
    clips.push_back(ui.own);
    const std::map<std::string, int> winners = plan_winners(clips);

    place_tool_window(26, 30);
    bool open = true;
    if (!ImGui::Begin("As It Plays In-World", &open)) {
        ImGui::End();
        if (!open) set_in_world(false);
        return;
    }
    help_button("vats-editor-viewer");
    hint("Your AO, the default motions and avatar physics run; your animation plays at its own priorities on the joints "
         "it keys. Each joint goes to the highest priority, on equal priority to the one started last.");
    ImGui::Checkbox("Only the joints your animation keys", &ui.only_yours);
    int yours = 0, lost = 0;
    for (auto& [joint, w] : winners)
        if (ui.own.joints.count(joint)) ++yours, lost += w != int(clips.size()) - 1;
    ImGui::Text("Your animation keys %d joints and wins %d", yours, yours - lost);
    if (ImGui::BeginTable("winners", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Joint");
        ImGui::TableSetupColumn("Driven by", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Priority");
        ImGui::TableHeadersRow();
        for (int n = 0; n < skel_.size(); ++n) {  // skeleton order
            const auto it = winners.find(skel_[n].name);
            if (it == winners.end() || (ui.only_yours && !ui.own.joints.count(skel_[n].name))) continue;
            const PlanClip& w = clips[size_t(it->second)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(skel_[n].name.c_str());
            ImGui::TableNextColumn();
            const bool own = w.own, loses = !own && ui.own.joints.count(skel_[n].name);
            const ImVec4 col = own ? ImVec4(kOwn.r, kOwn.g, kOwn.b, 1) : loses ? ImVec4(1, 0.6f, 0.35f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(col, "%s", w.name.c_str());
            if (loses) ImGui::SetItemTooltip("Your animation keys this joint at %d and loses it", ui.own.joints.at(skel_[n].name));
            ImGui::TableNextColumn();
            ImGui::Text("%d", w.joints.at(skel_[n].name));
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if (!open) set_in_world(false);
}

// Tools > Loop Tools > Test as My Walk / Run: the clip as your walk while you walk for real, with the ground speed
// against the stride speed the treadmill measures (08 LP-8), and the rate that would make them agree.
void App::draw_walk_test_window() {
    if (!walk_test_ || !in_world_ui_) return;
    InWorldUi& ui = *in_world_ui_;
    const Clip& c = avatar_clip(doc_.project);
    if (!(c == ui.gait_seen)) ui.gait_seen = c, ui.gait = measure_gait(*rig_, c, shape());
    const ui::Host::Locomotion l = host_.locomotion();
    place_tool_window(24, 16);
    bool open = true;
    if (ImGui::Begin(walk_test_ == 1 ? "Test as My Walk###walk_test" : "Test as My Run###walk_test", &open)) {
        help_button("loop-tools");
        hint(walk_test_ == 1 ? "Walk with your usual keys. While you walk, your animation plays as your walk on your screen; "
                               "others see your ordinary walk."
                             : "Run with your usual keys. While you run, your animation plays as your run on your screen; "
                               "others see your ordinary run.");
        ImGui::Text("%s", l.moving ? (walk_test_ == 1 ? "Walking: your animation plays" : "Running: your animation plays")
                                   : "Standing: your animation plays only while you move; your AO or the default "
                                     "stand plays now");
        ImGui::Text("Ground speed %.2f m/s%s", l.moving ? ui.speed : l.speed, l.moving ? " (averaged)" : "");
        if (ui.gait.speed <= 0) {
            ImGui::TextDisabled("No foot contacts found in the %s: no stride speed", c.loop ? "loop" : "animation");
        } else {
            ImGui::Text("Stride %.2f m, cycle %.2f s: the clip walks at %.2f m/s", ui.gait.stride, ui.gait.cycle, ui.gait.speed);
            if (ui.speed > 0.1) {
                const double rate = ui.speed / ui.gait.speed;
                ImGui::Text("Suggested rate: %.2fx%s", rate,
                            std::fabs(rate - 1) < 0.05 ? " (the feet keep pace)" : rate > 1 ? " (the feet slide backwards)" : " (the feet slide forwards)");
                ImGui::SetItemTooltip("SL plays the animation at its own speed whatever you walk at; Match stretches the cycle "
                                      "so its stride speed is this ground speed");
                if (ImGui::Button("Match Cycle to This Speed")) {
                    int frames = 0;
                    const Gait g = ui.gait;
                    const double target = ui.speed;
                    edit("Match Cycle to Walking Speed", [&](Clip& cl) { frames = match_speed_by_time(cl, g, target); });
                    sync_actor_timing(doc_.project);
                    status("The cycle is now " + count_noun(frames, "frame") + " long");
                }
            }
        }
        if (ImGui::Button("Stop")) open = false;
        ImGui::SetItemTooltip("The editor holds your avatar again: it sits down and every other motion stops");
    }
    ImGui::End();
    if (!open) start_walk_test(0);
}

// --- The seat (items 4 and 46) --------------------------------------------------------------------------------

bool App::place_selected_at(const Vec3& p, std::string& why) {
    int limb = -1;
    if (const HandleRef* h = primary_handle(); h && !h->pole) limb = h->limb;
    const int node = primary();
    if (limb < 0 && node >= 0)
        if (const int l = rig_->limb_of_bone(node); l >= 0 && rig_->limbs()[l].end == node && l < int(limb_states_.size()) &&
                                                     limb_states_[l].uses_ik)
            limb = l;
    if (limb < 0 && node < 0) return why = "Select a bone or an IK handle first", false;
    bool ok = true;
    edit("Place on Furniture", [&](Clip& c) {
        if (limb >= 0) {  // an IK limb: its target goes to the point, turned as it is
            Xform t = limb_states_[limb].target;
            t.pos = p;
            return key_limb_target(c, *rig_, frame_, limb, t, shape());
        }
        if (pin_at(c, *rig_, node, frame_) < 0 && !pin_here(c, *rig_, frame_, node, -1, shape(), why)) return void(ok = false);
        ok = key_pinned_point(c, *rig_, frame_, node, Xform{globals_[size_t(node)].rot, p}, shape());
        if (!ok) why = skel_[node].name + " could not be held there";
    });
    return ok;
}

bool App::seat_click(ImVec2 m) {
    if (!seat_pick_) return false;
    Vec3 origin, dir, hit;
    projector_.ray(camera_, m.x, m.y, origin, dir);
    if (!host_.seat_point(origin, origin + dir * 64.0, false, hit)) {
        status("That is not the furniture you sit on: click a point on it (Esc cancels)");
        return true;
    }
    seat_pick_ = false;
    std::string why;
    if (!place_selected_at(hit, why)) return status(why), true;
    char buf[128];
    std::snprintf(buf, sizeof buf, " placed on the furniture at %.3f, %.3f, %.3f", hit.x, hit.y, hit.z);
    status((primary_handle() ? rig_->limbs()[size_t(primary_handle()->limb)].label : primary() >= 0 ? skel_[primary()].name : "") + buf);
    return true;
}

// Automatic contact: each selected bone straight down (0.3 m above it to 1 m below) onto the seat. Only for furniture you
// created every part of (the host refuses the ray otherwise).
void App::settle_on_seat() {
    std::vector<int> nodes = selection_;
    std::vector<HandleRef> handles = handles_;
    int placed = 0;
    std::string why;
    for (const HandleRef& h : handles) {
        if (h.pole || h.limb < 0 || h.limb >= int(limb_states_.size())) continue;
        const Vec3 at = limb_states_[size_t(h.limb)].target.pos;
        Vec3 hit;
        if (!host_.seat_point(at + Vec3{0, 0, 0.3}, at - Vec3{0, 0, 1.0}, true, hit)) continue;
        select_handle(h, false);
        placed += place_selected_at(hit, why);
    }
    for (int n : nodes) {
        const Vec3 at = globals_[size_t(n)].pos;
        Vec3 hit;
        if (!host_.seat_point(at + Vec3{0, 0, 0.3}, at - Vec3{0, 0, 1.0}, true, hit)) continue;
        select(n, false);
        placed += place_selected_at(hit, why);
    }
    clear_selection();
    for (int n : nodes) select(n, true);
    for (const HandleRef& h : handles) select_handle(h, true);
    status(placed ? "Settled " + std::to_string(placed) + " on the furniture" : "Nothing selected is above the furniture");
}

void App::draw_seat_section() {
    const ui::Host::Seat s = host_.seat();
    ImGui::SeparatorText("Your seat");
    if (!s.seated) {
        hint("Sit on a piece of furniture in-world before opening the editor: the editor then leaves you seated, measures "
             "where you sit and lets you place pins and IK targets on it.");
        return;
    }
    const Vec3 e = quat_to_euler(s.rot);
    ImGui::Text("You sit on %s", s.name.empty() ? "(an object)" : s.name.c_str());
    ImGui::Text("Offset from its root %.3f, %.3f, %.3f m", s.pos.x, s.pos.y, s.pos.z);
    ImGui::Text("Rotation %.1f, %.1f, %.1f deg", e.x, e.y, e.z);
    ImGui::SetItemTooltip("Your avatar in the furniture root prim's frame, as sit systems place it");
    if (icon_label_button(icon::kSeat, "Use as the Sit Target")) {
        // Your actor (the first) lands where you sit now: the sit target is the measured seat less its placement.
        const Xform root = Xform{s.rot, s.pos} * doc_.project.actors.front().placement().inverse();
        const Vec3 re = quat_to_euler(root.rot);
        Json a = Json::array();
        for (double v : {root.pos.x, root.pos.y, root.pos.z, re.x, re.y, re.z}) a.push(v);
        scene_edit("Sit Target from Your Seat", [a](Project& pr) {
            for (int k = 0; k < int(std::max<size_t>(pr.actors.size(), 1)); ++k) {
                Json& ex = actor_clip(pr, k).export_settings;
                if (!ex.is_object()) ex = Json::object();
                ex.set("sit_root", a);
            }
        });
        status("The sit lines below now seat your actor where you sit");
    }
    ImGui::SetItemTooltip("The sit target below becomes your seat, so the AVsitter and nPose lines seat your actor where "
                          "you sit now");
    ImGui::BeginDisabled(seat_pick_);
    if (icon_label_button(icon::kPlace, seat_pick_ ? "Click the furniture...###seat_pick" : "Place on Furniture Point###seat_pick", "", seat_pick_)) {
        seat_pick_ = true;
        status(std::string("Click a point on the furniture for the selected bone or IK handle (Esc cancels)") +
               real_mode_swap_note());
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The next click on the furniture moves the selected bone's IK target there, or holds the bone "
                          "there with a pin. Only the point you click is read, never the furniture's shape.");
    ImGui::SameLine();
    ImGui::BeginDisabled(s.contact != 1 || (selection_.empty() && handles_.empty()));
    if (icon_label_button(icon::kSeat, "Settle on Furniture")) settle_on_seat();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(s.contact == 1 ? "Each selected bone or IK handle drops straight down onto the furniture"
                                         : "Only on furniture you created every part of");
    if (s.contact == 0) {
        if (icon_label_small_button(icon::kCheck, "Check Whether You Made It")) host_.check_seat();
        ImGui::SetItemTooltip("Selects the furniture as the viewer's Edit does, to read who created each part, then "
                              "deselects it");
    } else {
        hint(s.contact == 1 ? "You created every part of it: automatic contact is on."
                            : "Not every part is yours: place points by clicking.");
    }
}

}  // namespace vats
