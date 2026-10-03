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

    // The lowest the eye may go, a render-time clamp that leaves yaw, pitch and distance alone: Second Life's
    // "don't let camera go underground" (llagentcamera.cpp:2338-2347, camera z >= land + getCameraMinOffGround()).
    // The Second Life preset sets it; off (very low) elsewhere and in ortho.
    double min_eye_z = -1e30;

    Vec3 orbit_dir() const {  // from the unclamped eye towards the target: yaw and pitch alone
        return Vec3{-std::cos(pitch) * std::cos(yaw), -std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
    }
    Vec3 eye() const {
        Vec3 e = target - orbit_dir() * distance;
        if (!ortho && e.z < min_eye_z) e.z = min_eye_z;
        return e;
    }
    Vec3 forward() const {  // from the camera towards the target
        if (ortho || min_eye_z <= -1e29) return orbit_dir();
        Vec3 f = target - eye();
        return f.length() > 1e-9 ? f.normalized() : orbit_dir();
    }
    Vec3 right() const { return forward().cross({0, 0, 1}).normalized(); }
    Vec3 up() const { return right().cross(forward()); }

    Mat4 view() const { return look_at(eye(), target, {0, 0, 1}); }
    Mat4 projection(double aspect) const {
        // Near 0.01 m is the floor the viewer drops its near plane to when the camera is focused within 0.5 m
        // (llviewerdisplay.cpp:815-822, MIN_NEAR_PLANE llcamera.h:49), so a close zoom never slices the face. Far
        // reaches past the Second Life preset's 240 m zoom limit.
        if (!ortho) return perspective(fov, aspect, 0.01, 512.0);
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
    // Keeps the camera where it is and turns it to look at a new target (Second Life's Alt+click): the offset
    // becomes camera minus the new focus, as LLAgentCamera::setFocusGlobal does (llagentcamera.cpp:3125-3131).
    void focus_on(const Vec3& point) { look(eye(), point); }
    void look(const Vec3& e, const Vec3& point) {
        Vec3 d = e - point;
        double len = d.length();
        if (len < 1e-4) return;
        target = point;
        distance = len;
        pitch = std::clamp(std::asin(d.z / len), -kPi / 2 + kDegToRad, kPi / 2 - kDegToRad);  // cameraOrbitOver's 1..179
        yaw = std::atan2(d.y, d.x);
    }
};

// Second Life's Alt camera, for the Second Life control preset: LLAgentCamera (llagentcamera.cpp) with the
// focus not on the avatar, which is where an Alt+click leaves it. SL's mCameraFocusOffsetTarget (camera minus
// focus) is -orbit_dir() * distance here and its focus is target. Every move ends in cameraZoomIn(1), which is what
// stops the camera at the focus object instead of passing through it.
namespace slcam {

constexpr double kLandMinZoom = 0.15, kAvatarMinZoom = 0.5, kObjectMinZoom = 0.02;  // llagentcamera.cpp:95-98
// calcCameraMinDistance's fudge "that lets you zoom in on avatars a bit more" (llagentcamera.cpp:81-83).
constexpr double kAvatarZoomMinX = 0.55, kAvatarZoomMinY = 0.7, kAvatarZoomMinZ = 1.15;
constexpr double kObjectExtentsPadding = 0.5;  // llagentcamera.cpp:108
// getCameraMaxZoomDistance (llagentcamera.cpp:2470): min(MAX_CAMERA_DISTANCE_FROM_OBJECT 496, draw distance 256 - 1,
// region width 256 - CAMERA_FUDGE_FROM_OBJECT 16).
constexpr double kMaxZoom = 240;
constexpr double kNear = 0.01;         // the near plane at the distances where the minimum bites (Camera::projection)
constexpr double kMinOffGround = 0.5;  // getCameraMinOffGround, llagentcamera.cpp:2586; the ground is z 0
constexpr double kAgentDepth = 0.45, kAgentWidth = 0.60;  // DEFAULT_AGENT_DEPTH / WIDTH, indra_constants.h:43-44

// What the focus is on (SL's mFocusObject): the land, an avatar (its body, or something attached to it, which SL
// swaps for the avatar, llagentcamera.cpp:3171-3179), or an object.
struct Focus {
    enum Kind { Land, Avatar, Object } kind = Avatar;
    // The avatar object: LLVOAvatar's position is the middle of its box (llvoavatar.cpp:5825), turned with the agent,
    // and its scale is the agent size: depth, width, body height.
    Xform avatar{Quat{}, {0, 0, 0.84}};
    Vec3 size{kAgentDepth, kAgentWidth, 1.68};
};

// LLAgentCamera::calcCameraMinDistance (llagentcamera.cpp:623-787), line for line: how close the camera may come
// to the focus given where it looks from, from the avatar's box shrunk or grown by the focus's offset in it.
inline bool min_distance(const Camera& cam, const Focus& f, double& obj_min_distance) {
    const bool soft_limit = true;  // avatars
    const Quat inv_object_rot = f.avatar.rot.conj();
    const Vec3 target_offset_origin = inv_object_rot.rotate(cam.target - f.avatar.pos);
    const Vec3 camera_offset_target = inv_object_rot.rotate(cam.eye() - cam.target);
    Vec3 object_extents = f.size.mul({kAvatarZoomMinX, kAvatarZoomMinY, kAvatarZoomMinZ});
    bool target_outside_object_extents = false;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(target_offset_origin[i]) * 2 > object_extents[i] + kObjectExtentsPadding)
            target_outside_object_extents = true;
        object_extents[i] += camera_offset_target[i] > 0 ? -target_offset_origin[i] * 2 : target_offset_origin[i] * 2;
    }
    for (int i = 0; i < 3; ++i) object_extents[i] = std::max(object_extents[i], 0.001);  // "so far that the object inverts"
    Vec3 cam_abs_norm{std::fabs(camera_offset_target.x), std::fabs(camera_offset_target.y), std::fabs(camera_offset_target.z)};
    for (int i = 0; i < 3; ++i) cam_abs_norm[i] = std::max(cam_abs_norm[i], 0.001);
    cam_abs_norm = cam_abs_norm.normalized();
    const Vec3 scaled{cam_abs_norm.x / object_extents.x, cam_abs_norm.y / object_extents.y, cam_abs_norm.z / object_extents.z};
    const int axis = scaled.x > scaled.y && scaled.x > scaled.z ? 0 : scaled.y > scaled.z ? 1 : 2;
    obj_min_distance = cam_abs_norm[axis] < 0.001 ? object_extents[axis] * 0.5 : object_extents[axis] * 0.5 / cam_abs_norm[axis];

    Vec3 ts{std::fabs(target_offset_origin.x), std::fabs(target_offset_origin.y), std::fabs(target_offset_origin.z)};
    ts = ts.length() > 1e-6 ? ts.normalized() : Vec3{};  // LLVector3::normalize zeroes a tiny vector
    ts = {ts.x / object_extents.x, ts.y / object_extents.y, ts.z / object_extents.z};
    const int split = ts.x > ts.y && ts.x > ts.z ? 0 : ts.y > ts.z ? 1 : 2;
    // As in SL, the camera's offset from the object is not turned into the object's frame before the dot product.
    const double camera_offset_clip = (cam.eye() - f.avatar.pos)[split], target_offset_clip = target_offset_origin[split];
    if (target_outside_object_extents &&
        ((camera_offset_clip > 0 && target_offset_clip > 0) || (camera_offset_clip < 0 && target_offset_clip < 0)))
        return false;
    obj_min_distance = std::min(obj_min_distance, 10 * std::sqrt(3.0));  // "diagonal of 10 by 10 cube"
    obj_min_distance += kNear + (soft_limit ? 0.1 : 0.2);
    return true;
}

