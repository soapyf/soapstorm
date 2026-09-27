/**
 * @file fsvatsclipmotion.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsvatsclipmotion.h"

#include "llcharacter.h"

#include <algorithm>
#include <cmath>

VATsClipMotion::Playback VATsClipMotion::sPlayback;

namespace
{
    constexpr double MAX_OFFSET = 5.0;  // the .anim position range, clamped the same way the exporter does

    double fps() { return std::clamp(VATsClipMotion::sPlayback.clip->fps, 1, 120); }
    double last_frame() { return std::max(VATsClipMotion::sPlayback.clip->end_frame, 1); }
}

VATsClipMotion::VATsClipMotion(const LLUUID& id) : LLMotion(id)
{
    mName = "vats_clip_preview";
}

bool VATsClipMotion::getLoop() { return sPlayback.clip && sPlayback.clip->loop; }

F32 VATsClipMotion::getDuration() { return sPlayback.clip ? F32(last_frame() / fps()) : 0.f; }

F32 VATsClipMotion::getEaseInDuration() { return sPlayback.clip ? F32(sPlayback.clip->ease_in) : 0.f; }

F32 VATsClipMotion::getEaseOutDuration() { return sPlayback.clip ? F32(sPlayback.clip->ease_out) : 0.f; }

LLJoint::JointPriority VATsClipMotion::getPriority()
{
    // The viewer lowers 7 and up (additive) to 6 on import (llkeyframemotion.cpp), so do the same.
    S32 p = sPlayback.clip ? sPlayback.clip->priority : 0;
    return LLJoint::JointPriority(llclamp(p, 0, (S32)LLJoint::ADDITIVE_PRIORITY - 1));
}

LLMotion::LLMotionInitStatus VATsClipMotion::onInitialize(LLCharacter* character)
{
    if (!character || !sPlayback.clip || !sPlayback.skeleton)
        return STATUS_FAILURE;

    mBound.clear();
    for (const Joint& j : sPlayback.joints)
    {
        S32 node = sPlayback.skeleton->find(j.name);
        LLJoint* joint = character->getJoint(j.name);
        if (node < 0 || !joint)
            continue;  // the floater refuses clips with unknown joints before it gets here

        LLPointer<LLJointState> state = new LLJointState(joint);
        state->setUsage(j.position ? LLJointState::ROT | LLJointState::POS : LLJointState::ROT);
        // -1 in a .anim joint means "the animation's priority", as LLKeyframeMotion reads it.
        state->setPriority(j.priority < 0 ? LLJoint::USE_MOTION_PRIORITY
                                          : LLJoint::JointPriority(llmin(j.priority, (S32)LLJoint::ADDITIVE_PRIORITY - 1)));
        addJointState(state);
        mBound.push_back({ node, state, j.position });
    }
    return STATUS_SUCCESS;
}

bool VATsClipMotion::onUpdate(F32 time, U8* joint_mask)
{
    Playback& pb = sPlayback;
    if (!pb.clip || !pb.rig)
        return false;

    const vats::Clip& clip = *pb.clip;
    double frame = pb.frame;
    if (pb.playing)
    {
        if (pb.resync)
        {
            mTimeShift = pb.frame / fps() - time;
            pb.resync = false;
        }
        frame = (time + mTimeShift) * fps();
        if (clip.loop && clip.loop_out > clip.loop_in && frame > clip.loop_out)
            frame = clip.loop_in + std::fmod(frame - clip.loop_in, double(clip.loop_out - clip.loop_in));
        frame = std::clamp(frame, 0.0, last_frame());
        pb.frame = frame;
    }

    // The same values VATs' .anim exporter writes: rest x pose rotation; the pelvis position as an
    // offset, other joints as rest position plus offset, clamped to the format's 5 m.
    const vats::Evaluation e = vats::evaluate(*pb.rig, clip, frame, nullptr);
    for (Bound& b : mBound)
    {
        const vats::Node& node = (*pb.skeleton)[b.node];
        const vats::Quat q = (node.rest * e.pose.rot[b.node]).normalized();
        b.state->setRotation(LLQuaternion(F32(q.x), F32(q.y), F32(q.z), F32(q.w)));
        if (b.position)
        {
            vats::Vec3 p = e.pose.offset[b.node] + (b.node == 0 ? vats::Vec3{} : node.pos);
            b.state->setPosition(LLVector3(F32(std::clamp(p.x, -MAX_OFFSET, MAX_OFFSET)),
                                           F32(std::clamp(p.y, -MAX_OFFSET, MAX_OFFSET)),
                                           F32(std::clamp(p.z, -MAX_OFFSET, MAX_OFFSET))));
        }
    }
    // Non-looping clips stop at their end while playing; a paused preview holds its frame.
    return !pb.playing || clip.loop || frame < last_frame();
}
