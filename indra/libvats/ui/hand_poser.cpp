// Viewport Avatar Toolset - the Hands panel: curl and spread fingers by dragging dots.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/06 section 4.6 and docs/spec/02 AM-38.
#include <cmath>

#include "app.h"
#include "widgets.h"
#include "theme.h"
#include "imgui_internal.h"
#include "vats/edit.h"
#include "vats/pose_tools.h"

namespace vats {
namespace {

const char* const kFingers[5] = {"Pinky", "Ring", "Middle", "Index", "Thumb"};
// Dot layout for the left hand (x as a fraction of the half, y in px); the right half is mirrored.
const float kDotX[6] = {0.17f, 0.32f, 0.50f, 0.68f, 0.86f, 0.50f};
const float kDotY[6] = {62, 42, 34, 42, 76, 86};
constexpr double kDegPerPx = 0.6;

}  // namespace

void App::draw_hand_poser() {
    if (!show_hands_) return;
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    // On the first frame the viewport has not been docked to size yet (--window hands): wait until it has.
    if (viewport_max_.x - viewport_min_.x < 200 * dpi || viewport_max_.y - viewport_min_.y < 150 * dpi) return;
    // Beside the view where a panel leaves room, its bottom at the view's, so it covers none of the avatar (in the
    // view's middle it covered the hands it poses); else in the view's bottom-right corner. Shrunk to fit either way.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const Box work{vp->WorkPos.x, vp->WorkPos.y, vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y};
    const Box view{viewport_min_.x, viewport_min_.y, viewport_max_.x, viewport_max_.y};
    const float margin = 10 * dpi;
    const BesidePlace place = beside_view(work, view, 340 * dpi + 2 * ImGui::GetStyle().WindowPadding.x, margin);
    const float s = dpi * place.scale;
    ImGui::SetNextWindowSize(ImVec2(0, 0));
    // It never resizes, so a place left off the screen (a smaller window since) comes back inside it.
    const float x = place.side > 0 ? view.x1 + margin : place.side < 0 ? view.x0 - margin : view.x1 - margin;
    ImGui::SetNextWindowPos(ImVec2(x, view.y1 - margin), ImGuiCond_FirstUseEver, ImVec2(place.side > 0 ? 0.f : 1.f, 1));
    if (const ImGuiWindow* w = ImGui::FindWindowByName("Hand Poser"); w && !w->Appearing) {
        const Box in = clamp_window(work, {w->Pos.x, w->Pos.y, w->Pos.x + w->Size.x, w->Pos.y + w->Size.y});
        if (in.x0 != w->Pos.x || in.y0 != w->Pos.y) ImGui::SetNextWindowPos(ImVec2(in.x0, in.y0));
    }
    if (!ImGui::Begin("Hand Poser", &show_hands_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
        return ImGui::End();
    help_button("hand-poser");

    ImVec2 size(340 * s, 108 * s), o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pad", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), timeline_background(), 6);
    dl->AddLine(ImVec2(o.x + size.x / 2, o.y + 6), ImVec2(o.x + size.x / 2, o.y + size.y - 6), IM_COL32(255, 255, 255, 30));
    dl->AddText(ImVec2(o.x + 8, o.y + 4), IM_COL32(150, 154, 163, 255), "Left");
    dl->AddText(ImVec2(o.x + size.x - ImGui::CalcTextSize("Right").x - 8, o.y + 4), IM_COL32(150, 154, 163, 255), "Right");

    ImVec2 m = ImGui::GetIO().MousePos;
    auto dot_pos = [&](int side, int d) {  // side 0 = left hand (left half), 1 = right hand
        float half = size.x / 2, fx = side == 0 ? kDotX[d] : 1 - kDotX[d];
        return ImVec2(o.x + side * half + fx * half, o.y + kDotY[d] * s);
    };
    auto radius = [&](int d) { return (d == 5 ? 13.f : 6.5f) * s; };
    int hover_side = -1, hover_dot = -1;
    for (int side = 0; side < 2; ++side)
        for (int d = 0; d < 6; ++d) {
            ImVec2 p = dot_pos(side, d);
            if (std::hypot(m.x - p.x, m.y - p.y) <= radius(d) + 4 * s) hover_side = side, hover_dot = d;
        }

    // The bones a dot drives, with the weight of its sideways swing.
    auto bones_of = [&](int side, int d, std::vector<std::pair<std::string, double>>& fingers) {
        const char* suffix = side == 0 ? "Left" : "Right";
        if (d == 5) {
            const std::pair<const char*, double> all[] = {{"Index", 1.0}, {"Middle", 0.35}, {"Ring", -0.35}, {"Pinky", -1.0}};
            for (auto& [f, w] : all) fingers.emplace_back(std::string("mHand") + f + "%d" + suffix, w * 0.5);
        } else {
            fingers.emplace_back(std::string("mHand") + kFingers[d] + "%d" + suffix, 1.0);
        }
    };
    auto segment = [](const std::string& pattern, int n) {
        char b[64];
        std::snprintf(b, sizeof b, pattern.c_str(), n);
        return std::string(b);
    };

    if (ImGui::IsItemActivated() && hover_dot >= 0) {
        hand_drag_side_ = hover_side;
        hand_drag_dot_ = hover_dot;
        hand_press_ = m;
        hand_start_clip_ = doc_.clip();
        if (settings_.respect_joint_limits) hand_limits_ = template_limits(skel_, shape());
        if (ImGui::IsMouseDoubleClicked(0)) {  // reset these fingers
            std::vector<std::pair<std::string, double>> fingers;
            bones_of(hover_side, hover_dot, fingers);
            edit("Reset Fingers", [&](Clip& c) {
                std::vector<std::string> bones;
                for (auto& f : fingers)
                    for (int n = 1; n <= 3; ++n) bones.push_back(segment(f.first, n)), key_euler(c, bones.back(), frame_, {});
                mirror_edit(bones);  // PT-1
            });
            hand_drag_dot_ = -1;
        } else {
            doc_.history.begin(doc_.clip());
            // An IK solve overrides the dots: say so, naming the limb and the IK/FK shortcut.
            std::vector<std::pair<std::string, double>> fingers;
            bones_of(hover_side, hover_dot, fingers);
            for (auto& f : fingers) {
                int limb = rig_->limb_of_bone(skel_.find(segment(f.first, 1)));
                if (limb < 0 || !limb_states_[limb].ik_on) continue;
                std::string key;
                for (auto& [id, a] : actions_)
                    if (id == "ik_toggle" && a.key) key = std::string(" (") + key_label(a.key) + ")";
                status(rig_->limbs()[limb].label + " is in IK here, so the IK overrides these dots. Switch it to FK" +
                       key + " to pose it by hand.");
                break;
            }
        }
    }
    if (hand_drag_dot_ >= 0 && ImGui::IsItemActive()) {
        double dx = m.x - hand_press_.x, dy = m.y - hand_press_.y;
        std::vector<std::pair<std::string, double>> fingers;
        bones_of(hand_drag_side_, hand_drag_dot_, fingers);
        Clip& clip = doc_.clip();
        clip = hand_start_clip_;  // absolute from the press, so the drag never drifts
        if (dx == 0 && dy == 0) fingers.clear();  // a click that has not moved yet keys nothing
        std::vector<std::string> keyed;  // PT-1
        const RigConstraints* rc = constraints();  // null with Respect Joint Limits off
        for (auto& [pattern, swing_weight] : fingers)
            for (int n = 1; n <= 3; ++n) {
                std::string bone = segment(pattern, n);
                int node = skel_.find(bone);
                if (node < 0) continue;
                // The body's limit, else (Respect Joint Limits on) the anatomical one: a long drag bent it back through
                // the hand.
                const JointLimit* lim = rc ? rc->find(bone) : nullptr;
                if (!lim && settings_.respect_joint_limits) lim = hand_limits_.find(bone);
                const Quat start = euler_to_quat(curve_euler(hand_start_clip_, bone, frame_));
                key_rotation(clip, bone, frame_,
                             finger_segment_rotation(skel_, node, start, dy * kDegPerPx, dx * kDegPerPx * swing_weight,
                                                     n == 1, lim, shape()));
                keyed.push_back(bone);
            }
        mirror_edit(keyed);
        ImVec2 p = dot_pos(hand_drag_side_, hand_drag_dot_);
        dl->AddLine(p, ImVec2(p.x + float(dx), p.y + float(dy)), IM_COL32(255, 222, 70, 200), 2);
    }
    // How far a dot's fingers are curled now (the "All fingers" dot: their average), for the rings and the readout.
    auto curl_of = [&](int side, int d) {
        std::vector<std::pair<std::string, double>> fingers;
        bones_of(side, d, fingers);
        double sum = 0;
        for (auto& f : fingers)
            sum += finger_curl_degrees(skel_, doc_.clip(), {segment(f.first, 1), segment(f.first, 2), segment(f.first, 3)}, frame_);
        return fingers.empty() ? 0.0 : sum / double(fingers.size());
    };
    auto dot_name = [&](int side, int d) { return std::string(side == 0 ? "Left " : "Right ") + (d == 5 ? "fingers" : kFingers[d]); };
    auto degrees = [](double v) { return std::to_string(int(std::lround(v))) + "\xc2\xb0"; };
    if (hand_drag_dot_ >= 0 && ImGui::IsItemDeactivated()) {
        if (doc_.history.commit("Pose Fingers", doc_.clip())) {
            mark_dirty();
            status("Posed " + dot_name(hand_drag_side_, hand_drag_dot_) + ": curled " + degrees(curl_of(hand_drag_side_, hand_drag_dot_)));
        }
        hand_drag_dot_ = -1;
    }

    for (int side = 0; side < 2; ++side)
        for (int d = 0; d < 6; ++d) {
            ImVec2 p = dot_pos(side, d);
            bool dragged = hand_drag_side_ == side && hand_drag_dot_ == d;
            bool hot = hover_side == side && hover_dot == d;
            ImU32 c = dragged ? IM_COL32(255, 222, 70, 255) : hot ? IM_COL32(255, 240, 170, 255) : IM_COL32(200, 202, 208, 255);
            dl->AddCircleFilled(p, radius(d), c);
            dl->AddCircle(p, radius(d), IM_COL32(10, 11, 13, 255), 0, 1.5f);
            // The curl, kept after the dot springs back: a ring round it, full at 270 degrees (a fist); blue bent back.
            const double curl = curl_of(side, d);
            if (std::fabs(curl) >= 1) {
                const float a0 = -0.5f * float(kPi), sweep = float(std::clamp(curl / 270.0, -1.0, 1.0) * 2 * kPi);
                dl->PathArcTo(p, radius(d) + 3 * s, std::min(a0, a0 + sweep), std::max(a0, a0 + sweep), 24);
                dl->PathStroke(curl > 0 ? accent_colour() : IM_COL32(110, 170, 255, 255), 0, 2.f * s);
            }
        }
    // The dot under the pointer, or the one dragged: its name and its curl.
    const int show_side = hand_drag_dot_ >= 0 ? hand_drag_side_ : hover_side;
    const int show_dot = hand_drag_dot_ >= 0 ? hand_drag_dot_ : hover_dot;
    const std::string name = show_dot < 0 ? "" : dot_name(show_side, show_dot) + "  " + degrees(curl_of(show_side, show_dot)) + " curl";
    dl->AddText(ImVec2(o.x + (size.x - ImGui::CalcTextSize(name.c_str()).x) / 2, o.y + size.y - 18 * s),
                IM_COL32(255, 240, 170, 255), name.c_str());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x);
    ImGui::TextDisabled("Drag down to curl, sideways to spread. Double-click resets.");
    ImGui::PopTextWrapPos();
    ImGui::End();
}

}  // namespace vats
