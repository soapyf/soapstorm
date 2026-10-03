// Viewport Avatar Toolset - bone heat on the robust Laplacian. See vats/bone_heat.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Written from the papers named in the header. The sparse Cholesky is the textbook up-looking algorithm (elimination
// tree, row patterns by reachability, one column of L per row of A), ordered by nested dissection in space.
#include "vats/bone_heat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>

namespace vats {

namespace {

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// A triangle's area from its edge lengths, in the form that stays accurate for needle-thin triangles (lengths sorted
// a >= b >= c). 0 when the lengths make no triangle.
double area_of(double a, double b, double c) {
    if (a < b) std::swap(a, b);
    if (a < c) std::swap(a, c);
    if (b < c) std::swap(b, c);
    const double q = (a + (b + c)) * (c - (a - b)) * (c + (a - b)) * (a + (b - c));
    return q > 0 ? 0.25 * std::sqrt(q) : 0.0;
}

// The cotangent of the angle opposite the side of length c, in the triangle with sides a, b, c.
double cot_opposite(double a, double b, double c, double area) { return (a * a + b * b - c * c) / (4 * area); }

std::uint64_t edge_key(int i, int j) {
    if (i > j) std::swap(i, j);
    return (std::uint64_t(std::uint32_t(i)) << 32) | std::uint32_t(j);
}

// Sums the per-corner cotangent halves into edges.
struct EdgeSum {
    std::unordered_map<std::uint64_t, double> w;
    void add(int i, int j, double v) {
        if (i != j) w[edge_key(i, j)] += v;
    }
    void into(MeshLaplacian& lap, double scale) {
        std::vector<std::pair<std::uint64_t, double>> all(w.begin(), w.end());
        std::sort(all.begin(), all.end());
        lap.diagonal.assign(size_t(lap.n), 0.0);
        for (const auto& [k, v] : all) {
            const int i = int(k >> 32), j = int(k & 0xffffffffu);
            const double x = v * scale;
            lap.edges.push_back({i, j});
            lap.weight.push_back(x);
            lap.diagonal[size_t(i)] += x, lap.diagonal[size_t(j)] += x;
            lap.negative += x < -1e-12;
        }
    }
};

MeshLaplacian cotangent_laplacian(const std::vector<Vec3>& p, const std::vector<std::array<int, 3>>& tris) {
    MeshLaplacian lap;
    lap.n = int(p.size());
    lap.mass.assign(p.size(), 0.0);
    EdgeSum sum;
    for (const auto& t : tris) {
        const Vec3 &a = p[size_t(t[0])], &b = p[size_t(t[1])], &c = p[size_t(t[2])];
        const double area2 = (b - a).cross(c - a).length();
        if (!(area2 > 1e-300)) continue;  // a zero-area triangle has no cotangents
        for (int k = 0; k < 3; ++k) {
            const Vec3 &o = p[size_t(t[k])], &u = p[size_t(t[(k + 1) % 3])], &v = p[size_t(t[(k + 2) % 3])];
            const double cot = (u - o).dot(v - o) / area2;  // the angle at o, opposite edge u-v
            sum.add(t[(k + 1) % 3], t[(k + 2) % 3], cot / 2);
            lap.mass[size_t(t[k])] += area2 / 6;
        }
    }
    sum.into(lap, 1.0);
    return lap;
}

// The tufted cover as a halfedge mesh: every triangle twice (front and back), each edge's fins glued so the cover
// wraps around the edge like a book's pages, closed and manifold whatever the input. Lengths are intrinsic.
struct Cover {
    std::vector<int> next, twin, tail, face;
    std::vector<double> len;

