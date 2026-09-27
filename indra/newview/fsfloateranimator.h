/**
 * @file fsfloateranimator.h
 * @brief Viewport Avatar Toolset in the viewer: open a .vat or .anim and play it on your avatar
 *        (VATs spec 09, stage 6b).
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

#ifndef FS_FLOATERANIMATOR_H
#define FS_FLOATERANIMATOR_H

#include "llfloater.h"

#include <memory>

#include "vats/clip.h"
#include "vats/rig.h"
#include "vats/skeleton.h"

class LLSliderCtrl;

class FSFloaterAnimator : public LLFloater
{
public:
    FSFloaterAnimator(const LLSD& key);
    ~FSFloaterAnimator() override;

    bool postBuild() override;
    void onClose(bool app_quitting) override;
    void draw() override;

private:
    void onOpenFile();
    void onFileChosen(const std::vector<std::string>& files);
    void onPlayPause();
    void onStop();
    void onScrub();
    void onLoop();

    bool loadSkeleton();
    void start();  // (re)starts the preview from the current frame
    void stop();   // removes the preview motion from the avatar
    void refresh();
    void setStatus(const std::string& text);

    vats::Skeleton mSkeleton;
    std::unique_ptr<vats::Rig> mRig;
    vats::Clip mClip;
    bool mHaveClip = false;
    LLUUID mMotionID;
    bool mMotionIsKeyframe = false;  // "Preview as uploaded": the viewer's own LLKeyframeMotion
    LLSliderCtrl* mScrub = nullptr;
};

#endif // FS_FLOATERANIMATOR_H
