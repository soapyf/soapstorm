/**
 * @file fsanimtimelinectrl.h
 * @brief VATs Animator timeline strip: frame ruler, playhead and key diamonds; click or drag to scrub
 *        (VATs spec 09, stage 6d).
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

#ifndef FS_ANIMTIMELINECTRL_H
#define FS_ANIMTIMELINECTRL_H

#include "lluictrl.h"

#include <functional>
#include <utility>
#include <vector>

class FSAnimTimelineCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params> {};

    // Set by the owner; the control only draws and reports scrubs.
    std::function<int()> lastFrame;                 // frames run 0..lastFrame
    std::function<double()> currentFrame;
    std::function<std::vector<double>()> keyFrames;  // frames holding a key on the shown joints
    std::function<void(double)> onScrub;             // the user clicked or dragged to this frame
    std::function<std::vector<std::pair<double, double>>()> bands;  // pins on the shown joints: [from, to] frames

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;

protected:
    friend class LLUICtrlFactory;
    FSAnimTimelineCtrl(const Params& p) : LLUICtrl(p) {}

private:
    S32 xOf(double frame) const;
    double frameAt(S32 x) const;
};

#endif // FS_ANIMTIMELINECTRL_H
