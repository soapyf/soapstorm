// Viewport Avatar Toolset - the rotate, move and scale gizmo drawn over the 3D view.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/04 VP-41..VP-49 (scale: VP-46, props only). The gizmo reports world-space deltas measured from the press,
// so dragging back to the start restores the start value.
#pragma once

#include <string>

#include "imgui.h"
#include "view_math.h"

namespace vats {

enum class GizmoKind { Rotate, Move, Scale };

class Gizmo {
public:
    enum Part { None, AxisX, AxisY, AxisZ, ViewRing, Free, PlaneYZ, PlaneZX, PlaneXY, Centre };

    // Sets up the gizmo for this frame. axes = orientation whose X/Y/Z columns are the gizmo axes.
    void place(GizmoKind kind, const Vec3& centre, const Quat& axes, const Camera& cam, const Projector& proj,
               float size_px);
    bool visible() const { return visible_; }
    // Gimbal mode: three rings on independent (not orthogonal) axes; call after place().
    void set_axes(const Vec3 axes[3]) {
        if (dragging()) return;  // keep the axes a drag started with
        for (int i = 0; i < 3; ++i) custom_[i] = axes[i].normalized();
        use_custom_ = true;
    }
    // Rotate only: just the Z ring (placing an actor turns it about the vertical only). Call after place().
    void set_z_only(bool z) { z_only_ = z; }
    // The signed angle (radians) of the last ring drag, for per-channel gimbal edits.
    double last_angle() const { return last_angle_; }

    Part hit(ImVec2 mouse) const;
    void begin_drag(Part part, ImVec2 mouse);
    bool dragging() const { return drag_ != None; }
    Part drag_part() const { return drag_; }
    // World-space change since the press. snap = Ctrl held.
    void drag(ImVec2 mouse, bool snap, double snap_deg, Quat& rotation, Vec3& translation);
    void end_drag() { drag_ = None; }
    // Scale drags: the per-axis factor since the press (1 on untouched axes), in the gizmo's axes.
    const Vec3& scale() const { return scale_; }

    void draw(ImDrawList* dl, Part hover) const;

private:
    Vec3 axis(int i) const {
        if (use_custom_) return custom_[i];
        return axes_.rotate(i == 0 ? Vec3{1, 0, 0} : i == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1});
    }
    bool screen(const Vec3& p, ImVec2& out) const;
    void ring_points(int i, ImVec2* pts, bool* front) const;

    GizmoKind kind_ = GizmoKind::Rotate;
    Vec3 centre_;
    Quat axes_;
    Camera cam_;
    Projector proj_;
    float size_px_ = 90;
    double radius_ = 0.1;  // world units
    ImVec2 centre_px_;
    bool visible_ = false;

    Part drag_ = None;
    ImVec2 press_;
    ImVec2 tangent_;     // ring: screen tangent at the grab point; arrow: projected axis direction
    double axis_px_ = 1;  // arrow: projected axis length in pixels
    Vec3 grab_point_;     // plane: where the ray first hit the plane
    Quat frozen_axes_;
    Vec3 custom_[3];
    bool use_custom_ = false;
    bool z_only_ = false;
    double last_angle_ = 0;
    Vec3 scale_{1, 1, 1};
    std::string readout_;
};

}  // namespace vats
