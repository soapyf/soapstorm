// Viewport Avatar Toolset - the rotate, move and scale gizmo drawn over the 3D view.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "gizmo.h"

#include <cmath>
#include <cstdio>

namespace vats {
namespace {

constexpr int kSegments = 64;
constexpr float kHitPx = 9;
const ImU32 kAxisColour[3] = {IM_COL32(242, 77, 77, 255), IM_COL32(102, 230, 89, 255), IM_COL32(89, 140, 255, 255)};
const ImU32 kHot = IM_COL32(255, 230, 51, 255);
const ImU32 kViewColour = IM_COL32(115, 217, 255, 255);

double dot2(ImVec2 a, ImVec2 b) { return double(a.x) * b.x + double(a.y) * b.y; }
ImVec2 sub(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
double len2(ImVec2 a) { return std::sqrt(dot2(a, a)); }

double segment_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
    ImVec2 ab = sub(b, a), ap = sub(p, a);
    double t = std::clamp(dot2(ap, ab) / std::max(dot2(ab, ab), 1e-9), 0.0, 1.0);
    return len2(ImVec2(float(ap.x - ab.x * t), float(ap.y - ab.y * t)));
}

ImU32 with_alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(a * 255) << 24); }

}  // namespace

bool Gizmo::screen(const Vec3& p, ImVec2& out) const {
    double x, y;
    if (!proj_.to_screen(p, x, y)) return false;
    out = ImVec2(float(x), float(y));
    return true;
}

void Gizmo::place(GizmoKind kind, const Vec3& centre, const Quat& axes, const Camera& cam, const Projector& proj,
                  float size_px) {
    z_only_ = false;
    kind_ = kind;
    centre_ = centre;
    axes_ = dragging() ? frozen_axes_ : axes;  // axes stay put while a drag is running
    if (!dragging()) use_custom_ = false;
    cam_ = cam;
    proj_ = proj;
    size_px_ = size_px;
    radius_ = size_px * proj.world_per_pixel(cam, centre);
    visible_ = (centre - cam.eye()).dot(cam.forward()) > 0.02 && screen(centre, centre_px_);
}

void Gizmo::ring_points(int i, ImVec2* pts, bool* front) const {
    Vec3 a = axis(i), u = axis((i + 1) % 3);
    u = (u - a * u.dot(a)).normalized();  // gimbal axes are not orthogonal
    if (u.length() < 1e-6) u = (std::fabs(a.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}).cross(a).normalized();
    Vec3 v = a.cross(u);
    Vec3 to_eye = (cam_.eye() - centre_).normalized();
    for (int k = 0; k <= kSegments; ++k) {
        double t = 2 * kPi * k / kSegments;
        Vec3 d = u * std::cos(t) + v * std::sin(t);
        Vec3 p = centre_ + d * radius_;
        if (!screen(p, pts[k])) pts[k] = centre_px_;
        front[k] = d.dot(to_eye) >= -0.05;
    }
}

