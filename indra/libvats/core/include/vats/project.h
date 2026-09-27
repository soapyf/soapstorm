// Viewport Avatar Toolset - project files: the native .vat format and .hxanim migration.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/03 sections 2.6, 3.4 and 3.5; docs/spec/02 AM-1, AM-29, AM-140.
// Fields the core does not model yet (export, props, meta) are kept as JSON. "anchors" map to Clip::pins
// ({joint, via, target, from, to, pos: [x, y, z], rot: [x, y, z, w], start_key?, release_key?}); unknown
// fields inside an anchor are kept in Pin::extra.
// Clip::constraints are saved as an array of lowercase hex strings, one per 86-byte record.
// Clip::orphans are saved as [{name, priority, rot: [[t, x, y, z, w], ...], pos: [[t, x, y, z], ...]}].
// Clip::ik_solve is saved as "ik_solve": "literal" only when it is IkSolve::Literal; an .hxanim loads as
// Literal.
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "vats/clip.h"
#include "vats/json.h"

namespace vats {

// 2 adds "actors" (spec 08 GR-5). A project with one actor is still written as version 1, unchanged.
inline constexpr int kProjectVersion = 2;

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
    Json extra = Json::object();
    Json clip_extra = Json::object();  // unknown fields of this actor's "clip" object, written back (IO-43)

    Xform placement() const { return {Quat::axis_angle({0, 0, 1}, rot_z * kDegToRad), pos}; }
    bool operator==(const Actor&) const = default;
};

struct Project {
    Clip clip;  // the active actor's clip (the only clip of a single-actor project)
    std::vector<Actor> actors;  // empty = one actor; else at least two, actors[active] describes `clip`
    int active = 0;
    Json meta = Json::object();
    Json extra = Json::object();  // unknown top-level fields, written back in their order (IO-43)

    // Set by load_project, never saved.
    bool read_only = false;  // the file has a newer version than this build knows (IO-43)
    bool migrated = false;   // the file was an .hxanim: save it under a new name (IO-40)
};

// Loads a "vats-project" or "hexton-sl-anim" document. For an .hxanim, source_path (when not
// empty) goes to meta.migrated_from and unknown fields to meta.hexton_extra. On failure returns
// false, sets err and leaves out unchanged.
bool load_project(std::string_view text, Project& out, std::string& err, std::string_view source_path = {});

// GR-1 helpers. Timing (fps, length, loop) is shared by every actor (GR-3): sync_actor_timing copies
// the active clip's to the others.
const Clip& actor_clip(const Project& p, int i);
Clip& actor_clip(Project& p, int i);
void set_active_actor(Project& p, int i);
void sync_actor_timing(Project& p);

// Writes the native format. Empty curves and tracks are skipped.
std::string save_project(const Project& p);

}  // namespace vats
