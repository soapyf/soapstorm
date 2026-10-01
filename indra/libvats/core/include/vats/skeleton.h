// Viewport Avatar Toolset - the Second Life Bento skeleton.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Built from the viewer's avatar_skeleton.xml (joints, aliases, collision volumes) and
// avatar_lad.xml (attachment points, male skeleton parameter). Spec: docs/spec/01.
//
// Node order: 133 joints (document order), 47 attachment points (ascending id), then the 26 collision
// volumes (document order). Volumes are animatable pseudo-joints (README decision 12): the viewer's
// .anim lookup finds them by name (e.g. BELLY), which fitted mesh follows.
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vats/math.h"

namespace vats {

enum class Category { Body, Hands, Face, Wings, Tail, HindLimbs, Groin, AttachmentPoints, CollisionVolumes };
constexpr int kCategoryCount = 9;

struct Node {
    std::string name;
    int parent = -1;     // -1 for root-level nodes (mPelvis, Avatar Center)
    Vec3 pos;            // rest translation in the parent frame
    Vec3 end;            // tail vector in the node's own frame (display only)
    Quat rest;           // rest rotation in the parent frame
    std::string group;   // XML group, "Attachment" or "Collision"
    Category category = Category::Body;
    bool base = false;   // support="base" (pre-Bento joint)
    Vec3 scale{1, 1, 1}; // XML bone scale (joints only)
    Vec3 pivot;          // XML pivot: legacy-mesh bind offset (SK-I11), joints only
    bool connected = false;  // XML connected="true"
    // A pseudo-joint hung off a joint: an attachment point or a collision volume. Not part of the
    // joint tree for BVH, body parts or mesh skinning; always carries position keys.
    bool attachment = false;
    bool volume = false;  // a collision volume (attachment is also true)
    int attach_id = 0;   // avatar_lad.xml id, attachment points only
    std::vector<std::string> aliases;
    std::vector<int> children;
};

struct CollisionVolume {
    std::string name;
    int joint = -1;
    Vec3 pos;
    Quat rot;
    Vec3 scale;  // ellipsoid semi-axes, metres
    Vec3 end;    // display tail in the volume's own frame
    std::string group;  // XML group
    int node = -1;  // its node index (after the attachment points)
};

// Body-shape distortion per node: joint positions are offset, then scaled by the parent's scale.
struct Shape {
    std::vector<Vec3> scale;   // 1,1,1 = none
    std::vector<Vec3> offset;  // 0,0,0 = none
    // A mesh body's rig axes (dae.h, rig_axes_from_parts): per node, the bone axes its file authored, as a rotation
    // from the node's SL rest frame, and the display tail they give the bone in that frame. Empty where the body has
    // none (identity and zero per node likewise). Posing and display only: keys stay in SL's joint frames.
    std::vector<Quat> axes;
    std::vector<Vec3> tails;
};

// True when shape gives node i rig axes.
inline bool has_rig_axes(const Shape* shape, int i) {
    return shape && i >= 0 && i < static_cast<int>(shape->axes.size()) && !(shape->axes[i] == Quat{});
}
// A local rotation (relative to rest, as Pose::rot and the keys hold it) as it reads about a joint's rig axes, and
// back: the same turn in the joint's frame, R = axes * rig * axes^-1.
inline Quat to_rig_axes(const Quat& axes, const Quat& rot) { return (axes.conj() * rot * axes).normalized(); }
inline Quat from_rig_axes(const Quat& axes, const Quat& rig) { return (axes * rig * axes.conj()).normalized(); }

// A pose: rotation relative to the rest rotation, and translation offset from the rest position.
struct Pose {
    std::vector<Quat> rot;
    std::vector<Vec3> offset;

