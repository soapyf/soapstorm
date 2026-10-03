// Viewport Avatar Toolset - COLLADA (.dae) import for props and rigged meshes.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/03 sections 2.5 and 3.3; docs/spec/01 SK-40 (skin-target index space).
// Everything comes out in SL space (Z up, metres). Rigged meshes are skinned on the CPU by skin_prop,
// with the same conventions as AvatarMesh::skin.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
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

// The viewer's reading of a skin joint name, exactly as its uploader matches it: SL's joint names and LL's aliases
// (avatar_skeleton.xml: hip, abdomen, lThigh...), collision volume names (upper case) and attachment point names; also
// after a namespace or armature prefix ("rig:mNeck", "Armature_mChest"). An SK-40 index, or -1.
int viewer_skin_joint(const Skeleton& skel, std::string_view name);
// §3.3.4 name mapping, steps 1-4: viewer_skin_joint, else the name in any case ("belly", "l_upper_arm"), which the
// viewer would not read: loose is then set, for the reader to warn. An SK-40 index, or -1 when unmapped. (Step 5, a
// node sid, needs the document and is applied inside load_dae.)
int map_skin_joint(const Skeleton& skel, std::string_view name, bool* loose = nullptr);
// The readers' warning for a name map_skin_joint read loosely.
std::string loose_joint_warning(std::string_view name);

// §3.3.4 rig scale from rest / bind distance ratios (bones > 0.3 m out): the unit (1, 0.01, 0.001, 0.0254, 0.1,
// 10, 100) that a third of the ratios lie within 5 % of, else the median snapped to a unit within 5 %, else the
// file's declared unit when the median is within a factor of 2 of it (a rig with its own proportions, which SL
// uploads at that unit), else the median. measured gets the median.
double rig_scale_of(std::vector<double> ratios, double& measured, double declared = 1);

struct DaeMaterial {
    std::string name;  // the COLLADA material id, or "" for the default material
    std::array<float, 4> rgba{0.82f, 0.80f, 0.78f, 1.0f};  // white tint when textured
    std::string texture;  // resolved path of the diffuse texture; empty when none or not found
    bool blend = false;   // alpha-blended (transparency or diffuse alpha < 0.999)
    bool double_sided = false;  // only the default material
};

// The triangles of one material within one part. Its vertices are contiguous too.
struct DaeGroup {
    int material = 0;
    std::uint32_t first_vertex = 0, vertex_count = 0;
    std::uint32_t first_index = 0, index_count = 0;
};

// Spec 08 SK-1: one mesh object of the file, named as the file names it (a glTF mesh node, an FBX mesh node, a COLLADA
// node's geometry). Its vertices and triangles are contiguous, and the groups of its materials lie within them.
struct DaePart {
    std::string name;
    std::uint32_t first_vertex = 0, vertex_count = 0;
    std::uint32_t first_index = 0, index_count = 0;
};

// Spec 08 SK-2: a shape key (morph target) of one part: how far it moves the vertices it moves at full strength, in
// the model's space (SL axes, metres). A key of one name on several parts has an entry per part.
struct DaeShapeKey {
    std::string name;
    std::vector<std::uint32_t> vertices;  // the model's vertices it moves, ascending
    std::vector<float> dpos, dnrm;        // 3 per vertex listed; dnrm is empty when the file gives no normal offsets
    double initial = 0;                   // the file's own value: glTF weights, FBX DeformPercent, COLLADA MORPH_WEIGHT
};

// What a body shows of its models (spec 08 SK-3): the parts hidden, by name, and shape key values by name. A key not
// listed is at its file's value (DaeShapeKey::initial).
struct MeshLook {
    std::set<std::string> hidden;
    std::map<std::string, double> keys;
    bool empty() const { return hidden.empty() && keys.empty(); }
    bool operator==(const MeshLook&) const = default;
};

struct DaeModel {
    std::vector<float> positions, normals;  // 3 per vertex, SL space; rigged vertices in bind pose
    std::vector<float> uvs;                 // 2 per vertex, already flipped (v' = 1 - v)
    std::vector<std::uint32_t> indices;     // 3 per triangle, CCW front faces
    std::vector<DaeGroup> groups;           // per part, one per material in first-use order
    std::vector<DaeMaterial> materials;
    std::vector<DaePart> parts;             // in the file's order; every vertex is in one
    std::vector<DaeShapeKey> shape_keys;    // not applied to positions and normals (shown_model applies them)

