// Viewport Avatar Toolset - several named clips per project (spec 08 CL).
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include "vats/clips.h"
#include "vats/curve_ops.h"

#include <algorithm>

namespace vats {
namespace {

int actor_count(const Project& p) { return std::max<int>(1, int(p.actors.size())); }
bool is_active(const Project& p, int i) { return p.actors.empty() || i == p.active; }

// Another actor has one clip per take (an actor added since has none yet).
void fit_actor_clips(Project& p) {
    for (int i = 0; i < int(p.actors.size()); ++i)
        if (i != p.active && p.actors[i].clips.size() != p.clips.size()) p.actors[i].clips.resize(p.clips.size());
}

// A new clip with the project defaults: the settings of the clip it was made from, no keys (CL-2).
Clip fresh_from(const Clip& s) {
    Clip c;
    c.fps = s.fps, c.end_frame = s.end_frame, c.loop = s.loop, c.loop_in = s.loop_in, c.loop_out = s.loop_out;
    c.loop_tangents = s.loop_tangents, c.priority = s.priority, c.ease_in = s.ease_in, c.ease_out = s.ease_out;
    c.hand_pose = s.hand_pose, c.ik_solve = s.ik_solve, c.mirror_export = s.mirror_export;
    c.export_settings = s.export_settings;
    return c;
}

template <class V>
void move_item(V& v, int from, int to) {
    auto x = std::move(v[from]);
    v.erase(v.begin() + from);
    v.insert(v.begin() + to, std::move(x));
}

}  // namespace

int clip_count(const Project& p) { return std::max<int>(1, int(p.clips.size())); }

std::string clip_name(const Project& p, int k) { return p.clips.empty() ? "Clip" : p.clips[k].name; }

Clip& take_clip(Project& p, int i, int k) {
    fit_actor_clips(p);
    if (is_active(p, i)) return k == p.active_clip ? p.clip : p.clips[k].clip;
    return k == p.active_clip ? p.actors[i].clip : p.actors[i].clips[k];
}

void set_active_clip(Project& p, int k) {
    const int a = p.active_clip;
    if (k == a || k < 0 || k >= int(p.clips.size())) return;
    fit_actor_clips(p);
    for (int i = 0; i < actor_count(p); ++i) {
        Clip& cur = is_active(p, i) ? p.clip : p.actors[i].clip;
        Clip& old_slot = is_active(p, i) ? p.clips[a].clip : p.actors[i].clips[a];
        Clip& new_slot = is_active(p, i) ? p.clips[k].clip : p.actors[i].clips[k];
        old_slot = std::move(cur);
        cur = std::move(new_slot);
        new_slot = {};
    }
    p.active_clip = k;
}

void name_clips(Project& p) {
    if (!p.clips.empty()) return;
    p.clips.emplace_back();
    p.active_clip = 0;
    fit_actor_clips(p);
}

int add_clip(Project& p, const std::string& name, bool duplicate) {
    name_clips(p);
    fit_actor_clips(p);
    const int a = p.active_clip, at = a + 1;
    for (int i = 0; i < actor_count(p); ++i) {
        const Clip& src = take_clip(p, i, a);
        Clip c = duplicate ? src : fresh_from(src);
        if (is_active(p, i)) {
            ClipSlot s;
            s.name = name;
            if (duplicate) s.ao_state = p.clips[a].ao_state;
            s.clip = std::move(c);
            p.clips.insert(p.clips.begin() + at, std::move(s));
        } else {
            p.actors[i].clips.insert(p.actors[i].clips.begin() + at, std::move(c));
        }
    }
    set_active_clip(p, at);
    return at;
}

void delete_clip(Project& p, int k) {
    if (p.clips.size() < 2 || k < 0 || k >= int(p.clips.size())) return;
    if (k == p.active_clip) set_active_clip(p, k == 0 ? 1 : k - 1);
    fit_actor_clips(p);
    p.clips.erase(p.clips.begin() + k);
    for (int i = 0; i < int(p.actors.size()); ++i)
        if (i != p.active) p.actors[i].clips.erase(p.actors[i].clips.begin() + k);
    if (p.active_clip > k) --p.active_clip;
}

void move_clip(Project& p, int from, int to) {
    const int n = int(p.clips.size());
    if (from == to || from < 0 || to < 0 || from >= n || to >= n) return;
    fit_actor_clips(p);
    move_item(p.clips, from, to);
    for (int i = 0; i < int(p.actors.size()); ++i)
        if (i != p.active) move_item(p.actors[i].clips, from, to);
    int& a = p.active_clip;
    if (a == from) a = to;
    else if (from < a && to >= a) --a;
    else if (from > a && to <= a) ++a;
}

void apply_settings_to_all_clips(Project& p) {
    for (int i = 0; i < actor_count(p); ++i) {
        const Clip src = take_clip(p, i, p.active_clip);
        for (int k = 0; k < int(p.clips.size()); ++k) {
            if (k == p.active_clip) continue;
            Clip& c = take_clip(p, i, k);
            if (c.fps != src.fps) retime_clip(c, src.fps);
            c.priority = src.priority, c.ease_in = src.ease_in, c.ease_out = src.ease_out, c.hand_pose = src.hand_pose;
            c.mirror_export = src.mirror_export;
            Json ex = src.export_settings;
            if (!ex.is_object()) ex = Json::object();
            for (const char* own : {"name", "number"}) {
                std::erase_if(ex.obj, [&](const auto& kv) { return kv.first == own; });
                if (const Json* v = c.export_settings.find(own)) ex.set(own, *v);
            }
            c.export_settings = std::move(ex);
        }
    }
    sync_actor_timing(p);  // a retimed take's other actors follow (GR-3)
}

void for_each_clip(Project& p, const std::function<void(Clip&)>& fn) {
    for (int i = 0; i < actor_count(p); ++i)
        for (int k = 0; k < clip_count(p); ++k) fn(take_clip(p, i, k));
}

}  // namespace vats
