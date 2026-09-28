// Viewport Avatar Toolset - Simplify Curves (spec 08 SC).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/simplify.h"

#include <algorithm>
#include <cmath>

#include "vats/curve_ops.h"

namespace vats {
namespace {

constexpr double kGimbalMargin = 5;  // degrees from +-90 on rot_y where rotations are left alone
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

// One curve's range, refitted. Returns the keys in the range, or an empty list when the fit is no smaller.
std::vector<Key> fit(const FCurve& c, int from, int to, double tol, const std::vector<int>& keep, int had) {
    const int n = to - from + 1;
    std::vector<double> s(n), d(n);
    for (int i = 0; i < n; ++i) s[i] = c.evaluate(from + i);
    for (int i = 0; i < n; ++i) d[i] = (s[std::min(i + 1, n - 1)] - s[std::max(i - 1, 0)]) / std::max(1, std::min(i + 1, n - 1) - std::max(i - 1, 0));

    std::vector<char> is_key(n, 0), is_free(n, 0);
    is_key[0] = is_key[n - 1] = 1;
    std::vector<int> turns = extrema(s, tol);
    for (int i : turns) is_key[i] = 1, d[i] = 0;
    // The steepest frame of each leg between turns, when the leg bends (an S rather than a line).
    turns.insert(turns.begin(), 0), turns.push_back(n - 1);
    for (size_t t = 0; t + 1 < turns.size(); ++t) {
        const int a = turns[t], b = turns[t + 1];
        int best = -1;
        double m = 0;
        for (int i = a + 1; i < b; ++i)
            if (std::fabs(d[i]) > m) m = std::fabs(d[i]), best = i;
        if (best > a + 1 && best < b - 1 && m > kSteep * std::fabs(s[b] - s[a]) / (b - a)) is_key[best] = 1;
    }
    for (int f : keep)
        if (f >= from && f <= to) is_key[f - from] = 1;

    // The keys either side of the range, which the range's keys are fitted between.
    std::vector<Key> pre, post;
    for (const Key& k : c.keys) (k.frame < from - 1e-6 ? pre : post).push_back(k);
    std::erase_if(post, [&](const Key& k) { return k.frame <= to + 1e-6; });

    // Schneider-style: fit, find the worst frame of each segment that misses, then either free the segment's
    // handles (on the curve's own slope) or split it there. Each pass only adds keys or frees handles, so it ends.
    // ponytail: with Loop-Aware Tangents the app re-slopes the keys at the loop points afterwards; the fit does
    // not see that (a small change next to the seam).
    FCurve out;
    for (;;) {
        out.keys = pre;
        for (int i = 0; i < n; ++i) {
            if (!is_key[i]) continue;
            Key k;
            k.frame = from + i;
            k.value = s[i];
            out.keys.push_back(k);
        }
        out.keys.insert(out.keys.end(), post.begin(), post.end());
        for (size_t j = pre.size(); j + post.size() < out.keys.size(); ++j) {
            Key& k = out.keys[j];
            const int i = int(std::lround(k.frame)) - from;
            if (!is_free[i]) continue;
            const double ls = j > 0 ? k.frame - out.keys[j - 1].frame : 1;
            const double rs = j + 1 < out.keys.size() ? out.keys[j + 1].frame - k.frame : 1;
            k.left = k.right = Handle::Free;
            k.lx = k.frame - ls / 3, k.ly = k.value - d[i] * ls / 3;
            k.rx = k.frame + rs / 3, k.ry = k.value + d[i] * rs / 3;
        }
        out.recompute_handles();
        bool missed = false;
        std::vector<int> split;
        for (int a = 0, b = 1; b < n; ++b) {
            if (!is_key[b]) continue;
            int worst = -1;
            double err = tol;
            for (int i = a + 1; i < b; ++i)
                if (const double e = std::fabs(out.evaluate(from + i) - s[i]); e > err) err = e, worst = i;
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
    std::vector<Key> range;
    for (const Key& k : out.keys)
        if (k.frame >= from - 1e-6 && k.frame <= to + 1e-6) range.push_back(k);
    if (int(range.size()) >= had) range.clear();
    return range;
}

}  // namespace

SimplifyResult simplify_curves(Clip& clip, const std::vector<std::string>& tracks, const SimplifyOptions& opt) {
    SimplifyResult r;
    const int from = std::max(opt.from, 0), to = opt.to < 0 ? clip.end_frame : opt.to;
    if (to - from < 2) return r;
    for (auto& [name, track] : clip.curves) {
        if (!tracks.empty() && std::find(tracks.begin(), tracks.end(), name) == tracks.end()) continue;
        bool gimbal = false;
        if (auto y = track.find("rot_y"); y != track.end() && !y->second.empty() && near_gimbal(y->second, from, to)) {
            gimbal = true;
            r.skipped.push_back(name + ": near gimbal lock, rotation left as it is");
        }
        for (auto& [ch, c] : track) {
            const bool rot = ch.rfind("rot_", 0) == 0, pos = ch.rfind("pos_", 0) == 0;
            if ((!rot && !pos) || (rot && gimbal) || c.empty()) continue;
            int had = 0;
            bool stepped = false;
            for (size_t k = 0; k < c.keys.size(); ++k) {
                const double f = c.keys[k].frame;
                const bool in = f >= from - 1e-6 && f <= to + 1e-6;
                const bool into = k + 1 < c.keys.size() && f < from && c.keys[k + 1].frame > from;  // holds into the range
                had += in;
                stepped |= (in || into) && c.keys[k].interp == Interp::Constant;
            }
            if (stepped) {
                r.skipped.push_back(name + " " + ch + ": stepped keys, left as they are");
                continue;
            }
            // Keys just outside the range hold the curve there (as Filter Curves does).
            bool earlier = false, later = false;
            for (const Key& k : c.keys) earlier |= k.frame < from - 1e-6, later |= k.frame > to + 1e-6;
            FCurve work = c;
            if (earlier && from > 0) insert_on_curve(work, from - 1);
            if (later) insert_on_curve(work, to + 1);
            std::vector<Key> range = fit(work, from, to, rot ? opt.tol_deg : opt.tol_mm / 1000, opt.keep, had);
            r.before += had;
            if (range.empty()) {
                r.after += had;
                continue;
            }
            r.after += int(range.size());
            std::erase_if(work.keys, [&](const Key& k) { return k.frame >= from - 1e-6 && k.frame <= to + 1e-6; });
            work.keys.insert(std::upper_bound(work.keys.begin(), work.keys.end(), double(from),
                                              [](double v, const Key& k) { return v < k.frame; }),
                             range.begin(), range.end());
            work.recompute_handles();
            c = std::move(work);
        }
    }
    return r;
}

}  // namespace vats
