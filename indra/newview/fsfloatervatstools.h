/**
 * @file fsfloatervatstools.h
 * @brief VATs Tools: IK, Hold/Bind pins, hand and body poses (VATs spec 09 stage 6f), working on the
 *        VATs Animator's open clip through its undo history.
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

#ifndef FS_FLOATERVATSTOOLS_H
#define FS_FLOATERVATSTOOLS_H

#include "llfloater.h"

#include <string>

class FSFloaterAnimator;

class FSFloaterVATsTools : public LLFloater
{
public:
    FSFloaterVATsTools(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    FSFloaterAnimator* animator(bool need_clip = true);  // the Animator, or null with the reason in the status
    void fillLists(FSFloaterAnimator* a);  // limbs, joints and starter poses, once the skeleton is loaded
    void refreshLimb();
    // IK (6f)
    void onSwitchIk(bool to_ik);
    void onKeyIkFromAvatar();
    // Hold and Bind
    void onHold();
    void onBind();
    void onRelease();
    void onDeletePin();
    // Poses
    void onApplyPose();

    void setStatus(const std::string& text);

    bool mFilled = false;
};

#endif // FS_FLOATERVATSTOOLS_H
