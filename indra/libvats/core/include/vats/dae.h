// Viewport Avatar Toolset - COLLADA (.dae) import for props and rigged meshes.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/03 sections 2.5 and 3.3; docs/spec/01 SK-40 (skin-target index space).
// Everything comes out in SL space (Z up, metres). Rigged meshes are skinned on the CPU by skin_prop,
// with the same conventions as AvatarMesh::skin.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vats/math.h"
#include "vats/skeleton.h"

namespace vats {

// SK-40 extended index space: [skeleton nodes 0..size-1, mRoot, collision volumes...].
inline int dae_root(const Skeleton& s) { return s.size(); }
inline int dae_volume(const Skeleton& s, int volume) { return s.size() + 1 + volume; }
inline int dae_index_count(const Skeleton& s) { return s.size() + 1 + static_cast<int>(s.volumes().size()); }

// §3.3.4 name mapping, steps 1-4: an SK-40 index, or -1 when unmapped. (Step 5, a node sid, needs the
// document and is applied inside load_dae.)
int map_skin_joint(const Skeleton& skel, std::string_view name);

struct DaeMaterial {
    std::string name;  // the COLLADA material id, or "" for the default material
    std::array<float, 4> rgba{0.82f, 0.80f, 0.78f, 1.0f};  // white tint when textured
    std::string texture;  // resolved path of the diffuse texture; empty when none or not found
    bool blend = false;   // alpha-blended (transparency or diffuse alpha < 0.999)
    bool double_sided = false;  // only the default material
};

// The triangles of one material. Its vertices are contiguous too.
struct DaeGroup {
    int material = 0;
    std::uint32_t first_vertex = 0, vertex_count = 0;
    std::uint32_t first_index = 0, index_count = 0;
};

struct DaeModel {
    std::vector<float> positions, normals;  // 3 per vertex, SL space; rigged vertices in bind pose
    std::vector<float> uvs;                 // 2 per vertex, already flipped (v' = 1 - v)
    std::vector<std::uint32_t> indices;     // 3 per triangle, CCW front faces
    std::vector<DaeGroup> groups;           // one per material, in first-use order
    std::vector<DaeMaterial> materials;

    bool rigged = false;
    // Rigged only: 4 influences per vertex, largest first, weights summing to 1. Unused slots have
    // weight 0 and the mRoot index. Joint indices are in the SK-40 space (dae_root, dae_volume).
    std::vector<int> joints;
    std::vector<float> weights;
    // Rigged only: the bind pose of every SK-40 index (rig-scaled, orthonormal). Joints the file does
    // not bind keep the SL rest pose; mRoot is identity.
    std::vector<Xform> binds;

    Vec3 bounds_min, bounds_max;  // of positions

    int vertex_count() const { return static_cast<int>(positions.size() / 3); }
    int triangle_count() const { return static_cast<int>(indices.size() / 3); }
};

// IO-38 import report.
struct DaeReport {
    int triangles = 0;
    bool rigged = false;
    bool skins_as_static = false;  // the file has skins but none maps to SL: imported at bind shape (IO-37)
    int skipped_joint_nodes = 0;
    std::vector<std::string> unmapped_joints;  // every one, in first-seen order
    double scale = 1;           // applied: unit@meter, or the rig scale for rigged files
    double measured_scale = 0;  // rigged: the median ratio before snapping (0 when nothing to measure)
    std::string up_axis = "Z_UP";
    std::vector<std::string> unsupported;       // element names and features that were skipped
    std::vector<std::string> warnings;          // broken references, bad indices, ...
    std::vector<std::string> missing_textures;  // image paths that did not resolve
};

// Parses a COLLADA 1.4/1.5 document. dae_dir is the folder of the .dae, used to find textures.
// Returns false with err set on malformed or empty files; never crashes on bad indices.
bool load_dae(std::string_view xml_text, const std::string& dae_dir, const Skeleton& skel, DaeModel& out,
              DaeReport& report, std::string& err);

// Skins a rigged model for a pose: position = sum of w * (global x own shape scale x inverse bind) * v.
// globals come from Skeleton::global_pose with the same shape (may be null). Collision volumes follow
// their joint (SK-40); mRoot is identity. A static model is copied through. 3 floats per vertex out.
void skin_prop(const DaeModel& model, const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape,
               std::vector<float>& positions, std::vector<float>& normals);

// Spec 08 BD-3: the proportions a mesh body was rigged to. Every joint whose bind position (in any of the
// parts, first part wins) differs from the unshaped SL rest by more than tol_m is moved there; every other
// joint keeps base (or no shape when base is null). Scales always come from base. Returns false, leaving
// out as base, when no part overrides a joint.
bool shape_from_binds(const Skeleton& skel, const std::vector<const DaeModel*>& parts, const Shape* base, Shape& out,
                      double tol_m = 0.001);

}  // namespace vats
