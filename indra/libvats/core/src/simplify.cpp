// Viewport Avatar Toolset - Simplify Curves (spec 08 SC).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/simplify.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "vats/curve_ops.h"

namespace vats {
namespace {

constexpr double kGimbalMargin = 5;  // degrees from +-90 on rot_y where rotations are fitted as rotations
constexpr double kSteep = 1.2;       // an inflection key needs a slope this many times its leg's mean

bool near_gimbal(const FCurve& y, int from, int to) {
    for (int f = from; f <= to; ++f)
        if (std::fabs(std::fmod(std::fabs(y.evaluate(f)), 180.0) - 90) < kGimbalMargin) return true;
    return false;
}

// Turning points that reverse by more than tol (a zigzag with threshold tol), so noise smaller than the
// tolerance makes no extrema. The ends are not included.
std::vector<int> extrema(const std::vector<double>& s, double tol) {
    std::vector<int> out;
    int dir = 0, lo = 0, hi = 0, ext = 0;
    for (int i = 1; i < int(s.size()); ++i) {
        if (dir == 0) {
            if (s[i] > s[hi]) hi = i;
            if (s[i] < s[lo]) lo = i;
            if (s[hi] - s[lo] <= tol) continue;
            dir = hi > lo ? 1 : -1;
            if (const int first = dir > 0 ? lo : hi; first > 0) out.push_back(first);
            ext = dir > 0 ? hi : lo;
        } else if (dir * (s[i] - s[ext]) >= 0) {
            ext = i;
        } else if (dir * (s[ext] - s[i]) > tol) {
            out.push_back(ext);
            dir = -dir, ext = i;
        }
    }
    return out;
}

// Keys on every frame the channel needs on its own: the range's ends, the turns (reversals by more than tol) and the
// steepest frame of each leg that bends (an inflection). d gets the slope at each frame (0 at a turn).
void initial_keys(const std::vector<double>& s, double tol, std::vector<char>& is_key, std::vector<double>& d) {
    const int n = int(s.size());
    for (int i = 0; i < n; ++i) d[i] = (s[std::min(i + 1, n - 1)] - s[std::max(i - 1, 0)]) / std::max(1, std::min(i + 1, n - 1) - std::max(i - 1, 0));
    is_key[0] = is_key[n - 1] = 1;
    std::vector<int> turns = extrema(s, tol);
    for (int i : turns) is_key[i] = 1, d[i] = 0;
    turns.insert(turns.begin(), 0), turns.push_back(n - 1);
    for (size_t t = 0; t + 1 < turns.size(); ++t) {
        const int a = turns[t], b = turns[t + 1];
        int best = -1;
        double m = 0;
        for (int i = a + 1; i < b; ++i)
            if (std::fabs(d[i]) > m) m = std::fabs(d[i]), best = i;
        if (best > a + 1 && best < b - 1 && m > kSteep * std::fabs(s[b] - s[a]) / (b - a)) is_key[best] = 1;
    }
}

// The range of one or more curves, refitted with keys on the same frames. miss(i, values) is how far the fit's
// values at frame from + i (one per curve) are from the curves as they were: the channel's own difference for one
// curve, the turn between the two rotations for a rotation's three. Returns each curve's keys in the range, or no
// lists when the fit has no fewer keys than the `had` it replaces.
std::vector<std::vector<Key>> fit(const std::vector<const FCurve*>& cs, int from, int to, double tol, const std::vector<int>& keep,
                                  int had, const std::function<double(int, const std::vector<double>&)>& miss) {
    const int n = to - from + 1, m = int(cs.size());
    std::vector<std::vector<double>> s(m, std::vector<double>(n)), d(m, std::vector<double>(n));
    std::vector<char> is_key(n, 0), is_free(n, 0);
    for (int c = 0; c < m; ++c) {
        for (int i = 0; i < n; ++i) s[c][i] = cs[c]->evaluate(from + i);
        initial_keys(s[c], tol, is_key, d[c]);
    }
    for (int f : keep)
        if (f >= from && f <= to) is_key[f - from] = 1;

    // Schneider-style: fit, find the worst frame of each segment that misses, then either free the segment's
    // handles (on the curve's own slope) or split it there. Each pass only adds keys or frees handles, so it ends.
    // ponytail: with Loop-Aware Tangents the app re-slopes the keys at the loop points afterwards; the fit does
    // not see that (a small change next to the seam).
    std::vector<FCurve> out(m);
    std::vector<size_t> pre(m), post(m);
    std::vector<double> at(m);
    for (;;) {
        for (int c = 0; c < m; ++c) {
            // The keys either side of the range, which the range's keys are fitted between.
            std::vector<Key> before, after;
            for (const Key& k : cs[c]->keys) (k.frame < from - 1e-6 ? before : after).push_back(k);
            std::erase_if(after, [&](const Key& k) { return k.frame <= to + 1e-6; });
            pre[c] = before.size(), post[c] = after.size();
            out[c].keys = before;
            for (int i = 0; i < n; ++i) {
                if (!is_key[i]) continue;
                Key k;
                k.frame = from + i;
                k.value = s[c][i];
                out[c].keys.push_back(k);
            }
            out[c].keys.insert(out[c].keys.end(), after.begin(), after.end());
            for (size_t j = pre[c]; j + post[c] < out[c].keys.size(); ++j) {
                Key& k = out[c].keys[j];
                const int i = int(std::lround(k.frame)) - from;
                if (!is_free[i]) continue;
                const double ls = j > 0 ? k.frame - out[c].keys[j - 1].frame : 1;
                const double rs = j + 1 < out[c].keys.size() ? out[c].keys[j + 1].frame - k.frame : 1;
                k.left = k.right = Handle::Free;
                k.lx = k.frame - ls / 3, k.ly = k.value - d[c][i] * ls / 3;
                k.rx = k.frame + rs / 3, k.ry = k.value + d[c][i] * rs / 3;
            }
            out[c].recompute_handles();
        }
        bool missed = false;
        std::vector<int> split;
        for (int a = 0, b = 1; b < n; ++b) {
            if (!is_key[b]) continue;
            int worst = -1;
            double err = tol;
            for (int i = a + 1; i < b; ++i) {
                for (int c = 0; c < m; ++c) at[c] = out[c].evaluate(from + i);
                if (const double e = miss(i, at); e > err) err = e, worst = i;
            }
            if (worst >= 0) {
                missed = true;
                if (!is_free[a] || !is_free[b]) is_free[a] = is_free[b] = 1;
                else split.push_back(worst);
            }
            a = b;
        }
        if (!missed) break;
        for (int i : split) is_key[i] = 1;
    }
    std::vector<std::vector<Key>> ranges(m);
    int after = 0;
    for (int c = 0; c < m; ++c) {
        for (const Key& k : out[c].keys)
            if (k.frame >= from - 1e-6 && k.frame <= to + 1e-6) ranges[c].push_back(k);
        after += int(ranges[c].size());
    }
    if (after >= had) ranges.clear();
    return ranges;
}

}  // namespace

SimplifyResult simplify_curves(Clip& clip, const std::vector<std::string>& tracks, const SimplifyOptions& opt) {
    SimplifyResult r;
    const int from = std::max(opt.from, 0), to = opt.to < 0 ? clip.end_frame : opt.to;
    if (to - from < 2) return r;
    for (auto& [name, track] : clip.curves) {
        if (!tracks.empty() && std::find(tracks.begin(), tracks.end(), name) == tracks.end()) continue;
        // Near gimbal lock (rot_y within 5 degrees of +-90, a knee bent past a right angle) X and Z swing wildly
        // while the bone turns smoothly: the three rotation channels are fitted together, on the same frames, to
        // the turn between the rotations rather than to each angle.
        const auto y = track.find("rot_y");
        const bool gimbal = y != track.end() && !y->second.empty() && near_gimbal(y->second, from, to);
        // A curve's keys in the range, or -1 when it has stepped keys there (holding into it counts).
        auto count = [&](const FCurve& c) {
            int had = 0;
            for (size_t k = 0; k < c.keys.size(); ++k) {
                const double f = c.keys[k].frame;
                const bool in = f >= from - 1e-6 && f <= to + 1e-6;
                const bool into = k + 1 < c.keys.size() && f < from && c.keys[k + 1].frame > from;
                if ((in || into) && c.keys[k].interp == Interp::Constant) return -1;
                had += in;
            }
            return had;
        };
        // Keys just outside the range hold the curve there (as Filter Curves does).
        auto held = [&](const FCurve& c) {
            FCurve work = c;
            bool earlier = false, later = false;
            for (const Key& k : c.keys) earlier |= k.frame < from - 1e-6, later |= k.frame > to + 1e-6;
            if (earlier && from > 0) insert_on_curve(work, from - 1);
            if (later) insert_on_curve(work, to + 1);
            return work;
        };
        auto put = [&](FCurve& work, const std::vector<Key>& range) {
            std::erase_if(work.keys, [&](const Key& k) { return k.frame >= from - 1e-6 && k.frame <= to + 1e-6; });
            work.keys.insert(std::upper_bound(work.keys.begin(), work.keys.end(), double(from),
                                              [](double v, const Key& k) { return v < k.frame; }),
                             range.begin(), range.end());
            work.recompute_handles();
        };
        if (gimbal) {
            std::vector<FCurve*> rot;
            std::vector<int> axis;
            int had = 0;
            bool stepped = false;
            for (int a = 0; a < 3; ++a)
                if (auto it = track.find(kRotChannels[a]); it != track.end() && !it->second.empty()) {
                    const int k = count(it->second);
                    stepped |= k < 0, had += k, rot.push_back(&it->second), axis.push_back(a);
                }
            if (stepped) {
                r.skipped.push_back(name + ": stepped keys, rotation left as it is");
            } else {
                std::vector<FCurve> work;
                for (FCurve* c : rot) work.push_back(held(*c));
                std::vector<const FCurve*> in;
                for (const FCurve& w : work) in.push_back(&w);
                auto rotation = [&](const std::vector<double>& v) {  // channels not keyed read 0
                    Vec3 e;
                    for (size_t c = 0; c < rot.size(); ++c) e[axis[c]] = v[c];
                    return euler_to_quat(e);
                };
                std::vector<Quat> was;
                std::vector<double> v(rot.size());
                for (int f = from; f <= to; ++f) {
                    for (size_t c = 0; c < rot.size(); ++c) v[c] = work[c].evaluate(f);
                    was.push_back(rotation(v));
                }
                auto turn = [&](int i, const std::vector<double>& now) {
                    return 2 * std::acos(std::min(1.0, std::fabs(was[i].dot(rotation(now))))) * kRadToDeg;
                };
                const auto ranges = fit(in, from, to, opt.tol_deg, opt.keep, had, turn);
                r.before += had;
                r.after += ranges.empty() ? had : int(ranges.size() * ranges[0].size());
                if (!ranges.empty())
                    for (size_t c = 0; c < rot.size(); ++c) put(work[c], ranges[c]), *rot[c] = std::move(work[c]);
            }
        }
        for (auto& [ch, c] : track) {
            const bool rot = ch.rfind("rot_", 0) == 0, pos = ch.rfind("pos_", 0) == 0;
            if ((!rot && !pos) || (rot && gimbal) || c.empty()) continue;
            const int had = count(c);
            if (had < 0) {
                r.skipped.push_back(name + " " + ch + ": stepped keys, left as they are");
                continue;
            }
            FCurve work = held(c);
            const double tol = rot ? opt.tol_deg : opt.tol_mm / 1000;
            std::vector<double> was;
            for (int f = from; f <= to; ++f) was.push_back(work.evaluate(f));
            const auto ranges = fit({&work}, from, to, tol, opt.keep, had,
                                    [&](int i, const std::vector<double>& now) { return std::fabs(now[0] - was[i]); });
            r.before += had;
            if (ranges.empty()) {
                r.after += had;
                continue;
            }
            r.after += int(ranges[0].size());
            put(work, ranges[0]);
            c = std::move(work);
        }
    }
    return r;
}

}  // namespace vats
