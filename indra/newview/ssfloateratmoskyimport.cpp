/**
 * @file ssfloateratmoskyimport.cpp
 * @brief See ssfloateratmoskyimport.h.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssfloateratmoskyimport.h"

#include "ssatmoenvmanager.h"
#include "ssdiscpad.h" // <SS:Nexii> auto-derived disc padding for adopted disc textures
#include "ssfloateratmoenv.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llfloaterreg.h"
#include "lltextbox.h"

// Registration name this floater lives under - see LLViewerFloaterReg.
static const std::string IMPORT_FLOATER_NAME = "ss_atmo_sky_import";

// Checkbox name to SS_SKY_IMPORT_* flag, one row per grouping.
struct SSImportGroupRow
{
    const char* mName;
    U32 mFlag;
};

// The four field groups stamp as keyframes and are always on offer - every EEP sky carries them.
static const SSImportGroupRow FIELD_GROUPS[] =
{
    { "import_atmosphere", SS_SKY_IMPORT_ATMOSPHERE },
    { "import_lighting",   SS_SKY_IMPORT_LIGHTING },
    { "import_celestial",  SS_SKY_IMPORT_CELESTIAL },
    { "import_clouds",     SS_SKY_IMPORT_CLOUDS },
};

// The body groups are offered only when the target track still carries the standard sun/moon -
// see SSAtmoEnvPlanetary::standardSunIndex. A redesigned body is the author's work, not a slot
// for a dropped sky to overwrite.
static const SSImportGroupRow BODY_GROUPS[] =
{
    { "import_sun",  SS_SKY_IMPORT_SUN },
    { "import_moon", SS_SKY_IMPORT_MOON },
};

// Floater shell; all content is wired in postBuild.
SSFloaterAtmoSkyImport::SSFloaterAtmoSkyImport(const LLSD& key) :
    LLFloater(key)
{
}

// Wires the checkboxes (an empty selection parks the import button) and the two buttons.
bool SSFloaterAtmoSkyImport::postBuild()
{
    for (const SSImportGroupRow& row : FIELD_GROUPS)
    {
        getChild<LLUICtrl>(row.mName)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { refresh(); });
    }
    for (const SSImportGroupRow& row : BODY_GROUPS)
    {
        getChild<LLUICtrl>(row.mName)->setCommitCallback(
            [this](LLUICtrl*, const LLSD&) { refresh(); });
    }

    getChild<LLUICtrl>("import_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onClickImport(); });
    getChild<LLUICtrl>("cancel_button")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { closeFloater(); });

    refresh();
    return true;
}

// Opens the floater, then arms it. Racing drops only matter in that the second payload
// replaces the first - the newest sky is the one the author dropped last.
void SSFloaterAtmoSkyImport::show(LLSettingsSky::ptr_t sky, S32 track_index, F64 phase,
                                  const std::string& item_name, LLHandle<LLFloater> parent)
{
    if (!sky) return;

    LLFloaterReg::showInstance(IMPORT_FLOATER_NAME);

    SSFloaterAtmoSkyImport* floater =
        LLFloaterReg::findTypedInstance<SSFloaterAtmoSkyImport>(IMPORT_FLOATER_NAME);
    if (!floater) return;

    floater->setPayload(sky, track_index, phase, item_name, parent);
}

// Arms the dialog with a fresh drop. Groupings default to all on: an empty pick
// is refused by the button, not silently half-imported; unticking is deliberate.
// Body groups appear only when the track's system still has the standard sun/moon to
// translate onto - a drop offers what exists, nothing more.
void SSFloaterAtmoSkyImport::setPayload(LLSettingsSky::ptr_t sky, S32 track_index, F64 phase,
                                        const std::string& item_name, LLHandle<LLFloater> parent)
{
    mSky = sky;
    mTrackIndex = track_index;
    mPhase = phase;
    mItemName = item_name;
    mParent = parent;

    bool have_standard_sun = false;
    bool have_standard_moon = false;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (mgr->hasAsset()
        && track_index >= 0 && track_index < (S32)mgr->asset().mTracks.size())
    {
        const SSAtmoEnvPlanetary& planetary = mgr->asset().mTracks[(size_t)track_index].mPlanetary;
        have_standard_sun = planetary.standardSunIndex() >= 0;
        have_standard_moon = planetary.standardMoonIndex() >= 0;
    }

    for (const SSImportGroupRow& row : FIELD_GROUPS)
    {
        getChild<LLCheckBoxCtrl>(row.mName)->set(true);
    }
    for (const SSImportGroupRow& row : BODY_GROUPS)
    {
        LLCheckBoxCtrl* check = getChild<LLCheckBoxCtrl>(row.mName);
        const bool offer = (row.mFlag == SS_SKY_IMPORT_SUN) ? have_standard_sun : have_standard_moon;
        check->setVisible(offer);
        check->set(offer);
    }

    refresh();
}

// The groupings the checkboxes currently ask for.
U32 SSFloaterAtmoSkyImport::checkedGroups() const
{
    U32 groups = 0;
    for (const SSImportGroupRow& row : FIELD_GROUPS)
    {
        if (getChild<LLCheckBoxCtrl>(row.mName)->get())
        {
            groups |= row.mFlag;
        }
    }
    for (const SSImportGroupRow& row : BODY_GROUPS)
    {
        LLCheckBoxCtrl* check = getChild<LLCheckBoxCtrl>(row.mName);
        if (check->isInVisibleChain() && check->get())
        {
            groups |= row.mFlag;
        }
    }
    return groups;
}

// Target line and button state. The asset the drop was captured against may have been unloaded
// or reshaped while the dialog sat open - the text says so rather than letting Import
// silently do nothing.
void SSFloaterAtmoSkyImport::refresh()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    const bool have_track = mgr->hasAsset()
        && mTrackIndex >= 0
        && mTrackIndex < (S32)mgr->asset().mTracks.size();

    std::string target;
    if (!have_track)
    {
        target = "No track to import into - the environment was unloaded or changed.";
    }
    else
    {
        const SSAtmoEnvTrack& track = mgr->asset().mTracks[(size_t)mTrackIndex];
        target = llformat("Importing \"%s\" into track \"%s\" at %d%% of its cycle.",
                          mItemName.c_str(), track.mName.c_str(),
                          (S32)(mPhase * 100.0 + 0.5));
    }
    getChild<LLTextBox>("target_text")->setText(target);

    const bool can_import = have_track && mSky && checkedGroups() != 0;
    getChild<LLUICtrl>("import_button")->setEnabled(can_import);
}

// Stamps the checked groupings into the captured track at the captured phase. The guards
// refresh() showed are re-run - not from stale text, but the asset can change
// between the last poll and this click.
void SSFloaterAtmoSkyImport::onClickImport()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (mSky && mgr->hasAsset())
    {
        SSAtmoEnvAsset& asset = mgr->editable();
        if (mTrackIndex >= 0 && mTrackIndex < (S32)asset.mTracks.size())
        {
            const U32 groups = checkedGroups();
            if (groups)
            {
                SSAtmoEnvTrack& track = asset.mTracks[(size_t)mTrackIndex];

                // <SS:Nexii> Snapshot the standard bodies' disc textures so an adopted texture is distinguishable from one the sky left at stock. The bodies below may be rewritten - the same standard-body checks it performs - so a snapshot:body match stays meaningful across it.
                S32 sun_body = -1;
                S32 moon_body = -1;
                LLUUID sun_texture_before;
                LLUUID moon_texture_before;
                if (groups & SS_SKY_IMPORT_SUN)
                {
                    sun_body = track.mPlanetary.standardSunIndex();
                    if (sun_body >= 0)
                    {
                        sun_texture_before =
                            track.mPlanetary.mBodies[(size_t)sun_body].mCustomTexture;
                    }
                }
                if (groups & SS_SKY_IMPORT_MOON)
                {
                    moon_body = track.mPlanetary.standardMoonIndex();
                    if (moon_body >= 0)
                    {
                        moon_texture_before =
                            track.mPlanetary.mBodies[(size_t)moon_body].mCustomTexture;
                    }
                }

                track.mAtmosphere.addKeyframesFromSky(*mSky, mPhase, groups);
                track.mCloudDome.addKeyframesFromSky(*mSky, mPhase, groups);
                // The body groups re-check their standard bodies inside - whatever the author
                // redesigned between drop and click is left as they left it.
                track.mPlanetary.translateSettingsSky(*mSky, groups);

                // <SS:Nexii> A disc texture the import actually adopted (the sky's own sun/moon art, not a stock value) gets its disc padding auto-derived from the alpha, like a hand-picked texture (ssdiscpad.h; gated on SSAtmoDiscPadAuto). A still-loading texture is left in the module's retry slot - the environment floater's draw poll lands it.
                if (sun_body >= 0)
                {
                    const LLUUID& adopted =
                        track.mPlanetary.mBodies[(size_t)sun_body].mCustomTexture;
                    LL_INFOS("AtmoMagicEnv") << "Sky import sun body " << sun_body
                        << ": texture before " << sun_texture_before << ", adopted " << adopted
                        << ", groups 0x" << std::hex << groups << std::dec << LL_ENDL;
                    if (adopted != sun_texture_before)
                    {
                        LL_INFOS("AtmoMagicEnv") << "Sky import auto-deriving disc padding for sun "
                            << sun_body << " from " << adopted << LL_ENDL;
                        ssDiscPadAutoDerive(mTrackIndex, sun_body, adopted);
                    }
                    else
                    {
                        LL_INFOS("AtmoMagicEnv") << "Sky import sun body " << sun_body
                            << ": adopted texture equals pre-import texture - no derive" << LL_ENDL;
                    }
                }
                if (moon_body >= 0)
                {
                    const LLUUID& adopted =
                        track.mPlanetary.mBodies[(size_t)moon_body].mCustomTexture;
                    LL_INFOS("AtmoMagicEnv") << "Sky import moon body " << moon_body
                        << ": texture before " << moon_texture_before << ", adopted " << adopted
                        << ", groups 0x" << std::hex << groups << std::dec << LL_ENDL;
                    if (adopted != moon_texture_before)
                    {
                        LL_INFOS("AtmoMagicEnv") << "Sky import auto-deriving disc padding for moon "
                            << moon_body << " from " << adopted << LL_ENDL;
                        ssDiscPadAutoDerive(mTrackIndex, moon_body, adopted);
                    }
                    else
                    {
                        LL_INFOS("AtmoMagicEnv") << "Sky import moon body " << moon_body
                            << ": adopted texture equals pre-import texture - no derive" << LL_ENDL;
                    }
                }
                // One immediate re-check of a just-queued derivation - the sky's own disc art
                // is the common case and is usually already decoded by now.
                ssDiscPadPoll();

                if (SSFloaterAtmoEnv* parent = (SSFloaterAtmoEnv*)mParent.get())
                {
                    parent->refreshPreview();
                    parent->refreshStatus();
                }
            }
        }
    }

    mSky = nullptr;
    closeFloater();
}
