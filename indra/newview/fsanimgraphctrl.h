/**
 * @file fsanimgraphctrl.h
 * @brief VATs Animator graph editor: the selected joints' curves, keys to select, drag and box select,
 *        tangent commands (VATs spec 09, stage 6e). The same editing as the VATs app's graph editor; every
 *        edit is a libvats curve_ops call recorded in the Animator's History.
 *
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

#ifndef FS_ANIMGRAPHCTRL_H
#define FS_ANIMGRAPHCTRL_H

#include "lluictrl.h"

#include <functional>
#include <string>
#include <vector>

#include "vats/curve_ops.h"
#include "vats/history.h"

class FSAnimGraphCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params> {};

    // Set by the owner.
    vats::Clip* clip = nullptr;  // null while no clip is open
    vats::History* history = nullptr;
    std::function<std::vector<std::string>()> tracks;  // the joints whose curves show
    std::function<double()> currentFrame;
    std::function<void(double)> onScrub;
    std::function<void()> changed;  // an edit was committed

    void frameAll();
    void frameSelected();
    void applyTangent(vats::Tangent t, const std::string& label);
    void deleteSelected();
    void clipReplaced(bool refit);  // New, Open (refit), Undo, Redo: the selection refers to the old clip
    bool hasSelection() const { return !mSel.empty(); }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleMiddleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMiddleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    void onMouseCaptureLost() override;

protected:
    friend class LLUICtrlFactory;
    FSAnimGraphCtrl(const Params& p) : LLUICtrl(p) {}

private:
    struct Channel
    {
        std::string track, channel;
        LLColor4 colour;
    };
    enum class Drag { Nothing, Scrub, Move, Box, Handle, Pan };  // not None: X11 defines it as a macro

    void build();  // channels from tracks(); drops keys no longer there from the selection
    const vats::FCurve* curve(const Channel& c) const;
    bool selected(const Channel& c, int key) const;
    int curveUnder(S32 x, S32 y) const;
    void fit(double f0, double f1, double v0, double v1);
    void finishDrag(bool cancel);
    F32 xOf(double f) const;
    F32 yOf(double v) const;
    double fAt(S32 x) const;
    double vAt(S32 y) const;
    S32 plotHeight() const;

    std::vector<std::string> mTracks;
    std::vector<Channel> mChannels;
    std::vector<vats::KeyRef> mSel;
    double mT0 = -2, mT1 = 32, mV0 = -100, mV1 = 100;
    bool mFitPending = true;

    Drag mDrag = Drag::Nothing;
    S32 mPressX = 0, mPressY = 0, mX = 0, mY = 0;
    MASK mPressMask = 0;
    vats::Clip mPressClip;
    std::vector<vats::KeyRef> mPressSel;
    double mPressT0 = 0, mPressT1 = 0, mPressV0 = 0, mPressV1 = 0;
    vats::KeyRef mHandleKey;
    bool mHandleRight = false;
};

#endif // FS_ANIMGRAPHCTRL_H
