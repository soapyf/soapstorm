// Viewport Avatar Toolset - camera and projection maths for the viewport.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The scene is drawn in SL space directly (Z up); nothing is converted to a Y-up engine frame.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "vats/math.h"

namespace vats {

// Column-major 4x4, as OpenGL expects.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += at(i, k) * o.at(k, j);
                r.at(i, j) = s;
            }
        return r;
    }
    // Transforms a point; returns clip-space x, y, z, w.
    void apply(const Vec3& p, double out[4]) const {
        for (int i = 0; i < 4; ++i) out[i] = at(i, 0) * p.x + at(i, 1) * p.y + at(i, 2) * p.z + at(i, 3);
    }
};

inline Mat4 perspective(double fov_y_rad, double aspect, double znear, double zfar) {
    Mat4 r;
    double f = 1.0 / std::tan(fov_y_rad / 2);
    r.m[0] = float(f / aspect);
    r.m[5] = float(f);
    r.m[10] = float((zfar + znear) / (znear - zfar));
    r.m[11] = -1;
    r.m[14] = float(2 * zfar * znear / (znear - zfar));
    r.m[15] = 0;
    return r;
}

// Parallel projection of the box [-half_w, half_w] x [-half_h, half_h] x [-zfar, -znear] (view space) to clip space.
inline Mat4 orthographic(double half_w, double half_h, double znear, double zfar) {
    Mat4 r;
    r.m[0] = float(1 / half_w);
    r.m[5] = float(1 / half_h);
    r.m[10] = float(-2 / (zfar - znear));
    r.m[14] = float(-(zfar + znear) / (zfar - znear));
    return r;
}

inline Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 f = (target - eye).normalized(), s = f.cross(up).normalized(), u = s.cross(f);
    Mat4 r;
    r.at(0, 0) = float(s.x), r.at(0, 1) = float(s.y), r.at(0, 2) = float(s.z);
    r.at(1, 0) = float(u.x), r.at(1, 1) = float(u.y), r.at(1, 2) = float(u.z);
    r.at(2, 0) = float(-f.x), r.at(2, 1) = float(-f.y), r.at(2, 2) = float(-f.z);
    r.at(0, 3) = float(-s.dot(eye)), r.at(1, 3) = float(-u.dot(eye)), r.at(2, 3) = float(f.dot(eye));
    return r;
}

// Orbit camera around a target, always upright (world Z up). Spec 04 VP-55..VP-58.
struct Camera {
    static constexpr double kDefaultDistance = 3.2;
    static constexpr double kFov = 40 * kDegToRad;

    Vec3 target{0, 0, 1.0};
    double yaw = 0.6, pitch = 0.18, distance = kDefaultDistance;
    double fov = kFov;  // vertical; a host whose camera has its own lens (the viewer's) sets it
    // Orthographic (VP-67): parallel rays along forward(). The view is as tall at the target as the perspective
    // one (ortho_half_height), so distance stays the zoom and framing keeps its meaning. Anything up to
    // kOrthoBack behind the eye still draws and picks, since a close zoom puts the eye inside the body.
    bool ortho = false;
    static constexpr double kOrthoBack = 10;
    double ortho_half_height() const { return distance * std::tan(fov / 2); }

