// Viewport Avatar Toolset - motion clean-up filters: One-Euro, Savitzky-Golay, zero-phase Butterworth.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/curve_filter.h"

#include <algorithm>
#include <cmath>

#include "vats/curve_ops.h"

namespace vats {
namespace {


// The samples continued past both ends: odd reflection through the end value, or the next and previous
// periods of a loop (each shifted by the drift).
struct Padded {
    const std::vector<double>& x;
    bool loop;
    double drift;

    double operator[](long i) const {
        const long n = long(x.size());
        if (loop) {
            const long q = i >= 0 ? i / n : -((-i + n - 1) / n);
            return x[size_t(i - q * n)] + double(q) * drift;
        }
        if (i < 0) return 2 * x[0] - x[size_t(std::min(-i, n - 1))];
        if (i >= n) return 2 * x[size_t(n - 1)] - x[size_t(std::max(2 * (n - 1) - i, 0L))];
        return x[size_t(i)];
    }
};

// Causal, so it is warmed up over the padding before the first sample: a whole period (or more) of a loop,
// so the seam matches the steady state, else three time constants of the reflected start, so it enters
// the first sample moving as the curve does there.
std::vector<double> one_euro(const Padded& p, long n, double fps, const FilterSettings& s, double beta) {
    auto alpha = [fps](double fc) { return 1 / (1 + fps / (2 * kPi * std::max(fc, 1e-3))); };
    const long warm = long(std::ceil(3 * fps / (2 * kPi * std::max(s.min_cutoff, 0.05))));  // three time constants
    const long pad = p.loop ? std::max(n, warm) : std::min(n - 1, warm);
    std::vector<double> out(static_cast<size_t>(n));
    double xh = p[-pad], dxh = 0, prev = xh;
    for (long i = -pad; i < n; ++i) {
        const double x = p[i], dx = (x - prev) * fps;
        prev = x;
        dxh += alpha(s.d_cutoff) * (dx - dxh);
        xh += alpha(s.min_cutoff + beta * std::fabs(dxh)) * (x - xh);
        if (i >= 0) out[size_t(i)] = xh;
    }
    return out;
}

// The centre weights of a least-squares fit of a degree-`order` polynomial over -half..half: row 0 of
// (A^T A)^-1 A^T, with the normal equations solved by Gaussian elimination.
std::vector<double> sg_weights(int half, int order) {
    const int m = order + 1;
    std::vector<std::vector<double>> a(size_t(m), std::vector<double>(size_t(m + 1), 0));
    for (int r = 0; r < m; ++r) {
        for (int c = 0; c < m; ++c)
            for (int i = -half; i <= half; ++i) a[r][c] += std::pow(double(i), r + c);
        a[r][m] = r == 0;
    }
    for (int c = 0; c < m; ++c) {
        int piv = c;
        for (int r = c + 1; r < m; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        std::swap(a[c], a[piv]);
        for (int r = 0; r < m; ++r)
            if (r != c) {
                const double f = a[r][c] / a[c][c];
                for (int k = c; k <= m; ++k) a[r][k] -= f * a[c][k];
            }
    }
    std::vector<double> w;
    for (int i = -half; i <= half; ++i) {
        double v = 0;
        for (int k = 0; k < m; ++k) v += a[k][m] / a[k][k] * std::pow(double(i), k);
        w.push_back(v);
    }
    return w;
}

std::vector<double> savitzky_golay(const Padded& p, long n, const FilterSettings& s) {
    const int half = std::clamp(s.sg_half, 1, 30), order = std::clamp(s.sg_order, 0, 2 * half - 1);
    const std::vector<double> w = sg_weights(half, order);
    std::vector<double> out(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i)
        for (int k = -half; k <= half; ++k) out[size_t(i)] += w[size_t(k + half)] * p[i + k];
    return out;
}

// Butterworth low-pass as cascaded biquads (bilinear transform, cutoff pre-warped), each started in its
// steady state for the first input so the padding does not ring.
void butter_pass(std::vector<double>& y, double fps, double fc, int order) {
    const double w0 = 2 * kPi * fc / fps, cw = std::cos(w0);
    for (int k = 0; k < order / 2; ++k) {
        const double q = 1 / (2 * std::cos(kPi * (2 * k + 1) / (2.0 * order))), al = std::sin(w0) / (2 * q), a0 = 1 + al;
        const double b0 = (1 - cw) / 2 / a0, b1 = (1 - cw) / a0, b2 = b0, a1 = -2 * cw / a0, a2 = (1 - al) / a0;
        double s2 = (b2 - a2) * y[0], s1 = (b1 - a1) * y[0] + s2;
        for (double& v : y) {
            const double x = v;
            v = b0 * x + s1;
            s1 = b1 * x - a1 * v + s2;
            s2 = b2 * x - a2 * v;
        }
    }
}

std::vector<double> butterworth(const Padded& p, long n, double fps, const FilterSettings& s) {
    const double fc = std::clamp(s.cutoff, 0.01, 0.45 * fps);
    const int order = std::clamp(s.order + s.order % 2, 2, 8);
    const long want = long(std::ceil(3 * fps / fc)) * order;
    const long pad = p.loop ? std::max(n, want) : std::min(n - 1, want);
    std::vector<double> y;
    for (long i = -pad; i < n + pad; ++i) y.push_back(p[i]);
    butter_pass(y, fps, fc, order);
    std::reverse(y.begin(), y.end());
    butter_pass(y, fps, fc, order);
    std::reverse(y.begin(), y.end());
    return {y.begin() + pad, y.begin() + pad + n};
}

// Values of a curve at whole frames a .. a + n - 1.
std::vector<double> sample(const FCurve& c, int a, int n) {
    std::vector<double> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[size_t(i)] = c.evaluate(a + i);
    return x;
}

// The frames filter_curves samples: the loop when from..to is inside it, else from..to.
struct Span {
    int a = 0, n = 0;
    bool loop = false;
};
Span span_of(const Clip& clip, int from, int to) {
    from = std::max(from, 0);
    if (clip.loop && clip.loop_out - clip.loop_in >= 4 && from >= clip.loop_in && to <= clip.loop_out)
        return {clip.loop_in, clip.loop_out - clip.loop_in, true};
    return {from, std::max(to - from + 1, 0), false};
}

}  // namespace

const char* filter_name(FilterKind k) {
    switch (k) {
        case FilterKind::OneEuro: return "One-Euro";
        case FilterKind::SavitzkyGolay: return "Savitzky-Golay";
        case FilterKind::Butterworth: return "Butterworth";
    }
    return "";
}

std::vector<double> filter_samples(const std::vector<double>& x, double fps, const FilterSettings& s, bool position,
                                   bool loop, double loop_drift) {
    const long n = long(x.size());
    if (n < 3 || !(fps > 0)) return x;
    const Padded p{x, loop, loop ? loop_drift : 0};
    switch (s.kind) {
        case FilterKind::OneEuro: return one_euro(p, n, fps, s, position ? s.beta_m : s.beta);
        case FilterKind::SavitzkyGolay: return savitzky_golay(p, n, s);
        case FilterKind::Butterworth: return butterworth(p, n, fps, s);
    }
    return x;
}

double jerk_rms(const std::vector<double>& x, double fps, bool loop, double loop_drift) {
    const long n = long(x.size()), terms = loop ? n : n - 3;
    if (n < 4) return 0;
    const Padded p{x, loop, loop ? loop_drift : 0};
    double sum = 0;
    for (long i = 0; i < terms; ++i) {
        const double j = (p[i + 3] - 3 * p[i + 2] + 3 * p[i + 1] - p[i]) * fps * fps * fps;
        sum += j * j;
    }
    return std::sqrt(sum / double(terms));
}

void filter_curves(Clip& clip, const std::vector<CurveId>& curves, int from, int to, const FilterSettings& s) {
    from = std::max(from, 0);
    const Span sp = span_of(clip, from, to);
    if (sp.n < 3) return;
    for (const CurveId& id : curves) {
        auto t = clip.curves.find(id.track);
        if (t == clip.curves.end() || !t->second.count(id.channel) || t->second.at(id.channel).empty()) continue;
        FCurve& c = t->second.at(id.channel);
        const bool metres = id.channel.rfind("pos_", 0) == 0 || id.channel.rfind("pole_", 0) == 0;
        const std::vector<double> x = sample(c, sp.a, sp.n);
        const double drift = sp.loop ? c.evaluate(sp.a + sp.n) - x[0] : 0;
        const std::vector<double> y = filter_samples(x, clip.fps, s, metres, sp.loop, drift);
        // Keys just outside the range hold the curve there (as a punched-in take does).
        bool earlier = false, later = false;
        for (const Key& k : c.keys) earlier |= k.frame < from - 1e-6, later |= k.frame > to + 1e-6;
        if (earlier && from > 0) insert_on_curve(c, from - 1);
        if (later) insert_on_curve(c, to + 1);
        std::erase_if(c.keys, [&](const Key& k) { return k.frame >= from - 1e-6 && k.frame <= to + 1e-6; });
        for (int f = from; f <= to; ++f) {
            const int i = f - sp.a, q = i / sp.n;  // q = 1 only at loop_out: one period on
            Key k;
            k.frame = f;
            k.value = y[size_t(i - q * sp.n)] + q * drift;
            c.keys.push_back(k);
        }
        std::sort(c.keys.begin(), c.keys.end(), [](const Key& a, const Key& b) { return a.frame < b.frame; });
        c.recompute_handles();
    }
}

std::vector<JointShake> shake_scores(const Clip& clip, const std::vector<std::string>& tracks, int from, int to) {
    const Span sp = span_of(clip, from, to);
    std::vector<JointShake> out;
    for (const std::string& name : tracks) {
        auto t = clip.curves.find(name);
        if (t == clip.curves.end()) continue;
        JointShake js{name};
        for (int part = 0; part < 2; ++part) {
            double sum = 0;
            for (const char* ch : part ? kPosChannels : kRotChannels) {
                auto c = t->second.find(ch);
                if (c == t->second.end() || c->second.empty()) continue;
                const std::vector<double> x = sample(c->second, sp.a, sp.n);
                const double drift = sp.loop ? c->second.evaluate(sp.a + sp.n) - x[0] : 0;
                sum += std::pow(jerk_rms(x, clip.fps, sp.loop, drift), 2);
            }
            (part ? js.pos : js.rot) = std::sqrt(sum);
        }
        out.push_back(js);
    }
    return out;
}

}  // namespace vats
