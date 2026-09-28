// Viewport Avatar Toolset - project files: the native .vat format.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/03 sections 2.6, 3.4 and 3.5; docs/spec/02 AM-1, AM-29, AM-140.
// Fields the core does not model yet (export, props, meta) are kept as JSON. "anchors" map to Clip::pins
// ({joint, via, target, from, to, pos: [x, y, z], rot: [x, y, z, w], start_key?, release_key?}); unknown
// fields inside an anchor are kept in Pin::extra.
// Clip::constraints are saved as an array of lowercase hex strings, one per 86-byte record.
// Clip::orphans are saved as [{name, priority, rot: [[t, x, y, z, w], ...], pos: [[t, x, y, z], ...]}].
// Clip::ik_solve is saved as "ik_solve": "literal" only when it is IkSolve::Literal; a converted project
// loads as Literal.
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "vats/clip.h"
#include "vats/json.h"

namespace vats {

// 2 adds "actors" (spec 08 GR-5), 3 adds "clips" (spec 08 CL-3). A project with one actor and one clip is still
// written as version 1, unchanged; one with several actors and one clip as version 2.
inline constexpr int kProjectVersion = 3;

// One named clip of a project with several (spec 08 CL-1), such as the stands, walks and sits of an AO set.
// Clips are takes of the whole scene: every actor has one clip per slot and switching the clip switches every
// actor, so the actors of a take keep one timing (GR-3). The active actor's clips live here, another actor's in
// Actor::clips.
struct ClipSlot {
    std::string name = "Clip";
    std::string ao_state;  // the AO state this clip plays in the AO notecards (ao_notecard.h); "" = none
    Clip clip;             // the active actor's clip of this take; unused for the active take (it is Project::clip)
    Json extra = Json::object();  // unknown fields of the slot, written back (IO-43)
    bool operator==(const ClipSlot&) const = default;
};

// One avatar of a couple or group scene (spec 08 GR-1). Its placement is from the shared origin, the
// sit target.
struct Actor {
    std::string name = "Actor";
    std::array<float, 3> colour{0.85f, 0.62f, 0.45f};
    std::string body;  // "" = the view's current body, else a body id ("sl-default", ...) or "mesh:<id>"
    Vec3 pos;          // metres, SL space
    double rot_z = 0;  // degrees about Z
    bool hidden = false, locked = false;
    Clip clip;  // unused for the active actor, whose clip is Project::clip
    std::vector<Clip> clips;  // CL-1: one per Project::clips slot, [active_clip] unused (it is `clip`); unused for the active actor
    Json extra = Json::object();
    Json clip_extra = Json::object();  // unknown fields of this actor's "clip" object, written back (IO-43)

    Xform placement() const { return {Quat::axis_angle({0, 0, 1}, rot_z * kDegToRad), pos}; }
    bool operator==(const Actor&) const = default;
};

struct Project {
    Clip clip;  // the active actor's clip (the only clip of a single-actor project)
    std::vector<Actor> actors;  // empty = one actor; else at least two, actors[active] describes `clip`
    int active = 0;
    std::vector<ClipSlot> clips;  // CL-1: empty = one clip, "Clip"; else clips[active_clip] describes `clip`
    int active_clip = 0;
    Json meta = Json::object();
    Json extra = Json::object();  // unknown top-level fields, written back in their order (IO-43)

    // Set by load_project, never saved.
    bool read_only = false;  // the file has a newer version than this build knows (IO-43)
    bool migrated = false;   // converted from another app's format: save it under a new name (IO-40)
};

// Loads a "vats-project" document (with VATS_LEGACY_IMPORT also a legacy project, converted; source_path
// is recorded in its meta). On failure returns false, sets err and leaves out unchanged.
bool load_project(std::string_view text, Project& out, std::string& err, std::string_view source_path = {});

// GR-1 helpers. Timing (fps, length, loop) is shared by every actor (GR-3): sync_actor_timing copies each take's,
// from the active actor's clip of that take, to the other actors' (CL-1), and gives an actor added since one clip
// per take. set_active_actor brings the actor's clips of every take along.
const Clip& actor_clip(const Project& p, int i);
Clip& actor_clip(Project& p, int i);
void set_active_actor(Project& p, int i);
void sync_actor_timing(Project& p);

// GR-6: a loaded animation replaces actor i's clip (i = 0 for a single-actor project). The timing stays the scene's
// (GR-3): the clip is retimed to the scene's frame rate keeping its timing (retime_clip), the scene grows to the clip's
// length when that is longer, and the scene's Loop and loop points apply. The actor keeps its own props, audio track
// and export settings; binds to actors the project doesn't have (or to the actor itself) are dropped.
struct ActorLoad {
    int file_fps = 30, fps = 30;      // the clip's rate as loaded, and the scene's
    int clip_frames = 0;              // the clip's length at the scene's rate
    int scene_was = 0, scene_now = 0; // the scene's length before and after
    int dropped_binds = 0;
};
ActorLoad load_into_actor(Project& p, int i, Clip clip);

// Writes the native format. Empty curves and tracks are skipped.
std::string save_project(const Project& p);

}  // namespace vats