// LLAgentCamera::cameraZoomIn (llagentcamera.cpp:996-1061): scales the distance to the focus, never nearer than
// the focus allows ("Don't move through focus point") and never beyond 240 m or four times the distance now.
inline void zoom_in(Camera& cam, const Focus& f, double fraction) {
    const double current = cam.distance;
    double d = current * fraction, min_zoom = kLandMinZoom;
    if (f.kind == Focus::Avatar) min_distance(cam, f, min_zoom);  // SL ignores the bool: min_zoom may be set anyway
    else if (f.kind == Focus::Object) min_zoom = kObjectMinZoom;
    d = std::max(d, min_zoom);
    d = std::min(d, std::min(kMaxZoom, current * 4));  // MAINT-3154
    cam.distance = d;
}

// LLAgentCamera::cameraOrbitIn (llagentcamera.cpp:1092-1143), the wheel and Alt+Up/Down: moves meters towards the
// focus, stopping 0.5 m from an avatar, 2 cm from an object, 15 cm from the land; then cameraZoomIn(1).
inline void orbit_in(Camera& cam, const Focus& f, double meters) {
    const double min_zoom = f.kind == Focus::Avatar ? kAvatarMinZoom : f.kind == Focus::Object ? kObjectMinZoom : kLandMinZoom;
    cam.distance = std::min(std::max(cam.distance - meters, min_zoom), kMaxZoom);
    zoom_in(cam, f, 1);
}

// LLAgentCamera::cameraOrbitAround (llagentcamera.cpp:903-921): the offset turns about world Z.
inline void orbit_around(Camera& cam, const Focus& f, double radians) {
    cam.yaw += radians;
    zoom_in(cam, f, 1);
}

// LLAgentCamera::cameraOrbitOver (llagentcamera.cpp:927-953): up or down about the camera's left axis, the angle
// from straight up kept within 1..179 degrees.
inline void orbit_over(Camera& cam, const Focus& f, double angle) {
    const double from_up = kPi / 2 - cam.pitch;
    cam.pitch = kPi / 2 - std::clamp(from_up - angle, 1 * kDegToRad, 179 * kDegToRad);
    zoom_in(cam, f, 1);
}

