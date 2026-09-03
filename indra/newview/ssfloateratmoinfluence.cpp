/**
 * @file ssfloateratmoinfluence.cpp
 * @brief See ssfloateratmoinfluence.h.
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

#include "llviewerprecompiledheaders.h"

#include "ssfloateratmoinfluence.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvmanager.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llfocusmgr.h"
#include "llsliderctrl.h"
#include "lltextbox.h"

static const F64 STATUS_POLL_INTERVAL = 0.25;

// Floater shell; all content is wired in postBuild.
SSFloaterAtmoInfluence::SSFloaterAtmoInfluence(const LLSD& key) :
    LLFloater(key)
{
}

// One table row per influence mapping: accessors into the asset plus a live-effect probe from the applier.
void SSFloaterAtmoInfluence::buildRows()
{
    auto effect = [](std::function<F32(const SSAtmoEnvSkyModulation&)> pick)
    {
        return [pick]() -> F32
        {
            return pick(SSAtmoEnvApplier::instance().lastModulation());
        };
    };

    mRows = {
        { "cover",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mCloudCoverEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mCloudCoverStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mCoverTarget * m.mCoverBlend; }) },

        { "wind",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mWindScrollEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mWindScrollStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mWind; }) },

        { "water_fog",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mWaterFogEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mWaterFogStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mPrecip; }) },

        { "storm",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mStormDarkeningEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mStormDarkeningStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mDarkening; }) },

        { "cold",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mColdSkyEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mColdSkyStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mCold; }) },

        { "rainbow",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mRainbowEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mRainbowStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mRainbow; }) },

        { "corona",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mCoronaEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mCoronaStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mCorona; }) },

        { "ice_halo",
          [](SSAtmoEnvWeatherInfluence& i) -> bool& { return i.mIceHaloEnabled; },
          [](SSAtmoEnvWeatherInfluence& i) -> F32&  { return i.mIceHaloStrength; },
          effect([](const SSAtmoEnvSkyModulation& m) { return m.mIceHalo; }) },
    };
}

// Wires master, per-row and reset callbacks from the row table.
bool SSFloaterAtmoInfluence::postBuild()
{
    buildRows();

    getChild<LLUICtrl>("master_enabled")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitMaster(); });

    for (const Row& row : mRows)
    {
        const Row captured = row;
        getChild<LLUICtrl>(row.mPrefix + "_enabled")->setCommitCallback(
            [this, captured](LLUICtrl*, const LLSD&) { onCommitRow(captured); });
        getChild<LLUICtrl>(row.mPrefix + "_strength")->setCommitCallback(
            [this, captured](LLUICtrl*, const LLSD&) { onCommitRow(captured); });
    }

    getChild<LLUICtrl>("reset_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickReset(); });

    refreshAll();
    return true;
}

// Opens targeting the track index passed in the key.
void SSFloaterAtmoInfluence::onOpen(const LLSD& key)
{
    setTrack(key.asInteger());
}

// Retargets the floater at another track.
void SSFloaterAtmoInfluence::setTrack(S32 index)
{
    mTrackIndex = index;
    refreshAll();
}

// The edited track's influence block in the LIVE asset, or false when nothing is loaded.
bool SSFloaterAtmoInfluence::influence(SSAtmoEnvWeatherInfluence** out) const
{
    *out = nullptr;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return false;

    SSAtmoEnvAsset& asset = mgr->editable();
    if (mTrackIndex < 0 || mTrackIndex >= (S32)asset.mTracks.size()) return false;

    *out = &asset.mTracks[(size_t)mTrackIndex].mWeatherInfluence;
    return true;
}

// Polls fast; only the readouts refresh while a slider is captured, so the drag is not fought.
void SSFloaterAtmoInfluence::draw()
{
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mLastPoll > STATUS_POLL_INTERVAL)
    {
        mLastPoll = now;

        LLView* captured = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());
        if (!captured || !captured->hasAncestor(this))
        {
            refreshAll();
        }
        else
        {
            refreshReadouts();
        }
    }

    LLFloater::draw();
}

// Rewrites every control's enable/value state from the asset.
void SSFloaterAtmoInfluence::refreshAll()
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    const bool have = influence(&infl);

    LLCheckBoxCtrl* master = getChild<LLCheckBoxCtrl>("master_enabled");
    master->setEnabled(have);
    master->set(have && infl->mEnabled);

    const bool rows_live = have && infl->mEnabled;

    for (const Row& row : mRows)
    {
        LLCheckBoxCtrl* check = getChild<LLCheckBoxCtrl>(row.mPrefix + "_enabled");
        LLSliderCtrl* slider = getChild<LLSliderCtrl>(row.mPrefix + "_strength");

        check->setEnabled(rows_live);
        slider->setEnabled(rows_live && row.mEnabled(*infl));

        if (have)
        {
            check->set(row.mEnabled(*infl));
            slider->setValue(row.mStrength(*infl));
        }
    }

    getChild<LLUICtrl>("reset_button")->setEnabled(have);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    getChild<LLTextBox>("track_text")->setText(
        have ? ("Editing: " + mgr->asset().mTracks[(size_t)mTrackIndex].mName)
             : std::string("No environment loaded."));

    refreshReadouts();
}

// Live percentage per row of what each mapping is doing right now.
void SSFloaterAtmoInfluence::refreshReadouts()
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    const bool have = influence(&infl);

    for (const Row& row : mRows)
    {
        LLTextBox* readout = getChild<LLTextBox>(row.mPrefix + "_readout");
        if (!have || !infl->mEnabled || !row.mEnabled(*infl))
        {
            readout->setText(std::string("off"));
            continue;
        }

        const F32 effect = llclamp(row.mEffect(), 0.f, 1.f);
        readout->setText(llformat("%d%% now", (S32)(effect * 100.f + 0.5f)));
    }
}

// Master toggle straight into the asset.
void SSFloaterAtmoInfluence::onCommitMaster()
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    if (!influence(&infl)) return;

    infl->mEnabled = getChild<LLCheckBoxCtrl>("master_enabled")->get();
    refreshAll();
}

// Row toggle and strength straight into the asset.
void SSFloaterAtmoInfluence::onCommitRow(const Row& row)
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    if (!influence(&infl)) return;

    row.mEnabled(*infl) = getChild<LLCheckBoxCtrl>(row.mPrefix + "_enabled")->get();
    row.mStrength(*infl) = llclamp(
        (F32)getChild<LLSliderCtrl>(row.mPrefix + "_strength")->getValueF32(), 0.f, 1.f);

    getChild<LLSliderCtrl>(row.mPrefix + "_strength")->setEnabled(row.mEnabled(*infl));
    refreshReadouts();
}

// Back to default influence settings.
void SSFloaterAtmoInfluence::onClickReset()
{
    SSAtmoEnvWeatherInfluence* infl = nullptr;
    if (!influence(&infl)) return;

    *infl = SSAtmoEnvWeatherInfluence();
    refreshAll();
}
