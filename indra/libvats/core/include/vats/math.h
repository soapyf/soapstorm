// Viewport Avatar Toolset - core math.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// SL space: right-handed, +X forward, +Y left, +Z up, metres.
// Quaternions are Hamilton: a * b applies b first, then a.
#pragma once

#include <algorithm>
#include <cmath>

namespace vats {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

struct Vec3 {
    double x = 0, y = 0, z = 0;

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
    double& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }

    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double length() const { return std::sqrt(dot(*this)); }
    Vec3 mul(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 normalized() const { double l = length(); return l > 0 ? *this * (1.0 / l) : Vec3{}; }
    bool operator==(const Vec3&) const = default;
};

struct Quat {
    double w = 1, x = 0, y = 0, z = 0;

    static Quat axis_angle(const Vec3& axis, double radians) {
        Vec3 a = axis.normalized();
        double s = std::sin(radians * 0.5);
        return {std::cos(radians * 0.5), a.x * s, a.y * s, a.z * s};
    }

    Quat operator*(const Quat& o) const {
        return {w * o.w - x * o.x - y * o.y - z * o.z,
                w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w};
    }
    Quat conj() const { return {w, -x, -y, -z}; }
    Quat operator-() const { return {-w, -x, -y, -z}; }
    bool operator==(const Quat&) const = default;
    double dot(const Quat& o) const { return w * o.w + x * o.x + y * o.y + z * o.z; }
    Quat normalized() const {
        double l = std::sqrt(dot(*this));
        return l > 0 ? Quat{w / l, x / l, y / l, z / l} : Quat{};
    }
    Vec3 rotate(const Vec3& v) const {
        Vec3 u{x, y, z};
        Vec3 t = u.cross(v) * 2.0;
        return v + t * w + u.cross(t);
    }
    // Angle of this rotation in radians, in [0, pi].
    double angle() const { return 2.0 * std::acos(std::min(1.0, std::fabs(w) / std::sqrt(dot(*this)))); }
};

// Normalised lerp along the shorter arc; this is how SL plays .anim rotation keys.
inline Quat nlerp(const Quat& a, const Quat& b, double t) {
    Quat bb = a.dot(b) < 0 ? -b : b;
    return Quat{a.w + (bb.w - a.w) * t, a.x + (bb.x - a.x) * t, a.y + (bb.y - a.y) * t,
                a.z + (bb.z - a.z) * t}.normalized();
}

// Rigid transform: rotate, then translate.
struct Xform {
    Quat rot;
    Vec3 pos;

    Vec3 apply(const Vec3& v) const { return rot.rotate(v) + pos; }
    Xform operator*(const Xform& o) const { return {rot * o.rot, apply(o.pos)}; }
    Xform inverse() const { Quat r = rot.conj(); return {r, -r.rotate(pos)}; }
    bool operator==(const Xform&) const = default;
};

// Euler triple in degrees, R = Rz(z) * Ry(y) * Rx(x): X first, then Y, then Z, about parent axes.
inline Quat euler_to_quat(const Vec3& deg) {
    return (Quat::axis_angle({0, 0, 1}, deg.z * kDegToRad) * Quat::axis_angle({0, 1, 0}, deg.y * kDegToRad) *
            Quat::axis_angle({1, 0, 0}, deg.x * kDegToRad))
        .normalized();
}

inline Vec3 quat_to_euler(const Quat& qin) {
    Quat q = qin.normalized();
    // Rotation matrix entries we need (row, column).
    double r00 = 1 - 2 * (q.y * q.y + q.z * q.z);
    double r01 = 2 * (q.x * q.y - q.w * q.z);
    double r10 = 2 * (q.x * q.y + q.w * q.z);
    double r11 = 1 - 2 * (q.x * q.x + q.z * q.z);
    double r20 = 2 * (q.x * q.z - q.w * q.y);
    double r21 = 2 * (q.y * q.z + q.w * q.x);
    double r22 = 1 - 2 * (q.x * q.x + q.y * q.y);
    if (std::fabs(r20) < 0.999999) {
        return {std::atan2(r21, r22) * kRadToDeg, std::asin(-r20) * kRadToDeg, std::atan2(r10, r00) * kRadToDeg};
    }
    // Gimbal lock: all of X folds into Z.
    return {0, (r20 > 0 ? -90.0 : 90.0), std::atan2(-r01, r11) * kRadToDeg};
}

// Wrap v into (ref - 180, ref + 180].
inline double wrap_near(double v, double ref) {
    double d = std::fmod(v - ref, 360.0);
    if (d > 180) d -= 360;
    if (d <= -180) d += 360;
    return ref + d;
}

// The Euler triple for q that is closest to ref, so curves stay continuous.
inline Vec3 nearest_euler(const Quat& q, const Vec3& ref) {
    Vec3 a = quat_to_euler(q);
    Vec3 b{a.x + 180, 180 - a.y, a.z + 180};
    for (int i = 0; i < 3; ++i) {
        a[i] = wrap_near(a[i], ref[i]);
        b[i] = wrap_near(b[i], ref[i]);
    }
    Vec3 da = a - ref, db = b - ref;
    return da.dot(da) <= db.dot(db) ? a : b;
}

}  // namespace vats
