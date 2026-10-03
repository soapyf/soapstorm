// Viewport Avatar Toolset - a 3x4 affine matrix for the file readers and writers (core-private).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Row-major, column vectors (p' = M p), the way COLLADA writes them. dae.cpp keeps an older private copy.
#pragma once

#include <cmath>

#include "vats/math.h"

namespace vats {

struct Affine {
    double m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

    Affine operator*(const Affine& o) const {
        Affine r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j] + (j == 3 ? m[i][3] : 0);
        return r;
    }
    Vec3 dir(const Vec3& v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    Vec3 origin() const { return {m[0][3], m[1][3], m[2][3]}; }
    Vec3 point(const Vec3& v) const { return dir(v) + origin(); }
    double det() const {
        const auto& a = m;
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    }
    // General inverse (a scale is not a rotation); a singular matrix gives the identity.
    Affine inverse() const {
        const auto& a = m;
        const double c[3][3] = {
            {a[1][1] * a[2][2] - a[1][2] * a[2][1], a[0][2] * a[2][1] - a[0][1] * a[2][2], a[0][1] * a[1][2] - a[0][2] * a[1][1]},
            {a[1][2] * a[2][0] - a[1][0] * a[2][2], a[0][0] * a[2][2] - a[0][2] * a[2][0], a[0][2] * a[1][0] - a[0][0] * a[1][2]},
            {a[1][0] * a[2][1] - a[1][1] * a[2][0], a[0][1] * a[2][0] - a[0][0] * a[2][1], a[0][0] * a[1][1] - a[0][1] * a[1][0]}};
        const double d = a[0][0] * c[0][0] + a[0][1] * c[1][0] + a[0][2] * c[2][0];
        if (!(std::fabs(d) > 1e-300)) return {};
        Affine r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = c[i][j] / d;
        for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * a[0][3] + r.m[i][1] * a[1][3] + r.m[i][2] * a[2][3]);
        return r;
    }
    // For normals: the inverse transpose of the 3x3 part.
    Affine normal_matrix() const {
        const Affine inv = inverse();
        Affine r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = inv.m[j][i];
        return r;
    }
    // The rotation of the 3x3 part after Gram-Schmidt on its columns (a mirror becomes a proper rotation).
    Quat rotation() const {
        Vec3 x = Vec3{m[0][0], m[1][0], m[2][0]}.normalized();
        const Vec3 c1{m[0][1], m[1][1], m[2][1]};
        Vec3 y = (c1 - x * x.dot(c1)).normalized();
        if (x.length() == 0 || y.length() == 0) return {};
        const Vec3 z = x.cross(y);
        const double r[3][3] = {{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}};
        Quat q;
        const double tr = r[0][0] + r[1][1] + r[2][2];
        if (tr > 0) {
            const double s = std::sqrt(tr + 1) * 2;
            q = {s / 4, (r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s};
        } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
            const double s = std::sqrt(1 + r[0][0] - r[1][1] - r[2][2]) * 2;
            q = {(r[2][1] - r[1][2]) / s, s / 4, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s};
        } else if (r[1][1] > r[2][2]) {
            const double s = std::sqrt(1 + r[1][1] - r[0][0] - r[2][2]) * 2;
            q = {(r[0][2] - r[2][0]) / s, (r[0][1] + r[1][0]) / s, s / 4, (r[1][2] + r[2][1]) / s};
        } else {
            const double s = std::sqrt(1 + r[2][2] - r[0][0] - r[1][1]) * 2;
            q = {(r[1][0] - r[0][1]) / s, (r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, s / 4};
        }
        return q.normalized();
    }

    static Affine from_xform(const Xform& x) {
        const Vec3 c[3] = {x.rot.rotate({1, 0, 0}), x.rot.rotate({0, 1, 0}), x.rot.rotate({0, 0, 1})};
        Affine r;
        for (int i = 0; i < 3; ++i) r.m[i][0] = c[0][i], r.m[i][1] = c[1][i], r.m[i][2] = c[2][i], r.m[i][3] = x.pos[i];
        return r;
    }
    static Affine scaling(const Vec3& s) {
        Affine r;
        r.m[0][0] = s.x, r.m[1][1] = s.y, r.m[2][2] = s.z;
        return r;
    }
    // From 16 column-major values (glTF's layout): element (row r, column c) is f[c * 4 + r].
    static Affine from_columns(const float* f) {
        Affine r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) r.m[i][j] = f[j * 4 + i];
        return r;
    }
    // Y up to SL's Z up: (x, y, z) -> (x, -z, y).
    static Affine y_up() {
        Affine r;
        r.m[1][1] = 0, r.m[1][2] = -1, r.m[2][1] = 1, r.m[2][2] = 0;
        return r;
    }
};

}  // namespace vats