    bool delaunay(int h) const {
        const int t = twin[size_t(h)];
        const double a = cot_at(h), b = cot_at(t);
        return a + b >= -1e-10;
    }
    // The cotangent of the angle opposite halfedge h in its face.
    double cot_at(int h) const {
        const int h1 = next[size_t(h)], h2 = next[size_t(h1)];
        const double c = len[size_t(h)], a = len[size_t(h1)], b = len[size_t(h2)];
        const double area = area_of(a, b, c);
        return area > 0 ? cot_opposite(a, b, c, area) : -std::numeric_limits<double>::infinity();
    }
    // Flips edge h intrinsically; false when it cannot be flipped (both sides one face, or no valid triangles after).
    bool flip(int h) {
        const int t = twin[size_t(h)];
        if (face[size_t(h)] == face[size_t(t)]) return false;
        const int a1 = next[size_t(h)], a2 = next[size_t(a1)], b1 = next[size_t(t)], b2 = next[size_t(b1)];
        const double lij = len[size_t(h)], ljk = len[size_t(a1)], lki = len[size_t(a2)], lil = len[size_t(b1)],
                     llj = len[size_t(b2)];
        // Lay the two triangles out flat: i at the origin, j on +x, k above, l below.
        const double xk = (lij * lij + lki * lki - ljk * ljk) / (2 * lij), yk = std::sqrt(std::max(0.0, lki * lki - xk * xk));
        const double xl = (lij * lij + lil * lil - llj * llj) / (2 * lij), yl = std::sqrt(std::max(0.0, lil * lil - xl * xl));
        const double lkl = std::hypot(xk - xl, yk + yl);
        auto ok = [](double x, double y, double z) {
            return x > 0 && y > 0 && z > 0 && x < y + z && y < x + z && z < x + y;
        };
        if (!std::isfinite(lkl) || !ok(lkl, lki, lil) || !ok(lkl, llj, ljk)) return false;
        const int k = tail[size_t(a2)], l = tail[size_t(b2)];
        const int fa = face[size_t(h)], fb = face[size_t(t)];
        // h becomes l->k in face A (with a2 and b1), t becomes k->l in face B (with b2 and a1).
        tail[size_t(h)] = l, tail[size_t(t)] = k;
        next[size_t(h)] = a2, next[size_t(a2)] = b1, next[size_t(b1)] = h;
        next[size_t(t)] = b2, next[size_t(b2)] = a1, next[size_t(a1)] = t;
        face[size_t(b1)] = fa, face[size_t(a1)] = fb;
        len[size_t(h)] = len[size_t(t)] = lkl;
        return true;
    }
};

MeshLaplacian robust_laplacian(const std::vector<Vec3>& p, const std::vector<std::array<int, 3>>& tris, double mollify) {
    MeshLaplacian lap;
    lap.n = int(p.size());
    lap.mass.assign(p.size(), 0.0);
    const int m = int(tris.size());
    if (m == 0) {
        lap.diagonal.assign(p.size(), 0.0);
        return lap;
    }
    // Every edge's fins (the triangles on it), and its length.
    struct Fin {
        int face, k;  // the input triangle and its corner k: the edge from corner k to corner k+1
    };
    std::unordered_map<std::uint64_t, std::vector<Fin>> fins;
    fins.reserve(size_t(m) * 2);
    for (int f = 0; f < m; ++f)
        for (int k = 0; k < 3; ++k) fins[edge_key(tris[size_t(f)][size_t(k)], tris[size_t(f)][size_t((k + 1) % 3)])].push_back({f, k});
    // Mollification (as the paper does it): every edge longer by the same delta, so that every triangle's
    // inequalities hold with a slack of at least eps.
    double mean = 0;
    for (const auto& [key, list] : fins) mean += (p[size_t(key >> 32)] - p[size_t(key & 0xffffffffu)]).length();
    mean /= double(fins.size());
    const double eps = mollify * mean;
    double delta = 0;
    for (const auto& t : tris) {
        const double l0 = (p[size_t(t[0])] - p[size_t(t[1])]).length(), l1 = (p[size_t(t[1])] - p[size_t(t[2])]).length(),
                     l2 = (p[size_t(t[2])] - p[size_t(t[0])]).length();
        delta = std::max({delta, eps - (l0 + l1 - l2), eps - (l1 + l2 - l0), eps - (l2 + l0 - l1)});
    }
    lap.mollified = delta;

    Cover c;
    const int nh = 6 * m;
    c.next.resize(size_t(nh)), c.twin.assign(size_t(nh), -1), c.tail.resize(size_t(nh)), c.face.resize(size_t(nh));
    c.len.resize(size_t(nh));
    // Face 2f is triangle f as given (corners 0, 1, 2), face 2f+1 its back (0, 2, 1). Halfedge 3F + k leaves corner k
    // of face F. The back's halfedge 2 - k runs along the front's halfedge k the other way.
    for (int f = 0; f < m; ++f)
        for (int side = 0; side < 2; ++side) {
            const int F = 2 * f + side;
            const std::array<int, 3> v = side == 0 ? tris[size_t(f)] : std::array<int, 3>{tris[size_t(f)][0], tris[size_t(f)][2], tris[size_t(f)][1]};
            for (int k = 0; k < 3; ++k) {
                const int h = 3 * F + k;
                c.next[size_t(h)] = 3 * F + (k + 1) % 3;
                c.tail[size_t(h)] = v[size_t(k)];
                c.face[size_t(h)] = F;
                c.len[size_t(h)] = (p[size_t(v[size_t(k)])] - p[size_t(v[size_t((k + 1) % 3)])]).length() + delta;
            }
        }
    // Glue the fins of each edge in their order around it: the side of fin i facing the next fin (its halfedge runs
    // lo -> hi) to the side of the next fin facing back (its halfedge runs hi -> lo).
    for (auto& [key, list] : fins) {
        const int lo = int(key >> 32), hi = int(key & 0xffffffffu);
        const Vec3 d = p[size_t(hi)] - p[size_t(lo)];
        const double dd = d.dot(d);
        struct Side {
            int plus, minus;
            double angle;
        };
        std::vector<Side> sides;
        Vec3 u0;
        for (const Fin& fin : list) {
            const auto& t = tris[size_t(fin.face)];
            const int from = t[size_t(fin.k)], other = t[size_t((fin.k + 2) % 3)];
            const int front = 3 * (2 * fin.face) + fin.k, back = 3 * (2 * fin.face + 1) + (2 - fin.k);
            Vec3 u = p[size_t(other)] - p[size_t(lo)];
            if (dd > 0) u = u - d * (u.dot(d) / dd);
            if (sides.empty()) u0 = u;
            const double angle = std::atan2(d.normalized().dot(u0.cross(u)), u0.dot(u));
            sides.push_back(from == lo ? Side{front, back, angle} : Side{back, front, angle});
        }
        std::stable_sort(sides.begin(), sides.end(), [](const Side& a, const Side& b) { return a.angle < b.angle; });
        for (size_t i = 0; i < sides.size(); ++i) {
            const int a = sides[i].plus, b = sides[(i + 1) % sides.size()].minus;
            c.twin[size_t(a)] = b, c.twin[size_t(b)] = a;
        }
    }
    // Intrinsic Delaunay by flips. ponytail: a cap on the flips guards against round-off cycling.
    std::vector<int> queue;
    std::vector<char> queued(size_t(nh), 0);
    auto push = [&](int h) {
        const int e = std::min(h, c.twin[size_t(h)]);
        if (!queued[size_t(e)]) queued[size_t(e)] = 1, queue.push_back(e);
    };
    for (int h = 0; h < nh; ++h) push(h);
    long long budget = 20LL * nh;
    for (size_t at = 0; at < queue.size() && budget > 0; ++at) {
        const int e = queue[at];
        queued[size_t(e)] = 0;
        if (c.delaunay(e)) continue;
        const int a1 = c.next[size_t(e)], a2 = c.next[size_t(a1)], t = c.twin[size_t(e)], b1 = c.next[size_t(t)], b2 = c.next[size_t(b1)];
        if (!c.flip(e)) continue;
        ++lap.flips, --budget;
        for (int x : {a1, a2, b1, b2}) push(x);
        if (at > 1 << 20) queue.erase(queue.begin(), queue.begin() + long(at) + 1), at = size_t(-1);  // keep it short
    }
    // The cotangent weights and the lumped areas of the cover, halved: it holds every triangle twice.
    EdgeSum sum;
    for (int h = 0; h < nh; ++h) {
        const int h1 = c.next[size_t(h)], h2 = c.next[size_t(h1)];
        const double a = c.len[size_t(h1)], b = c.len[size_t(h2)], cl = c.len[size_t(h)];
        const double area = area_of(a, b, cl);
        if (!(area > 0)) continue;
        sum.add(c.tail[size_t(h)], c.tail[size_t(h1)], cot_opposite(a, b, cl, area) / 2);
        lap.mass[size_t(c.tail[size_t(h)])] += area / 3 / 2;
    }
    sum.into(lap, 0.5);
    return lap;
}

// ---------------------------------------------------------------------------------------------
// Triangles in a bounding volume hierarchy: segment tests (is a bone visible?) and nearest points (weight transfer).

struct TriBvh {
    struct Node {
        Vec3 lo, hi;
        int first = 0, count = 0, right = -1;  // a leaf holds order[first .. first+count); else the left child is next
    };
    const std::vector<Vec3>* p = nullptr;
    const std::vector<std::array<int, 3>>* t = nullptr;
    std::vector<Node> nodes;
    std::vector<int> order;

