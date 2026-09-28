// Viewport Avatar Toolset - the Clean Up Foot Sliding window (Tools menu).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/07 RT-9 and 08 FC. The logic is in the core (footlock.h).
#include <cmath>
#include <cstdio>
#include <string>

#include "app.h"
#include "imgui.h"
#include "vats/footlock.h"

namespace vats {

void App::draw_foot_lock_window() {
    if (!show_foot_lock_) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Clean Up Foot Sliding", &show_foot_lock_)) return ImGui::End();

    // Both legs, or only the legs of the selected bones.
    FootLockOptions fl;
    fl.shape = export_shape();
    fl.heel_toe = foot_heel_toe_, fl.to_ground = foot_to_ground_;
    bool l = false, r = false;
    for (int n : selection_) {
        const std::string& name = skel_[n].name;
        bool leg = name.rfind("mHip", 0) == 0 || name.rfind("mKnee", 0) == 0 || name.rfind("mAnkle", 0) == 0 ||
                   name.rfind("mFoot", 0) == 0 || name.rfind("mToe", 0) == 0;
        if (leg && name.find("Left") != std::string::npos) l = true;
        if (leg && name.find("Right") != std::string::npos) r = true;
    }
    if (l || r) fl.left = l, fl.right = r;
    ImGui::Text("Legs: %s", fl.left && fl.right ? "both" : fl.left ? "left only (from the selection)" : "right only (from the selection)");

    // The ground, measured again whenever the clip changes (it evaluates every frame, so not every draw).
    if (!(foot_measured_ == doc_.clip()) || foot_measured_heel_toe_ != fl.heel_toe || foot_measured_legs_ != l + 2 * r) {
        foot_measured_ = doc_.clip();
        foot_measured_heel_toe_ = fl.heel_toe, foot_measured_legs_ = l + 2 * r;
        foot_ground_ = foot_ground(*rig_, foot_measured_, fl);
    }
    ImGui::Text("Ground: %.1f cm %s the floor", std::fabs(foot_ground_) * 100, foot_ground_ >= 0 ? "above" : "below");
    ImGui::SetItemTooltip("The lowest heel or toe over the animation, against the floor the avatar stands on at rest");

    ImGui::Checkbox("Heel and Toe", &foot_heel_toe_);
    ImGui::SetItemTooltip("Heel and toe land and leave separately (a heel-toe roll); off, the ankle alone");
    ImGui::Checkbox("Put Feet on the Ground", &foot_to_ground_);
    ImGui::SetItemTooltip("First moves the hips so the lowest foot touches the floor: fixes a take that floats or sinks");

    if (ImGui::Button("Clean Up")) {
        std::vector<std::string> report;
        edit("Clean Up Foot Sliding", [&](Clip& c) { report = lock_feet(c, *rig_, fl); });
        foot_report_ = report;
        std::string s;
        for (auto& line : report) s += (s.empty() ? "" : "; ") + line;
        status(s);
    }
    ImGui::SetItemTooltip("Holds planted feet still with leg IK; where a leg cannot reach, lowers the hips instead of "
                          "straightening the knee");
    if (!foot_report_.empty()) {
        ImGui::SeparatorText("Last clean-up");
        for (auto& line : foot_report_) ImGui::BulletText("%s", line.c_str());
    }
    ImGui::End();
}

}  // namespace vats
