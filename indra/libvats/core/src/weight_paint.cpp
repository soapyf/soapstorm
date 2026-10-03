// Viewport Avatar Toolset - weight painting. See vats/weight_paint.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/weight_paint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

#include "vats/bone_heat.h"

namespace vats {

PaintMesh paint_mesh(const DaeModel& m) {
    PaintMesh pm;
    const int nv = m.vertex_count();
    double size = (m.bounds_max - m.bounds_min).length();
    if (!(size > 0)) size = 1;
    const double q = size * 1e-6;
    std::map<std::array<long long, 3>, int> at;
    pm.weld.resize(size_t(nv));
    for (int v = 0; v < nv; ++v) {
        const Vec3 p{m.positions[size_t(v) * 3], m.positions[size_t(v) * 3 + 1], m.positions[size_t(v) * 3 + 2]};
        const auto [it, fresh] = at.try_emplace({std::llround(p.x / q), std::llround(p.y / q), std::llround(p.z / q)}, int(pm.at.size()));
        if (fresh) pm.at.push_back(p), pm.members.emplace_back();
        pm.weld[size_t(v)] = it->second;
        pm.members[size_t(it->second)].push_back(v);
    }
    std::vector<std::set<int>> next(pm.at.size());
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = m.indices[i + size_t(k)], b = m.indices[i + size_t((k + 1) % 3)];
            if (a >= std::uint32_t(nv) || b >= std::uint32_t(nv)) continue;
            const int wa = pm.weld[a], wb = pm.weld[b];
            if (wa != wb) next[size_t(wa)].insert(wb), next[size_t(wb)].insert(wa);
        }
    pm.next.resize(next.size());
    for (size_t i = 0; i < next.size(); ++i) pm.next[i].assign(next[i].begin(), next[i].end());
    return pm;
}

double paint_falloff(const PaintBrush& b, double d) {
    if (!(b.radius > 0) || d >= b.radius) return 0;
    const double t = d / b.radius;
    switch (b.falloff) {
        case PaintFalloff::Constant: return 1;
        case PaintFalloff::Linear: return 1 - t;
        case PaintFalloff::Smooth: break;
    }
    return (1 - t * t) * (1 - t * t);
}

Vec3 mirror_point(const Vec3& p) { return {p.x, -p.y, p.z}; }

int sk40_of_node(const Skeleton& skel, int node) {
    if (node < 0 || node >= skel.size()) return -1;
    if (node < skel.joint_count()) return node;
    for (size_t v = 0; v < skel.volumes().size(); ++v)
        if (skel.volumes()[v].node == node) return dae_volume(skel, int(v));
    return -1;
}

int node_of_sk40(const Skeleton& skel, int sk40) {
    if (sk40 < 0) return -1;
    if (sk40 < skel.size()) return sk40;
    const int v = sk40 - dae_volume(skel, 0);
    return v >= 0 && v < int(skel.volumes().size()) ? skel.volumes()[size_t(v)].node : -1;
}

int dominant_joint(const DaeModel& model, int vertex) {
    if (vertex < 0 || size_t(vertex) * 4 + 3 >= model.weights.size()) return -1;
    int best = -1;
    float most = 0;
    for (size_t k = 0; k < 4; ++k)
        if (const float w = model.weights[size_t(vertex) * 4 + k]; w > most) most = w, best = model.joints[size_t(vertex) * 4 + k];
    return best;
}

int mirror_joint(const Skeleton& skel, int joint) {
    if (joint < 0) return joint;
    if (joint < skel.size()) {
        const int m = skel.find(Skeleton::mirror_name(skel[joint].name));
        return m >= 0 ? m : joint;
    }
    const int v = joint - dae_root(skel) - 1;
    if (v < 0 || v >= int(skel.volumes().size())) return joint;
    const int m = skel.find_volume(Skeleton::mirror_name(skel.volumes()[size_t(v)].name));
    return m >= 0 ? dae_volume(skel, m) : joint;
}