    void build(const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& tris) {
        p = &points, t = &tris;
        order.resize(tris.size());
        std::iota(order.begin(), order.end(), 0);
        nodes.clear();
        if (!tris.empty()) split(0, int(tris.size()));
    }
    Vec3 centre(int i) const {
        const auto& x = (*t)[size_t(i)];
        return ((*p)[size_t(x[0])] + (*p)[size_t(x[1])] + (*p)[size_t(x[2])]) * (1.0 / 3);
    }
    int split(int first, int count) {
        const int me = int(nodes.size());
        nodes.push_back({});
        Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300}, clo = lo, chi = hi;
        for (int i = first; i < first + count; ++i) {
            for (int k = 0; k < 3; ++k) {
                const Vec3& v = (*p)[size_t((*t)[size_t(order[size_t(i)])][size_t(k)])];
                for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], v[a]), hi[a] = std::max(hi[a], v[a]);
            }
            const Vec3 c = centre(order[size_t(i)]);
            for (int a = 0; a < 3; ++a) clo[a] = std::min(clo[a], c[a]), chi[a] = std::max(chi[a], c[a]);
        }
        nodes[size_t(me)].lo = lo, nodes[size_t(me)].hi = hi;
        if (count <= 4) {
            nodes[size_t(me)].first = first, nodes[size_t(me)].count = count;
            return me;
        }
        int axis = 0;
        for (int a = 1; a < 3; ++a)
            if (chi[a] - clo[a] > chi[axis] - clo[axis]) axis = a;
        const int mid = first + count / 2;
        std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count,
                         [&](int a, int b) { return centre(a)[axis] < centre(b)[axis]; });
        split(first, mid - first);
        const int right = split(mid, first + count - mid);
        nodes[size_t(me)].right = right;
        return me;
    }
    static bool segment_box(const Vec3& a, const Vec3& d, const Vec3& lo, const Vec3& hi) {
        double t0 = 0, t1 = 1;
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(d[k]) < 1e-300) {
                if (a[k] < lo[k] || a[k] > hi[k]) return false;
                continue;
            }
            double u = (lo[k] - a[k]) / d[k], v = (hi[k] - a[k]) / d[k];
            if (u > v) std::swap(u, v);
            t0 = std::max(t0, u), t1 = std::min(t1, v);
            if (t0 > t1) return false;
        }
        return true;
    }
    // Whether the segment a-b crosses a triangle, other than those with corner skip.
    const std::vector<int>* piece = nullptr;  // per triangle, when only one piece's triangles count (blocked's own)
    bool blocked(const Vec3& a, const Vec3& b, int skip, int own_piece = -1) const {
        if (nodes.empty()) return false;
        const Vec3 d = b - a;
        int stack[128], top = 0;
        stack[top++] = 0;
        while (top) {
            const Node& n = nodes[size_t(stack[--top])];
            if (!segment_box(a, d, n.lo, n.hi)) continue;
            if (n.count == 0) {
                const int me = int(&n - nodes.data());
                if (top + 2 > 128) return false;
                stack[top++] = n.right, stack[top++] = me + 1;
                continue;
            }
            for (int i = n.first; i < n.first + n.count; ++i) {
                const auto& x = (*t)[size_t(order[size_t(i)])];
                if (x[0] == skip || x[1] == skip || x[2] == skip) continue;
                if (piece && own_piece >= 0 && (*piece)[size_t(order[size_t(i)])] != own_piece) continue;
                // Moller-Trumbore, both faces.
                const Vec3 &v0 = (*p)[size_t(x[0])], e1 = (*p)[size_t(x[1])] - v0, e2 = (*p)[size_t(x[2])] - v0;
                const Vec3 q = d.cross(e2);
                const double det = e1.dot(q);
                if (std::fabs(det) < 1e-30) continue;
                const double inv = 1 / det;
                const Vec3 s = a - v0;
                const double u = s.dot(q) * inv;
                if (u < 0 || u > 1) continue;
                const Vec3 r = s.cross(e1);
                const double v = d.dot(r) * inv;
                if (v < 0 || u + v > 1) continue;
                const double tt = e2.dot(r) * inv;
                if (tt > 1e-6 && tt < 1 - 1e-6) return true;
            }
        }
        return false;
    }
    static double box_distance2(const Vec3& q, const Vec3& lo, const Vec3& hi) {
        double s = 0;
        for (int k = 0; k < 3; ++k) {
            const double d = q[k] < lo[k] ? lo[k] - q[k] : q[k] > hi[k] ? q[k] - hi[k] : 0;
            s += d * d;
        }
        return s;
    }
    // The closest point of triangle abc to q, as barycentric weights (Ericson's region tests).
    static Vec3 closest_on_triangle(const Vec3& q, const Vec3& a, const Vec3& b, const Vec3& c) {
        const Vec3 ab = b - a, ac = c - a, aq = q - a;
        const double d1 = ab.dot(aq), d2 = ac.dot(aq);
        if (d1 <= 0 && d2 <= 0) return {1, 0, 0};
        const Vec3 bq = q - b;
        const double d3 = ab.dot(bq), d4 = ac.dot(bq);
        if (d3 >= 0 && d4 <= d3) return {0, 1, 0};
        const double vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) {
            const double v = d1 / (d1 - d3);
            return {1 - v, v, 0};
        }
        const Vec3 cq = q - c;
        const double d5 = ab.dot(cq), d6 = ac.dot(cq);
        if (d6 >= 0 && d5 <= d6) return {0, 0, 1};
        const double vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            const double w = d2 / (d2 - d6);
            return {1 - w, 0, w};
        }
        const double va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
            const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            return {0, 1 - w, w};
        }
        const double den = 1 / (va + vb + vc);
        const double v = vb * den, w = vc * den;
        return {1 - v - w, v, w};
    }
    // The nearest triangle to q and the barycentric weights of the nearest point on it; -1 when there is none.
    int nearest(const Vec3& q, Vec3& bary) const {
        int best = -1;
        double best_d = 1e300;
        if (nodes.empty()) return -1;
        std::vector<int> stack{0};
        while (!stack.empty()) {
            const int ni = stack.back();
            stack.pop_back();
            const Node& n = nodes[size_t(ni)];
            if (box_distance2(q, n.lo, n.hi) >= best_d) continue;
            if (n.count == 0) {
                const int l = ni + 1, r = n.right;
                const double dl = box_distance2(q, nodes[size_t(l)].lo, nodes[size_t(l)].hi);
                const double dr = box_distance2(q, nodes[size_t(r)].lo, nodes[size_t(r)].hi);
                if (dl < dr) stack.push_back(r), stack.push_back(l);
                else stack.push_back(l), stack.push_back(r);
                continue;
            }
            for (int i = n.first; i < n.first + n.count; ++i) {
                const auto& x = (*t)[size_t(order[size_t(i)])];
                const Vec3 &a = (*p)[size_t(x[0])], &b = (*p)[size_t(x[1])], &c = (*p)[size_t(x[2])];
                const Vec3 w = closest_on_triangle(q, a, b, c);
                const Vec3 at = a * w.x + b * w.y + c * w.z;
                const double d = (at - q).dot(at - q);
                if (d < best_d) best_d = d, best = order[size_t(i)], bary = w;
            }
        }
        return best;
    }
};

