/**
 * @file fsvatsclipmotion.h
 * @brief Plays a Viewport Avatar Toolset clip on the local avatar (VATs spec 09, stage 6b).
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

#include "vats/clip.h"
#include "vats/rig.h"
#include "vats/skeleton.h"

// Evaluates a vats::Clip each frame (FK, IK and pins) and writes the result into joint states, with
// exactly the rotations and positions VATs' .anim exporter would write, so the preview matches the
// uploaded file. Only the local avatar plays one, so the floater and the motion share one Playback.
class VATsClipMotion : public LLMotion
{
public:
    struct Joint
    {
        std::string name;  // viewer joint name, as in the exported .anim
        S32 priority = 0;
        bool position = false;
    };
    struct Playback
    {
        const vats::Skeleton* skeleton = nullptr;
        const vats::Rig* rig = nullptr;
        const vats::Clip* clip = nullptr;
        std::vector<Joint> joints;  // the joints the exported .anim animates
        bool playing = true;
        bool resync = true;  // playing restarts from `frame`
        double frame = 0;    // the frame shown; while paused, the frame to hold
    };
    static Playback sPlayback;

    VATsClipMotion(const LLUUID& id);
    static LLMotion* create(const LLUUID& id) { return new VATsClipMotion(id); }

    bool getLoop() override;
    F32 getDuration() override;
    F32 getEaseInDuration() override;
    F32 getEaseOutDuration() override;
    LLJoint::JointPriority getPriority() override;
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
    };
    std::vector<Bound> mBound;
    F64 mTimeShift = 0.0;  // seconds added to the motion's time so playing resumes at the held frame
};

#endif // FS_VATSCLIPMOTION_H