    bool rigged = false;
    // Rigged only: 4 influences per vertex, largest first, weights summing to 1. Unused slots have
    // weight 0 and the mRoot index. Joint indices are in the SK-40 space (dae_root, dae_volume).
    std::vector<int> joints;
    std::vector<float> weights;
    // Rigged only: the bind pose of every SK-40 index (rig-scaled, orthonormal). Joints the file does
    // not bind keep the SL rest pose; mRoot is identity.
    std::vector<Xform> binds;
    std::vector<bool> bound;  // Rigged only: which SK-40 indices the file itself gave a bind for
    // Rigged only: the rig axes, each bound index's bind rotation as the file authored it (SL space, after the
    // up-axis and quarter turns), kept when settle_rig replaced the binds' bone-orientation rotations with SL's rest
    // rotations. Empty when the file's binds are in SL's frames. Posing and display only (rig_axes_from_parts);
    // skinning uses binds.
    std::vector<Quat> rig_axes;
    // settle_rig's quarter turns about Z (0..3) for the binds and for the vertices, and whether the file
    // had enough evidence to decide them itself. Parts of one body that could not decide (eyes, teeth: a
    // few joints close to the centre line) take the decision of a part that could (apply_rig_turn).
    int turn_binds = 0, turn_vertices = 0;
    bool turn_decided = false;

    Vec3 bounds_min, bounds_max;  // of positions
    // A mapped rig's spare chains (rig_map.h RM-8): SL joint -> what the chain holds ("scarf"), shown beside its name.
    std::map<std::string, std::string> labels;

    int vertex_count() const { return static_cast<int>(positions.size() / 3); }
    int triangle_count() const { return static_cast<int>(indices.size() / 3); }
};

// A bone of the file's own armature (spec 08 RM-1), whatever its name: the input of rig mapping (rig_map.h).
struct SourceBone {
    std::string name;  // as the skin names it
    int parent = -1;   // index into DaeReport::bones; parents come first
    // Where the bone is bound: SL axes after the file's up axis, metres at the file's declared unit. A skinned bone's
    // is its skin's bind; any other bone's (an IK target, an _end tip) follows its parent's bind as the file's
    // node tree places it.
    Xform bind;
    bool skinned = false;  // a skin binds it
    double weight = 0;     // the sum of its skin weights over every vertex
};

// A foreign armature mapped onto SL's (rig_map.h): the loaders bind skins by this table instead of by SL name.
struct SkinRemap {
    std::map<std::string, int> joints;  // source bone -> SK-40 index its weights go to; bones not listed are dropped
    std::map<int, Xform> binds;         // SK-40 index -> its bind, in SourceBone::bind's space
    int turn = 0;                       // quarter turns about Z after the up axis, so the model faces +X
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
    std::vector<SourceBone> bones;              // the file's armature, mapped or not (empty when it has none)
    bool remapped = false;                      // loaded through a SkinRemap
    bool scratch = false;                       // rigged from scratch by the mapping beside it (auto_rig.h)
    bool painted = false;                       // weights painted in VATs, from the mapping beside it, over the file's own
    MeshLook look;                              // what the mapping file beside it shows (load_mesh_file, rig_map.h)
};

// Parses a COLLADA 1.4/1.5 document. dae_dir is the folder of the .dae, used to find textures.
// Returns false with err set on malformed or empty files; never crashes on bad indices.
// remap: bind the skins through it instead of by SL name (rig_map.h); null for an SL-named rig.
bool load_dae(std::string_view xml_text, const std::string& dae_dir, const Skeleton& skel, DaeModel& out,
              DaeReport& report, std::string& err, const SkinRemap* remap = nullptr);