// Nested dissection in space: a set split in half across its longest side, the vertices of the first half with a
// neighbour in the second (the separator) numbered after both halves, each half the same way.
void dissect(std::vector<int>& set, size_t first, size_t last, const std::vector<Vec3>& pts,
             const std::vector<std::vector<int>>& adj, std::vector<long long>& stamp, long long& serial, std::vector<int>& out) {
    const size_t count = last - first;
    if (count <= 64) {
        out.insert(out.end(), set.begin() + long(first), set.begin() + long(last));
        return;
    }
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (size_t i = first; i < last; ++i)
        for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], pts[size_t(set[i])][a]), hi[a] = std::max(hi[a], pts[size_t(set[i])][a]);
    int axis = 0;
    for (int a = 1; a < 3; ++a)
        if (hi[a] - lo[a] > hi[axis] - lo[axis]) axis = a;
    const size_t mid = first + count / 2;
    std::nth_element(set.begin() + long(first), set.begin() + long(mid), set.begin() + long(last),
                     [&](int a, int b) { return pts[size_t(a)][axis] < pts[size_t(b)][axis]; });
    const long long left = ++serial * 2, right = left + 1;
    for (size_t i = first; i < mid; ++i) stamp[size_t(set[i])] = left;
    for (size_t i = mid; i < last; ++i) stamp[size_t(set[i])] = right;
    std::vector<int> sep;
    size_t keep = first;
    for (size_t i = first; i < mid; ++i) {
        const int v = set[i];
        const bool cut = std::any_of(adj[size_t(v)].begin(), adj[size_t(v)].end(), [&](int u) { return stamp[size_t(u)] == right; });
        if (cut) sep.push_back(v);
        else set[keep++] = v;
    }
    // [first, keep) the left half without the separator; [mid, last) the right half.
    std::vector<int> right_half(set.begin() + long(mid), set.begin() + long(last));
    std::copy(right_half.begin(), right_half.end(), set.begin() + long(keep));
    const size_t right_first = keep, right_last = keep + right_half.size();
    if (keep - first == count || right_last - right_first == count) {  // no progress: number them as they are
        out.insert(out.end(), set.begin() + long(first), set.begin() + long(right_last));
        out.insert(out.end(), sep.begin(), sep.end());
        return;
    }
    dissect(set, first, keep, pts, adj, stamp, serial, out);
    dissect(set, right_first, right_last, pts, adj, stamp, serial, out);
    out.insert(out.end(), sep.begin(), sep.end());
}

}  // namespace

