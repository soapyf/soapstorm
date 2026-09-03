/**
 * @file ssfloateratmoplanetary.h
 * @brief Atmo Magic: planetary system designer floater.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

#ifndef SS_FLOATERATMOPLANETARY_H
#define SS_FLOATERATMOPLANETARY_H

#include "llfloater.h"
#include "lluictrl.h"

#include <functional>
#include <vector>

struct SSAtmoEnvCelestialBody;
struct SSAtmoEnvPlanetary;

class SSOrbitViewCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params();
    };

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;

    void zoomBy(S32 steps);
    void resetZoom() { mZoom = 1.f; }

    void setPlanetaryAccessor(std::function<SSAtmoEnvPlanetary*()> accessor) { mPlanetary = accessor; }

    void setSelectedIndex(S32 index) { mSelectedIndex = index; }
    void setSelectCallback(std::function<void(S32)> cb) { mOnSelect = cb; }
    void setDragCallback(std::function<void()> cb) { mOnDrag = cb; }

protected:
    friend class LLUICtrlFactory;
    SSOrbitViewCtrl(const Params& p);

private:
    struct Placement
    {
        S32 mIndex = -1;
        bool mResolved = false;
        F32 mAnchorX = 0.f;
        F32 mAnchorY = 0.f;
        F32 mRingRadius = 0.f;
        F32 mTiltRad = 0.f;
        F32 mX = 0.f;
        F32 mY = 0.f;
        F32 mDrawRadius = 4.f;
        S32 mPairPartner = -1;
        F32 mPairCentreX = 0.f;
        F32 mPairCentreY = 0.f;
        F32 mRingCentreX = 0.f;
        F32 mRingCentreY = 0.f;
        F32 mCounterRingRadius = 0.f;
        F32 mCounterCentreX = 0.f;
        F32 mCounterCentreY = 0.f;
        bool mHasCounterHandle = false;
    };

    void computeLayout(const SSAtmoEnvPlanetary& planetary, std::vector<Placement>& out) const;

    static void projectOnRing(F32 anchor_x, F32 anchor_y, F32 ring_radius, F32 tilt_rad,
                              F32 phase_deg, F32& out_x, F32& out_y);

    static F32 inversePhaseDeg(F32 anchor_x, F32 anchor_y, F32 tilt_rad, S32 x, S32 y);

    static bool sunPairMembers(const SSAtmoEnvPlanetary& planetary, S32 index,
                               S32& out_senior, S32& out_junior);

    static void placePairMembers(const SSAtmoEnvPlanetary& planetary, std::vector<Placement>& out,
                                 S32 senior, S32 junior, F32 centre_x, F32 centre_y);

    S32 hitTest(const std::vector<Placement>& placements, S32 x, S32 y, bool& out_on_body) const;

    S32 handleHitTest(const std::vector<Placement>& placements, S32 x, S32 y, bool& out_antipodal) const;

    void drawRing(F32 centre_x, F32 centre_y, F32 radius, F32 tilt_rad, const LLColor4& color) const;

    std::function<SSAtmoEnvPlanetary*()> mPlanetary;
    std::function<void(S32)> mOnSelect;
    std::function<void()> mOnDrag;

    enum EDragMode { DRAG_NONE, DRAG_RING, DRAG_PAIR, DRAG_CENTRE };

    S32 mSelectedIndex = -1;
    S32 mDragIndex = -1;
    EDragMode mDragMode = DRAG_NONE;
    bool mDragAntipodal = false;
    F32 mDragOffsetDeg = 0.f;
    S32 mHoverHandleIndex = -1;
    bool mHoverHandleAntipodal = false;
    S32 mHoverIndex = -1;
    bool mHoverOnBody = false;
    F32 mZoom = 1.f;
};

class SSFloaterAtmoPlanetary : public LLFloater
{
public:
    SSFloaterAtmoPlanetary(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    void setTrack(S32 index);

private:
    SSAtmoEnvPlanetary* planetary();
    SSAtmoEnvCelestialBody* selectedBody();

    void refreshAll();

    void flushFocusedPropertyField();

    void refreshTitle();

    void rebuildBodyList();

    void refreshBodyFields();

    void onSelectBody();
    void onClickAddBody(S32 kind);
    void onClickRemoveBody();

    void onCommitBodyName();
    void onCommitBodyShading();

    void onCommitBodyScalars();
    void onCommitBodyStarType();
    void onCommitBodyHome();
    void onCommitBodyLight();
    void onCommitBodyRing();
    void onCommitBodyTexture();

    void onOrbitSelect(S32 index);
    void onOrbitDrag();

    S32 mTrackIndex = 0;

    S32 mSelectedBodyIndex = 0;

    F64 mLastPoll = 0.0;
};

#endif