Gizmo::Part Gizmo::hit(ImVec2 m) const {
    if (!visible_) return None;
    ImVec2 pts[kSegments + 1];
    bool front[kSegments + 1];
    if (kind_ == GizmoKind::Rotate) {
        Part best = None;
        double best_d = kHitPx;
        for (int i = z_only_ ? 2 : 0; i < 3; ++i) {
            ring_points(i, pts, front);
            for (int k = 0; k < kSegments; ++k) {
                if (!front[k] || !front[k + 1]) continue;
                double d = segment_distance(m, pts[k], pts[k + 1]);
                if (d < best_d) best_d = d, best = Part(AxisX + i);
            }
        }
        if (best != None || z_only_) return best;
        double r = len2(sub(m, centre_px_));
        if (std::fabs(r - size_px_ * 1.15) < kHitPx) return ViewRing;
        if (r < size_px_) return Free;
        return None;
    }
    // Move: centre square, then planes, then arrows. Scale: centre square, then axes (VP-46).
    const float centre_half = kind_ == GizmoKind::Scale ? 8 : 9;
    if (std::fabs(m.x - centre_px_.x) <= centre_half && std::fabs(m.y - centre_px_.y) <= centre_half) return Centre;
    for (int i = 0; i < 3 && kind_ == GizmoKind::Move; ++i) {
        Vec3 u = axis((i + 1) % 3), v = axis((i + 2) % 3);
        ImVec2 q[4];
        bool ok = screen(centre_ + (u * 0.22 + v * 0.22) * radius_, q[0]) &&
                  screen(centre_ + (u * 0.42 + v * 0.22) * radius_, q[1]) &&
                  screen(centre_ + (u * 0.42 + v * 0.42) * radius_, q[2]) &&
                  screen(centre_ + (u * 0.22 + v * 0.42) * radius_, q[3]);
        if (!ok) continue;
        int pos = 0, neg = 0;  // inside a convex quad of either winding: all edge crosses share a sign
        for (int k = 0; k < 4; ++k) {
            ImVec2 e = sub(q[(k + 1) % 4], q[k]), p = sub(m, q[k]);
            (e.x * p.y - e.y * p.x > 0 ? pos : neg)++;
        }
        if (pos == 4 || neg == 4) return Part(PlaneYZ + i);
    }
    Part best = None;
    double best_d = kHitPx;
    for (int i = 0; i < 3; ++i) {
        ImVec2 tip;
        if (!screen(centre_ + axis(i) * radius_, tip) || len2(sub(tip, centre_px_)) < 10) continue;
        double d = segment_distance(m, centre_px_, tip);
        if (d < best_d) best_d = d, best = Part(AxisX + i);
    }
    return best;
}

void Gizmo::begin_drag(Part part, ImVec2 m) {
    drag_ = part;
    press_ = m;
    frozen_axes_ = axes_;
    readout_.clear();
    if (kind_ == GizmoKind::Rotate && part >= AxisX && part <= AxisZ) {
        // Tangent of the ring at the front point nearest the press.
        int i = part - AxisX;
        ImVec2 pts[kSegments + 1];
        bool front[kSegments + 1];
        ring_points(i, pts, front);
        int best = 0;
        double best_d = 1e30;
        for (int k = 0; k < kSegments; ++k) {
            if (!front[k]) continue;
            double d = segment_distance(m, pts[k], pts[k + 1]);
            if (d < best_d) best_d = d, best = k;
        }
        ImVec2 t = sub(pts[best + 1], pts[best]);
        double l = std::max(len2(t), 1e-6);
        tangent_ = ImVec2(float(t.x / l), float(t.y / l));
    } else if (kind_ != GizmoKind::Rotate && part >= AxisX && part <= AxisZ) {
        ImVec2 tip;
        screen(centre_ + axis(part - AxisX) * radius_, tip);
        ImVec2 d = sub(tip, centre_px_);
        axis_px_ = std::max(len2(d), 1.0);
        tangent_ = ImVec2(float(d.x / axis_px_), float(d.y / axis_px_));
    } else if (part >= PlaneYZ && part <= PlaneXY) {
        Vec3 o, dir, n = axis(part - PlaneYZ);
        proj_.ray(cam_, m.x, m.y, o, dir);
        double den = dir.dot(n);
        grab_point_ = std::fabs(den) > 1e-6 ? o + dir * ((centre_ - o).dot(n) / den) : centre_;
    }
}