MeshLaplacian mesh_laplacian(const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& triangles,
                             LaplacianKind kind, double mollify) {
    std::vector<std::array<int, 3>> tris;
    tris.reserve(triangles.size());
    for (const auto& t : triangles)
        if (t[0] != t[1] && t[1] != t[2] && t[2] != t[0]) tris.push_back(t);
    return kind == LaplacianKind::Robust ? robust_laplacian(points, tris, mollify) : cotangent_laplacian(points, tris);
}

bool SparseCholesky::factor(const MeshLaplacian& lap, const std::vector<double>& extra, const std::vector<Vec3>& points) {
    const int n = lap.n;
    n_ = n;
    std::vector<std::vector<int>> adj(static_cast<size_t>(n));
    for (const auto& e : lap.edges) adj[size_t(e[0])].push_back(e[1]), adj[size_t(e[1])].push_back(e[0]);
    // The ordering.
    std::vector<int> set(static_cast<size_t>(n));
    std::iota(set.begin(), set.end(), 0);
    std::vector<long long> stamp(static_cast<size_t>(n), 0);
    long long serial = 0;
    perm_.clear();
    perm_.reserve(size_t(n));
    dissect(set, 0, set.size(), points, adj, stamp, serial, perm_);
    std::vector<int> iperm(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) iperm[size_t(perm_[size_t(k)])] = k;
    // The permuted matrix's upper triangle by columns: column j holds rows i <= j.
    std::vector<std::vector<std::pair<int, double>>> up(static_cast<size_t>(n));
    for (int v = 0; v < n; ++v)
        up[size_t(iperm[size_t(v)])].push_back({iperm[size_t(v)], lap.diagonal[size_t(v)] + (extra.empty() ? 0.0 : extra[size_t(v)])});
    for (size_t e = 0; e < lap.edges.size(); ++e) {
        int i = iperm[size_t(lap.edges[e][0])], j = iperm[size_t(lap.edges[e][1])];
        if (i > j) std::swap(i, j);
        up[size_t(j)].push_back({i, -lap.weight[e]});
    }
    // The elimination tree (with path compression through ancestor).
    std::vector<int> parent(static_cast<size_t>(n), -1), ancestor(size_t(n), -1);
    for (int k = 0; k < n; ++k)
        for (const auto& [i0, v] : up[size_t(k)]) {
            for (int i = i0; i != -1 && i < k;) {
                const int next = ancestor[size_t(i)];
                ancestor[size_t(i)] = k;
                if (next == -1) {
                    parent[size_t(i)] = k;
                    break;
                }
                i = next;
            }
        }
    // Row k's pattern of L: the nodes reached from A's row k up the tree, in an order the solve can use.
    std::vector<int> mark(static_cast<size_t>(n), -1), stack(static_cast<size_t>(n)), path(static_cast<size_t>(n));
    auto ereach = [&](int k) {
        int top = n;
        mark[size_t(k)] = k;
        for (const auto& [i0, v] : up[size_t(k)]) {
            if (i0 >= k) continue;
            int len = 0;
            for (int i = i0; mark[size_t(i)] != k; i = parent[size_t(i)]) path[size_t(len++)] = i, mark[size_t(i)] = k;
            while (len > 0) stack[size_t(--top)] = path[size_t(--len)];
        }
        return top;
    };
    std::vector<long long> counts(static_cast<size_t>(n), 1);  // the diagonal
    for (int k = 0; k < n; ++k)
        for (int top = ereach(k); top < n; ++top) ++counts[size_t(stack[size_t(top)])];
    lp_.assign(size_t(n) + 1, 0);
    for (int j = 0; j < n; ++j) lp_[size_t(j) + 1] = lp_[size_t(j)] + counts[size_t(j)];
    li_.assign(size_t(lp_[size_t(n)]), 0);
    lx_.assign(size_t(lp_[size_t(n)]), 0.0);
    std::vector<long long> fill(lp_.begin(), lp_.end() - 1);
    std::vector<double> x(static_cast<size_t>(n), 0.0);
    std::fill(mark.begin(), mark.end(), -1);
    for (int k = 0; k < n; ++k) {
        const int top = ereach(k);
        for (const auto& [i, v] : up[size_t(k)]) x[size_t(i)] += v;
        double d = x[size_t(k)];
        x[size_t(k)] = 0;
        for (int s = top; s < n; ++s) {
            const int i = stack[size_t(s)];
            const double lki = x[size_t(i)] / lx_[size_t(lp_[size_t(i)])];
            x[size_t(i)] = 0;
            for (long long p = lp_[size_t(i)] + 1; p < fill[size_t(i)]; ++p) x[size_t(li_[size_t(p)])] -= lx_[size_t(p)] * lki;
            d -= lki * lki;
            li_[size_t(fill[size_t(i)])] = k;
            lx_[size_t(fill[size_t(i)]++)] = lki;
        }
        if (!(d > 0)) return false;
        li_[size_t(fill[size_t(k)])] = k;
        lx_[size_t(fill[size_t(k)]++)] = std::sqrt(d);
    }
    return true;
}

void SparseCholesky::solve(std::vector<double>& b) const {
    const int n = n_;
    std::vector<double> y(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) y[size_t(k)] = b[size_t(perm_[size_t(k)])];
    for (int j = 0; j < n; ++j) {  // L y = b
        y[size_t(j)] /= lx_[size_t(lp_[size_t(j)])];
        for (long long p = lp_[size_t(j)] + 1; p < lp_[size_t(j) + 1]; ++p) y[size_t(li_[size_t(p)])] -= lx_[size_t(p)] * y[size_t(j)];
    }
    for (int j = n - 1; j >= 0; --j) {  // L' x = y
        for (long long p = lp_[size_t(j)] + 1; p < lp_[size_t(j) + 1]; ++p) y[size_t(j)] -= lx_[size_t(p)] * y[size_t(li_[size_t(p)])];
        y[size_t(j)] /= lx_[size_t(lp_[size_t(j)])];
    }
    for (int k = 0; k < n; ++k) b[size_t(perm_[size_t(k)])] = y[size_t(k)];
}

