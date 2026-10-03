// Viewport Avatar Toolset - a Bento face pose fitted to a mesh body's shape key. See vats/face_fit.h.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/face_fit.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace vats {
namespace {

// The bones a face pose may move: Bento's face (the eyes, lids, brows, lips, jaw, ears, nose...), never the head.
bool face_bone(const Skeleton& skel, int j) {
    return j >= 0 && j < skel.joint_count() && !skel[j].volume && skel[j].category == Category::Face;
}

// Solves a x = b (n x n, row-major) by elimination with partial pivoting. False when singular.
bool solve(std::vector<double> a, std::vector<double> b, std::vector<double>& x) {
    const size_t n = b.size();
    for (size_t c = 0; c < n; ++c) {
        size_t p = c;
        for (size_t r = c + 1; r < n; ++r)
            if (std::fabs(a[r * n + c]) > std::fabs(a[p * n + c])) p = r;
        if (!(std::fabs(a[p * n + c]) > 1e-300)) return false;
        if (p != c) {
            for (size_t k = 0; k < n; ++k) std::swap(a[p * n + k], a[c * n + k]);
            std::swap(b[p], b[c]);
        }
        for (size_t r = c + 1; r < n; ++r) {
            const double f = a[r * n + c] / a[c * n + c];
            if (f == 0) continue;
            for (size_t k = c; k < n; ++k) a[r * n + k] -= f * a[c * n + k];
            b[r] -= f * b[c];
        }
    }
    x.assign(n, 0);
    for (size_t c = n; c-- > 0;) {
        double s = b[c];
        for (size_t k = c + 1; k < n; ++k) s -= a[c * n + k] * x[k];
        x[c] = s / a[c * n + c];
    }
    return true;
}

}  // namespace

