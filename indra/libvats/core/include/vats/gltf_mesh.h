// Viewport Avatar Toolset - glTF 2.0 / GLB rigged mesh import (spec 08 RG-1: stock Blender writes glTF now).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The same conventions as load_dae and load_fbx_mesh (SL space, measured rig scale, SK-40 binds, settle_rig), so
// the rest of the app cannot tell a glTF part from a COLLADA one. glTF is Y up and metres; a skinned mesh's own
// node transform is ignored, as the format says; skins bind by node name (map_skin_joint).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vats/dae.h"

namespace vats {

// dir is the file's folder, for buffers and textures beside it. load_mesh_file (fbx.h) dispatches .gltf/.glb here.
bool load_gltf_mesh(const std::vector<std::uint8_t>& bytes, const std::string& dir, const Skeleton& skel, DaeModel& out,
                    DaeReport& report, std::string& err, const SkinRemap* remap = nullptr);

}  // namespace vats
