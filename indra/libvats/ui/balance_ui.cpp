// Viewport Avatar Toolset - View > Centre of Mass, Tools > Auto-Balance... and Jump Arc..., and the IK targets'
// Pull (spec 08 section 21: CM, JA, RC).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "imgui.h"
#include "vats/balance.h"
#include "vats/edit.h"
#include "vats/jump_arc.h"
#include "vats/reach.h"

namespace vats {
namespace {

void grey_text(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

}  // namespace

// CM-1: the support polygon on the ground, the centre of mass and its plumb line; red when it falls outside.
void App::draw_balance(ImDrawList* dl) const {
    if (!show_com_ || globals_.empty()) return;
    const Balance b = balance_of(skel_, globals_, shape());
    if (!b.contact) return;
    auto at = [&](const Vec3& p, ImVec2& out) {
        double x, y;
        if (!projector_.to_screen(p, x, y)) return false;
        out = ImVec2(float(x), float(y));
        return true;
    };
    std::vector<ImVec2> poly;
    for (const Vec3& p : b.support)
        if (ImVec2 s; at(p, s)) poly.push_back(s);
    if (poly.size() == b.support.size() && poly.size() >= 3) {
        dl->AddConvexPolyFilled(poly.data(), int(poly.size()), IM_COL32(120, 200, 255, 40));
        dl->AddPolyline(poly.data(), int(poly.size()), IM_COL32(120, 200, 255, 170), ImDrawFlags_Closed, 1.5f);
    }
    const ImU32 c = b.inside() ? IM_COL32(90, 230, 140, 255) : IM_COL32(255, 70, 60, 255);
    ImVec2 top, bottom;
    const bool t = at(b.com, top), g = at(b.ground, bottom);
    if (t && g) dl->AddLine(top, bottom, (c & 0x00FFFFFF) | (140u << 24), 1.5f);
    if (g) dl->AddCircleFilled(bottom, 4.5f, c);
    if (t) {
        dl->AddCircleFilled(top, 6.5f, c);
        dl->AddCircle(top, 6.5f, IM_COL32(10, 12, 14, 220), 0, 1.5f);
    }
}

// CM-2: Tools > Auto-Balance...
void App::draw_auto_balance_panel() {
    if (!show_auto_balance_) return;
    place_tool_window(5, 24, 22);
    if (!ImGui::Begin("Auto-Balance", &show_auto_balance_)) return ImGui::End();
    help_button("balance");
    const Clip& clip = doc_.clip();
    grey_text("Moves the hips over the planted feet on every frame of the range, holding the feet with leg IK.");
    const float label_w = ImGui::GetFontSize() * 6.5f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
    };
    if (balance_.to < 0) balance_.to = clip.end_frame;
    label("From frame");
    ImGui::InputInt("##bal_from", &balance_.from);
    label("To frame");
    ImGui::InputInt("##bal_to", &balance_.to);
    balance_.from = std::clamp(balance_.from, 0, clip.end_frame);
    balance_.to = std::clamp(balance_.to, balance_.from, clip.end_frame);
    double ra, rb;
    const bool range = clip_range(ra, rb);
    ImGui::BeginDisabled(!range);
    if (ImGui::SmallButton("Timeline Range")) balance_.from = int(std::floor(ra)), balance_.to = int(std::ceil(rb));
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(range ? "Use the range picked on the timeline" : "Shift-drag a frame range on the timeline first");
    ImGui::SameLine();
    if (ImGui::SmallButton("Whole Clip")) balance_.from = 0, balance_.to = clip.end_frame;
    float margin = float(balance_.margin * 100);
    label("Margin");
    if (ImGui::SliderFloat("##bal_margin", &margin, 0.f, 6.f, "%.1f cm")) balance_.margin = margin / 100;
    ImGui::SetItemTooltip("How far inside the feet's outline the centre of mass is brought");
    label("Smoothing");
    ImGui::SliderInt("##bal_smooth", &balance_.smooth, 0, 10, "%d frames");
    ImGui::SetItemTooltip("The hips' correction is averaged over this many frames each side");
    ImGui::Checkbox("Counter-Lean the Torso", &balance_.counter_lean);
    ImGui::SetItemTooltip("mTorso also leans back towards the feet, so the hips move less");
    if (ImGui::Button("Balance")) {
        AutoBalanceOptions opt = balance_;
        opt.shape = export_shape();
        std::string report;
        edit("Auto-Balance", [&](Clip& c) { report = auto_balance(c, *rig_, opt); });
        status(report);
    }
    ImGui::End();
}

// JA-1: Tools > Jump Arc...
void App::draw_jump_arc_panel() {
    if (!show_jump_arc_) return;
    place_tool_window(6, 24, 22);
    if (!ImGui::Begin("Jump Arc", &show_jump_arc_)) return ImGui::End();
    help_button("balance");
    const Clip& clip = doc_.clip();
    grey_text("Keys the hips on a free-fall arc between the takeoff and landing frames. The two ends keep their keys.");
    const float label_w = ImGui::GetFontSize() * 6.5f;
    auto label = [&](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
    };
    const int here = int(std::lround(frame_));
    for (int k = 0; k < 2; ++k) {
        int& f = k ? jump_.landing : jump_.takeoff;
        ImGui::PushID(k);
        label(k ? "Landing" : "Takeoff");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        ImGui::InputInt("##frame", &f);
        f = std::clamp(f, 0, clip.end_frame);
        ImGui::SameLine();
        if (ImGui::SmallButton("Current Frame")) f = here;
        ImGui::PopID();
    }
    float g = float(jump_.gravity);
    label("Gravity");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat("##gravity", &g, 0.05f, 0.5f, 50.f, "%.2f m/s²")) jump_.gravity = std::clamp(double(g), 0.5, 50.0);
    ImGui::SetItemTooltip("9.81 is Earth's; lower floats, higher snaps");
    ImGui::Checkbox("Forward Travel", &jump_.forward);
    ImGui::SetItemTooltip("The hips travel forward (X) at an even speed from the takeoff to the landing position");
    ImGui::Checkbox("Keep Lateral Motion", &jump_.keep_lateral);
    ImGui::SetItemTooltip("The hips' side-to-side (Y) keys stay; untick to travel sideways at an even speed too");
    const int span = jump_.landing - jump_.takeoff;
    if (span >= 2) {
        const double t = span / double(std::max(clip.fps, 1));
        const double dz = curve_offset(clip, "mPelvis", jump_.landing).z - curve_offset(clip, "mPelvis", jump_.takeoff).z;
        ImGui::TextDisabled("%.2f s in the air, the hips rise %.1f cm", t, jump_apex(t, dz, jump_.gravity) * 100);
    }
    if (ImGui::Button("Apply Jump Arc")) {
        std::string msg;
        bool ok = false;
        edit("Jump Arc", [&](Clip& c) { ok = jump_arc(c, jump_, msg); });  // a refusal changes nothing: no step
        status(ok ? msg : "Jump Arc: " + msg);
    }
    ImGui::End();
}

