// Viewport Avatar Toolset - height-variant exports: the same animation baked for avatars of other heights.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 20 (HV, idea 43). Properties > Export, "Also export for heights": each height is an SL
// shape (SL Default with its Height slider set), and the export bakes IK and pins against it as against a bake shape
// (IO-13), so contacts hold on that body. The files carry the height in their names (_H175).
#pragma once

#include <string>
#include <vector>

#include "vats/export_name.h"
#include "vats/json.h"
#include "vats/shape.h"
#include "vats/skeleton.h"

namespace vats {

// HV-1: the height the shape editor shows for an avatar of this shape: the viewer's body size
// (LLVOAvatar::computeBodySize, the Z llGetAgentSize reports) plus kShapeEditorExtra, the allowance Firestorm's shape
// editor adds for the top of the head and the soles. Body size is pelvis to foot down the left leg, then torso,
// chest, neck, head and sqrt 2 x the skull, each from its joint's local position with the shape (offsets, the
// parent's scale). null = the unshaped skeleton. SL Default (female) is 1.88 m.
constexpr double kShapeEditorExtra = 0.195;
double avatar_height(const Skeleton& skel, const Shape* shape);

// HV-2: SL Default (female, or male) made height_m tall by its shape sliders, found by bisection: "Height" (id 33)
// first, then "Leg Length" (id 692) once Height is at its end. A height out of their reach gets the nearest one
// they make; reached = that height.
BodyShape height_shape(const Skeleton& skel, const AvatarParams& params, bool male, double height_m,
                       double* reached = nullptr);

// HV-3: the presets, and the range a custom height is clamped to.
constexpr double kHeightPresets[] = {1.75, 1.95, 2.15};
constexpr double kHeightMin = 1.4, kHeightMax = 2.4;
constexpr int kMaxHeights = 3;

// The export settings' "heights": numbers in metres, clamped to the range, at most kMaxHeights; none = off.
std::vector<double> export_heights(const Json& export_settings);
// "H175": the height in whole centimetres.
std::string height_tag(double height_m);

// HV-4: the files one export writes per actor, in order: for each height (0 = the bake shape itself, then the
// heights), the file and, with "both", its mirrored copy.
struct AnimVariant {
    bool mirrored = false;
    double height = 0;  // 0 = the bake shape
};
std::vector<AnimVariant> anim_variants(bool mirrored, bool both, const std::vector<double>& heights);
// export_file_name with "_<height_tag>" before the extension for a height variant.
std::string variant_file_name(const ExportNaming& naming, const std::string& project_stem, const AnimVariant& v,
                              const std::string& ext);

}  // namespace vats
