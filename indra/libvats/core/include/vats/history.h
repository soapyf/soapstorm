// Viewport Avatar Toolset - undo and redo.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Snapshot-based, one step per user operation (spec 02 section 2.13).
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vats/clip.h"
#include "vats/project.h"

namespace vats {

// The actor list of a couple or group scene (spec 08 GR), for whole-scene steps: adding, removing,
// renaming or placing actors. The active actor's clip travels separately, as in Project. Since spec 08 CL also the
// clip list, for switching, adding, removing, renaming and moving clips.
struct SceneState {
    std::vector<Actor> actors;
    int active = 0;
    std::vector<ClipSlot> clips = {};
    int active_clip = 0;
    std::unordered_map<std::string, RigConstraints> joint_limits = {};
    std::map<std::string, MeshLook> mesh_looks = {};  // spec 08 SK-3
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
        ++serial_;
        if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
        recorded_ = true;
        return true;
    }
    // True once after commit() recorded a clip step, for edits made after every edit (blocking mode, spec 08 KT-2).
    bool take_recorded() { return std::exchange(recorded_, false); }
    // The last step's clip before it, and a way to replace its clip after it (the follow-up edit joins the step).
    const Clip& last_before() const { return undo_.back().before; }
    void amend_last(const Clip& after) { undo_.back().after = after, ++serial_; }
    // Abandons an open step, returning the clip as it was (for a cancelled drag).
    Clip cancel() {
        open_ = false;
        return std::move(pending_);
    }

    // A scene step, recorded at once (actor operations are not drags). ponytail: copies every actor's
    // clip twice per step; diff the actor list if scenes get large.
    void record_scene(const std::string& label, Clip clip_before, SceneState before, Clip clip_after, SceneState after) {
        open_ = recorded_ = false;
        undo_.push_back({label, std::move(clip_before), std::move(clip_after), after.active, std::move(before),
                         std::move(after)});
        redo_.clear();
        ++serial_;
        if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
    }

    // Not while a step is open (a drag or field edit in progress): its snapshot would be lost (AM-124).
    bool can_undo() const { return !open_ && !undo_.empty(); }
    bool can_redo() const { return !open_ && !redo_.empty(); }
    const std::string& undo_label() const { return undo_.back().label; }
    const std::string& redo_label() const { return redo_.back().label; }

    // Each returns what to show and moves the step to the other stack.
    Restore undo_step() {
        recorded_ = false;
        Step s = std::move(undo_.back());
        undo_.pop_back();
        ++serial_;
        Restore r{s.before, s.scene_before ? s.scene_before->active : s.actor, s.scene_before};
        redo_.push_back(std::move(s));
        return r;
    }
    Restore redo_step() {
        recorded_ = false;
        Step s = std::move(redo_.back());
        redo_.pop_back();
        ++serial_;
        Restore r{s.after, s.scene_after ? s.scene_after->active : s.actor, s.scene_after};
        undo_.push_back(std::move(s));
        return r;
    }
    Clip undo() { return undo_step().clip; }
    Clip redo() { return redo_step().clip; }
    void clear() {
        undo_.clear();
        redo_.clear();
        open_ = recorded_ = false;
        ++serial_;
    }

    struct Step {
        std::string label;
        Clip before, after;
        int actor = 0;
        std::optional<SceneState> scene_before, scene_after;
    };
    // The steps undo would take back, oldest first, for readers of past steps (Motion Quality compares a tool's
    // before and after), and a number that changes whenever that list does.
    const std::vector<Step>& undo_steps() const { return undo_; }
    const std::vector<Step>& redo_steps() const { return redo_; }  // the steps undone, the next redo last
    unsigned long long serial() const { return serial_; }

private:
    static constexpr size_t kMaxSteps = 500;  // ponytail: whole-clip snapshots; per-track diffs if memory matters
    std::vector<Step> undo_, redo_;
    Clip pending_;
    bool open_ = false;
    bool recorded_ = false;
    int actor_ = 0;
    unsigned long long serial_ = 0;
};

}  // namespace vats
