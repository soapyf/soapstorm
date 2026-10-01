// Viewport Avatar Toolset - the viewport's bone glyphs as plain triangles, for the app and the viewer alike.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Every glyph is a closed solid wound counter-clockwise seen from outside, three points per triangle, so a host
// may cull back faces and should shade each triangle flat with its own normal, (b - a) x (c - a).
#pragma once

#include <vector>

#include "vats/skeleton.h"

namespace vats {

// A bone's spike (spec VP-6): a four-sided double pyramid from head to tail, widest (9 % of the length) at 15 %
// along. frame is the bone's display frame (SK-21), whose X (or Z) axis gives the roll. 8 triangles.
void bone_glyph(std::vector<Vec3>& tris, const Vec3& head, const Vec3& tail, const Quat& frame);

// A ring round axis at a joint, radius to the middle of its tube. Folded bones are drawn with it.
void joint_ring(std::vector<Vec3>& tris, const Vec3& at, const Vec3& axis, double radius);

// How each node's glyph is drawn in a pose (globals from Skeleton::global_pose, shape may be null):
//   -2  none: a zero-length tail, nothing to draw;
//   -1  a spike from the joint to the tail;
//   k   a ring at the joint: the joint's spike would lie on an earlier joint's, joint to tail either way round:
//       Bento's mSpine1..4 at rest, whose offsets cancel in pairs, and mFaceEyeAlt*, twins of mEye*. k counts
//       the rings already at that joint, so rings at one point nest instead of hiding each other.
// The bones stay where SL has them; only the glyph changes.
std::vector<int> glyph_kinds(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape);

// The world tail of node i: its display end, shape-scaled, through globals; a mesh body's rig tail when shape has one.
Vec3 glyph_tail(const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape, int i);

// Radius of ring k at a joint, for a bone len long: 16 mm, less for short bones (an eye's 25 mm), each nested
// ring half as big again.
inline double ring_radius(int k, double len) { return (len * 0.3 < 0.016 ? len * 0.3 : 0.016) * (1 + 0.55 * k); }

}  // namespace vats
