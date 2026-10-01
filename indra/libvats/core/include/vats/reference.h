// Viewport Avatar Toolset - a reference picture, or a numbered picture sequence, in the view (spec 08 RF).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The picture is drawn as a quad: a backdrop filling the view behind everything, or a plane in the scene behind the
// actor. A sequence follows the timeline at its own frame rate. The quads' corners run bottom-left, bottom-right,
// top-right, top-left of the picture (flips already applied), so a host maps them to UVs (0,1) (1,1) (1,0) (0,0).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "vats/json.h"
#include "vats/math.h"

namespace vats {

// The camera view a reference can be locked to; Any = every view.
enum class ReferenceView { Any, Front, Back, Right, Left, Top };

struct Reference {
    std::string path;            // the picture, or one picture of the sequence; stored like props (relative or absolute)
    bool sequence = false;       // the numbered pictures beside path follow the timeline
    bool in_scene = false;       // a plane in the scene behind the actor; false = a backdrop behind everything
    bool hidden = false;
    ReferenceView view = ReferenceView::Any;  // shown only while the camera looks from near this view
    double opacity = 0.5;        // 0..1
    double scale = 1;            // backdrop: of the view's height; in the scene: of 2 m tall
    double offset_x = 0, offset_y = 0;  // right and up; backdrop: in view heights; in the scene: metres
    bool flip_x = false, flip_y = false;
    int frame_offset = 0;        // the timeline frame the sequence's first picture shows on
    double fps = 30;             // the sequence's own frame rate
    Json extra = Json::object();  // unknown fields, written back (IO-43)

    bool operator==(const Reference&) const = default;
};

// Where the camera sits for a view, from the target (Front = +X, where the avatar faces); Any = Front.
Vec3 reference_view_dir(ReferenceView v);
// Whether the reference shows for a camera whose eye lies in direction eye_dir from its target (within 15 degrees
// of its locked view).
bool reference_shows(const Reference& r, const Vec3& eye_dir);

// The numbered pictures of the sequence any_picture belongs to: the same name around its last run of digits, in
// number order ("walk_0001.png", "walk_0002.png", ...). Just any_picture when its name has no digits.
std::vector<std::string> sequence_files(const std::string& any_picture);
// The sequence picture shown at a timeline frame, 0..count-1 (held at the first before frame_offset and at the last
// after the end); -1 when count is 0.
int sequence_index(const Reference& r, double frame, int clip_fps, int count);

// The backdrop's corners in clip space (x, y; z = 0) for a view of this width / height and a picture of this
// width / height.
std::array<Vec3, 4> reference_backdrop_quad(const Reference& r, double view_aspect, double picture_aspect);
// The plane's corners in the scene: facing the locked view (Front when Any), 1.5 m behind `at` as that view sees it,
// standing on the ground (Top: lying just under it, its top towards +X).
std::array<Vec3, 4> reference_scene_quad(const Reference& r, const Vec3& at, double picture_aspect);

Json reference_to_json(const Reference& r);
bool reference_from_json(const Json& j, Reference& out, std::string& err);

}  // namespace vats
