// Viewport Avatar Toolset - scalar animation curves.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/fcurve.h"

#include <algorithm>
#include <cmath>

namespace vats {
namespace {

double bezier(double p0, double p1, double p2, double p3, double t) {
    double u = 1 - t;
    return u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
}

// One side's automatic handle point for key k; side = -1 (left) or +1 (right).
void auto_handle(const std::vector<Key>& keys, size_t k, int side, Handle type, double& hx, double& hy) {
    const Key& K = keys[k];
    const Key* P = k > 0 ? &keys[k - 1] : nullptr;
    const Key* N = k + 1 < keys.size() ? &keys[k + 1] : nullptr;
    const Key* nb = side < 0 ? P : N;
    double len = nb ? std::fabs(nb->frame - K.frame) / 3.0 : 1.0;

    double m = 0;
    if (type == Handle::Vector) {
        if (nb) {
            hx = K.frame + (nb->frame - K.frame) / 3.0;
            hy = K.value + (nb->value - K.value) / 3.0;
            return;
        }
    } else if (type != Handle::Flat) {
        if (P && N) {
            bool extremum = (K.value >= P->value && K.value >= N->value) || (K.value <= P->value && K.value <= N->value);
            m = (type != Handle::Auto && extremum) ? 0 : (N->value - P->value) / (N->frame - P->frame);
        } else if ((P || N) && type == Handle::Auto) {
            const Key* o = P ? P : N;
            m = (o->value - K.value) / (o->frame - K.frame);
        }
    }
    hx = K.frame + side * len;
    hy = K.value + side * m * len;

    // Plateau: never overshoot the neighbour's value.
    if (type == Handle::Plateau && nb && hy != K.value) {
        double lo = std::min(K.value, nb->value), hi = std::max(K.value, nb->value);
        double cy = std::clamp(hy, lo, hi);
        if (cy != hy) {
            double s = (cy - K.value) / (hy - K.value);
            hx = K.frame + (hx - K.frame) * s;
            hy = K.value + (hy - K.value) * s;
        }
    }
}

}  // namespace

bool same_frame(double a, double b) { return std::fabs(a - b) < 1e-5 * std::max(1.0, std::fabs(a)); }

int FCurve::find(double frame) const {
    for (size_t i = 0; i < keys.size(); ++i)
        if (same_frame(keys[i].frame, frame)) return static_cast<int>(i);
    return -1;
}

double FCurve::evaluate(double f) const {
    if (keys.empty()) return 0;
    if (f <= keys.front().frame) return keys.front().value;
    if (f >= keys.back().frame) return keys.back().value;
    auto it = std::upper_bound(keys.begin(), keys.end(), f, [](double v, const Key& k) { return v < k.frame; });
    const Key& A = *(it - 1);
    const Key& B = *it;
    double span = B.frame - A.frame;
    if (A.interp == Interp::Constant || span <= 0) return A.value;
    double lin = (f - A.frame) / span;
    if (A.interp == Interp::Linear) return A.value + (B.value - A.value) * lin;

    double x1 = std::clamp(A.rx, A.frame, B.frame), x2 = std::clamp(B.lx, A.frame, B.frame);
    double lo = 0, hi = 1, t = lin;
    for (int i = 0; i < 30; ++i) {
        double x = bezier(A.frame, x1, x2, B.frame, t);
        if (std::fabs(x - f) < 1e-5) break;
        (x < f ? lo : hi) = t;
        t = (lo + hi) * 0.5;
    }
    return bezier(A.value, A.ry, B.ly, B.value, t);
}

int FCurve::set_key(double frame, double value, std::optional<Interp> interp) {
    int i = find(frame);
    if (i >= 0) {
        Key& k = keys[i];
        double d = value - k.value;
        k.value = value;
        k.ly += d;
        k.ry += d;
        if (interp) k.interp = *interp;
    } else {
        auto it = std::upper_bound(keys.begin(), keys.end(), frame, [](double v, const Key& k) { return v < k.frame; });
        Key k;
        k.frame = frame;
        k.value = value;
        k.interp = interp ? *interp : (it != keys.begin() ? (it - 1)->interp : Interp::Bezier);
        k.lx = frame - 1;
        k.ly = value;
        k.rx = frame + 1;
        k.ry = value;
        auto pos = keys.insert(it, k);  // insert first: it may reallocate, invalidating begin()
        i = static_cast<int>(pos - keys.begin());
    }
    recompute_handles();
    return i;
}

bool FCurve::remove_key(double frame) {
    int i = find(frame);
    if (i < 0) return false;
    keys.erase(keys.begin() + i);
    recompute_handles();
    return true;
}

void FCurve::apply_tangent(int index, Tangent t) {
    Key& k = keys[index];
    auto both = [&](Handle h) {
        k.interp = Interp::Bezier;
        k.left = k.right = h;
    };
    switch (t) {
        case Tangent::Auto: both(Handle::AutoClamped); break;
        case Tangent::Spline: both(Handle::Auto); break;
        case Tangent::Plateau: both(Handle::Plateau); break;
        case Tangent::Linear: both(Handle::Vector); break;
        case Tangent::Flat: both(Handle::Flat); break;
        case Tangent::Stepped: k.interp = Interp::Constant; break;
        case Tangent::Break: k.left = k.right = Handle::Free; break;
        case Tangent::Unify: {
            k.left = k.right = Handle::Aligned;
            double ux = k.rx - k.lx, uy = k.ry - k.ly;
            double ul = std::hypot(ux, uy);
            if (ux <= 0 || ul <= 0) {
                ux = 1;
                uy = 0;
            } else {
                ux /= ul;
                uy /= ul;
            }
            double rl = std::hypot(k.rx - k.frame, k.ry - k.value), ll = std::hypot(k.lx - k.frame, k.ly - k.value);
            k.rx = k.frame + ux * rl;
            k.ry = k.value + uy * rl;
            k.lx = k.frame - ux * ll;
            k.ly = k.value - uy * ll;
            break;
        }
    }
    recompute_handles();
}

void FCurve::recompute_handles() {
    for (size_t i = 0; i < keys.size(); ++i) {
        Key& k = keys[i];
        if (k.left != Handle::Aligned && k.left != Handle::Free) auto_handle(keys, i, -1, k.left, k.lx, k.ly);
        if (k.right != Handle::Aligned && k.right != Handle::Free) auto_handle(keys, i, +1, k.right, k.rx, k.ry);
    }
}

}  // namespace vats
