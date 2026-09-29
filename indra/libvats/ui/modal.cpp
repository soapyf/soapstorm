// Viewport Avatar Toolset - Blender-style modal move and rotate (G / R over the view).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/04 VP-51 and section 3.3.
#include <cmath>
#include <cstdio>

#include "app.h"

namespace vats {

bool App::modal_pivot(Vec3& pivot, Quat& local) const {
    const auto& props = doc_.clip().props;
    if (selected_prop_ >= 0 && selected_prop_ < int(props.size())) {
        Xform f = prop_frame(props[selected_prop_]);
        pivot = f.pos, local = f.rot;
        return !props[selected_prop_].rigged;
    }
    if (const HandleRef* h = primary_handle()) {
        const LimbState& s = limb_states_[h->limb];
        pivot = h->pole ? s.pole : s.target.pos;
        local = h->pole ? Quat{} : s.target.rot;
        return true;
    }
    int p = primary();
    if (p < 0) return false;
    pivot = globals_[p].pos, local = local_axes(p);
    return true;
}

void App::start_modal(Modal kind) {
    Vec3 pivot;
    Quat local;
    if (!modal_pivot(pivot, local)) return status("Select a bone first");
    if (kind == Modal::Rotate && primary_handle() && primary_handle()->pole) kind = Modal::Move;  // poles only move
    modal_ = kind;
    modal_axis_ = -1;
    modal_local_ = false;
    modal_press_ = ImGui::GetIO().MousePos;
    drag_tool_ = kind == Modal::Move ? Tool::Move : Tool::Rotate;
    doc_.history.begin(doc_.clip());
    capture_edit_start();
}

void App::end_modal(bool confirm) {
    if (confirm) {
        const char* label = modal_ == Modal::Move ? "Move" : modal_ == Modal::Tween ? "Tween" : "Rotate";
        if (doc_.history.commit(label, doc_.clip())) mark_dirty();
    } else {
        doc_.clip() = doc_.history.cancel();  // back to the start value
        status("Cancelled");
    }
    modal_ = Modal::None;
    auto_ik_.on = false;
    skip_shortcuts_ = true;
}

bool App::modal_input(ImVec2 m) {
    if (modal_ == Modal::None) return false;
    ImGuiIO& io = ImGui::GetIO();
    // Confirm: left click, Enter, Space. Cancel: right click, Esc, Undo (VP-51).
    if (ImGui::IsMouseClicked(0) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
        ImGui::IsKeyPressed(ImGuiKey_Space)) {
        end_modal(true);
        return true;
    }
    if (ImGui::IsMouseClicked(1) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))) {
        end_modal(false);
        return true;
    }
    if (modal_ == Modal::Tween) {  // spec 08 TW-1: the mouse sets the tween, nothing else
        tween_drag(m);
        return true;
    }
    // X / Y / Z: world axis, then the local axis, then free again.
    const ImGuiKey axes[3] = {ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z};
    for (int a = 0; a < 3; ++a)
        if (ImGui::IsKeyPressed(axes[a], false)) {
            if (modal_axis_ != a) modal_axis_ = a, modal_local_ = false;
            else if (!modal_local_) modal_local_ = true;
            else modal_axis_ = -1;
        }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false) && modal_ == Modal::Rotate) modal_ = Modal::Trackball;

    Vec3 pivot;
    Quat local;
    modal_pivot(pivot, local);
    // Measure from the start state, not the live one.
    if (primary() >= 0 && !primary_handle() && selected_prop_ < 0) pivot = drag_start_global_.pos, local = drag_start_global_.rot * skel_.bone_axes(primary(), shape());
    Vec3 axis = modal_axis_ < 0 ? camera_.forward()
                                : (modal_local_ ? local : Quat{}).rotate(modal_axis_ == 0 ? Vec3{1, 0, 0}
                                                                         : modal_axis_ == 1 ? Vec3{0, 1, 0}
                                                                                            : Vec3{0, 0, 1});
    double sx = modal_press_.x, sy = modal_press_.y;
    projector_.to_screen(pivot, sx, sy);
    ImVec2 d(m.x - modal_press_.x, m.y - modal_press_.y);
    Quat r;
    Vec3 t;
    char readout[160];
    const char* axis_name = modal_axis_ < 0 ? "view" : modal_axis_ == 0 ? "X" : modal_axis_ == 1 ? "Y" : "Z";
    const char* space = modal_axis_ < 0 ? "" : modal_local_ ? " (local)" : " (world)";
    if (modal_ == Modal::Rotate) {
        double a0 = std::atan2(modal_press_.y - sy, modal_press_.x - sx), a1 = std::atan2(m.y - sy, m.x - sx);
        double angle = std::remainder(a1 - a0, 2 * kPi);
        if (io.KeyCtrl) {
            double step = snap_deg_ * kDegToRad;
            angle = std::round(angle / step) * step;
        }
        // Follow the cursor even when the axis points away from the camera.
        double sign = axis.dot(camera_.forward()) >= 0 ? 1 : -1;
        r = Quat::axis_angle(axis, angle * sign);
        std::snprintf(readout, sizeof readout, "Rotate %s%s  %.1f°    X Y Z constrain, R trackball, Ctrl snap, "
                      "click/Enter confirm, Esc cancel", axis_name, space, angle * kRadToDeg);
    } else if (modal_ == Modal::Trackball) {
        r = Quat::axis_angle(camera_.up(), d.x * 0.01) * Quat::axis_angle(camera_.right(), d.y * 0.01);
        std::snprintf(readout, sizeof readout, "Trackball    click/Enter confirm, Esc cancel");
    } else {
        double wpp = projector_.world_per_pixel(camera_, pivot);
        t = (camera_.right() * d.x - camera_.up() * d.y) * wpp;
        if (modal_axis_ >= 0) t = axis * t.dot(axis);
        std::snprintf(readout, sizeof readout, "Move %s%s  %.3f m    X Y Z constrain, click/Enter confirm, Esc cancel",
                      modal_axis_ < 0 ? "free" : axis_name, space, t.length());
    }
    apply_delta(r, t);
    modal_readout_ = readout;
    return true;
}

}  // namespace vats
