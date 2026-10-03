// Viewport Avatar Toolset - a Bento face pose fitted to a mesh body's shape key.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 SK-5. Second Life plays bones, never shape keys: an .anim moves joints and SL meshes have no morphs.
// So a viseme or an expression a model has as a shape key reaches SL only as the face bones' pose that moves the
// skinned mesh the same way: this finds it, by least squares over the vertices the face bones carry, the key's own
// moving and the rest staying put. The bones' weights decide which bone can move which vertex; a key on vertices
// no face bone carries cannot be fitted at all.
#pragma once

#include <string>
#include <vector>

#include "vats/dae.h"
#include "vats/pose_ops.h"
#include "vats/skeleton.h"

namespace vats {

struct FaceFitOptions {
    bool positions = true;  // the bones move too (position keys: "Move face bones"); else they only turn
    int iterations = 40;    // Levenberg-Marquardt steps at most
};

struct FaceFit {
    std::vector<int> bones;  // the face bones fitted: those weighted to the key's vertices
    Pose pose;               // rest but for them: their turns, and offsets with positions
    int vertices = 0;        // the vertices the key moves
    double target_rms = 0;   // how far the key moves them (metres, root mean square)
    double residual_rms = 0; // how far the fitted pose leaves them from where the key puts them
    double max_error = 0;    // the worst of them
    double explained = 0;    // 1 - residual² / target² over them: 1 is a perfect fit, 0 no better than rest
    double reachable = 0;    // the share of the key's motion (by |d|²) on vertices some face bone carries at all
    double moved_others = 0; // root mean square of how far the pose moves vertices the key leaves still
    std::string why;         // why nothing was fitted (no such key, no face weights); empty otherwise
};

// Fits face bones to model's shape key key (full strength) on top of look (the key itself at 0, no part hidden), on a
// body with shape (the mesh body's own joints, BD-3; null for SL's). model: as loaded, with its shape keys.
FaceFit fit_face_pose(const Skeleton& skel, const Shape* shape, const DaeModel& model, const MeshLook& look,
                      const std::string& key, const FaceFitOptions& opt = {});

// How much of shape key `key`'s motion (by |d|²) falls on vertices a Bento face bone carries, 0..1: FaceFit::reachable
// without the fit. A face key (a viseme, a smile) is near 1; a body key (Emaciated) or a garment's is near 0, and the
// editor offers a face pose only for the first. 0 for an unrigged model or a key it does not have.
double shape_key_face_share(const Skeleton& skel, const DaeModel& model, const std::string& key);

// The fit as a face pose for the pose library (kind "face", like Save Face Pose): the fitted bones' Euler degrees, and
// their offsets when the fit moved them. Applied, keyed or packed as an expression like any face pose.
LibraryItem face_fit_pose(const Skeleton& skel, const FaceFit& fit, const std::string& name);

}  // namespace vats
