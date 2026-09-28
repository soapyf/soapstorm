// Viewport Avatar Toolset - converting between clips and .anim files.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/03 sections 2.1-2.2, docs/spec/02 AM-120.
#pragma once

#include <string>
#include <vector>

#include "vats/anim_file.h"
#include "vats/clip.h"
#include "vats/skeleton.h"

namespace vats {

struct AnimExportOptions {
    double reduce_rot_deg = 0.05;  // 0 and 0 = keep every frame
    double reduce_pos_m = 0.0005;
    int max_gap = 60;              // frames between kept keys
    const Shape* shape = nullptr;  // body IK and pins are baked against (IO-13); null = no shape
    ExternalTarget external;       // cross-actor pin targets (GR-4); empty = those pins are skipped
    // The joint positions non-pelvis position keys are written from: the skeleton's plus this shape's offsets
    // (the worn avatar with its mesh joint positions, "Your avatar"); null = the skeleton's defaults (IO-11).
    const Shape* positions = nullptr;
    // Joints whose position a worn mesh overrides (the viewer's avatar), by name. Without positions, face bones
    // with position keys among them get a warning: the keys pull a mesh head towards the default face.
    std::vector<std::string> worn_overrides;
    // IO-11b ("Leave out bones that don't move", off by default): joints other than the pelvis whose rotation stays
    // within reduce_rot_deg of rest on every frame get no rotation keys, so other animations (an AO's blinks) move
    // them; a joint left with neither rotations nor positions gets no record.
    bool leave_out_static_rotations = false;
};

struct AnimExportResult {
    AnimFile file;
    std::vector<std::string> errors;    // non-empty = do not write
    std::vector<std::string> warnings;
    int static_positions = 0;  // joints whose position channels moved nothing and were left out (IO-11a)
    int static_rotations = 0;  // joints whose rotations stayed at rest and were left out (IO-11b, when asked)
};

// Samples the clip on every integer frame and packs it the way the viewer does. With IK in use or
// pins present the samples come from the full evaluation (AM-120, IO-8).
AnimExportResult export_anim(const Skeleton& skel, const Clip& clip, const AnimExportOptions& opt = {});

struct AnimImportResult {
    Clip clip;
    std::vector<std::string> report;  // remapped or unknown joints, legacy format, fps guess
};

// IO-11a: true when node's offset stays within tol of zero on every frame. Export writes no position keys
// for such a joint (the pelvis aside): they would only pin it to the export's joint position, overriding a
// worn mesh's own.
bool static_position(const std::vector<Pose>& frames, int node, double tol);
// IO-11b: true when node's rotation stays within tol_deg of rest on every frame.
bool static_rotation(const std::vector<Pose>& frames, int node, double tol_deg);

// Builds a clip from a parsed .anim. fps_override > 0 skips the frame-rate guess.
AnimImportResult import_anim(const Skeleton& skel, const AnimFile& file, int fps_override = 0);

// IO-22 raw import: the parsed file and the clip it became. While the clip is unedited, re-export
// writes the original codes, so a foreign .anim (keys off whole frames) comes back byte for byte.
struct RawAnim {
    AnimFile file;
    Clip clip;  // as imported
};
// The original file when clip still matches the import (props aside: they are not in a .anim); else null.
const AnimFile* raw_reexport(const RawAnim& raw, const Clip& clip);

// Keys to keep so that linear playback of samples stays within tol of every sample
// (tol is degrees for rotations, metres for positions). The first and last samples and every
// anchor (a frame where the source curves have a key) are always kept, which makes
// export(import(file)) keep the file's keys.
std::vector<int> reduce_rotation_keys(const std::vector<Quat>& samples, double tol_deg, int max_gap,
                                      const std::vector<char>& anchors = {});
std::vector<int> reduce_position_keys(const std::vector<Vec3>& samples, double tol_m, int max_gap,
                                      const std::vector<char>& anchors = {});

}  // namespace vats