Vec3 closest_on_segment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 d = b - a;
    const double dd = d.dot(d);
    const double t = dd > 0 ? std::clamp((p - a).dot(d) / dd, 0.0, 1.0) : 0.0;
    return a + d * t;
}

void keep_four(std::vector<std::pair<int, double>>& w, int root, double floor, int* joints, float* weights) {
    // Same joint listed twice: summed.
    std::sort(w.begin(), w.end());
    std::vector<std::pair<int, double>> merged;
    for (const auto& x : w)
        if (!merged.empty() && merged.back().first == x.first) merged.back().second += x.second;
        else merged.push_back(x);
    std::stable_sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    double total = 0;
    size_t keep = 0;
    for (; keep < merged.size() && keep < 4; ++keep) {
        if (keep > 0 && merged[keep].second < floor) break;
        total += std::max(0.0, merged[keep].second);
    }
    for (size_t k = 0; k < 4; ++k) {
        if (k < keep && total > 0) {
            joints[k] = merged[k].first;
            weights[k] = float(std::max(0.0, merged[k].second) / total);
        } else if (k == 0 && !merged.empty()) {
            joints[k] = merged[0].first, weights[k] = 1.f;
        } else {
            joints[k] = root, weights[k] = 0.f;
        }
    }
}

bool bone_heat(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices,
               const std::vector<std::uint32_t>& piece, const std::vector<HeatBone>& bones, int root,
               const BoneHeatOptions& opt, std::vector<int>& joints, std::vector<float>& weights, BoneHeatStats* stats_out) {
    BoneHeatStats stats;
    auto cancelled = [&] { return opt.cancel && opt.cancel->load(); };
    auto progress = [&](double f, const char* what) {
        if (opt.progress) opt.progress(f, what);
    };
    const size_t total = positions.size() / 3;
    if (bones.empty() || piece.empty()) return false;
    for (std::uint32_t v : piece)
        if (v >= total) return false;
    // Weld vertices at one position (seams), within a millionth of the piece's size.
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (std::uint32_t v : piece)
        for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], double(positions[v * 3 + size_t(a)])), hi[a] = std::max(hi[a], double(positions[v * 3 + size_t(a)]));
    const double size = std::max((hi - lo).length(), 1e-9), q = size * 1e-6;
    struct KeyHash {
        size_t operator()(const std::array<long long, 3>& k) const {
            return size_t(k[0] * 73856093LL ^ k[1] * 19349663LL ^ k[2] * 83492791LL);
        }
    };
    std::unordered_map<std::array<long long, 3>, int, KeyHash> at;
    std::vector<int> weld(total, -1);  // per vertex of the model: its welded vertex, -1 outside the piece
    std::vector<Vec3> pts;
    for (std::uint32_t v : piece) {
        const Vec3 x{positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
        const std::array<long long, 3> key{std::llround(x.x / q), std::llround(x.y / q), std::llround(x.z / q)};
        const auto [it, fresh] = at.try_emplace(key, int(pts.size()));
        if (fresh) pts.push_back(x);
        weld[v] = it->second;
    }
    const int n = int(pts.size());
    stats.vertices = n;
    std::vector<std::array<int, 3>> tris;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= total || b >= total || c >= total || weld[a] < 0 || weld[b] < 0 || weld[c] < 0) continue;
        const std::array<int, 3> t{weld[a], weld[b], weld[c]};
        if (t[0] != t[1] && t[1] != t[2] && t[2] != t[0]) tris.push_back(t);
    }
    progress(0.05, "Finding each vertex's nearest bone");
    // Each vertex's nearest joint (over its bones) and whether that bone is in sight.
    std::vector<int> nodes, node_of;  // the joints, each once; per bone, its joint's place in nodes
    for (const HeatBone& b : bones) {
        auto it = std::find(nodes.begin(), nodes.end(), b.node);
        if (it == nodes.end()) it = nodes.insert(nodes.end(), b.node);
        node_of.push_back(int(it - nodes.begin()));
    }
    // Pieces (connected by triangles). A bone is in sight of a vertex when the line to it crosses no surface of the
    // vertex's own piece: eyeballs, teeth or a strap shut inside the body are inside it, not its outside.
    std::vector<int> up(static_cast<size_t>(n));
    std::iota(up.begin(), up.end(), 0);
    auto root_of = [&](int v) {
        while (up[size_t(v)] != v) v = up[size_t(v)] = up[size_t(up[size_t(v)])];
        return v;
    };
    for (const auto& t : tris) up[size_t(root_of(t[0]))] = root_of(t[1]), up[size_t(root_of(t[1]))] = root_of(t[2]);
    std::vector<int> piece_of(static_cast<size_t>(n)), tri_piece(tris.size());
    for (int v = 0; v < n; ++v) piece_of[size_t(v)] = root_of(v);
    for (size_t t = 0; t < tris.size(); ++t) tri_piece[t] = piece_of[size_t(tris[t][0])];
    TriBvh bvh;
    bvh.build(pts, tris);
    bvh.piece = &tri_piece;
    std::vector<double> heat(static_cast<size_t>(n), 0.0), dist(size_t(n), 0.0);
    std::vector<std::vector<int>> nearest(static_cast<size_t>(n));  // indices into nodes
    std::vector<Vec3> target(static_cast<size_t>(n));
    {
        const unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        std::atomic<int> next{0};
        std::atomic<int> hidden{0}, out_of_reach{0};
        auto work = [&] {
            std::vector<double> d(nodes.size());
            std::vector<Vec3> c(nodes.size());
            for (int v; (v = next.fetch_add(1)) < n;) {
                if ((v & 1023) == 0 && cancelled()) return;
                for (size_t k = 0; k < nodes.size(); ++k) d[k] = 1e300;
                for (size_t bi = 0; bi < bones.size(); ++bi) {
                    const HeatBone& b = bones[bi];
                    const size_t k = size_t(node_of[bi]);
                    const Vec3 x = closest_on_segment(pts[size_t(v)], b.a, b.b);
                    const double dd = (x - pts[size_t(v)]).length();
                    if (dd < d[k]) d[k] = dd, c[k] = x;
                }
                const double best = *std::min_element(d.begin(), d.end());
                for (size_t k = 0; k < nodes.size(); ++k)
                    if (d[k] <= best * (1 + 1e-9) + 1e-12) nearest[size_t(v)].push_back(int(k));
                const double dmin = std::max(best, size * 1e-6);
                dist[size_t(v)] = dmin;
                target[size_t(v)] = c[size_t(nearest[size_t(v)][0])];
                if (opt.reach > 0 && best > opt.reach) ++out_of_reach;
                else if (bvh.blocked(pts[size_t(v)], target[size_t(v)], v, piece_of[size_t(v)])) ++hidden;
                else heat[size_t(v)] = double(nearest[size_t(v)].size()) * opt.c / (dmin * dmin);
            }
        };
        std::vector<std::thread> pool;
        for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work);
        work();
        for (auto& t : pool) t.join();
        stats.hidden = hidden, stats.out_of_reach = out_of_reach;
    }
    if (cancelled()) return false;
    // A piece no bone can see is heated by its nearest bones regardless, and said so.
    std::vector<char> lit(static_cast<size_t>(n), 0), touched(size_t(n), 0);
    for (const auto& t : tris)
        for (int v : t) touched[size_t(v)] = 1;
    for (int v = 0; v < n; ++v)
        if (heat[size_t(v)] > 0) lit[size_t(root_of(v))] = 1;
    std::vector<char> counted(static_cast<size_t>(n), 0);
    for (int v = 0; v < n; ++v) {
        const int r = root_of(v);
        if (!counted[size_t(r)] && touched[size_t(v)]) counted[size_t(r)] = 1, ++stats.pieces;
        if (!lit[size_t(r)] || !touched[size_t(v)]) {
            heat[size_t(v)] = double(nearest[size_t(v)].size()) * opt.c / (dist[size_t(v)] * dist[size_t(v)]);
            ++stats.unreached;
        }
    }
    progress(0.15, "Building the Laplacian");
    auto t0 = std::chrono::steady_clock::now();
    MeshLaplacian lap = mesh_laplacian(pts, tris, opt.laplacian);
    stats.laplacian_s = seconds_since(t0);
    stats.flips = lap.flips;
    stats.negative_edges = lap.negative;
    if (cancelled()) return false;
    // Vertices the solve cannot hold (on no triangle with area) take their nearest bone outright.
    std::vector<int> solve_index(static_cast<size_t>(n), -1), solved;
    for (int v = 0; v < n; ++v)
        if (lap.mass[size_t(v)] > 0 && touched[size_t(v)]) solve_index[size_t(v)] = int(solved.size()), solved.push_back(v);
    MeshLaplacian sub;
    sub.n = int(solved.size());
    sub.diagonal.assign(solved.size(), 0.0);
    std::vector<Vec3> sub_pts(solved.size());
    std::vector<double> extra(solved.size());
    for (size_t i = 0; i < solved.size(); ++i) {
        const int v = solved[i];
        sub_pts[i] = pts[size_t(v)];
        extra[i] = lap.mass[size_t(v)] * heat[size_t(v)];
    }
    for (size_t e = 0; e < lap.edges.size(); ++e) {
        const int i = solve_index[size_t(lap.edges[e][0])], j = solve_index[size_t(lap.edges[e][1])];
        if (i < 0 || j < 0) continue;
        sub.edges.push_back({i, j});
        sub.weight.push_back(lap.weight[e]);
        sub.diagonal[size_t(i)] += lap.weight[e], sub.diagonal[size_t(j)] += lap.weight[e];
    }
    progress(0.25, "Factoring");
    t0 = std::chrono::steady_clock::now();
    SparseCholesky chol;
    const bool factored = sub.n > 0 && chol.factor(sub, extra, sub_pts);
    stats.factor_s = seconds_since(t0);
    stats.factor_nonzeros = chol.nonzeros();
    if (cancelled()) return false;
    std::vector<std::vector<std::pair<int, double>>> found(static_cast<size_t>(n));  // per vertex: (joint, weight)
    std::vector<char> by_nearest(static_cast<size_t>(n), 0);
    if (!factored && sub.n > 0) stats.failed = true;
    for (int v = 0; v < n; ++v)
        if (solve_index[size_t(v)] < 0 || stats.failed) by_nearest[size_t(v)] = 1;
    if (factored) {
        // One solve per joint that is some vertex's nearest: its heat source p (shared on ties) times M H.
        std::vector<int> sources;
        std::vector<char> is_source(nodes.size(), 0);
        for (int v : solved)
            for (int k : nearest[size_t(v)]) is_source[size_t(k)] = 1;
        for (size_t k = 0; k < nodes.size(); ++k)
            if (is_source[k]) sources.push_back(int(k));
        t0 = std::chrono::steady_clock::now();
        std::mutex lock;
        std::atomic<int> next{0}, done{0};
        double lo_w = 0, hi_w = 0;
        int out_of_range = 0;
        auto work = [&] {
            std::vector<double> b(solved.size());
            for (int s; (s = next.fetch_add(1)) < int(sources.size());) {
                if (cancelled()) return;
                const int k = sources[size_t(s)];
                for (size_t i = 0; i < solved.size(); ++i) {
                    const auto& nearby = nearest[size_t(solved[i])];
                    const bool mine = std::find(nearby.begin(), nearby.end(), k) != nearby.end();
                    b[i] = mine ? extra[i] / double(nearby.size()) : 0.0;
                }
                chol.solve(b);
                std::lock_guard<std::mutex> g(lock);
                for (size_t i = 0; i < solved.size(); ++i) {
                    const double w = b[i];
                    lo_w = std::min(lo_w, w), hi_w = std::max(hi_w, w);
                    out_of_range += w < -1e-6 || w > 1 + 1e-6;
                    if (w > opt.floor * 0.5) found[size_t(solved[i])].push_back({nodes[size_t(k)], w});
                }
                const int d = ++done;
                if (opt.progress) opt.progress(0.3 + 0.65 * d / double(sources.size()), "Spreading the heat of each bone");
            }
        };
        const unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work);
        work();
        for (auto& t : pool) t.join();
        if (cancelled()) return false;
        stats.solve_s = seconds_since(t0);
        stats.min_weight = lo_w, stats.max_weight = hi_w, stats.out_of_range = out_of_range;
    }
    // Into the model's four slots, every original vertex as its welded one.
    std::vector<int> wj(static_cast<size_t>(n) * 4);
    std::vector<float> ww(static_cast<size_t>(n) * 4);
    for (int v = 0; v < n; ++v) {
        auto& list = found[size_t(v)];
        if (by_nearest[size_t(v)] || list.empty()) {
            list.clear();
            for (int k : nearest[size_t(v)]) list.push_back({nodes[size_t(k)], 1.0 / double(nearest[size_t(v)].size())});
        }
        keep_four(list, root, opt.floor, &wj[size_t(v) * 4], &ww[size_t(v) * 4]);
    }
    if (joints.size() < total * 4) joints.resize(total * 4, root);
    if (weights.size() < total * 4) weights.resize(total * 4, 0.f);
    for (std::uint32_t v : piece)
        for (int k = 0; k < 4; ++k) {
            joints[size_t(v) * 4 + size_t(k)] = wj[size_t(weld[v]) * 4 + size_t(k)];
            weights[size_t(v) * 4 + size_t(k)] = ww[size_t(weld[v]) * 4 + size_t(k)];
        }
    progress(1.0, "Done");
    if (stats_out) *stats_out = stats;
    return true;
}

