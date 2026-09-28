// Viewport Avatar Toolset - retargeting a whole folder, and saved mappings.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/07 RT-12..RT-14. Each file goes through the same steps as the Retarget dialog (retarget, foot
// clean-up, fit_to_limits); the result is one project or .anim per file and a report row.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "vats/retarget.h"
#include "vats/rig.h"

namespace vats {

// RT-12: a mapping made by hand, as a rig table ({"name", "bones": {SL joint: [source name]}}) with both sides
// written out, so parse_rig_table reads back the same map. Unmapped right-side joints are written as [] (the
// parser would otherwise copy the left side's names).
std::string rig_table_json(const std::string& name, const BoneMap& map, const SourceAnim& src);
// Every rig table (*.json) in dir, sorted by name; files that are not rig tables are skipped.
std::vector<RigTable> load_rig_tables(const std::string& dir);

// RT-14: the source is a Mixamo rig (a joint name holds "mixamorig", ignoring case).
bool is_mixamo(const SourceAnim& src);

// Reads a .bvh, .fbx, .gltf or .glb file into a SourceAnim, as File > Import Animation (Retarget)... does.
bool read_source_file(const std::string& path, SourceAnim& out, std::string& err);

struct BatchRetargetOptions {
    std::vector<RigTable> tables;
    int table = -1;  // index into tables; -1 = the best match for each file (best_rig_table)
    RetargetOptions retarget;
    FitOptions fit;
    bool lock_feet = true;  // RT-9 clean-up
    bool anim = false;      // write .anim files instead of projects
};

struct BatchRow {
    std::string file;    // the source file's name
    std::string output;  // the written file's name; "" = nothing written
    bool fits = false;
    std::size_t bytes = 0;  // the .anim size after fitting (0 when nothing was made)
    int frames = 0, fps = 0;
    std::string notes;  // the rig used, fitting steps, why it failed
};

struct BatchReport {
    std::string out_dir;  // <folder>/retargeted
    std::vector<BatchRow> rows;
    bool mixamo = false;  // some source was a Mixamo rig (RT-14)
};

// Writes data to path; false with why. The app passes its write with a .bak of any file it replaces.
using BatchWrite = std::function<bool(const std::string& path, const std::string& data, std::string& why)>;

// RT-13: every .bvh, .fbx, .gltf and .glb directly in folder (not its subfolders), sorted by name, into
// <folder>/retargeted/<stem>.vat or .anim. A stem already written in this run gets its extension added
// (walk_fbx.vat). A file that cannot be read or mapped gets a row with the reason and no output.
BatchReport batch_retarget(const Skeleton& skel, const Rig& rig, const std::string& folder,
                           const BatchRetargetOptions& opt, const BatchWrite& write);

}  // namespace vats
