// Viewport Avatar Toolset - weight painting: touching up a rigged mesh's skin weights with a brush.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 6 (RG-15). A dab changes one joint's weight on the vertices within the brush, more at its
// centre; every other weight of each vertex makes room or takes up the rest, so each vertex still sums to 1 and keeps
// at most SL's four influences. The brush works on the model's rest positions (the caller maps a hit on the posed
// mesh back), so a mirrored dab is the same dab across the body's middle (y to -y) on the mirrored joint.
#pragma once

#include <vector>

#include "vats/dae.h"
#include "vats/skeleton.h"

namespace vats {

enum class PaintOp { Add, Subtract, Smooth };
enum class PaintFalloff { Smooth, Linear, Constant };

struct PaintBrush {
    PaintOp op = PaintOp::Add;
    double radius = 0.05;  // metres
    double strength = 0.5;  // 0..1: how far one dab goes at the centre
    PaintFalloff falloff = PaintFalloff::Smooth;
    // Only the surface under the brush (connected to the vertex nearest its centre within its reach); off, every vertex
    // within the radius, across gaps too.
    bool connected = true;
};

// Vertices at one position (seams) paint as one, and smoothing reads each one's neighbours along the mesh's edges.
struct PaintMesh {
    std::vector<int> weld;                  // per vertex: its welded vertex
    std::vector<std::vector<int>> members;  // per welded vertex: the vertices at it
    std::vector<Vec3> at;                   // per welded vertex: where it is (rest)
    std::vector<std::vector<int>> next;     // per welded vertex: its neighbours
};
PaintMesh paint_mesh(const DaeModel& model);

// How much of a dab reaches distance d from its centre (0..1, times the strength).
double paint_falloff(const PaintBrush& brush, double d);

// One dab of brush at centre on joint (an SK-40 index) over model's weights. Returns how many vertices changed. A
// vertex whose last weight is taken away gives it to the joint its neighbours carry most, else to the joint's parent.
// seed: a vertex of the surface the brush is on (a corner of the triangle under the pointer), so a connected brush keeps
// to that surface and not one just under it (a body under a garment); -1 takes the vertex nearest the centre.
int paint_dab(const Skeleton& skel, DaeModel& model, const PaintMesh& mesh, int joint, const Vec3& centre, const PaintBrush& brush,
              int seed = -1);
// The vertex nearest p, of within's vertices when given: where a mirrored dab starts on the same part.
int nearest_vertex(const DaeModel& model, const Vec3& p, const DaePart* within = nullptr);

// The dab's twin across the body's middle: the centre's mirror image (y to -y) and the mirrored joint's SK-40 index
// (itself for one on the middle).
Vec3 mirror_point(const Vec3& p);
int mirror_joint(const Skeleton& skel, int joint);

// The SK-40 index of a skeleton node (a joint, or a collision volume's node), -1 for an attachment point.
int sk40_of_node(const Skeleton& skel, int node);
// The other way: the skeleton node of an SK-40 index (a joint, or a collision volume's node); -1 for mRoot or none.
int node_of_sk40(const Skeleton& skel, int sk40);
// The joint (SK-40) that carries most of a vertex's weight, -1 when it has none: what a click on the body picks.
int dominant_joint(const DaeModel& model, int vertex);

// A stroke (every dab from press to release) as one undo step: the vertices it changed, before and after.
struct PaintStroke {
    std::vector<int> vertices;
    std::vector<int> joints_before, joints_after;     // 4 per vertex listed
    std::vector<float> weights_before, weights_after;
    bool empty() const { return vertices.empty(); }
};
// begin_stroke keeps the model's weights as the press found them; end_stroke turns them into the stroke's changes.
std::vector<float> begin_stroke(const DaeModel& model, std::vector<int>& joints);
PaintStroke end_stroke(const DaeModel& model, const std::vector<int>& joints_before, const std::vector<float>& weights_before);
// Puts a stroke's vertices back as they were before it (or, redo, after it).
void undo_stroke(const PaintStroke& stroke, DaeModel& model, bool redo = false);

}  // namespace vats