double part_coverage(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices, const DaePart& source,
                     const DaePart& target, double reach) {
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    const std::uint32_t s0 = source.first_vertex, s1 = source.first_vertex + source.vertex_count;
    for (std::uint32_t v = s0; v < s1; ++v) pts.push_back({positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]});
    for (std::uint32_t i = source.first_index; i + 2 < source.first_index + source.index_count && i + 2 < indices.size(); i += 3) {
        const std::uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= s0 && a < s1 && b >= s0 && b < s1 && c >= s0 && c < s1) tris.push_back({int(a - s0), int(b - s0), int(c - s0)});
    }
    if (tris.empty() || target.vertex_count == 0) return 0;
    TriBvh bvh;
    bvh.build(pts, tris);
    int within = 0;
    for (std::uint32_t v = target.first_vertex; v < target.first_vertex + target.vertex_count; ++v) {
        const Vec3 p{positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
        Vec3 bary;
        const int t = bvh.nearest(p, bary);
        if (t < 0) continue;
        const auto& x = tris[size_t(t)];
        const Vec3 at = pts[size_t(x[0])] * bary.x + pts[size_t(x[1])] * bary.y + pts[size_t(x[2])] * bary.z;
        within += (at - p).length() <= reach;
    }
    return double(within) / target.vertex_count;
}

