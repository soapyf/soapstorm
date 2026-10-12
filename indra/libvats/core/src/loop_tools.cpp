// Viewport Avatar Toolset - loop tools.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/loop_tools.h"

#include "vats/curve_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vats {

namespace {

bool is_rotation(const std::string& ch) { return ch.rfind("rot", 0) == 0; }
bool is_position(const std::string& ch) { return ch.rfind("pos", 0) == 0; }

// The value the loop should end on: the start value, or a whole number of turns away for a rotation.
double seam_target(const std::string& ch, double v_in, double v_out) {
    return is_rotation(ch) ? v_in + 360.0 * std::round((v_out - v_in) / 360.0) : v_in;
}

// A key at f that leaves the curve's shape as it was (a plain set_key would recompute its neighbours' handles).
void ensure_key(FCurve& c, double f) { insert_on_curve(c, f); }

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

// IK targets and poles are placed in avatar space, as the hips are: a planted foot has to move with the hips
// when their travel changes, or the leg stretches for a target it can no longer reach.
// IK controllers placed in avatar space, as the hips are: every limb's but the fingers', which live in the
// wrist's space (rig.cpp controller_space).
bool avatar_space_ik(const std::string& track) {
    if (track.rfind("ik.", 0) != 0) return false;
    for (const char* f : {"Thumb", "Index", "Middle", "Ring", "Pinky"})
        if (track.compare(3, std::strlen(f), f) == 0) return false;
    return true;
}

void add_line_to_ik(Clip& clip, const char* axis, double a, double per_frame) {
    const std::string pos = std::string("pos_") + axis, pole = std::string("pole_") + axis;
    for (auto& [name, track] : clip.curves) {
        if (!avatar_space_ik(name)) continue;
        for (const std::string& ch : {pos, pole})
            if (auto c = track.find(ch); c != track.end()) add_line(c->second, a, per_frame);
    }
}

}  // namespace

double Travel::speed() const { return std::hypot(vx, vy); }

LoopRange loop_range(const Clip& clip) {
    if (clip.loop && clip.loop_out > clip.loop_in) return {clip.loop_in, clip.loop_out};
    return {0, std::max(clip.end_frame, 1)};
}

namespace {

// Whole-loop blend: g over a..b with g(a) = 0, g(b) = d and slopes m0 at a, m1 at b (units per frame), a cubic
// Hermite. Added to a channel it closes the seam by d and changes its slopes at the ends by m0 and m1, spread so
// thinly over the cycle that no frame shows it. It is 0 before a and d after b.
struct SeamCorrection {
    double a, b, d, m0, m1;
    double operator()(double f) const {
        const double L = b - a;
        if (f <= a) return 0;
        if (f >= b) return d;
        const double t = (f - a) / L, t2 = t * t, t3 = t2 * t;
        return d * (-2 * t3 + 3 * t2) + L * (m0 * (t3 - 2 * t2 + t) + m1 * (t3 - t2));
    }
};

// The slope (units per frame) leaving key i and arriving at key i, read from the interpolation itself so a
// seamless curve measures as seamless again: a step has none, a line its own, a Bezier its handle.
double leave_slope(const FCurve& c, size_t i) {
    const Key& k = c.keys[i];
    if (k.interp == Interp::Constant || i + 1 >= c.keys.size()) return 0;
    if (k.interp == Interp::Linear) return (c.keys[i + 1].value - k.value) / (c.keys[i + 1].frame - k.frame);
    return k.rx > k.frame ? (k.ry - k.value) / (k.rx - k.frame) : 0;
}
double arrive_slope(const FCurve& c, size_t i) {
    if (i == 0) return 0;
    const Key &p = c.keys[i - 1], &k = c.keys[i];
    if (p.interp == Interp::Constant) return 0;
    if (p.interp == Interp::Linear) return (k.value - p.value) / (k.frame - p.frame);
    return k.frame > k.lx ? (k.value - k.ly) / (k.frame - k.lx) : 0;
}

}  // namespace

