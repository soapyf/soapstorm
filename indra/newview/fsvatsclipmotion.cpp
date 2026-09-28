/**
 * @file fsvatsclipmotion.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsvatsclipmotion.h"

#include "llcharacter.h"
#include "lljoint.h"

#include <algorithm>
#include <cmath>

VATsClipMotion::Playback VATsClipMotion::sEditor;

namespace
{
    constexpr double MAX_OFFSET = 5.0;  // the .anim position range, clamped the same way the exporter does
}

VATsClipMotion::VATsClipMotion(const LLUUID& id) : LLMotion(id)
{
    mName = "vats_editor";
}

LLMotion::LLMotionInitStatus VATsClipMotion::onInitialize(LLCharacter* character)
{
    if (!character || !sEditor.skeleton)
        return STATUS_FAILURE;

    mCharacter = character;
    mBound.clear();
    for (const Joint& j : sEditor.joints)
    {
        S32 node = sEditor.skeleton->find(j.name);
        LLJoint* joint = character->getJoint(j.name);
        if (node < 0 || !joint)
            continue;

        LLPointer<LLJointState> state = new LLJointState(joint);
        state->setUsage(j.position ? LLJointState::ROT | LLJointState::POS : LLJointState::ROT);
        // The editor's hold: above any animation, as Black Dragon's poser does. As it plays in-world: the priority the
        // uploaded animation gives this joint, so it blends with the AO and the rest as it will there.
        state->setPriority(LLJoint::JointPriority(j.priority));
        addJointState(state);
        mBound.push_back({ node, state, j.position, j.base });
    }
    LL_INFOS("VATsAnimator") << mName << " bound " << mBound.size() << " of " << sEditor.joints.size() << " joints"
                              << LL_ENDL;
    return STATUS_SUCCESS;
}

bool VATsClipMotion::onUpdate(F32, U8*)
{
    if (!sEditor.pose || !sEditor.skeleton)
        return true;  // nothing to show this frame; the editor stops the motion

    // Rotations as VATs' .anim exporter writes them (rest x pose rotation). Positions are the pose's offsets on
    // the joint's own position in the worn avatar (Joint::base: sliders, mesh joint offsets), not on VATs'
    // default rest, so a mesh head keeps its shape; clamped to the format's 5 m.
    const vats::Pose& pose = *sEditor.pose;
    // The pin: the root is where the region put the avatar this frame; the pelvis is placed so the body is drawn
    // where the root was when pinned (Playback::pin).
    LLJoint* root = mCharacter ? mCharacter->getRootJoint() : nullptr;
    const bool pin = sEditor.pin && root;
    if (root)
        sEditor.root_at_update = root->getPosition();
    if (pin && !sEditor.pinned)
    {
        sEditor.pin_pos = root->getPosition();
        sEditor.pin_rot = root->getRotation();
        sEditor.pinned = true;
    }
    for (Bound& b : mBound)
    {
        if (b.node >= (S32)pose.rot.size())
            continue;
        const vats::Node& node = (*sEditor.skeleton)[b.node];
        const vats::Quat q = (node.rest * pose.rot[b.node]).normalized();
        vats::Vec3 p = pose.offset[b.node] + vats::Vec3{ b.base.mV[VX], b.base.mV[VY], b.base.mV[VZ] };
        // Never hand the avatar a non-finite value: it would spread to the head, the camera and the
        // agent updates sent to the region.
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w) ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        {
            if (!mWarnedNonFinite)
                LL_WARNS("VATsAnimator") << "non-finite value for " << node.name << "; the joint is left as it was" << LL_ENDL;
            mWarnedNonFinite = true;
            continue;
        }
        LLQuaternion rot(F32(q.x), F32(q.y), F32(q.z), F32(q.w));
        LLVector3 pos(F32(std::clamp(p.x, -MAX_OFFSET, MAX_OFFSET)), F32(std::clamp(p.y, -MAX_OFFSET, MAX_OFFSET)),
                      F32(std::clamp(p.z, -MAX_OFFSET, MAX_OFFSET)));
        if (pin && b.node == 0)
        {
            // Pelvis in the root's frame such that its world placement is the one it would have on the pinned root.
            const LLQuaternion inv_root = ~root->getRotation();
            rot = rot * sEditor.pin_rot * inv_root;
            pos = (sEditor.pin_pos - root->getPosition() + pos * sEditor.pin_rot) * inv_root;
        }
        b.state->setRotation(rot);
        if (b.position)
        {
            b.state->setPosition(pos);
        }
    }
    return true;  // the editor removes the motion when it closes
}