// LLAgentCamera::cameraPanLeft / cameraPanUp (llagentcamera.cpp:1167-1206): the focus moves along the camera's
// left and up axes and the camera with it.
inline void pan(Camera& cam, const Focus& f, double left, double up) {
    cam.target += cam.right() * -left + cam.up() * up;
    zoom_in(cam, f, 1);
}

}  // namespace slcam

// Camera glide: smoothly eases the camera to a new focus or look direction, blending user inputs (orbit,
// pan, zoom, wheel, view keys and view-cube clicks) smoothly into the target while the glide runs without
// jumping or restarting, matching Second Life viewer's LLAgentCamera easing.
struct CameraGlide {
    Camera target_cam;
    bool active = false;
    bool fixed_eye = false;  // a focus glide: the eye stays put and only the look-at point travels (SL Alt-click)
    double half_life = 0.06;  // exponential smoothing half-life in seconds (ZoomTime ~0.4s)

    // Focus on a point (SL Alt-click): the eye stays where it is and turns to look at point.
    void focus_on(const Camera& current, const Vec3& point) {
        // The eye is the one on screen: a turn or an input still blending in stops here, else the eye would jump to
        // where that glide was heading. The lens and ground stay the target's (they apply at once anyway).
        if (!active || !fixed_eye) {
            const Camera was = target_cam;
            target_cam = current;
            if (active) target_cam.fov = was.fov, target_cam.ortho = was.ortho, target_cam.min_eye_z = was.min_eye_z;
            active = true;
        }
        fixed_eye = true;
        target_cam.focus_on(point);
        target_cam.yaw = current.yaw + std::remainder(target_cam.yaw - current.yaw, 2 * kPi);
    }

    // Look from a direction (View Cube): sets target yaw and pitch to face along direction.
    void look_from(const Camera& current, const Vec3& dir) {
        if (!active) {
            target_cam = current;
            active = true;
        }
        fixed_eye = false;
        const Vec3 d = dir.normalized();
        const double horiz = std::hypot(d.x, d.y);
        double yaw = horiz > 1e-6 ? std::atan2(d.y, d.x) : current.yaw;
        const double limit = target_cam.ortho ? kPi / 2 - 1e-4 : 1.5;
        double pitch = std::clamp(std::atan2(d.z, horiz), -limit, limit);
        yaw = current.yaw + std::remainder(yaw - current.yaw, 2 * kPi);
        target_cam.yaw = yaw;
        target_cam.pitch = pitch;
    }

    // Apply an input mutation. While a glide is active, changes the target so smoothing blends into it.
    // When idle, mutates cam directly and keeps target in sync.
    template <typename Fn>
    void apply_input(Camera& cam, Fn&& fn) {
        if (active) {
            fixed_eye = false;  // input moved the eye: blend the parts from here
            fn(target_cam);
        } else {
            fn(cam);
            target_cam = cam;
        }
    }

    // Step current camera toward target using frame-rate independent exponential smoothing.
    void update(Camera& cam, double dt, bool reduce_motion = false) {
        if (!active) return;
        if (reduce_motion || dt >= 1.0) {
            cam = target_cam;
            active = false;
            return;
        }
        if (dt <= 0) return;

        const double s = std::clamp(1.0 - std::pow(2.0, -dt / half_life), 0.0, 1.0);
        if (fixed_eye) {  // turn from the fixed eye toward a look-at point gliding to the new focus
            const Vec3 eye = target_cam.eye();
            const double yaw0 = cam.yaw;
            cam.look(eye, cam.target + (target_cam.target - cam.target) * s);
            cam.yaw = yaw0 + std::remainder(cam.yaw - yaw0, 2 * kPi);
            cam.fov = target_cam.fov, cam.ortho = target_cam.ortho, cam.min_eye_z = target_cam.min_eye_z;
            if ((cam.target - target_cam.target).length() < 1e-4) cam = target_cam, active = false, fixed_eye = false;
            return;
        }
        cam.target += (target_cam.target - cam.target) * s;
        cam.distance += (target_cam.distance - cam.distance) * s;
        const double dyaw = std::remainder(target_cam.yaw - cam.yaw, 2 * kPi);
        cam.yaw += dyaw * s;
        cam.pitch += (target_cam.pitch - cam.pitch) * s;
        cam.fov = target_cam.fov;
        cam.ortho = target_cam.ortho;
        cam.min_eye_z = target_cam.min_eye_z;

        if ((cam.target - target_cam.target).length() < 1e-4 &&
            std::abs(dyaw) < 1e-4 &&
            std::abs(cam.pitch - target_cam.pitch) < 1e-4 &&
            std::abs(cam.distance - target_cam.distance) < 1e-4) {
            cam = target_cam;
            active = false;
        }
    }

    void snap(Camera& cam) {
        if (active) {
            cam = target_cam;
            active = false;
        }
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