int make_loop_seamless(Clip& clip, int blend_frames) {
    const LoopRange r = loop_range(clip);
    const double a = r.in, b = r.out;
    const bool whole = blend_frames == kBlendWholeLoop;
    const double w0 = b - std::clamp(blend_frames, 0, r.out - r.in);
    int changed = 0;
    for (auto& [name, track] : clip.curves)
        for (auto& [ch, c] : track) {
            if (c.keys.size() < 2) continue;
            ensure_key(c, a);
            ensure_key(c, b);
            const double va = c.evaluate(a), vb = c.evaluate(b);
            const double d = seam_target(ch, va, vb) - vb;
            bool moved = false;
            // IK targets hold still while a foot is planted; spread over the loop they would slide under it, so
            // they close the seam at the end key as the other modes do.
            if (whole && name.rfind("ik.", 0) != 0) {
                // Value and slope together: the end arrives at the start's value, both ends at their mean slope.
                const double ds = arrive_slope(c, size_t(c.find(b))) - leave_slope(c, size_t(c.find(a)));
                const double tiny = is_position(ch) ? 1e-6 : 1e-4;  // metres or degrees per frame
                if (std::fabs(d) > 1e-9 || std::fabs(ds) > tiny) {
                    const SeamCorrection g{a, b, d, ds / 2, -ds / 2};
                    for (Key& k : c.keys)
                        if (k.frame >= a - 1e-9) k.value += g(k.frame), k.ly += g(std::max(k.lx, a)), k.ry += g(k.rx);
                    moved = true;
                }
            } else if (std::fabs(d) > 1e-9) {
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
                moved = true;
            }
            c.recompute_handles();
            // Match the slopes exactly where both ends are Bezier: the end leaves the way the start arrives (automatic
            // handles at a and b would otherwise be re-derived from the keys outside the loop and keep half a kink).
            const size_t ia = size_t(c.find(a)), ib = size_t(c.find(b));
            Key& ka = c.keys[ia];
            Key& kb = c.keys[ib];
            bool kinked = false;
            if (ka.interp == Interp::Bezier && ib > 0 && c.keys[ib - 1].interp == Interp::Bezier) {
                const double sa = leave_slope(c, ia), sb = arrive_slope(c, ib);
                const double s = (sa + sb) / 2;
                kinked = std::fabs(sa - sb) > 1e-6;
                for (Key* k : {&ka, &kb}) {
                    k->left = k->right = Handle::Aligned;
                    k->ly = k->value - s * (k->frame - k->lx);
                    k->ry = k->value + s * (k->rx - k->frame);
                }
            }
            if (moved || kinked) ++changed;
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
        add_line_to_ik(clip, i ? "y" : "x", r.in, -drift / frames);
    }
    return t;
}

