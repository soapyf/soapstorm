// Viewport Avatar Toolset - Tools > Push Hands Out of the Body: hands that sink into the mesh body shown, pushed out
// through the arms (mesh_contact.h).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <cmath>
#include <cstdio>
#include <string>

#include "app.h"
#include "icon_button.h"
#include "icons.h"
#include "imgui.h"
#include "theme.h"
#include "vats/mesh_contact.h"

namespace vats {

void App::draw_hands_out_window() {
    if (!show_hands_out_) return;
    place_tool_window("Push Hands Out of the Body", 24, 20);
    if (!ImGui::Begin("Push Hands Out of the Body", &show_hands_out_)) {
        hands_out_state_ = 0;  // a push asked for and not shown is not started later unasked
        return ImGui::End();
    }
    help_button("animation-check#Hands inside the body");
    const MeshBody* b = mesh_body();
    if (!b) {
        hands_out_state_ = 0;
        ImGui::TextWrapped("Show a mesh body first (View > Body). This tests the hands against that body's own mesh, so "
                           "wide hips or a big belly count as they look.");
        return ImGui::End();
    }
    double a = 0, z = 0;
    const bool ranged = clip_range(a, z);
    const int from = ranged ? int(std::lround(a)) : 0, to = ranged ? int(std::lround(z)) : doc_.clip().end_frame;
    ImGui::TextWrapped("Where a hand or forearm sinks into %s, it is moved out through the arm, easing out and back "
                       "around each contact.", b->name.c_str());
    ImGui::Text("Frames %d to %d%s", from, to, ranged ? " (the marked range)" : " (the whole animation)");
    if (hands_out_state_ == 1) {  // shown for a frame before the work starts, which holds the window
        ImGui::TextUnformatted("Working... a long take takes up to a minute.");
        hands_out_state_ = 2;
    } else if (hands_out_state_ == 2) {
        std::vector<const DaeModel*> parts;
        for (const std::string& path : b->parts) parts.push_back(prop_model(path));
        PushOutReport r;
        edit("Push Hands Out of the Body", [&](Clip& c) { r = push_hands_out(c, *rig_, parts, resized(view_body_shape(), body_size()), from, to, constraints()); });
        char buf[240];
        if (r.deepest_frame < 0)
            std::snprintf(buf, sizeof buf, "The hands stay out of the body on every frame");
        else
            std::snprintf(buf, sizeof buf, "Moved the left hand on %d frames and the right on %d; the deepest was the %s "
                          "hand, %.1f cm in at frame %d", r.frames[0], r.frames[1], r.deepest_side ? "right" : "left",
                          r.deepest * 100, r.deepest_frame);
        hands_out_report_ = buf;
        status(buf);
        hands_out_state_ = 0;
    } else if (primary_button("Push Out", "", 0, icon::kCleanUp)) {
        hands_out_state_ = 1;
    }
    ImGui::SetItemTooltip("Keys the arm (its IK target, or the shoulder, elbow and wrist) on the frames it moves; one undo "
                          "step");
    if (!hands_out_report_.empty()) ImGui::TextWrapped("%s", hands_out_report_.c_str());
    ImGui::End();
}

}  // namespace vats