// After loading a rigged model: makes the file's bind pose the avatar's rest pose in SL axes, the way SL
// itself treats a correctly exported devkit. bound flags the SK-40 indices the file gave a bind for.
// 1. Binds and vertices each get the quarter turn about Z that lines them up with the SL skeleton
//    (Blender rigs face -Y; some exporters turn the joints but not the vertices).
// 2. Binds carrying a bone-orientation convention (Blender's Y-along-bone, an FBX node's axes) get the
//    SL rest rotations, since SL joints turn in their own frames. Files already in SL axes are unchanged.
// Notes each change in report.warnings.
// Also records bound in model.bound.
// foreign: a remapped rig (SkinRemap), already turned to face +X. Step 1 is skipped, and every bind takes SL's rest
// rotation: a foreign rig's bind rotations are its own bone axes, never a pose against SL's rest.
void settle_rig(DaeModel& model, const Skeleton& skel, const std::vector<bool>& bound, DaeReport& report,
                bool foreign = false);
// A remapped load's binds: remap.binds turned and scaled as the vertices are, SL's rest for every other SK-40 index.
// bound gets the indices remap binds.
void remap_binds(const Skeleton& skel, const SkinRemap& remap, DaeModel& model, std::vector<bool>& bound);
// Also keeps the file's own bind rotations in model.rig_axes when step 2 replaces them.
// Turns an undecided model's binds and vertices by the given quarter turns about Z, the same way settle_rig
// would have. Only the difference from what the model already has is applied, so calling it twice is safe.
void apply_rig_turn(DaeModel& model, int turn_binds, int turn_vertices);

// Skins a rigged model for a pose: position = sum of w * (global x own shape scale x inverse bind) * v.
// globals come from Skeleton::global_pose with the same shape (may be null). Collision volumes follow
// their joint (SK-40); mRoot is identity. A static model is copied through. 3 floats per vertex out.
void skin_prop(const DaeModel& model, const Skeleton& skel, const std::vector<Xform>& globals, const Shape* shape,
               std::vector<float>& positions, std::vector<float>& normals);

// Spec 08 SK-1..3: the model as a body shows it. Hidden parts are left out (vertices, triangles and groups, the rest
// renumbered), and each shape key is applied at its look value, else at its file's value: positions move by value x
// offset and normals likewise, then are normalised. The result has no shape keys; everything else is kept.
DaeModel shown_model(const DaeModel& model, const MeshLook& look);
// For each vertex of shown_model(model, look), the model's vertex it was made from: what a change to the shown model
// (a rig export fix) is carried back through.
std::vector<std::uint32_t> shown_vertex_sources(const DaeModel& model, const MeshLook& look);
// The model's shape key names in the file's order, each once; and the value a look gives a key (its file's when the
// look does not set it).
std::vector<std::string> shape_key_names(const DaeModel& model);
// The part of a body's look that one of its models keeps in its mapping file: the model's own parts hidden and its own
// keys' values, so each file of a body says only what concerns it.
MeshLook own_look(const DaeModel& model, const MeshLook& look);
double shape_key_value(const DaeModel& model, const MeshLook& look, const std::string& name);
// The group a shape key's name lists it under: the text before " - " ("Body - Obese": Body) or before the first '.'
// ("vrc.v_aa": vrc); "" when it has neither. rest (if given) gets where the rest of the name starts ("Obese", "v_aa").
std::string shape_key_group(const std::string& name, size_t* rest);

// Spec 08 BD-3: the proportions a mesh body was rigged to. Every joint whose bind position (in any of the
// parts, first part wins) differs from the unshaped SL rest by more than tol_m is moved there; every other
// joint keeps base (or no shape when base is null); collision volumes likewise. Scales always come from base. Returns false, leaving
// out as base, when no part overrides a joint.
bool shape_from_binds(const Skeleton& skel, const std::vector<const DaeModel*>& parts, const Shape* base, Shape& out,
                      double tol_m = 0.001);

// A mesh body's rig axes: for every joint a part authored its own bone axes for (model.rig_axes, first part wins),
// out.axes gets them as a rotation from the joint's SL rest frame and out.tails the bone's display tail in that frame:
// along the rig's bone axis (the signed axis that points at the child joints on most of its bones, Y for Blender),
// as long as the child joint it points at, else as far as the vertices the joint mostly carries reach along it, else
// the SL bone's length. Other nodes get identity and zero. Returns false, leaving out, when no part has rig axes.
bool rig_axes_from_parts(const Skeleton& skel, const std::vector<const DaeModel*>& parts, Shape& out);

}  // namespace vats