double remove_path(Clip& clip, double window_s, const std::string& hip) {
    auto it = clip.curves.find(hip);
    const int n = clip.end_frame;
    if (it == clip.curves.end() || n < 2) return 0;
    const int half = std::max(1, int(std::lround(window_s * std::max(clip.fps, 1) / 2)));
    const char* ch[2] = {"pos_x", "pos_y"};
    std::vector<double> path[2];
    for (int i = 0; i < 2; ++i) {
        auto c = it->second.find(ch[i]);
        if (c == it->second.end() || c->second.empty()) continue;
        // The path: a centred box average of the position over the window (one walk cycle averages its own sway
        // away). Past the ends the take is continued by odd reflection (through its end value), so a take that
        // starts or ends mid-stride keeps moving there rather than being pulled back towards the middle.
        std::vector<double> x(size_t(n) + 1);
        for (int f = 0; f <= n; ++f) x[size_t(f)] = c->second.evaluate(f);
        auto at = [&](int f) {
            if (f < 0) return 2 * x[0] - x[size_t(std::min(-f, n))];
            if (f > n) return 2 * x[size_t(n)] - x[size_t(std::max(2 * n - f, 0))];
            return x[size_t(f)];
        };
        path[i].resize(size_t(n) + 1);
        for (int f = 0; f <= n; ++f) {
            double sum = 0;
            for (int k = -half; k <= half; ++k) sum += at(f + k);
            path[i][size_t(f)] = sum / (2 * half + 1);
        }
        auto offset = [&](double f) {  // between whole frames, held past the ends
            const double g = std::clamp(f, 0.0, double(n));
            const int f0 = std::min(int(g), n - 1);
            return path[i][size_t(f0)] + (path[i][size_t(f0) + 1] - path[i][size_t(f0)]) * (g - f0);
        };
        // Only the motion relative to the first frame comes out: a pose held off-centre stays where it is (Move
        // to Origin moves it). The path is not a straight line, so a sparsely keyed curve first gets a key every
        // frame (its shape kept), or between its keys it would follow the chord rather than the path.
        auto take_out = [&](FCurve& curve) {
            if (curve.empty()) return;
            for (int f = 0; f <= n; ++f) insert_on_curve(curve, f);
            for (Key& k : curve.keys) {
                k.value -= offset(k.frame) - path[i][0], k.ly -= offset(k.lx) - path[i][0];
                k.ry -= offset(k.rx) - path[i][0];
            }
        };
        take_out(c->second);
        const std::string axis = i ? "y" : "x";
        for (auto& [name, track] : clip.curves) {
            if (!avatar_space_ik(name)) continue;
            for (const std::string& n2 : {"pos_" + axis, "pole_" + axis})
                if (auto ik = track.find(n2); ik != track.end()) take_out(ik->second);
        }
    }
    double length = 0;  // along the path, for the speed reported
    for (int f = 1; f <= n; ++f) {
        const double dx = path[0].empty() ? 0 : path[0][size_t(f)] - path[0][size_t(f) - 1];
        const double dy = path[1].empty() ? 0 : path[1][size_t(f)] - path[1][size_t(f) - 1];
        length += std::hypot(dx, dy);
    }
    return length * std::max(clip.fps, 1) / n;
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
        add_line_to_ik(clip, i ? "y" : "x", r.in, per_second[i] / std::max(clip.fps, 1));
    }
}

Vec3 center_on_origin(Clip& clip, const std::string& hip) {
    const LoopRange r = loop_range(clip);
    Vec3 mean;
    auto it = clip.curves.find(hip);
    if (it == clip.curves.end()) return mean;
    double* m[2] = {&mean.x, &mean.y};
    const char* ch[2] = {"pos_x", "pos_y"};
    for (int i = 0; i < 2; ++i) {
        auto c = it->second.find(ch[i]);
        if (c == it->second.end() || c->second.empty()) continue;
        double sum = 0;
        for (int f = r.in; f <= r.out; ++f) sum += c->second.evaluate(f);
        *m[i] = sum / (r.out - r.in + 1);
        for (Key& k : c->second.keys) shift_value(k, -*m[i]);
        const std::string axis = i ? "y" : "x";
        for (auto& [name, track] : clip.curves) {
            if (!avatar_space_ik(name)) continue;
            for (const std::string& n : {"pos_" + axis, "pole_" + axis})
                if (auto t = track.find(n); t != track.end())
                    for (Key& k : t->second.keys) shift_value(k, -*m[i]);
        }
    }
    for (Pin& p : clip.pins)  // held in the world (avatar space), not to another bone
        if (p.target.empty() && p.target_actor.empty()) p.pos.x -= mean.x, p.pos.y -= mean.y;
    return mean;
}

bool cycle_offset(Clip& clip, int start) {
    const LoopRange r = loop_range(clip);
    if (start <= r.in || start >= r.out) return false;
    const double a = r.in, b = r.out, f = start;
    for_each_track_map(clip, [&](std::map<std::string, Track>& curves) {
        for (auto& [name, track] : curves)
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
    });
    return true;
}

}  // namespace vats
