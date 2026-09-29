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
};

// A pose: rotation relative to the rest rotation, and translation offset from the rest position.
struct Pose {
    std::vector<Quat> rot;
    std::vector<Vec3> offset;

    explicit Pose(size_t n = 0) : rot(n), offset(n) {}
};

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
    // (nodes past its end stay at rest); pelvis: where the pelvis stands in the actor's space (feet at the origin).
    // Rotations become rotations from rest and the pelvis's place its offset from rest; no other joint is offset, so
    // global_pose(result, shape) poses any body on its own joint positions, with these rotations and this hip travel.
    Pose pose_from_live(const std::vector<Quat>& local, const Vec3& pelvis) const;
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