// RC-1: the Pull of the selected IK target, in Properties > Bone.
void App::draw_ik_target_properties(int limb) {
    const LimbInfo& l = rig_->limbs()[limb];
    ImGui::TextUnformatted((l.label + " IK").c_str());
    if (l.spine || l.finger) return ImGui::TextDisabled("Move the target in the view.");
    Clip& clip = doc_.clip();
    float pull = float(ik_pull(clip, l));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Pull");
    ImGui::SameLine(ImGui::GetFontSize() * 5.5f);
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::SliderFloat("##ik_pull", &pull, 0.f, 1.f, "%.2f");
    if (ImGui::IsItemActivated()) doc_.history.begin(clip);  // before the first change: a click sets the value at once
    if (changed) {
        if (pull > 0) clip.ik_pull[l.name] = pull;
        else clip.ik_pull.erase(l.name);
    }
    if (ImGui::IsItemDeactivated() && doc_.history.is_open() && doc_.history.commit("Pull", clip)) mark_dirty();
    ImGui::SetItemTooltip("%s", (std::string("When you drag this target out of reach, ") +
                                 (l.name.rfind("Arm", 0) == 0 ? "the spine leans towards it, then " : "") +
                                 "the hips follow by this share of the distance still missing (0 = off)").c_str());
}

// RC-1 at the end of a target drag: part of the same undo step.
// ponytail: on release only; live reach would re-run it from the press-time clip on every move of the drag.
bool App::reach_after_drag(int limb) {
    Clip& clip = doc_.clip();
    const double pull = ik_pull(clip, rig_->limbs()[limb]);
    const double moved = pull > 0 ? reach_with_body(clip, *rig_, frame_, limb, pull, shape()) : 0;
    if (moved <= 0) return false;
    char buf[64];
    std::snprintf(buf, sizeof buf, "Hips moved %.1f cm to reach", moved * 100);
    status(buf);
    return true;
}

}  // namespace vats
