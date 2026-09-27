/**
 * @file fsvatsclipmotion.h
 * @brief Shows the VATs editor's pose on the local avatar (VATs spec 09, stages 6b, U3, U4).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (c) 2026 Viewport Avatar Toolset contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef FS_VATSCLIPMOTION_H
#define FS_VATSCLIPMOTION_H

#include "llmotion.h"
#include "lljointstate.h"

#include "vats/skeleton.h"

#include <string>
#include <vector>

// Shows the pose the VATs editor evaluated (clip, IK, pins and its live previews), handed over every frame
// through sEditor, with exactly the rotations and positions VATs' .anim exporter writes. Only the local avatar
// plays it; nobody else sees it. The editor binds every joint of the avatar, each at the top priority, so its
// pose is the only one shown (FSVATsEditor also stops the avatar's other motions while it is open). Binding
// every joint at the top priority is the approach of Black Dragon's poser (BDPosingMotion, Black Dragon Viewer
// by NiranV Dean, LGPL-2.1).
class VATsClipMotion : public LLMotion
{
public:
    struct Joint
    {
        std::string name;       // viewer joint name
        bool position = false;  // the pose sets its position too: base plus the pose's offset
        LLVector3 base;         // the joint's own position (the worn avatar's), in its parent's frame
    };
    struct Playback
    {
        const vats::Skeleton* skeleton = nullptr;
        const vats::Pose* pose = nullptr;  // shown as is every update
        std::vector<Joint> joints;          // bound when the motion initialises
        // The visual pin (spec 09 U4b): while pin is on, the avatar is drawn with its root where it was when the
        // pin was taken, whatever the region does to its position. Local only; nothing is sent.
        bool pin = false;                   // set by the editor every frame (off while seated on an object)
        bool pinned = false;                // pin_pos and pin_rot hold the root as first seen with pin on
        LLVector3 pin_pos;                  // agent frame
        LLQuaternion pin_rot;
        LLVector3 root_at_update;           // the root's real position at the last update (for the editor's drawing)
    };
    static Playback sEditor;

    explicit VATsClipMotion(const LLUUID& id);
    static LLMotion* create(const LLUUID& id) { return new VATsClipMotion(id); }

    bool getLoop() override { return true; }
    // Zero: the editor decides when to stop. With a duration the motion controller schedules its own stop.
    F32 getDuration() override { return 0.f; }
    F32 getEaseInDuration() override { return 0.f; }
    F32 getEaseOutDuration() override { return 0.f; }
    LLJoint::JointPriority getPriority() override { return LLJoint::ADDITIVE_PRIORITY; }
    LLMotionBlendType getBlendType() override { return NORMAL_BLEND; }
    F32 getMinPixelArea() override { return 0.f; }
    LLMotionInitStatus onInitialize(LLCharacter* character) override;
    bool onActivate() override { return true; }
    bool onUpdate(F32 time, U8* joint_mask) override;
    void onDeactivate() override {}

private:
    struct Bound
    {
        S32 node;
        LLPointer<LLJointState> state;
        bool position;
        LLVector3 base;
    };
    std::vector<Bound> mBound;
    LLCharacter* mCharacter = nullptr;
    bool mWarnedNonFinite = false;
};

#endif // FS_VATSCLIPMOTION_H
