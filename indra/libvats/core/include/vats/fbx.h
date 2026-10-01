// Viewport Avatar Toolset - FBX import through ufbx (third_party/ufbx, MIT).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/07 RT-2.3 (animation for retargeting) and 08 BD-1 (rigged mesh bodies). Built only
// with the CMake option VATS_FBX (default ON); without it both readers return false with an error.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vats/dae.h"
#include "vats/retarget.h"

namespace vats {

// Skeleton animation for retargeting: joints are the bones (and their ancestors) in the source's own
// axes and units; the first animation stack is sampled at the file's frame rate.
bool read_fbx_source(const std::vector<std::uint8_t>& bytes, SourceAnim& out, std::string& err);

// Meshes as a DaeModel in SL space, rigged when any skin cluster maps to an SL joint (map_skin_joint).
// Every mesh in the file is merged. dir is the file's folder, used to find textures.
bool load_fbx_mesh(const std::vector<std::uint8_t>& bytes, const std::string& dir, const Skeleton& skel, DaeModel& out,
                   DaeReport& report, std::string& err);

// .dae or .fbx by extension: the one entry point for props and mesh bodies.
bool load_mesh_file(const std::string& path, const Skeleton& skel, DaeModel& out, DaeReport& report, std::string& err);

}  // namespace vats
