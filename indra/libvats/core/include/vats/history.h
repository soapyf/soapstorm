// Viewport Avatar Toolset - undo and redo.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Snapshot-based, one step per user operation (spec 02 section 2.13).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vats/clip.h"
#include "vats/project.h"

namespace vats {

// The actor list of a couple or group scene (spec 08 GR), for whole-scene steps: adding, removing,
// renaming or placing actors. The active actor's clip travels separately, as in Project.
struct SceneState {
    std::vector<Actor> actors;
    int active = 0;
    bool operator==(const SceneState&) const = default;
};

class History {
public:
    // What undo/redo hands back: the clip, whose actor it belongs to, and for scene steps the actor list.
    struct Restore {
        Clip clip;
        int actor = 0;
        std::optional<SceneState> scene;
    };
    // The actor that clip steps recorded from now on belong to (GR: undo returns to that actor).
    void set_actor(int actor) { actor_ = actor; }

    // Call before an edit (or at the start of a drag), then commit() once it is done.
    void begin(const Clip& before) {
        pending_ = before;
        open_ = true;
    }
    bool is_open() const { return open_; }
    // Records a step unless nothing changed. Returns whether a step was recorded.
    bool commit(const std::string& label, const Clip& after) {
        if (!open_) return false;
        open_ = false;
        if (pending_ == after) return false;
        undo_.push_back({label, std::move(pending_), after, actor_, {}, {}});
        redo_.clear();
        if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
        return true;
    }
    // Abandons an open step, returning the clip as it was (for a cancelled drag).
    Clip cancel() {
        open_ = false;
        return std::move(pending_);
    }

    // A scene step, recorded at once (actor operations are not drags). ponytail: copies every actor's
    // clip twice per step; diff the actor list if scenes get large.
    void record_scene(const std::string& label, Clip clip_before, SceneState before, Clip clip_after, SceneState after) {
        open_ = false;
        undo_.push_back({label, std::move(clip_before), std::move(clip_after), after.active, std::move(before),
                         std::move(after)});
        redo_.clear();
        if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
    }

    // Not while a step is open (a drag or field edit in progress): its snapshot would be lost (AM-124).
    bool can_undo() const { return !open_ && !undo_.empty(); }
    bool can_redo() const { return !open_ && !redo_.empty(); }
    const std::string& undo_label() const { return undo_.back().label; }
    const std::string& redo_label() const { return redo_.back().label; }

    // Each returns what to show and moves the step to the other stack.
    Restore undo_step() {
        Step s = std::move(undo_.back());
        undo_.pop_back();
        Restore r{s.before, s.scene_before ? s.scene_before->active : s.actor, s.scene_before};
        redo_.push_back(std::move(s));
        return r;
    }
    Restore redo_step() {
        Step s = std::move(redo_.back());
        redo_.pop_back();
        Restore r{s.after, s.scene_after ? s.scene_after->active : s.actor, s.scene_after};
        undo_.push_back(std::move(s));
        return r;
    }
    Clip undo() { return undo_step().clip; }
    Clip redo() { return redo_step().clip; }
    void clear() {
        undo_.clear();
        redo_.clear();
        open_ = false;
    }

private:
    static constexpr size_t kMaxSteps = 500;  // ponytail: whole-clip snapshots; per-track diffs if memory matters
    struct Step {
        std::string label;
        Clip before, after;
        int actor = 0;
        std::optional<SceneState> scene_before, scene_after;
    };
    std::vector<Step> undo_, redo_;
    Clip pending_;
    bool open_ = false;
    int actor_ = 0;
};

}  // namespace vats