FaceFit fit_face_pose(const Skeleton& skel, const Shape* shape, const DaeModel& model, const MeshLook& look,
                      const std::string& key, const FaceFitOptions& opt) {
    FaceFit fit;
    fit.pose = Pose(size_t(skel.size()));
    // The body as shown but for the key, every part in: the key's offsets index its vertices.
    MeshLook base_look = look;
    base_look.hidden.clear();
    base_look.keys[key] = 0;
    const DaeModel base = shown_model(model, base_look);
    const size_t nv = base.positions.size() / 3;
    std::vector<Vec3> d(nv);
    bool found = false;
    for (const DaeShapeKey& k : model.shape_keys) {
        if (k.name != key) continue;
        found = true;
        for (size_t i = 0; i < k.vertices.size() && i * 3 + 2 < k.dpos.size(); ++i)
            if (k.vertices[i] < nv) d[k.vertices[i]] += Vec3{k.dpos[i * 3], k.dpos[i * 3 + 1], k.dpos[i * 3 + 2]};
    }
    if (!found) return fit.why = "the model has no shape key " + key, fit;
    if (!base.rigged || base.joints.size() < nv * 4 || base.weights.size() < nv * 4) return fit.why = "the model is not rigged", fit;

    // The vertices the key moves, the face bones weighted to them, and how much of the motion those bones carry.
    std::vector<char> moved(nv, 0);
    std::map<int, double> carry;  // face bone -> its weight over the moved vertices
    double total = 0, reachable = 0;
    for (size_t v = 0; v < nv; ++v) {
        if (d[v].length() < 1e-5) continue;  // under 0.01 mm: still
        moved[v] = 1;
        ++fit.vertices;
        double face = 0;
        for (int k = 0; k < 4; ++k)
            if (const int j = base.joints[v * 4 + k]; face_bone(skel, j) && base.weights[v * 4 + k] > 0)
                face += base.weights[v * 4 + k], carry[j] += base.weights[v * 4 + k];
        total += d[v].dot(d[v]);
        if (face > 0) reachable += d[v].dot(d[v]);
    }
    if (!fit.vertices) return fit.why = "the shape key " + key + " moves nothing", fit;
    fit.target_rms = std::sqrt(total / fit.vertices);
    fit.reachable = total > 0 ? reachable / total : 0;
    for (const auto& [j, w] : carry)
        if (w > 1e-3) fit.bones.push_back(j);
    if (fit.bones.empty()) {
        fit.residual_rms = fit.max_error = 0;
        for (size_t v = 0; v < nv; ++v) fit.max_error = std::max(fit.max_error, moved[v] ? d[v].length() : 0.0);
        fit.residual_rms = fit.target_rms;
        return fit.why = "no face bone carries the vertices " + key + " moves: they are weighted to other bones (the head, "
                         "most likely), so no Bento pose can move them",
               fit;
    }

    // Every vertex those bones carry takes part: the key's to their place, the others to stay where they are.
    std::vector<char> fitted(size_t(skel.size()), 0);
    for (int j : fit.bones) fitted[size_t(j)] = 1;
    for (int j = 0; j < skel.size(); ++j)  // and what hangs from them (teeth, tongue on the jaw): parents come first
        if (skel[j].parent >= 0 && fitted[size_t(skel[j].parent)]) fitted[size_t(j)] = 1;
    DaeModel sub;
    sub.rigged = true;
    sub.binds = base.binds, sub.bound = base.bound;
    std::vector<size_t> src;  // sub vertex -> base vertex
    for (size_t v = 0; v < nv; ++v) {
        bool carried = moved[v];
        for (int k = 0; k < 4 && !carried; ++k) {
            const int j = base.joints[v * 4 + k];
            carried = j >= 0 && j < skel.size() && fitted[size_t(j)] && base.weights[v * 4 + k] > 0;
        }
        if (!carried) continue;
        src.push_back(v);
        for (int c = 0; c < 3; ++c) sub.positions.push_back(base.positions[v * 3 + c]), sub.normals.push_back(base.normals[v * 3 + c]);
        for (int k = 0; k < 4; ++k) sub.joints.push_back(base.joints[v * 4 + k]), sub.weights.push_back(base.weights[v * 4 + k]);
    }
    std::vector<float> pos, nrm;
    skin_prop(sub, skel, skel.global_pose(fit.pose, shape), shape, pos, nrm);
    std::vector<double> target(pos.size());
    for (size_t i = 0; i < src.size(); ++i)
        for (int c = 0; c < 3; ++c) target[i * 3 + c] = pos[i * 3 + c] + d[src[i]][c];

    // Parameters: per bone a rotation vector (radians) in its own frame, then with positions an offset (metres).
    const int per = opt.positions ? 6 : 3;
    const size_t np = fit.bones.size() * size_t(per);
    auto pose_of = [&](const std::vector<double>& x) {
        Pose p(size_t(skel.size()));
        for (size_t b = 0; b < fit.bones.size(); ++b) {
            const Vec3 r{x[b * per], x[b * per + 1], x[b * per + 2]};
            p.rot[size_t(fit.bones[b])] = Quat::axis_angle(r, r.length());
            if (opt.positions) p.offset[size_t(fit.bones[b])] = {x[b * per + 3], x[b * per + 4], x[b * per + 5]};
        }
        return p;
    };
    auto residual = [&](const std::vector<double>& x, std::vector<double>& r) {
        skin_prop(sub, skel, skel.global_pose(pose_of(x), shape), shape, pos, nrm);
        r.resize(pos.size());
        double s = 0;
        for (size_t i = 0; i < pos.size(); ++i) r[i] = pos[i] - target[i], s += r[i] * r[i];
        return s;
    };
    std::vector<double> x(np, 0), r, rt, step;
    double cost = residual(x, r), lambda = 1e-3;
    std::vector<double> jac(r.size() * np);
    for (int it = 0; it < opt.iterations; ++it) {
        // Central differences, steps well over the float rounding of skinned positions at head height (~1e-7 m): a
        // bone a few millimetres from its vertices moves them only ~3e-6 m for 1e-3 rad.
        std::vector<double> rm;
        for (size_t p = 0; p < np; ++p) {
            const double h = size_t(p % per) < 3 ? 1e-3 : 1e-4;  // radians, metres
            std::vector<double> xp = x, xm = x;
            xp[p] += h, xm[p] -= h;
            residual(xp, rt);
            residual(xm, rm);
            for (size_t i = 0; i < r.size(); ++i) jac[i * np + p] = (rt[i] - rm[i]) / (2 * h);
        }
        std::vector<double> a(np * np, 0), g(np, 0);
        for (size_t i = 0; i < r.size(); ++i) {
            const double* row = &jac[i * np];
            for (size_t p = 0; p < np; ++p) {
                if (row[p] == 0) continue;
                g[p] -= row[p] * r[i];
                for (size_t q = p; q < np; ++q) a[p * np + q] += row[p] * row[q];
            }
        }
        for (size_t p = 0; p < np; ++p)
            for (size_t q = 0; q < p; ++q) a[p * np + q] = a[q * np + p];
        bool better = false;
        for (int tries = 0; tries < 8 && !better; ++tries) {
            std::vector<double> damped = a;
            for (size_t p = 0; p < np; ++p) damped[p * np + p] += lambda * (a[p * np + p] + 1e-12);
            if (!solve(damped, g, step)) break;
            std::vector<double> xn = x;
            for (size_t p = 0; p < np; ++p) xn[p] += step[p];
            const double c = residual(xn, rt);
            if (c < cost) {
                better = true;
                const double gain = cost - c;
                x = xn, r = rt, cost = c, lambda = std::max(lambda / 3, 1e-9);
                if (gain < cost * 1e-9 + 1e-18) it = opt.iterations;  // settled
            } else {
                lambda *= 4;
            }
        }
        if (!better) break;
    }

    // The fit, measured over the key's vertices; and how far it moves the vertices the key leaves still.
    fit.pose = pose_of(x);
    residual(x, r);
    double err = 0, others = 0;
    int still = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        const double e = Vec3{r[i * 3], r[i * 3 + 1], r[i * 3 + 2]}.length();
        if (moved[src[i]]) err += e * e, fit.max_error = std::max(fit.max_error, e);
        else others += e * e, ++still;
    }
    fit.residual_rms = std::sqrt(err / fit.vertices);
    fit.explained = total > 0 ? 1 - err / total : 0;
    fit.moved_others = still ? std::sqrt(others / still) : 0;
    return fit;
}

