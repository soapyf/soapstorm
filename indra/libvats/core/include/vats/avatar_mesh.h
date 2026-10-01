// Viewport Avatar Toolset - the SL system avatar body mesh (.llm), morphed and skinned on the CPU.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Produces plain vertex and index arrays for the app to upload. Spec: docs/spec/01 sections 2.5-2.6.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vats/math.h"
#include "vats/shape.h"
#include "vats/skeleton.h"

namespace vats {

// SLDefault / SLDefaultMale: the shape SL gives a new avatar (SK-I3), female or male.
enum class Body { Female, Male, SkeletonOnly, SLDefault, SLDefaultMale };

// The shape FK runs with for a body: the male shape for Male, none (null) otherwise. The SL default
// shapes live in AvatarMesh: use AvatarMesh::shape(body), which covers every body.
inline const Shape* body_shape(const Skeleton& skel, Body body) {
    return body == Body::Male ? &skel.male_shape() : nullptr;
}

struct LlmMorph {
    std::string name;
    std::vector<std::uint32_t> index;  // vertex per entry
    std::vector<float> coord, normal;  // 3 floats per entry
};

// One LOD-0 .llm file (SK-31). Binormals, UVs and detail UVs are skipped.
struct LlmMesh {
    std::vector<float> coords, normals;  // 3 floats per vertex
    std::vector<float> weights;          // one per vertex; empty when the file has none
    std::vector<std::uint16_t> faces;    // 3 per triangle
    std::vector<std::string> skin_joints;
    std::vector<LlmMorph> morphs;
    std::vector<std::pair<std::int32_t, std::int32_t>> remaps;  // {src, dst}

    int vertex_count() const { return static_cast<int>(coords.size() / 3); }
    int face_count() const { return static_cast<int>(faces.size() / 3); }
    const LlmMorph* find_morph(std::string_view name) const;
};

bool parse_llm(const std::vector<std::uint8_t>& bytes, LlmMesh& out, std::string& err);

// SK-33: base coords and normals plus weighted morphs, then seam remaps, then normalised normals.
void morph_mesh(const LlmMesh& mesh, const std::vector<std::pair<std::string, float>>& morphs,
                std::vector<float>& coords, std::vector<float>& normals);

enum class Material { Skin, Eye };

struct MeshPart {
    std::string name;
    Material material = Material::Skin;
    std::uint32_t first_vertex = 0, vertex_count = 0;
    std::uint32_t first_index = 0, index_count = 0;
};

// A vertex's two influences: (1 - blend) * a + blend * b. Node indices; -1 is the virtual root (identity).
struct Influence {
    int a = -1, b = -1;
    float blend = 0;
};

class AvatarMesh {
public:
    enum File { Head, UpperBody, LowerBody, Eyelashes, Eye, FileCount };

    // Reads the five .llm files and avatar_lad.xml (for the SL default shapes) from dir. The skeleton must
    // outlive this object.
    bool load(const Skeleton& skel, const std::string& dir, std::string& err);
    // Morphs, palettes and weights for a body. Skeleton Only builds nothing.
    void build(Body body, bool finger_weights = true);
    // The same for any shape description (its morph lists; its Shape is what skin() must be given).
    void build(const BodyShape& shape, bool finger_weights = true);

    // The shape FK, skin() and export run with for a body; null for Female and Skeleton Only.
    const Shape* shape(Body body) const;
    const BodyShape& sl_default(bool male) const { return sl_default_[male]; }
    const AvatarParams& params() const { return params_; }

    // World-space positions and normals (3 floats per vertex) of every part for a pose. globals come from
    // Skeleton::global_pose with the same shape. No allocation once the outputs have grown. The eyeballs ride the
    // head at the built body's own eye positions: a shape carrying another body's (a worn mesh head's joint
    // positions) never moves them out of this head.
    void skin(const std::vector<Xform>& globals, const Shape* shape, std::vector<float>& positions,
              std::vector<float>& normals) const;

    const LlmMesh& file(File f) const { return files_[f]; }
    // SK-34 palette of a skinned file: node indices, -1 for the virtual root.
    const std::vector<int>& palette(File f) const { return palettes_[f]; }

    const std::vector<MeshPart>& parts() const { return parts_; }
    const std::vector<std::uint32_t>& indices() const { return indices_; }  // CCW front faces
    const std::vector<Influence>& influences() const { return influences_; }
    const std::vector<float>& rest_coords() const { return coords_; }  // morphed, before skinning
    int vertex_count() const { return static_cast<int>(coords_.size() / 3); }

private:
    struct Slot {
        int node = -1;  // -1: identity
        Vec3 bind;      // subtracted before the joint transform
        bool shaped = true;
        int parent = -1;  // a rigid eye: placed from its parent's transform at local (this body's rest), not its own
        Vec3 local;
    };

    const Skeleton* skel_ = nullptr;
    AvatarParams params_;
    BodyShape sl_default_[2];  // female, male
    LlmMesh files_[FileCount];
    std::vector<int> palettes_[FileCount];
    std::vector<MeshPart> parts_;
    std::vector<std::uint32_t> indices_;
    std::vector<Influence> influences_;
    std::vector<float> coords_, normals_;
    std::vector<Slot> slots_;
    std::vector<std::uint16_t> vslot_;  // two slots per vertex
    std::vector<float> vblend_;
    mutable std::vector<float> mats_;  // 12 floats per slot, per skin() call

    // null shape: Skeleton Only. own: the body's own proportions, where its eyes sit (null = none).
    void build(const BodyShape* shape, const Shape* own, bool finger_weights);
};

}  // namespace vats