void Gizmo::drag(ImVec2 m, bool snap, double snap_deg, Quat& rotation, Vec3& translation) {
    rotation = Quat{};
    translation = Vec3{};
    scale_ = {1, 1, 1};
    ImVec2 d = sub(m, press_);
    char buf[64];
    auto snap_angle = [&](double a) {
        if (!snap || snap_deg <= 0) return a;
        double s = snap_deg * kDegToRad;
        return std::round(a / s) * s;
    };
    auto snap_len = [&](double v) { return snap ? std::round(v / 0.01) * 0.01 : v; };
    static const char* names = "XYZ";

    if (kind_ == GizmoKind::Rotate) {
        if (drag_ >= AxisX && drag_ <= AxisZ) {
            double a = snap_angle(dot2(d, tangent_) / size_px_);
            last_angle_ = a;
            rotation = Quat::axis_angle(axis(drag_ - AxisX), a);
            std::snprintf(buf, sizeof buf, "%c %.1f°", names[drag_ - AxisX], a * kRadToDeg);
            readout_ = buf;
        } else if (drag_ == ViewRing) {
            double a0 = std::atan2(press_.y - centre_px_.y, press_.x - centre_px_.x);
            double a1 = std::atan2(m.y - centre_px_.y, m.x - centre_px_.x);
            double a = snap_angle(std::remainder(a1 - a0, 2 * kPi));
            rotation = Quat::axis_angle(cam_.forward(), a);
            std::snprintf(buf, sizeof buf, "View %.1f°", a * kRadToDeg);
            readout_ = buf;
        } else if (drag_ == Free) {
            rotation = Quat::axis_angle(cam_.up(), d.x * 0.01) * Quat::axis_angle(cam_.right(), d.y * 0.01);
        }
        return;
    }
    if (kind_ == GizmoKind::Scale) {
        double f = drag_ == Centre ? std::exp(0.01 * (d.x - d.y)) : std::max(0.01, 1 + dot2(d, tangent_) / axis_px_);
        if (snap) f = std::max(0.05, std::round(f * 10) / 10);
        if (drag_ == Centre) scale_ = {f, f, f};
        else if (drag_ >= AxisX && drag_ <= AxisZ) scale_[drag_ - AxisX] = f;
        const char* ax = drag_ == Centre ? "XYZ" : drag_ == AxisX ? "X" : drag_ == AxisY ? "Y" : "Z";
        std::snprintf(buf, sizeof buf, "%s ×%.2f", ax, f);
        readout_ = buf;
        return;
    }
    if (drag_ >= AxisX && drag_ <= AxisZ) {
        double dist = snap_len(dot2(d, tangent_) / axis_px_ * radius_);
        translation = axis(drag_ - AxisX) * dist;
        std::snprintf(buf, sizeof buf, "%c %+.3f m", names[drag_ - AxisX], dist);
        readout_ = buf;
    } else if (drag_ >= PlaneYZ && drag_ <= PlaneXY) {
        Vec3 o, dir, n = axis(drag_ - PlaneYZ);
        proj_.ray(cam_, m.x, m.y, o, dir);
        double den = dir.dot(n);
        if (std::fabs(den) > 1e-6) {
            Vec3 p = o + dir * ((grab_point_ - o).dot(n) / den);
            translation = p - grab_point_;
            if (snap) {
                Vec3 u = axis((drag_ - PlaneYZ + 1) % 3), v = axis((drag_ - PlaneYZ + 2) % 3);
                translation = u * snap_len(translation.dot(u)) + v * snap_len(translation.dot(v));
            }
        }
        static const char* planes[] = {"YZ plane", "ZX plane", "XY plane"};
        readout_ = planes[drag_ - PlaneYZ];
    } else if (drag_ == Centre) {
        double wpp = proj_.world_per_pixel(cam_, centre_);
        translation = (cam_.right() * d.x - cam_.up() * d.y) * wpp;
    }
}

