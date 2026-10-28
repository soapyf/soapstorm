/**
 * @file ssfloatergamecontrolgyro.cpp
 * @brief See ssfloatergamecontrolgyro.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SkoomaStorm Viewer Source Code
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssfloatergamecontrolgyro.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llgamecontrol.h"
#include "llsliderctrl.h"
#include "lltextbox.h"

namespace
{
    const char* SLIDERS[] = { "gyro_min_sens", "gyro_max_sens", "gyro_slow_speed", "gyro_fast_speed",
                              "gyro_tighten", "gyro_smooth_speed", "gyro_smooth_time", "gyro_recalibrate" };
}

SSFloaterGameControlGyro::SSFloaterGameControlGyro(const LLSD& key)
:   LLFloater(key)
{
}

bool SSFloaterGameControlGyro::postBuild()
{
    mStatus = getChild<LLTextBox>("gyro_status");
    mReadout = getChild<LLTextBox>("gyro_readout");
    auto commit = [this](LLUICtrl*, const LLSD&) { onCommit(); };
    getChild<LLComboBox>("gyro_engage")->setCommitCallback(commit);
    getChild<LLComboBox>("gyro_space")->setCommitCallback(commit);
    getChild<LLCheckBoxCtrl>("gyro_invert_pitch")->setCommitCallback(commit);
    for (const char* name : SLIDERS)
    {
        getChild<LLSliderCtrl>(name)->setCommitCallback(commit);
    }
    getChild<LLButton>("gyro_calibrate")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLGameControl::calibrateGyro(); });
    // <GMDX> trackpads
    mPadsStatus = getChild<LLTextBox>("pads_status");
    mPadsReadout = getChild<LLTextBox>("pads_readout");
    auto pads_commit = [this](LLUICtrl*, const LLSD&) { onPadsCommit(); };
    for (const char* name : { "pads_right", "pads_left" })
    {
        getChild<LLComboBox>(name)->setCommitCallback(pads_commit);
    }
    for (const char* name : { "pads_sensitivity", "pads_friction" })
    {
        getChild<LLSliderCtrl>(name)->setCommitCallback(pads_commit);
    }
    for (const char* name : { "pads_momentum", "pads_invert_y" })
    {
        getChild<LLCheckBoxCtrl>(name)->setCommitCallback(pads_commit);
    }
    return true;
}

void SSFloaterGameControlGyro::onOpen(const LLSD& key)
{
    refresh();
}

void SSFloaterGameControlGyro::refresh()
{
    const LLGameControl::GyroOptions o = LLGameControl::getGyroOptions();
    getChild<LLComboBox>("gyro_engage")->setValue(std::to_string(o.mEngage));
    getChild<LLComboBox>("gyro_space")->setValue(o.mPlayerSpace ? "player" : "local");
    getChild<LLCheckBoxCtrl>("gyro_invert_pitch")->set(o.mInvertPitch);
    getChild<LLSliderCtrl>("gyro_min_sens")->setValue(o.mMinSens);
    getChild<LLSliderCtrl>("gyro_max_sens")->setValue(o.mMaxSens);
    getChild<LLSliderCtrl>("gyro_slow_speed")->setValue(o.mSlowSpeed);
    getChild<LLSliderCtrl>("gyro_fast_speed")->setValue(o.mFastSpeed);
    getChild<LLSliderCtrl>("gyro_tighten")->setValue(o.mTightenSpeed);
    getChild<LLSliderCtrl>("gyro_smooth_speed")->setValue(o.mSmoothSpeed);
    getChild<LLSliderCtrl>("gyro_smooth_time")->setValue(o.mSmoothTime);
    getChild<LLSliderCtrl>("gyro_recalibrate")->setValue(o.mRecalibrateSeconds);

    // <GMDX> trackpads
    const LLGameControl::TrackpadOptions p = LLGameControl::getTrackpadOptions();
    getChild<LLComboBox>("pads_right")->setValue(std::to_string(p.mRight));
    getChild<LLComboBox>("pads_left")->setValue(std::to_string(p.mLeft));
    getChild<LLSliderCtrl>("pads_sensitivity")->setValue(p.mSensitivity);
    getChild<LLCheckBoxCtrl>("pads_momentum")->set(p.mMomentum);
    getChild<LLSliderCtrl>("pads_friction")->setValue(p.mFriction);
    getChild<LLSliderCtrl>("pads_friction")->setEnabled(p.mMomentum);
    getChild<LLCheckBoxCtrl>("pads_invert_y")->set(p.mInvertY);
}

// <GMDX>
void SSFloaterGameControlGyro::onPadsCommit()
{
    LLGameControl::TrackpadOptions p;
    p.mRight = (U8)getChild<LLComboBox>("pads_right")->getValue().asInteger();
    p.mLeft = (U8)getChild<LLComboBox>("pads_left")->getValue().asInteger();
    p.mSensitivity = getChild<LLSliderCtrl>("pads_sensitivity")->getValueF32();
    p.mMomentum = getChild<LLCheckBoxCtrl>("pads_momentum")->get();
    p.mFriction = getChild<LLSliderCtrl>("pads_friction")->getValueF32();
    p.mInvertY = getChild<LLCheckBoxCtrl>("pads_invert_y")->get();
    getChild<LLSliderCtrl>("pads_friction")->setEnabled(p.mMomentum);
    LLGameControl::setTrackpadOptions(p);
    LLGameControl::saveToSettings();
}

void SSFloaterGameControlGyro::onCommit()
{
    LLGameControl::GyroOptions o;
    o.mEngage = (U8)getChild<LLComboBox>("gyro_engage")->getValue().asInteger();
    o.mPlayerSpace = getChild<LLComboBox>("gyro_space")->getValue().asString() == "player";
    o.mInvertPitch = getChild<LLCheckBoxCtrl>("gyro_invert_pitch")->get();
    o.mMinSens = getChild<LLSliderCtrl>("gyro_min_sens")->getValueF32();
    o.mMaxSens = getChild<LLSliderCtrl>("gyro_max_sens")->getValueF32();
    o.mSlowSpeed = getChild<LLSliderCtrl>("gyro_slow_speed")->getValueF32();
    o.mFastSpeed = getChild<LLSliderCtrl>("gyro_fast_speed")->getValueF32();
    o.mTightenSpeed = getChild<LLSliderCtrl>("gyro_tighten")->getValueF32();
    o.mSmoothSpeed = getChild<LLSliderCtrl>("gyro_smooth_speed")->getValueF32();
    o.mSmoothTime = getChild<LLSliderCtrl>("gyro_smooth_time")->getValueF32();
    o.mRecalibrateSeconds = getChild<LLSliderCtrl>("gyro_recalibrate")->getValueF32();
    LLGameControl::setGyroOptions(o);
    LLGameControl::saveToSettings();
}

void SSFloaterGameControlGyro::draw()
{
    F32 rates[3];
    if (!LLGameControl::hasGyro())
    {
        mStatus->setText(std::string("No controller with a gyro is connected. Gyro aim needs a DualSense, DualShock 4, Switch Pro or Steam Controller."));
        mReadout->setText(LLStringUtil::null);
    }
    else if (LLGameControl::isCalibratingGyro())
    {
        mStatus->setText(std::string("Calibrating: keep the controller still..."));
    }
    else if (LLGameControl::getGyroRates(rates))
    {
        mStatus->setText(std::string("Gyro on. It aims in mouselook; a pad resting still should read close to 0."));
        constexpr F32 DEG = 57.2957795f;
        mReadout->setText(llformat("Pitch %+6.1f   Yaw %+6.1f   Roll %+6.1f  deg/s", rates[0] * DEG, rates[1] * DEG, rates[2] * DEG));
    }
    else
    {
        mStatus->setText(std::string("A controller with a gyro is connected. Choose when gyro aim is on to start it."));
        mReadout->setText(LLStringUtil::null);
    }
    // <GMDX> trackpads
    if (!LLGameControl::hasTrackpads())
    {
        mPadsStatus->setText(std::string("No controller with a trackpad is connected (Steam Controller, Steam Deck, DualSense)."));
        mPadsReadout->setText(LLStringUtil::null);
    }
    else
    {
        mPadsStatus->setText(std::string("Trackpads work in every mode that uses the controller. Pad clicks can be bound in List view, except the left click while the left pad is a d-pad."));
        F32 lx, ly, rx, ry;
        const bool left = LLGameControl::getTrackpadFinger(0, lx, ly);
        const bool right = LLGameControl::getTrackpadFinger(1, rx, ry);
        // (a controller's only touchpad reads as Left here but acts as the right pad)
        mPadsReadout->setText(llformat("Left %s   Right %s",
            left ? llformat("%4.2f,%4.2f", lx, ly).c_str() : "  --     ",
            right ? llformat("%4.2f,%4.2f", rx, ry).c_str() : "  --     "));
    }
    LLFloater::draw();
}
