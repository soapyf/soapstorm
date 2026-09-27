// Viewport Avatar Toolset - loop tools.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/loop_tools.h"

#include <algorithm>
#include <cmath>

namespace vats {

namespace {

bool is_rotation(const std::string& ch) { return ch.rfind("rot", 0) == 0; }
bool is_position(const std::string& ch) { return ch.rfind("pos", 0) == 0; }

// The value the loop should end on: the start value, or a whole number of turns away for a rotation.
double seam_target(const std::string& ch, double v_in, double v_out) {
    return is_rotation(ch) ? v_in + 360.0 * std::round((v_out - v_in) / 360.0) : v_in;
}

void ensure_key(FCurve& c, double f) {
    if (c.find(f) < 0) c.set_key(f, c.evaluate(f));
}

// Moves a key and both handles by dv (so its shape is kept).
void shift_value(Key& k, double dv) { k.value += dv, k.ly += dv, k.ry += dv; }

void shift_frame(Key& k, double df) { k.frame += df, k.lx += df, k.rx += df; }

// Adds the straight line (f - a) * per_frame to a curve: exact for every interpolation, because Bezier
// curves stay Bezier under an added linear function of time.
void add_line(FCurve& c, double a, double per_frame) {
    for (Key& k : c.keys) {
        k.value += (k.frame - a) * per_frame;
        k.ly += (k.lx - a) * per_frame;
        k.ry += (k.rx - a) * per_frame;
    }
}

}  // namespace

double Travel::speed() const { return std::hypot(vx, vy); }

LoopRange loop_range(const Clip& clip) {
    if (clip.loop && clip.loop_out > clip.loop_in) return {clip.loop_in, clip.loop_out};
    return {0, std::max(clip.end_frame, 1)};
}

int make_loop_seamless(Clip& clip, int blend_frames) {
    const LoopRange r = loop_range(clip);
    const double a = r.in, b = r.out;
    const double w0 = b - std::clamp(blend_frames, 0, r.out - r.in);
    int changed = 0;
    for (auto& [name, track] : clip.curves)
        for (auto& [ch, c] : track) {
            if (c.keys.size() < 2) continue;
            const double va = c.evaluate(a), vb = c.evaluate(b);
            const double d = seam_target(ch, va, vb) - vb;
            ensure_key(c, a);
            ensure_key(c, b);
            if (std::fabs(d) > 1e-9) {
                if (w0 < b && w0 > a) {
                    ensure_key(c, w0);
                    for (Key& k : c.keys)  // ease the correction in over the blend frames
                        if (k.frame > w0 && k.frame <= b + 1e-9) {
                            const double t = std::min((k.frame - w0) / (b - w0), 1.0);
                            shift_value(k, d * t * t * (3 - 2 * t));
                        }
                } else {
                    shift_value(c.keys[c.find(b)], d);
                }
            }
            c.recompute_handles();
            // Match the slopes: the end leaves the way the start arrives, so the seam has no kink.
            Key& ka = c.keys[c.find(a)];
            Key& kb = c.keys[c.find(b)];
            bool kinked = false;
            if (ka.interp == Interp::Bezier) {
                const double sa = ka.rx > ka.frame ? (ka.ry - ka.value) / (ka.rx - ka.frame) : 0;
                const double sb = kb.frame > kb.lx ? (kb.value - kb.ly) / (kb.frame - kb.lx) : 0;
                const double s = (sa + sb) / 2;
                kinked = std::fabs(sa - sb) > 1e-6;
                for (Key* k : {&ka, &kb}) {
                    k->left = k->right = Handle::Aligned;
                    k->ly = k->value - s * (k->frame - k->lx);
                    k->ry = k->value + s * (k->rx - k->frame);
                }
            }
            if (std::fabs(d) > 1e-9 || kinked) ++changed;
        }
    return changed;
}

std::vector<SeamJump> loop_seam_jumps(const Clip& clip, double tol_deg, double tol_m) {
    const LoopRange r = loop_range(clip);
    std::vector<SeamJump> out;
    for (const auto& [name, track] : clip.curves)
        for (const auto& [ch, c] : track) {
            if (c.keys.size() < 2) continue;
            const double va = c.evaluate(r.in), vb = c.evaluate(r.out);
            const double jump = vb - seam_target(ch, va, vb);
            const double tol = is_rotation(ch) ? tol_deg : is_position(ch) ? tol_m : 0.01;
            if (std::fabs(jump) > tol) out.push_back({name, ch, jump});
        }
    return out;
}

Travel remove_travel(Clip& clip, const std::string& hip) {
    const LoopRange r = loop_range(clip);
    const double frames = r.out - r.in, seconds = frames / std::max(clip.fps, 1);
    Travel t;
    auto it = clip.curves.find(hip);
    if (it == clip.curves.end() || frames <= 0) return t;
    double* v[2] = {&t.vx, &t.vy};
    const char* ch[2] = {"pos_x", "pos_y"};
    for (int i = 0; i < 2; ++i) {
        auto c = it->second.find(ch[i]);
        if (c == it->second.end() || c->second.empty()) continue;
        const double drift = c->second.evaluate(r.out) - c->second.evaluate(r.in);
        *v[i] = drift / seconds;
        add_line(c->second, r.in, -drift / frames);
    }
    return t;
}

void add_travel(Clip& clip, const Travel& t, const std::string& hip) {
    const LoopRange r = loop_range(clip);
    const double per_second[2] = {t.vx, t.vy};
    const char* ch[2] = {"pos_x", "pos_y"};
    for (int i = 0; i < 2; ++i) {
        if (per_second[i] == 0) continue;
        FCurve& c = clip.curves[hip][ch[i]];
        if (c.empty()) c.set_key(r.in, 0), c.set_key(r.out, 0);
        add_line(c, r.in, per_second[i] / std::max(clip.fps, 1));
    }
}

bool cycle_offset(Clip& clip, int start) {
    const LoopRange r = loop_range(clip);
    if (start <= r.in || start >= r.out) return false;
    const double a = r.in, b = r.out, f = start;
    for (auto& [name, track] : clip.curves)
        for (auto& [ch, c] : track) {
            if (c.keys.size() < 2) continue;
            ensure_key(c, a);
            ensure_key(c, f);
            ensure_key(c, b);
            // Keys in [f, b) move to the front, keys in [a, f) to the back; the old end key (a copy of the
            // start on a seamless loop) is replaced by a copy of the new start.
            std::vector<Key> outside, moved;
            Key first;
            for (const Key& k : c.keys) {
                if (k.frame < a - 1e-9 || k.frame > b + 1e-9) {
                    outside.push_back(k);
                    continue;
                }
                if (same_frame(k.frame, b)) continue;
                Key m = k;
                shift_frame(m, k.frame >= f - 1e-9 ? -(f - a) : (b - f));
                if (same_frame(k.frame, f)) first = m;
                moved.push_back(m);
            }
            Key end = first;
            shift_frame(end, b - a);
            moved.push_back(end);
            c.keys = std::move(outside);
            c.keys.insert(c.keys.end(), moved.begin(), moved.end());
            std::sort(c.keys.begin(), c.keys.end(), [](const Key& x, const Key& y) { return x.frame < y.frame; });
            c.recompute_handles();
        }
    return true;
}

}  // namespace vats