double shape_key_face_share(const Skeleton& skel, const DaeModel& model, const std::string& key) {
    const size_t nv = model.positions.size() / 3;
    if (!model.rigged || model.joints.size() < nv * 4 || model.weights.size() < nv * 4) return 0;
    double total = 0, face = 0;
    for (const DaeShapeKey& k : model.shape_keys) {
        if (k.name != key) continue;
        for (size_t i = 0; i < k.vertices.size() && i * 3 + 2 < k.dpos.size(); ++i) {
            const size_t v = k.vertices[i];
            if (v >= nv) continue;
            const double d2 = double(k.dpos[i * 3]) * k.dpos[i * 3] + double(k.dpos[i * 3 + 1]) * k.dpos[i * 3 + 1] +
                              double(k.dpos[i * 3 + 2]) * k.dpos[i * 3 + 2];
            total += d2;
            for (int s = 0; s < 4; ++s)
                if (face_bone(skel, model.joints[v * 4 + s]) && model.weights[v * 4 + s] > 0) {
                    face += d2;
                    break;
                }
        }
    }
    return total > 0 ? face / total : 0;
}

LibraryItem face_fit_pose(const Skeleton& skel, const FaceFit& fit, const std::string& name) {
    LibraryItem it;
    it.id = new_item_id();
    it.kind = "face";
    it.name = name;
    for (int j : fit.bones) {
        it.bones[skel[j].name] = quat_to_euler(fit.pose.rot[size_t(j)]);
        if (fit.pose.offset[size_t(j)].length() > 1e-7) it.offsets[skel[j].name] = fit.pose.offset[size_t(j)];
    }
    return it;
}

}  // namespace vats
