// Viewport Avatar Toolset - expression packs: one short face-only .anim per expression, for expression HUDs.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 25 (EX). An expression is ARKit weights (the starter set) or a saved face pose; its
// clip keys only the face bones it moves, holding the face (or blinking, or breathing) with the .anim's own ease
// in and out, so a HUD can start and stop it over any body animation.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/facecap.h"
#include "vats/pose_ops.h"

namespace vats {

struct Expression {
    enum class Motion { Hold, Blink, Breathe };
    std::string name;                         // "smile", "wink L"; the file name comes from it
    std::map<std::string, double> weights;    // ARKit weights (VRM presets allowed)
    std::map<std::string, Vec3> bones;        // a face pose instead: Euler degrees
    std::map<std::string, Vec3> offsets;      // and its offsets, metres
    Motion motion = Motion::Hold;
};

// The starter set: smile, big smile, frown, surprise, wink L, wink R, angry, sad, blink loop, idle breathing,
// kiss, tongue out.
std::vector<Expression> starter_expressions();
// A saved face pose (LibraryItem kind "face") as an expression.
Expression face_pose_expression(const LibraryItem& pose);

struct ExpressionPackOptions {
    std::string prefix = "Face";  // file names: <prefix>_<name>
    int priority = 4;
    double length = 2;            // seconds a held expression lasts (blink and breathing loops are 4 s)
    double ease_in = 0.3, ease_out = 0.3;
    bool loop = true;             // held expressions loop (hold until stopped); blink and breathing always loop
    bool positions = false;       // Move face bones
    double scale = 1;             // FaceSettings::scale: the face moves sized for the bake shape's face
    int fps = 30;
};

// "<prefix>_<name>": lowercase, anything but letters and digits as one "_", at most 63 characters (SL's
// inventory limit). "Face", "wink L" -> "Face_wink_l".
std::string expression_anim_name(const std::string& prefix, const std::string& name);

// The expression's clip: face bones only (those it moves), its ease, priority and loop. No curves = the
// expression moves nothing (with positions off, a shape that only moves bones).
Clip expression_clip(const FaceTable& table, const Expression& e, const ExpressionPackOptions& opt);

struct PackFile {
    std::string name;  // without ".anim"
    Clip clip;
};
// A clip per expression that moves something, named by expression_anim_name ("_2", "_3" after a name already
// used); the names of those that move nothing go to skipped.
std::vector<PackFile> expression_pack(const FaceTable& table, const std::vector<Expression>& list,
                                      const ExpressionPackOptions& opt, std::vector<std::string>* skipped = nullptr);

}  // namespace vats