    Vec3 forward() const {  // from the camera towards the target
        return Vec3{-std::cos(pitch) * std::cos(yaw), -std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
    }
    Vec3 eye() const { return target - forward() * distance; }
    Vec3 right() const { return forward().cross({0, 0, 1}).normalized(); }
    Vec3 up() const { return right().cross(forward()); }

    Mat4 view() const { return look_at(eye(), target, {0, 0, 1}); }
    Mat4 projection(double aspect) const {
        if (!ortho) return perspective(fov, aspect, 0.01, 200.0);
        const double h = ortho_half_height();
        return orthographic(h * aspect, h, -kOrthoBack, 200.0);
    }
    // The unit direction from p towards the viewer: towards the eye, or straight back along the view in ortho.
    Vec3 to_viewer(const Vec3& p) const { return ortho ? -forward() : (eye() - p).normalized(); }

    void orbit(double dx, double dy) {
        yaw -= dx * 0.008;
        pitch = std::clamp(pitch + dy * 0.008, -1.5, 1.5);
    }
    void pan(double dx, double dy) {
        double k = 0.0015 * distance;
        target += right() * (-dx * k) + up() * (dy * k);
    }
    void zoom(double factor) { distance = std::clamp(distance * factor, 0.1, 30.0); }
    // Keeps the camera where it is and turns it to look at a new target (Second Life's Alt+click).
    void focus_on(const Vec3& point) {
        Vec3 e = eye(), d = e - point;
        double len = d.length();
        if (len < 1e-4) return;
        target = point;
        distance = std::clamp(len, 0.1, 30.0);
        pitch = std::clamp(std::asin(d.z / len), -1.5, 1.5);
        yaw = std::atan2(d.y, d.x);
    }
};

// Nearest hit distance of a ray on a triangle mesh (3 floats per vertex), or 1e30 (Moller-Trumbore).
template <class Index>
double ray_triangles(const Vec3& o, const Vec3& d, const std::vector<float>& pos, const std::vector<Index>& idx) {
    double best = 1e30;
    auto vtx = [&](Index i) { return Vec3{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}; };
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        Vec3 a = vtx(idx[t]), e1 = vtx(idx[t + 1]) - a, e2 = vtx(idx[t + 2]) - a, pv = d.cross(e2);
        double det = e1.dot(pv);
        if (std::fabs(det) < 1e-12) continue;
        Vec3 tv = o - a;
        double u = tv.dot(pv) / det;
        if (u < 0 || u > 1) continue;
        Vec3 qv = tv.cross(e1);
        double v = d.dot(qv) / det, dist = e2.dot(qv) / det;
        if (v < 0 || u + v > 1 || dist <= 0 || dist >= best) continue;
        best = dist;
    }
    return best;
}

// Projection of world points into a viewport rectangle (pixels, origin top-left).
struct Projector {
    Mat4 view_proj;
    double x0 = 0, y0 = 0, w = 1, h = 1;

    // Returns false when the point is behind the camera.
    bool to_screen(const Vec3& p, double& sx, double& sy) const {
        double c[4];
        view_proj.apply(p, c);
        if (c[3] <= 1e-6) return false;
        sx = x0 + (c[0] / c[3] * 0.5 + 0.5) * w;
        sy = y0 + (1 - (c[1] / c[3] * 0.5 + 0.5)) * h;
        return true;
    }
    // World units per pixel at the depth of p (for constant-size gizmos).
    double world_per_pixel(const Camera& cam, const Vec3& p) const {
        if (cam.ortho) return 2 * cam.ortho_half_height() / h;
        double depth = (p - cam.eye()).dot(cam.forward());
        return 2 * depth * std::tan(cam.fov / 2) / h;
    }
    // A ray from the camera through a screen point. In ortho every ray runs along forward(), from kOrthoBack
    // behind the eye's plane (what the projection still draws).
    void ray(const Camera& cam, double sx, double sy, Vec3& origin, Vec3& dir) const {
        double nx = (sx - x0) / w * 2 - 1, ny = 1 - (sy - y0) / h * 2;
        double t = std::tan(cam.fov / 2);
        if (cam.ortho) {
            const double k = cam.ortho_half_height();
            dir = cam.forward();
            origin = cam.eye() - dir * Camera::kOrthoBack + cam.right() * (nx * k * w / h) + cam.up() * (ny * k);
            return;
        }
        origin = cam.eye();
        dir = (cam.forward() + cam.right() * (nx * t * w / h) + cam.up() * (ny * t)).normalized();
    }
};

}  // namespace vats
