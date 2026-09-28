// Viewport Avatar Toolset - body parts, mirroring, the pose/clip library and the pose and key clipboards.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/01 SK-23..SK-25; docs/spec/02 sections 2.9-2.10 (AM-90..98, AM-110..114); docs/spec/03
// sections 3.6 and 4.5. Every name mapping goes through Skeleton::mirror_name.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vats/clip.h"
#include "vats/curve_ops.h"

namespace vats {

// Body parts (SK-25, AM-90, AM-134)

enum class PartKind { Arm, Hand, Leg, Wing, HindLeg, Tail, Head, Torso, Category, Point };

struct BodyPart {
    PartKind kind = PartKind::Point;
    std::string side;                    // "Left", "Right" or ""
    std::string label;                   // "Left Arm", "Head", ...
    std::vector<int> bones;              // node indices
    std::vector<std::string> ik_tracks;  // "ik.ArmLeft", ... that belong to the part (AM-94, AM-134)
};

// "Left", "Right" or "", by the rule mirror_name uses: an "L "/"R " prefix, else "Left"/"Right" in the name.
std::string side_of(std::string_view name);

// The part a right-click on node picks: hand (wrist and fingers), arm (collar to elbow), leg, head, torso, then
// the node's category limited to its side. An attachment point is its own Point part.
BodyPart body_part_of(const Skeleton& skel, int node);
// A part by kind and side. "Copy/Select arm" is Arm plus Hand (AM-90). Category and Point have no fixed set and
// come back without bones.
BodyPart body_part(const Skeleton& skel, PartKind kind, const std::string& side);
// What "Select/Copy/Paste arm" acts on (AM-90, AM-97): an arm part plus its wrist and hand bones. Other parts
// come back unchanged.
BodyPart with_hand(const Skeleton& skel, BodyPart part);
// The bones a library pose of this kind covers (03 section 3.6): arm includes the wrist, hand does not. "pose"
// gives every non-attachment bone; "selection" gives none.
std::vector<int> pose_region(const Skeleton& skel, const std::string& kind, const std::string& side);

// Mirroring (AM-110..114)

enum class MirrorMode { LeftToRight, RightToLeft, Flip };

// Node src's rotation (relative to its rest) reflected onto node dst: rest * rot is mirrored as a whole, so a pair
// whose rest rotations are not mirror images still mirrors correctly (E-5).
Quat mirror_rotation(const Skeleton& skel, int src, int dst, const Quat& rot);
// Whether a curve channel changes sign under the mirror: rot_x, rot_z, pos_y, pole_y.
bool mirror_flips(std::string_view channel);
// A track's mirror name, or the name itself when the counterpart bone does not exist.
std::string mirror_track(const Skeleton& skel, const std::string& track);

// Mirror at a frame from the current (evaluated) pose. Left/Right keys the other side from the source side; Flip
// swaps every pair and mirrors centre bones in place. Pairs where neither track is animated are skipped. Position
// is keyed (Y negated) where either side has position or the bone is an attachment point, and the pos, rot and
// pole channels of ik tracks are mirrored the same way (02 section 5 item 4). Blend and pins are left alone.
void mirror_pose(Clip& clip, const Skeleton& skel, double frame, const Pose& current, MirrorMode mode);
// Keys each node's counterpart with the node's mirrored rotation (and position, as above) (AM-113).
void mirror_bones(Clip& clip, const Skeleton& skel, double frame, const Pose& current, const std::vector<int>& nodes);
// A mirrored copy for export (AM-114, 03 section 4.5): tracks, joint priorities and pins included.
Clip mirrored_clip(const Skeleton& skel, const Clip& clip);

// Pose/clip library (03 section 3.6; AM-91..95)

struct LibraryItem {
    std::string id, name, kind, side;   // kind: pose, arm, leg, hand, wing, hindleg, tail, head, selection
    std::map<std::string, Vec3> bones;  // pose items: Euler degrees
    std::optional<Vec3> hip;            // whole-body poses
    std::map<std::string, Vec3> offsets;  // pose items: position offsets from rest (face poses, 08 FA-4)
    bool clip = false;                  // clip items:
    double length = 0;
    std::map<std::string, Track> curves;
    std::vector<std::string> relative;
    std::string category;  // built-in poses only, not saved: the heading they are listed under
};

struct Library {
    std::vector<LibraryItem> items;
};

// A random UUID for a new item.
std::string new_item_id();
// Reads a vats-pose-library document (and, with VATS_LEGACY_IMPORT, a legacy one).
bool load_library(std::string_view json_text, Library& out, std::string& err);
// Writes a vats-pose-library document.
std::string save_library(const Library& lib);

// Save pose (AM-91): the displayed local rotations of bones, and the hip offset when with_hip.
LibraryItem make_pose(const Skeleton& skel, const Pose& displayed, const std::vector<int>& bones,
                      const std::string& kind, const std::string& side, bool with_hip);
// Apply pose at a frame (AM-92). Mirrored sends each bone to its counterpart and negates the hip's Y (and each
// offset's Y). The caller
// mirrors a region pose when its side differs from the clicked part's.
void apply_pose(Clip& clip, const Skeleton& skel, const LibraryItem& pose, double frame, bool mirrored);
// Save clip over [a, b] (AM-93/94). tracks are the part's or selection's track names.
LibraryItem make_clip(const Clip& clip, const std::vector<std::string>& tracks, double a, double b,
                      const std::string& kind, const std::string& side);
// Paste clip at frame `at` (AM-95). warnings, when given, gets one line per relative ik track whose limb is FK there.
void paste_clip(Clip& clip, const Skeleton& skel, const LibraryItem& item, double at, bool mirrored,
                std::vector<std::string>* warnings);

// Pose clipboard (AM-96/97)

struct PoseEntry {
    std::string track;
    bool ik = false;
    Vec3 euler, pos;  // bone and pin tracks: curve values
    bool has_pos = false;
    std::map<std::string, double> channels;  // ik tracks: every non-empty channel
};

struct PoseClipboard {
    std::vector<PoseEntry> entries;
};

// Copies the selected tracks at a frame; with nothing selected, every animated bone and every ik track.
PoseClipboard copy_pose(const Clip& clip, double frame, const std::vector<std::string>& selected);
// Pastes by AM-96's target rules: nothing selected = own names; one entry = onto every selected track of its kind
// (bone or ik); otherwise each selected track that has an entry.
void paste_pose(Clip& clip, const PoseClipboard& cb, double frame, const std::vector<std::string>& selected);
// Body-part paste (AM-97): each part bone takes its own entry, else its counterpart's entry mirrored (rotation only).
void paste_pose_part(Clip& clip, const PoseClipboard& cb, double frame, const std::vector<std::string>& part_bones);

// Graph key clipboard (AM-98, TG-96)

struct CopiedKey {
    std::string track, channel;
    double offset = 0, value = 0;  // offset from the earliest copied key's frame
    Interp interp = Interp::Bezier;
};

struct KeyClipboard {
    std::vector<CopiedKey> keys;
};

KeyClipboard copy_keys(const Clip& clip, const std::vector<KeyRef>& sel);
// Sets the keys at frame + offset on their own track and channel. Returns them as the new selection.
std::vector<KeyRef> paste_keys(Clip& clip, const KeyClipboard& cb, double frame);

}  // namespace vats
