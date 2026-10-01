// Viewport Avatar Toolset - the priority planner: which of several animations wins each bone in SL.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 16 (PP). The context clips are the user's own files (an AO stand, a dance, a
// furniture pose) or, in the viewer, the names and priorities of the animations running on the user's own avatar;
// never keyframes, never another avatar's.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "vats/anim_convert.h"
#include "vats/clip.h"
#include "vats/skeleton.h"

namespace vats {

// One animation as SL's blending sees it: the joints it has keys for, each at the priority it plays at there.
struct PlanClip {
    std::string name;
    std::map<std::string, int> joints;  // skeleton joint name -> priority (the joint's own, else the base)
    bool own = false;    // the project being edited
    bool stand = false;  // an AO stand (the PP-4 stand rule)

    bool operator==(const PlanClip&) const = default;
};

// PP-1: every joint record with keys, at its own priority or (-1) the base priority. A record without keys moves
// nothing in the viewer (its joint state gets no usage), so it claims nothing. Joints the skeleton does not know
// are left out.
PlanClip plan_clip_from_anim(const Skeleton& skel, const AnimFile& file, std::string name);
// A clip as it exports (with its own export settings and Export mirrored).
PlanClip plan_clip_from_clip(const Skeleton& skel, const Clip& clip, const AnimExportOptions& opt, std::string name);
// A .anim, or a .vat project (its active actor, exported on SL Default), read from disk. The name is the file
// name without its extension; a name with "stand" in it is marked as a stand. False with err when unreadable.
bool load_plan_clip(const Skeleton& skel, const std::string& path, PlanClip& out, std::string& err);

// PP-2, SL's rule per joint: the highest priority wins; on equal priority the animation started most recently
// wins. clips are in start order (the last started last). The winner's index, or -1 when no clip claims it.
int plan_winner(const std::vector<PlanClip>& clips, const std::string& joint);
// The winner of every claimed joint.
std::map<std::string, int> plan_winners(const std::vector<PlanClip>& clips);

struct PlanFinding {
    std::string rule;  // "loses", "stand_priority" or "face_hands_high"
    int clip = -1;     // the clip it is about (index into clips)
    std::vector<std::string> bones;
    std::string message;
};
// PP-3: the bones the own clip loses, one finding per winning clip. PP-4 (advice for AO makers): a stand that
// plays body joints at priority 4 or more, and a whole-body clip (it claims mPelvis and a shoulder) that keys
// face or hand bones at priority 5 or 6.
std::vector<PlanFinding> plan_lint(const Skeleton& skel, const std::vector<PlanClip>& clips);

}  // namespace vats
