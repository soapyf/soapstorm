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
#include "lltimer.h"

#include <algorithm>
#include <cmath>

VATsClipMotion::Playback VATsClipMotion::sPlayback;
VATsClipMotion::Playback VATsClipMotion::sLive;
VATsClipMotion::Playback VATsClipMotion::sEditor;

namespace
{
    constexpr double MAX_OFFSET = 5.0;  // the .anim position range, clamped the same way the exporter does

    double fps(const VATsClipMotion::Playback& pb) { return std::clamp(pb.clip->fps, 1, 120); }
    double last_frame(const VATsClipMotion::Playback& pb) { return std::max(pb.clip->end_frame, 1); }
}

VATsClipMotion::VATsClipMotion(const LLUUID& id, Playback* playback) : LLMotion(id), mPb(playback)
{
    mName = playback == &sLive ? "vats_live_capture" : playback == &sEditor ? "vats_editor" : "vats_clip_preview";
}

bool VATsClipMotion::getLoop() { return true; }  // never stopped by the controller; see getDuration

// Zero: the preview keeps its own time and decides when to stop. With a duration the motion controller
// schedules its own stop at (duration - ease out) after activation (llmotioncontroller.cpp), which ended
// the preview almost at once and handed the avatar back to the AO.
F32 VATsClipMotion::getDuration() { return 0.f; }

F32 VATsClipMotion::getEaseInDuration() { return mPb->clip && mPb != &sLive && !mPb->pose ? F32(mPb->clip->ease_in) : 0.f; }

F32 VATsClipMotion::getEaseOutDuration() { return 0.f; }  // the floater's Stop ends the preview

LLJoint::JointPriority VATsClipMotion::getPriority()
{
    // The viewer lowers 7 and up (additive) to 6 on import (llkeyframemotion.cpp), so do the same.
    S32 p = mPb->clip ? mPb->clip->priority : 0;
    return LLJoint::JointPriority(llclamp(p, 0, (S32)LLJoint::ADDITIVE_PRIORITY - 1));
}

LLMotion::LLMotionInitStatus VATsClipMotion::onInitialize(LLCharacter* character)
{
    if (!character || !mPb->clip || !mPb->skeleton)
        return STATUS_FAILURE;

    mBound.clear();
    for (const Joint& j : mPb->joints)
    {
        S32 node = mPb->skeleton->find(j.name);
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
    LL_INFOS("VATsAnimator") << mName << " bound " << mBound.size() << " of " << mPb->joints.size() << " joints"
                              << LL_ENDL;
    return STATUS_SUCCESS;
}

bool VATsClipMotion::onUpdate(F32 time, U8* joint_mask)
{
    Playback& pb = *mPb;
    if (!pb.clip || (!pb.rig && !pb.pose))
        return false;

    const vats::Clip& clip = *pb.clip;
    double frame = pb.frame;
    if (pb.playing && !pb.pose)
    {
        // The preview's own clock, not the motion time: it does not depend on how or when the controller
        // activated the motion.
        const F64 now = LLTimer::getTotalSeconds();
        if (pb.resync)
        {
            mPlayStart = now - pb.frame / fps(pb);
            pb.resync = false;
            pb.finished = false;
        }
        frame = (now - mPlayStart) * fps(pb);
        if (clip.loop && clip.loop_out > clip.loop_in && frame > clip.loop_out)
            frame = clip.loop_in + std::fmod(frame - clip.loop_in, double(clip.loop_out - clip.loop_in));
        if (!clip.loop && frame >= last_frame(pb))
        {
            frame = last_frame(pb);  // hold the last frame, like an editor; Play restarts
            pb.playing = false;
            pb.finished = true;
        }
        frame = std::clamp(frame, 0.0, last_frame(pb));
        pb.frame = frame;
    }

    // The same values VATs' .anim exporter writes: rest x pose rotation; the pelvis position as an
    // offset, other joints as rest position plus offset, clamped to the format's 5 m.
    vats::Evaluation e;
    if (!pb.pose)
    {
        e = vats::evaluate(*pb.rig, clip, frame, nullptr);
        if (pb.post)
            pb.post(e, frame);
    }
    const vats::Pose& pose = pb.pose ? *pb.pose : e.pose;
    for (Bound& b : mBound)
    {
        if (b.node >= (S32)pose.rot.size())
            continue;
        const vats::Node& node = (*pb.skeleton)[b.node];
        const vats::Quat q = (node.rest * pose.rot[b.node]).normalized();
        vats::Vec3 p = pose.offset[b.node] + (b.node == 0 ? vats::Vec3{} : node.pos);
        // Never hand the avatar a non-finite value: it would spread to the head, the camera and the
        // agent updates sent to the region.
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w) ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        {
            if (!mWarnedNonFinite)
                LL_WARNS("VATsAnimator") << "non-finite value for " << node.name << " at frame " << frame
                                          << "; the joint is left as it was" << LL_ENDL;
            mWarnedNonFinite = true;
            continue;
        }
        b.state->setRotation(LLQuaternion(F32(q.x), F32(q.y), F32(q.z), F32(q.w)));
        if (b.position)
        {
            b.state->setPosition(LLVector3(F32(std::clamp(p.x, -MAX_OFFSET, MAX_OFFSET)),
                                           F32(std::clamp(p.y, -MAX_OFFSET, MAX_OFFSET)),
                                           F32(std::clamp(p.z, -MAX_OFFSET, MAX_OFFSET))));
        }
    }
    return true;  // the floater's Stop removes the motion
}