namespace {

using Weights = std::vector<std::pair<int, double>>;

Weights read(const DaeModel& m, int v) {
    Weights w;
    for (int k = 0; k < 4; ++k)
        if (m.weights[size_t(v) * 4 + size_t(k)] > 0) w.push_back({m.joints[size_t(v) * 4 + size_t(k)], m.weights[size_t(v) * 4 + size_t(k)]});
    return w;
}

double weight_of(const Weights& w, int joint) {
    double x = 0;
    for (const auto& [j, v] : w) x += j == joint ? v : 0;
    return x;
}

// The joint a vertex's freed weight goes to: the one its neighbours carry most other than joint, else joint's parent
// (a collision volume's: its joint).
int heir(const Skeleton& skel, const std::vector<Weights>& before, const PaintMesh& pm, int w, int joint) {
    std::map<int, double> votes;
    for (int n : pm.next[size_t(w)])
        for (const auto& [j, x] : before[size_t(n)])
            if (j != joint) votes[j] += x;
    int best = -1;
    double most = 0;
    for (const auto& [j, x] : votes)
        if (x > most) most = x, best = j;
    if (best >= 0) return best;
    if (joint >= 0 && joint < skel.joint_count()) return skel[joint].parent >= 0 ? skel[joint].parent : joint;
    const int v = joint - dae_root(skel) - 1;
    return v >= 0 && v < int(skel.volumes().size()) ? skel.volumes()[size_t(v)].joint : joint;
}

}  // namespace

int nearest_vertex(const DaeModel& m, const Vec3& p, const DaePart* within) {
    const std::uint32_t from = within ? within->first_vertex : 0, to = within ? within->first_vertex + within->vertex_count : std::uint32_t(m.vertex_count());
    int best = -1;
    double d = 1e300;
    for (std::uint32_t v = from; v < to && v < std::uint32_t(m.vertex_count()); ++v) {
        const double e = (Vec3{m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]} - p).length();
        if (e < d) d = e, best = int(v);
    }
    return best;
}

