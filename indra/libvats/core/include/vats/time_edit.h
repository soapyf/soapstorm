// Viewport Avatar Toolset - editing time: insert, remove and stretch frames, copy and paste key ranges
// (spec 08 TE).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// `tracks` names the tracks to edit (bone names, "pin:<joint>", "ik.<limb>"); empty means every track, and
// then the clip's length, loop points and pins follow too (TE-4). Keys that end up past the end extend the
// clip. Automatic handles are recomputed.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "vats/clip.h"

namespace vats {

// Opens `frames` empty frames at `at`: keys at or after it move later.
void insert_time(Clip& clip, int at, int frames, const std::vector<std::string>& tracks = {});
// Deletes the frames a..b-1 and closes the gap: keys from b on move to a.
void remove_time(Clip& clip, int a, int b, const std::vector<std::string>& tracks = {});
// Keeps only the frames a..b (inclusive) of the whole animation: every curve gets a key at a and at b holding
// the pose there, everything outside goes, and frame a becomes frame 0. Last frame, the loop points and pins follow.
void crop_time(Clip& clip, int a, int b);
// Stretches or squashes a..b to a..a+length; later keys move by the difference.
void scale_time(Clip& clip, int a, int b, int length, const std::vector<std::string>& tracks = {});
// The same on fractional frames: a..b becomes a..to, later keys move by to - b; the length rounds (TE-5).
void scale_time_to(Clip& clip, double a, double b, double to, const std::vector<std::string>& tracks = {});
// Keys that span the whole animation (0 to Last frame) scaled about pivot by sx, as the dope sheet's scale handle
// does: Last frame and the loop points map with them from at_press, rounded, as Stretch Range moves them.
void scale_length_with_keys(Clip& clip, const Clip& at_press, double pivot, double sx);

// Keys of a..b (inclusive), frames relative to a.
struct KeyRange {
    int length = 0;
    std::map<std::string, Track> tracks;
    bool empty() const { return tracks.empty(); }
};
KeyRange copy_range(const Clip& clip, int a, int b, const std::vector<std::string>& tracks = {});
// Pastes at `at`. Overwrite replaces the keys already in at..at+length on those tracks; insert first opens
// length+1 frames on them. With mirror_with, left and right are swapped as Mirror does.
void paste_range(Clip& clip, const KeyRange& range, int at, bool insert, const Skeleton* mirror_with = nullptr);

}  // namespace vats