    explicit Pose(size_t n = 0) : rot(n), offset(n) {}
};

// Spec 09 build 35: the avatar size every SL viewer computes from the joints (LLAvatarAppearance::computeBodySize),
// from their local positions (rest, the shape's offsets and the pose's: an animation's position keys count) and
// the shape's scales. The viewer stands the root pelvis_to_foot above the ground's collision height and centres
// height on the agent's position, which the region keeps at the shape's own size (a viewer in a region with
// server-side appearance never sends it this one): an animation that makes height taller by d lowers the wearer by
// d / 2 on every viewer from the next time an animation starts or stops on it (when the viewer recomputes it), its own
// end included, and for as long as the joints keep those positions (after it ends too: nothing puts them back).
struct SlBodySize {
    double pelvis_to_foot = 0;  // LL's formula as it is: the hip's height counts with the sign of the leg's
    double height = 0;          // pelvis_to_foot plus the torso, chest, neck, head and skull above the pelvis
};

class Skeleton;
SlBodySize sl_body_size(const Skeleton& skel, const Pose* pose = nullptr, const Shape* shape = nullptr);

class Skeleton {
public:
    // Loads from the text of avatar_skeleton.xml and avatar_lad.xml.
    bool load(std::string_view skeleton_xml, std::string_view lad_xml, std::string& err);
    // Loads both files from a directory (e.g. data/character).
    bool load_dir(const std::string& dir, std::string& err);

    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<CollisionVolume>& volumes() const { return volumes_; }
    int joint_count() const { return joint_count_; }  // nodes before the attachment points
    int volume_start() const { return volume_start_; }  // first collision-volume node (= size() - 26)
    int size() const { return static_cast<int>(nodes_.size()); }
    const Node& operator[](int i) const { return nodes_[i]; }

    // The app's lookup: exact name, then case-insensitive name or alias. -1 when not found.
    int find(std::string_view name) const;
    // The display "bone frame" B of a joint (SK-21): the shortest-arc rotation from the signed principal
    // axis nearest end to end's direction; identity for tails under 1e-5 or already on an axis.
    static Quat bone_frame(const Vec3& end);
    Quat bone_frame(int i) const { return bone_frame(nodes_[i].end); }
    // The Local gizmo's axes of node i in its own frame: the mesh body's rig axes where shape has them, else the bone
    // frame.
    Quat bone_axes(int i, const Shape* shape) const { return has_rig_axes(shape, i) ? shape->axes[i] : bone_frame(i); }

    // The viewer's lookup: exact name, then exact alias (attachment names may use '_' for ' ').
    int find_viewer(std::string_view name) const;
    int find_volume(std::string_view name) const;

    // Left/Right partner of a name; a name without a side maps to itself.
    static std::string mirror_name(std::string_view name);
    int mirror(int node) const { return mirror_[node]; }

    // The viewer's "male" skeleton parameter at full weight, feet planted on the ground.
    const Shape& male_shape() const { return male_; }

    // Global transforms of every node. shape may be null.
    std::vector<Xform> global_pose(const Pose& pose, const Shape* shape = nullptr) const;
    // Spec 09 build 34: the pose an avatar shows in the world, read back from its joints, as a Pose like the editor's.
    // local: each node's rotation in its parent's frame as the avatar holds it now, the pelvis's in the actor's space
    // (nodes past its end stay at rest); pelvis: where the pelvis stands in the actor's space; rest: where that avatar's
    // own pelvis rests there (build 35: its root, whatever its shape or the frame). Rotations become rotations from rest
    // and the hip's travel from its own rest the pelvis's offset; no other joint is offset, so global_pose(result, shape)
    // poses any body on its own joint positions and pelvis height, with these rotations and this hip travel.
    Pose pose_from_live(const std::vector<Quat>& local, const Vec3& pelvis, const Vec3& rest) const;
    // Spec 09 build 35: where a worn avatar's pelvis rests in actor space, the ground at z = 0. The viewer stands an
    // avatar's root (its pelvis) sl_body_size().pelvis_to_foot above the ground, so an avatar whose legs and hips put
    // it higher than the SL default's rests that much above the default's pelvis.
    Vec3 worn_pelvis_rest(double worn_pelvis_to_foot) const;
    // Local transform of one node (rest and pose applied, shape offsets and parent scale).
    Xform local_xform(int i, const Pose& pose, const Shape* shape = nullptr) const;

private:
    std::vector<Node> nodes_;
    std::vector<CollisionVolume> volumes_;
    std::vector<int> mirror_;
    int joint_count_ = 0, volume_start_ = 0;
    std::unordered_map<std::string, int> exact_, lower_, viewer_alias_, volume_index_;
    Shape male_;
};

}  // namespace vats
