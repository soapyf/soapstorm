// Viewport Avatar Toolset - editing operations on a clip.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Everything the UI does to keys goes through here, so the app and a viewer integration share
// one set of rules. Spec: docs/spec/02 section 2.4.
#pragma once

#include <string>
#include <vector>

#include "vats/clip.h"

namespace vats {

// The curve values of a track at a frame (FK only, no IK or pins).
Vec3 curve_euler(const Clip& clip, const std::string& track, double frame);
Vec3 curve_offset(const Clip& clip, const std::string& track, double frame);

// Keys all three rotation channels; the Euler triple is the one nearest the curves' current value.
void key_rotation(Clip& clip, const std::string& track, double frame, const Quat& rotation);
void key_euler(Clip& clip, const std::string& track, double frame, const Vec3& euler_deg);
void key_offset(Clip& clip, const std::string& track, double frame, const Vec3& offset);

// "Set Key": re-keys the curves' current values; position too when the node is an attachment
// point or already has position keys.
void key_current(Clip& clip, const Skeleton& skel, int node, double frame);

// Removes keys at a frame. Returns how many curves lost a key.
int delete_keys_at(Clip& clip, const std::string& track, double frame);
int delete_keys_at_all(Clip& clip, double frame);

// Keys zero rotation, and zero offset if the track has position keys.
void reset_bone(Clip& clip, const std::string& track, double frame);

// Sorted, de-duplicated frames that hold a key on any channel of the track.
std::vector<double> key_frames(const Clip& clip, const std::string& track);
bool has_key_at(const Clip& clip, const std::string& track, double frame);

// After a tool re-keyed the frames between keys at a and b (keyed in `before` too), puts back the outer handles
// of those two keys (a's left, b's right) as `before` had them, frozen (Free), so the curves outside a..b keep
// their shape; their inner handles go back to automatic, to follow the new keys.
void keep_outer_handles(Clip& clip, const Clip& before, const std::string& track, int a, int b);

// Drops empty curves and tracks.
void prune(Clip& clip);

}  // namespace vats