void Gizmo::draw(ImDrawList* dl, Part hover, ImU32 tint) const {
    if (!visible_) return;
    const ImU32 view = tint ? tint : kViewColour;
    auto hot = [&](Part p) { return drag_ == p || (drag_ == None && hover == p); };
    if (kind_ == GizmoKind::Rotate) {
        if (!z_only_) {
            dl->AddCircleFilled(centre_px_, size_px_, tint ? with_alpha(tint, hot(Free) ? 0.22f : 0.12f)
                                                           : IM_COL32(255, 255, 255, hot(Free) ? 26 : 10), kSegments);
            dl->AddCircle(centre_px_, size_px_ * 1.15f, hot(ViewRing) ? kHot : view, kSegments, hot(ViewRing) ? 3 : 2);
        }
        ImVec2 pts[kSegments + 1];
        bool front[kSegments + 1];
        for (int i = z_only_ ? 2 : 0; i < 3; ++i) {
            ring_points(i, pts, front);
            Part p = Part(AxisX + i);
            ImU32 c = hot(p) ? kHot : kAxisColour[i];
            for (int k = 0; k < kSegments; ++k) {
                bool f = front[k] && front[k + 1];
                dl->AddLine(pts[k], pts[k + 1], f ? c : with_alpha(c, 0.18f), f ? (hot(p) ? 3.5f : 2.5f) : 1.5f);
            }
        }
    } else if (kind_ == GizmoKind::Scale) {
        for (int i = 0; i < 3; ++i) {  // axis lines ending in 10 px squares
            ImVec2 tip;
            if (!screen(centre_ + axis(i) * radius_, tip)) continue;
            Part p = Part(AxisX + i);
            ImU32 c = hot(p) ? kHot : kAxisColour[i];
            dl->AddLine(centre_px_, tip, c, hot(p) ? 3.5f : 2.5f);
            dl->AddRectFilled(ImVec2(tip.x - 5, tip.y - 5), ImVec2(tip.x + 5, tip.y + 5), c);
        }
        ImVec2 a(centre_px_.x - 8, centre_px_.y - 8), b(centre_px_.x + 8, centre_px_.y + 8);
        dl->AddRectFilled(a, b, with_alpha(hot(Centre) ? kHot : view, hot(Centre) ? 0.35f : 0.12f));
        dl->AddRect(a, b, hot(Centre) ? kHot : view, 0, 0, 2.0f);
    } else {
        for (int i = 0; i < 3; ++i) {
            Vec3 u = axis((i + 1) % 3), v = axis((i + 2) % 3);
            ImVec2 q[4];
            if (screen(centre_ + (u * 0.22 + v * 0.22) * radius_, q[0]) &&
                screen(centre_ + (u * 0.42 + v * 0.22) * radius_, q[1]) &&
                screen(centre_ + (u * 0.42 + v * 0.42) * radius_, q[2]) &&
                screen(centre_ + (u * 0.22 + v * 0.42) * radius_, q[3])) {
                Part p = Part(PlaneYZ + i);
                dl->AddQuadFilled(q[0], q[1], q[2], q[3], with_alpha(kAxisColour[i], hot(p) ? 0.45f : 0.22f));
                dl->AddQuad(q[0], q[1], q[2], q[3], hot(p) ? kHot : kAxisColour[i], 1.5f);
            }
        }
        for (int i = 0; i < 3; ++i) {
            ImVec2 tip;
            if (!screen(centre_ + axis(i) * radius_, tip)) continue;
            Part p = Part(AxisX + i);
            ImU32 c = hot(p) ? kHot : kAxisColour[i];
            ImVec2 dir = sub(tip, centre_px_);
            double l = len2(dir);
            if (l < 1) continue;
            ImVec2 n(float(dir.x / l), float(dir.y / l)), side(-n.y, n.x);
            ImVec2 base(tip.x - n.x * 12, tip.y - n.y * 12);
            dl->AddLine(centre_px_, base, c, hot(p) ? 3.5f : 2.5f);
            dl->AddTriangleFilled(tip, ImVec2(base.x + side.x * 6, base.y + side.y * 6),
                                  ImVec2(base.x - side.x * 6, base.y - side.y * 6), c);
        }
        ImVec2 a(centre_px_.x - 7, centre_px_.y - 7), b(centre_px_.x + 7, centre_px_.y + 7);
        dl->AddRectFilled(a, b, with_alpha(hot(Centre) ? kHot : view, 0.35f));
        dl->AddRect(a, b, hot(Centre) ? kHot : view, 0, 0, 1.5f);
    }
    if (dragging() && !readout_.empty()) {
        ImVec2 at(centre_px_.x + size_px_ * 0.8f, centre_px_.y - size_px_ * 0.9f);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy) dl->AddText(ImVec2(at.x + dx, at.y + dy), IM_COL32(0, 0, 0, 200), readout_.c_str());
        dl->AddText(at, IM_COL32(255, 255, 255, 255), readout_.c_str());
    }
}

}  // namespace vats
