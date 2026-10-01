// Viewport Avatar Toolset - several named clips per project (spec 08 CL): an AO set of stands, walks and sits, or
// the takes of a couples scene.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// The active clip stays Project::clip (and Actor::clip for the other actors), so everything that edits "the clip"
// edits the active one. The list itself is Project::clips (see project.h). Every function keeps each actor's
// clips in step with the list; none records undo (the app wraps each in a scene step).
#pragma once

#include <functional>
#include <string>

#include "vats/project.h"

namespace vats {

int clip_count(const Project& p);                    // at least 1
// Actor i's clip of take k (i = 0 in a single-actor project). Gives an actor added since one clip per take first.
Clip& take_clip(Project& p, int i, int k);
std::string clip_name(const Project& p, int k);      // "Clip" for a project with one unnamed clip
// Makes clip k the active one for every actor. Out of range or already active: nothing.
void set_active_clip(Project& p, int k);
// Turns a project with one unnamed clip into a list of one ("Clip"), so it can be renamed or given an AO state.
void name_clips(Project& p);
// Adds a clip after the active one and makes it active; returns its index. duplicate: a copy of the active clip
// (every actor's). Else an empty clip that inherits the project defaults from the active clip: its frame rate,
// length, loop, priority, eases, hand pose, IK solve and export settings (CL-2).
int add_clip(Project& p, const std::string& name, bool duplicate);
// Removes clip k (never the last one); the active clip moves to a neighbour when k was it.
void delete_clip(Project& p, int k);
// Moves clip `from` to position `to`; the active clip stays the same clip.
void move_clip(Project& p, int from, int to);
// CL-2: every other clip takes the active clip's frame rate (retimed, keeping its timing), priority, eases, hand pose
// and export settings, keeping its own export "name" and "number". Each actor's clips take that actor's settings.
void apply_settings_to_all_clips(Project& p);

// Every clip of the project: each actor's clip of each take (for path fix-ups on save and load).
void for_each_clip(Project& p, const std::function<void(Clip&)>& fn);

}  // namespace vats
