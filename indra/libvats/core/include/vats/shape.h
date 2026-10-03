// Viewport Avatar Toolset - body shapes from the viewer's visual-param system (avatar_lad.xml).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/01 SK-I3. Only the params that move the skeleton (param_skeleton), morph the head,
// upper body, lower body and eyelash meshes (param_morph) or size and move the collision volumes (volume_morph) are
// evaluated; textures, hair and skirt are left out.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vats/math.h"
#include "vats/skeleton.h"

namespace vats {

// One <param> of avatar_lad.xml, reduced to what shape evaluation needs.
struct VisualParam {
    struct Driven {
        int id = 0;
        float min1 = 0, max1 = 0, max2 = 0, min2 = 0;
    };
    struct Bone {
        std::string name;
        Vec3 scale, offset;  // deltas at weight 1
    };
    std::string name;
    float min = 0, max = 1, def = 0;
    int sex = 3;                                     // bit 1 female, bit 2 male (the viewer's ESex)
    std::vector<Driven> driven;                      // param_driver
    std::vector<Bone> bones;                         // param_skeleton
    std::vector<Bone> volumes;                       // volume_morph: a collision volume's scale and pos deltas
    std::vector<std::pair<int, std::string>> morphs; // param_morph: (mesh 0-3, morph name)
};

// Mesh indices of VisualParam::morphs and BodyShape::morphs, same order as AvatarMesh::File.
enum ShapeMesh { ShapeHead, ShapeUpperBody, ShapeLowerBody, ShapeEyelashes, ShapeMeshCount };

using AvatarParams = std::map<int, VisualParam>;  // by id

bool parse_avatar_params(std::string_view lad_xml, AvatarParams& out, std::string& err);

// A body: the skeleton distortion (feet planted, SK-28) and the morph weights of each mesh.
struct BodyShape {
    Shape shape;
    std::vector<std::pair<std::string, float>> morphs[ShapeMeshCount];
};

// Evaluates the params as the viewer does: every param at its value_default, overridden by weights
// (id -> weight), then driver params pushed through to what they drive; a param whose sex does not match
// the avatar's ("male" > 0.5) applies its default instead.
BodyShape evaluate_shape(const Skeleton& skel, const AvatarParams& params, const std::map<int, float>& weights = {});

// The shape SL gives a new avatar (SK-I3): every default, with "male" (id 80) at 0 or 1.
inline BodyShape sl_default_shape(const Skeleton& skel, const AvatarParams& params, bool male) {
    return evaluate_shape(skel, params, {{80, male ? 1.0f : 0.0f}});
}

}  // namespace vats