int paint_dab(const Skeleton& skel, DaeModel& m, const PaintMesh& pm, int joint, const Vec3& centre, const PaintBrush& brush, int seed_vertex) {
    const int nv = m.vertex_count(), root = dae_root(skel);
    if (joint < 0 || joint == root || m.joints.size() != size_t(nv) * 4 || pm.weld.size() != size_t(nv)) return 0;
    // The welded vertices the brush reaches, and what each carries before this dab (smoothing reads neighbours as they were).
    std::vector<int> hit;
    std::vector<double> reach;
    auto strength_at = [&](int w) {
        return paint_falloff(brush, (pm.at[size_t(w)] - centre).length()) * std::clamp(brush.strength, 0.0, 1.0);
    };
    if (brush.connected) {
        // Only the surface the brush is on: out from the vertex nearest its centre along the mesh's edges, within its
        // reach, so a hand near the face, or the other thigh, is left alone.
        int seed = seed_vertex >= 0 && seed_vertex < nv ? pm.weld[size_t(seed_vertex)] : -1;
        if (seed < 0) {
            double best = 1e300;
            for (size_t w = 0; w < pm.at.size(); ++w)
                if (const double d = (pm.at[w] - centre).length(); d < best) best = d, seed = int(w);
        }
        if (seed < 0 || (pm.at[size_t(seed)] - centre).length() > brush.radius) return 0;
        std::vector<char> seen(pm.at.size(), 0);
        std::vector<int> stack{seed};
        seen[size_t(seed)] = 1;
        while (!stack.empty()) {
            const int w = stack.back();
            stack.pop_back();
            const double f = strength_at(w);
            if (f <= 0) continue;
            hit.push_back(w), reach.push_back(f);
            for (int n : pm.next[size_t(w)])
                if (!seen[size_t(n)]) seen[size_t(n)] = 1, stack.push_back(n);
        }
    } else {
        for (size_t w = 0; w < pm.at.size(); ++w)
            if (const double f = strength_at(int(w)); f > 0) hit.push_back(int(w)), reach.push_back(f);
    }
    if (hit.empty()) return 0;
    std::vector<Weights> before(pm.at.size());
    std::vector<char> need(pm.at.size(), 0);
    for (int w : hit) {
        need[size_t(w)] = 1;
        for (int n : pm.next[size_t(w)]) need[size_t(n)] = 1;
    }
    for (size_t w = 0; w < pm.at.size(); ++w)
        if (need[w] && !pm.members[w].empty()) before[w] = read(m, pm.members[w][0]);
    int changed = 0;
    for (size_t i = 0; i < hit.size(); ++i) {
        const int w = hit[i];
        const double s = reach[i];
        Weights x = before[size_t(w)];
        const double was = weight_of(x, joint);
        double now = was;
        if (brush.op == PaintOp::Add) now = was + s * (1 - was);
        else if (brush.op == PaintOp::Subtract) now = was - s * was;
        else {
            double sum = 0;
            for (int n : pm.next[size_t(w)]) sum += weight_of(before[size_t(n)], joint);
            const double mean = pm.next[size_t(w)].empty() ? was : sum / double(pm.next[size_t(w)].size());
            now = was + s * (mean - was);
        }
        now = std::clamp(now, 0.0, 1.0);
        if (std::fabs(now - was) < 1e-7) continue;
        // The others share what is left in proportion; with no others, the freed weight goes to an heir.
        Weights out;
        double others = 0;
        for (const auto& [j, v] : x)
            if (j != joint) others += v;
        const double left = 1 - now;
        for (const auto& [j, v] : x)
            if (j != joint && others > 1e-12) out.push_back({j, v * left / others});
        if (others <= 1e-12 && left > 1e-9) out.push_back({heir(skel, before, pm, w, joint), left});
        if (now > 0) out.push_back({joint, now});
        int jj[4];
        float ww[4];
        keep_four(out, root, 1e-4, jj, ww);
        for (int v : pm.members[size_t(w)])
            for (int k = 0; k < 4; ++k) m.joints[size_t(v) * 4 + size_t(k)] = jj[k], m.weights[size_t(v) * 4 + size_t(k)] = ww[k];
        ++changed;
    }
    return changed;
}

std::vector<float> begin_stroke(const DaeModel& m, std::vector<int>& joints) {
    joints = m.joints;
    return m.weights;
}

PaintStroke end_stroke(const DaeModel& m, const std::vector<int>& jb, const std::vector<float>& wb) {
    PaintStroke s;
    const size_t nv = std::min(m.joints.size(), jb.size()) / 4;
    for (size_t v = 0; v < nv; ++v) {
        bool same = true;
        for (size_t k = 0; k < 4 && same; ++k) same = jb[v * 4 + k] == m.joints[v * 4 + k] && wb[v * 4 + k] == m.weights[v * 4 + k];
        if (same) continue;
        s.vertices.push_back(int(v));
        s.joints_before.insert(s.joints_before.end(), jb.begin() + long(v * 4), jb.begin() + long(v * 4 + 4));
        s.weights_before.insert(s.weights_before.end(), wb.begin() + long(v * 4), wb.begin() + long(v * 4 + 4));
        s.joints_after.insert(s.joints_after.end(), m.joints.begin() + long(v * 4), m.joints.begin() + long(v * 4 + 4));
        s.weights_after.insert(s.weights_after.end(), m.weights.begin() + long(v * 4), m.weights.begin() + long(v * 4 + 4));
    }
    return s;
}

void undo_stroke(const PaintStroke& s, DaeModel& m, bool redo) {
    const std::vector<int>& j = redo ? s.joints_after : s.joints_before;
    const std::vector<float>& w = redo ? s.weights_after : s.weights_before;
    for (size_t i = 0; i < s.vertices.size(); ++i) {
        const size_t v = size_t(s.vertices[i]);
        if (v * 4 + 4 > m.joints.size()) continue;
        for (size_t k = 0; k < 4; ++k) m.joints[v * 4 + k] = j[i * 4 + k], m.weights[v * 4 + k] = w[i * 4 + k];
    }
}

}  // namespace vats
