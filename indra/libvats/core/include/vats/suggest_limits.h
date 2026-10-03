// Viewport Avatar Toolset - suggest joint limits.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
//
// Spec: docs/spec/08 section 29 (JL). Suggest Limits tool combining anatomical templates, bind pose
// detection for bent-rigged joints, mesh collision sweeps, and optional animation excursions.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vats/clip.h"
#include "vats/dae.h"
#include "vats/rig_constraints.h"
#include "vats/skeleton.h"

namespace vats {

enum class LimitGroup {
    SpineNeckHead = 0,
    LeftArm,
    RightArm,
    LeftHand,
    RightHand,
    LeftLeg,
    RightLeg,
    HindLegs,
    Tail,
    Wings,
    Face,
    Other
};

constexpr int kLimitGroupCount = 12;

LimitGroup joint_limit_group(std::string_view name);
std::string_view limit_group_name(LimitGroup group);
LimitGroup mirror_limit_group(LimitGroup group);

struct SuggestedLimitRow {
    std::string joint;
    JointLimit limit;
};

struct SuggestLimitsOptions {
    bool use_templates = true;
    bool use_bind_pose = true;
    bool use_collision = true;
    bool use_animation = true;
    double collision_margin_deg = 3.0;
    double anim_margin_deg = 10.0;
};

// 1. Templates table: ordinary human anatomy ranges for limbs, spine, neck, head, hands/fingers,
// and Bento extras (tail, wings, hind limbs; face bones stay free).
// The way a bone points at rest, in its parent-relative (SL) frame, on the body shown: toward its first joint child
// as the shape places it (a creature's legs reach out, not down like SL's), else the shape's tail, else SL's bone.
Vec3 rest_bone_dir(const Skeleton& skel, const Shape* shape, int node);

RigConstraints template_limits(const Skeleton& skel, const Shape* shape = nullptr);

// 2. Bind pose detection: the joints Auto IK bends as hinges, rigged bent 20 to 150 degrees, hinge about that hinge from
// straight to a 150 degree fold.
void apply_bind_pose_limits(const Skeleton& skel, const Shape* shape, RigConstraints& constraints);

// 3. Collision sweep: turns each joint from bind pose in small steps, stops where its bone capsules hit other bones' or
// collision volumes (not the other side's same limb). models give the capsules' radii (from skin weights) and the bones
// that take part. A cone gets a stop per direction.
void apply_collision_sweep(const Skeleton& skel, const Shape* shape,
                           const std::vector<const DaeModel*>& models,
                           RigConstraints& constraints, double margin_deg = 3.0);

// 4. Animation excursions: widen limits if the opened clip exceeds them.
void widen_from_animation(const Skeleton& skel, const Shape* shape, const Clip& clip,
                          RigConstraints& constraints, double margin_deg = 10.0);

// Combined pipeline: suggests limits for a body, returning rows sorted by lowest confidence first.
std::vector<SuggestedLimitRow> suggest_joint_limits(const Skeleton& skel, const Shape* shape = nullptr,
                                                    const std::vector<const DaeModel*>& models = {},
                                                    const Clip* clip = nullptr,
                                                    const SuggestLimitsOptions& options = {});

// Snapshot structure for joint limit handle drags.
// Handles both applied limits (in the project) and pending limits (in Suggest Limits).
struct LimitDragSnapshot {
    bool is_pending = false;
    std::unordered_map<std::string, RigConstraints> applied_before;
    RigConstraints pending_before;

    static LimitDragSnapshot capture(bool editing_pending,
                                      const std::unordered_map<std::string, RigConstraints>& applied,
                                      const RigConstraints& pending) {
        LimitDragSnapshot s;
        s.is_pending = editing_pending;
        if (editing_pending) {
            s.pending_before = pending;
        } else {
            s.applied_before = applied;
        }
        return s;
    }

    // Cancels drag. Restores appropriate constraints.
    // Returns true if applied constraints were restored (project is dirty),
    // or false if pending constraints were restored (no project dirty flag).
    bool restore(std::unordered_map<std::string, RigConstraints>& applied,
                 RigConstraints& pending) const {
        if (is_pending) {
            pending = pending_before;
            return false;
        } else {
            applied = applied_before;
            return true;
        }
    }
};

// The pending joints that `include` takes (all when it is empty) and that differ from target's: a missing limit and
// an unlimited one are the same. Apply writes those into target and returns how many changed.
int count_limit_changes(const RigConstraints* target, const RigConstraints& pending,
                        const std::function<bool(const std::string& joint)>& include = nullptr);
int apply_limits(RigConstraints& target, const RigConstraints& pending,
                 const std::function<bool(const std::string& joint)>& include = nullptr);

// Apply Group, Apply Selected and Apply All, with the review's checkboxes as is_checked. Each returns how many limits
// changed.
int apply_group_limits(RigConstraints& target, const RigConstraints& pending, LimitGroup group,
                       const std::function<bool(const std::string& joint)>& is_checked = nullptr);

int apply_selected_limits(RigConstraints& target, const RigConstraints& pending,
                          const std::vector<std::string>& selected_joints,
                          const std::function<bool(const std::string& joint)>& is_checked = nullptr);

int apply_all_limits(RigConstraints& target, const RigConstraints& pending,
                     const std::function<bool(const std::string& joint)>& is_checked = nullptr);

// Set From Pose: the limit widened just enough to hold the joint at local_rot (relative to rest, SL's frame). An
// unlimited joint starts from its suggestion (template, else a 45 degree cone about the bone) and widens from there.
JointLimit widen_limit_to_pose(const Skeleton& skel, const Shape* shape, int node, const JointLimit* existing,
                               const Quat& local_rot);

// The Suggest Limits review: suggestions not yet applied, for the body they were made for.
struct PendingLimits {
    std::string body_id;       // the body they were suggested for; empty when there are none
    RigConstraints limits;     // the suggestions, as edited
    RigConstraints suggested;  // as suggested: Reset goes back to these

    // Edits to `limits` (handle drags, Reset, Set From Pose) undo on their own. Each step keeps the number of
    // project undo steps there were when it was made, so Ctrl+Z takes back whichever edit came last.
    struct Step {
        RigConstraints before;
        size_t depth = 0;
    };
    std::vector<Step> undo, redo;

    void start(const std::string& body, const std::vector<SuggestedLimitRow>& rows);
    void clear() { *this = {}; }
    bool for_body(const std::string& body) const { return !body_id.empty() && body_id == body; }
    // Call after an edit with `limits` as they were before it. False (and no step) when nothing changed.
    bool record(RigConstraints before, size_t depth);
    bool can_undo(size_t depth) const { return !undo.empty() && undo.back().depth == depth; }
    bool can_redo(size_t depth) const { return !redo.empty() && redo.back().depth == depth; }
    bool undo_edit(size_t depth);
    bool redo_edit(size_t depth);
};

// Which limits a Suggest Limits preview means while its panel is open.
enum class LimitPreview { Off, Suggested, Applied };

// The limits the limit tools show and edit: the suggestions while the open panel previews them (and they are this
// body's), else the applied ones.
bool edits_pending_limits(bool panel_open, LimitPreview preview, const PendingLimits& pending,
                          const std::string& body);
// The limits posing uses: none with Respect Joint Limits off or the preview Off, the edited set otherwise.
const RigConstraints* posing_limits(bool respect, bool panel_open, LimitPreview preview, const PendingLimits& pending,
                                    const std::string& body, const RigConstraints* applied);

}  // namespace vats
