// Viewport Avatar Toolset - automatic skin weights by bone heat, solved on the robust Laplacian.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 6 (RG-1, RG-13). Written from the papers, no code taken from anywhere:
// - Bone heat: I. Baran and J. Popovic, "Automatic Rigging and Animation of 3D Characters", SIGGRAPH 2007, section 4.
//   Each bone's weight w is the steady heat over the surface, -Laplacian(w) + H w = H p: a vertex's nearest bone heats it
//   (p = 1 for that bone) with strength H = c / d^2 when the straight line from the vertex to the bone stays inside the
//   body (crosses no surface), else 0, and heat diffuses over the surface between.
// - The Laplacian: N. Sharp and K. Crane, "A Laplacian for Nonmanifold Triangle Meshes", SGP 2020: the intrinsic
//   Delaunay cotangent Laplacian of the mesh's tufted cover, its edge lengths mollified first. It has no negative
//   weights, so the heat stays between 0 and 1 and smooth on holes, non-manifold edges, overlapping and degenerate
//   triangles and separate pieces, where the plain cotangent Laplacian does not.
// Geodesic voxel binding (voxel distances from the skeleton through the body) is deliberately not used here.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vats/dae.h"
#include "vats/math.h"

namespace vats {

enum class LaplacianKind { Robust, Cotangent };

// A surface Laplacian in its weak form: L (symmetric, positive semi-definite, each row summing to 0; off-diagonal
// entries are minus the edge weights) and the lumped vertex areas M. Laplacian(f) at vertex i is -(L f)_i / M_i.
struct MeshLaplacian {
    int n = 0;
    std::vector<std::array<int, 2>> edges;  // i < j, each once
    std::vector<double> weight;             // per edge: (cot a + cot b) / 2, summed over the faces on it
    std::vector<double> diagonal;           // sum of the weights at each vertex
    std::vector<double> mass;               // lumped area per vertex
    int flips = 0;                          // Robust: intrinsic edge flips made
    double mollified = 0;                   // Robust: the length added to every edge
    int negative = 0;                       // edges with a negative weight (the Robust one has none, up to round-off)
};

// The Laplacian of a triangle mesh: points, and triangles indexing them (degenerate triangles, repeated triangles,
// non-manifold edges and boundaries are all allowed). mollify: Robust only, the smallest triangle inequality slack as a
// fraction of the mean edge length (the paper's default 1e-5).
MeshLaplacian mesh_laplacian(const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& triangles,
                             LaplacianKind kind = LaplacianKind::Robust, double mollify = 1e-5);

// Sparse Cholesky factorisation of a symmetric positive-definite matrix, ordered by nested dissection on the points'
// positions (a mesh's graph splits cleanly in space), then solved for any number of right-hand sides.
class SparseCholesky {
public:
    // A = L + diag(extra) for the Laplacian lap (extra: one per vertex, may be empty). points: the vertices, for the
    // ordering. False when A is not positive definite (a piece with no heat source).
    bool factor(const MeshLaplacian& lap, const std::vector<double>& extra, const std::vector<Vec3>& points);
    // Solves A x = b in place. Safe from several threads at once.
    void solve(std::vector<double>& b) const;
    long long nonzeros() const { return static_cast<long long>(li_.size()); }

private:
    int n_ = 0;
    std::vector<int> perm_;  // perm_[k]: the original index of the k-th unknown
    std::vector<long long> lp_;
    std::vector<int> li_;
    std::vector<double> lx_;
};

// A bone for bone heat: the segment a-b, whose weights go to node (an SK-40 index). a == b is a point bone.
struct HeatBone {
    int node = -1;
    Vec3 a, b;
};

struct BoneHeatOptions {
    LaplacianKind laplacian = LaplacianKind::Robust;
    double c = 1.0;  // the heat constant; the paper finds c = 1 works
    // Weights below this are dropped before the four largest are kept (the rest renormalised).
    double floor = 0.005;
    // Metres; 0 for none. A vertex farther than this from its nearest bone (a scarf's streamer or a cape's hem out in
    // the air) is not heated by whichever bone happens to be nearest there: it takes its weights from its piece, from
    // where it hangs. A piece wholly out of reach is heated by its nearest bones regardless.
    double reach = 0;
    // Progress 0..1 and what runs; may be null. cancel: checked between steps; may be null.
    std::function<void(double, const std::string&)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

struct BoneHeatStats {
    int vertices = 0;          // welded vertices solved
    int hidden = 0;            // vertices whose nearest bone is behind the surface: they take heat from their neighbours
    int out_of_reach = 0;      // vertices out of reach of every bone (BoneHeatOptions::reach): likewise
    int unreached = 0;         // vertices in pieces no bone can see: heated by their nearest bones regardless
    int pieces = 0;            // connected pieces
    int flips = 0;             // Robust: intrinsic flips
    int negative_edges = 0;    // Cotangent: edges with a negative weight
    double min_weight = 0, max_weight = 0;  // over the raw solutions, before clamping: 0 and 1 when well behaved
    int out_of_range = 0;      // raw weights below -1e-6 or above 1 + 1e-6
    double laplacian_s = 0, factor_s = 0, solve_s = 0;
    long long factor_nonzeros = 0;
    bool failed = false;       // the factorisation failed (not positive definite): weights fell back to nearest bone
};

// Bone heat on one piece of a model: the vertices listed in piece and the triangles among indices that use only those
// (others are skipped). Vertices at the same position are welded first, so seams (UV, normal or material splits) weigh
// alike. joints and weights (grown to 4 per vertex of the model when short) get 4 per vertex of the piece: SK-40
// indices, largest first, summing to 1, unused slots the mRoot index root and weight 0. A bone is in sight of a vertex when the line
// to it crosses no triangle of the vertex's own connected piece: a garment over a body is not hidden from its bones by the
// body, nor a face by the eyeballs inside the head. False when cancelled.
bool bone_heat(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices,
               const std::vector<std::uint32_t>& piece, const std::vector<HeatBone>& bones, int root,
               const BoneHeatOptions& opt, std::vector<int>& joints, std::vector<float>& weights,
               BoneHeatStats* stats = nullptr);

// The closest point of a segment to p.
Vec3 closest_on_segment(const Vec3& p, const Vec3& a, const Vec3& b);

// Weight transfer (nearest surface point): each vertex of the target range takes the weights of the nearest point on
// the source range's triangles, blended from that triangle's corners, four at most, summing to 1. Both ranges are of
// the same model's vertices (the source already weighted in joints/weights). False when the source has no triangles.
bool transfer_weights(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices, const DaePart& source,
                      const DaePart& target, int root, std::vector<int>& joints, std::vector<float>& weights);

// How much of the target range lies within reach of the source range's surface (0..1): a garment over a body is
// nearly all of it.
double part_coverage(const std::vector<float>& positions, const std::vector<std::uint32_t>& indices, const DaePart& source,
                     const DaePart& target, double reach);

// Keeps the four largest weights (after the first, dropping any below floor), renormalised to sum to 1, largest first;
// the slots left get root and 0. Joints listed twice are summed first.
void keep_four(std::vector<std::pair<int, double>>& w, int root, double floor, int* joints, float* weights);

}  // namespace vats
