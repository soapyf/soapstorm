// Viewport Avatar Toolset - key reduction by the world-space error it causes anywhere on the body.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/world_reduce.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vats {

std::vector<int> rdp_keys(int n, double tol, int max_gap, const std::vector<char>& anchors,
                          const std::function<double(int, int, int)>& err) {
    std::vector<int> keep;
    if (n <= 0) return keep;
    std::vector<char> kept(n, 0);
    kept[0] = kept[n - 1] = 1;
    for (int f = 0; f < n && f < int(anchors.size()); ++f) kept[f] = kept[f] || anchors[f];
    if (tol <= 0) std::fill(kept.begin(), kept.end(), char(1));
    const int gap = max_gap > 0 ? max_gap : n;
    std::vector<std::pair<int, int>> runs;  // an explicit stack: a run can split at every frame
    for (int a = 0, b = 1; b < n; ++b)
        if (kept[b]) runs.emplace_back(a, b), a = b;
    while (!runs.empty()) {
        const auto [a, b] = runs.back();
        runs.pop_back();
        if (b - a < 2) continue;
        int worst = a + 1;
        double most = -1;
        for (int k = a + 1; k < b; ++k)
            if (double e = err(a, b, k); e > most) most = e, worst = k;
        if (most <= tol) {
            if (b - a <= gap) continue;
            worst = (a + b) / 2;  // WR-1: the 60-frame rule, not the error, splits this run
        }
        kept[worst] = 1;
        runs.emplace_back(a, worst);
        runs.emplace_back(worst, b);
    }
    for (int f = 0; f < n; ++f)
        if (kept[f]) keep.push_back(f);
    return keep;
}

std::vector<std::vector<double>> world_reach(const Skeleton& skel, const std::vector<std::vector<Xform>>& globals) {
    const int nodes = skel.size(), frames = int(globals.size());
    std::vector<std::vector<double>> reach(nodes, std::vector<double>(frames, 0.0));
    for (int i = 0; i < nodes; ++i) std::fill(reach[i].begin(), reach[i].end(), skel[i].end.length());  // its own tip
    for (int f = 0; f < frames; ++f) {
        const std::vector<Xform>& g = globals[f];
        for (int i = nodes - 1; i > 0; --i) {  // children come after their parents
            const int p = skel[i].parent;
            if (p < 0 || skel[i].volume) continue;  // a collision volume is no part of the body's reach
            reach[p][f] = std::max(reach[p][f], (g[i].pos - g[p].pos).length() + reach[i][f]);
        }
    }
    // Every node reaches at least kMinReach, so turning a leaf (a fingertip, an eye, an attachment point holding a
    // prop) is not free. ponytail: a prop longer than that needs a finer N; the props' own reach would fix it.
    constexpr double kMinReach = 0.1;
    for (auto& row : reach)
        for (double& r : row) r = std::max(r, kMinReach);
    return reach;
}

std::vector<double> world_budgets(const Skeleton& skel, const std::vector<char>& rot, const std::vector<char>& pos,
                                  double tol_m) {
    const int nodes = skel.size();
    std::vector<int> c(nodes, 0), up(nodes, 0), down(nodes, 0);
    for (int i = 0; i < nodes; ++i) {
        c[i] = (i < int(rot.size()) && rot[i]) + (i < int(pos.size()) && pos[i]);
        up[i] = c[i] + (skel[i].parent >= 0 ? up[skel[i].parent] : 0);
    }
    for (int i = nodes - 1; i >= 0; --i) {
        down[i] += c[i];
        if (const int p = skel[i].parent; p >= 0) down[p] = std::max(down[p], down[i]);
    }
    std::vector<double> budget(nodes, 0.0);
    for (int i = 0; i < nodes; ++i)
        if (c[i]) budget[i] = tol_m / (up[i] + down[i] - c[i]);
    return budget;
}

std::vector<int> reduce_rotation_keys_world(const std::vector<Quat>& s, const std::vector<double>& reach, double tol_m,
                                            int max_gap, const std::vector<char>& anchors) {
    return rdp_keys(int(s.size()), tol_m, max_gap, anchors, [&](int a, int b, int k) {
        const Quat q = nlerp(s[a], s[b], double(k - a) / double(b - a));  // as LLKeyframeMotion plays it
        const double half = std::acos(std::min(1.0, std::fabs(q.dot(s[k]))));
        return 2 * std::sin(half) * (k < int(reach.size()) ? reach[k] : 0.0);
    });
}

std::vector<int> reduce_position_keys_world(const std::vector<Vec3>& s, double scale, double tol_m, int max_gap,
                                            const std::vector<char>& anchors) {
    return rdp_keys(int(s.size()), tol_m, max_gap, anchors, [&](int a, int b, int k) {
        const double t = double(k - a) / double(b - a);
        return (s[a] + (s[b] - s[a]) * t - s[k]).length() * scale;
    });
}

}  // namespace vats