bool transfer_weights(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices, const DaePart& source,
                      const DaePart& target, int root, std::vector<int>& joints, std::vector<float>& weights) {
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    const std::uint32_t s0 = source.first_vertex, s1 = source.first_vertex + source.vertex_count;
    for (std::uint32_t v = s0; v < s1; ++v) pts.push_back({positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]});
    for (std::uint32_t i = source.first_index; i + 2 < source.first_index + source.index_count && i + 2 < indices.size(); i += 3) {
        const std::uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a < s0 || a >= s1 || b < s0 || b >= s1 || c < s0 || c >= s1) continue;
        tris.push_back({int(a - s0), int(b - s0), int(c - s0)});
    }
    if (tris.empty() || joints.size() < size_t(s1) * 4) return false;
    TriBvh bvh;
    bvh.build(pts, tris);
    for (std::uint32_t v = target.first_vertex; v < target.first_vertex + target.vertex_count; ++v) {
        Vec3 bary;
        const int t = bvh.nearest({positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]}, bary);
        if (t < 0) continue;
        std::vector<std::pair<int, double>> w;
        for (int c = 0; c < 3; ++c) {
            const size_t from = size_t(tris[size_t(t)][size_t(c)]) + s0;
            for (int k = 0; k < 4; ++k)
                if (weights[from * 4 + size_t(k)] > 0) w.push_back({joints[from * 4 + size_t(k)], bary[c] * weights[from * 4 + size_t(k)]});
        }
        if (w.empty()) continue;
        keep_four(w, root, 0.005, &joints[size_t(v) * 4], &weights[size_t(v) * 4]);
    }
    return true;
}

}  // namespace vats
