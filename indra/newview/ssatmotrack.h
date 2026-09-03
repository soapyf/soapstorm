/**
 * @file ssatmotrack.h
 * @brief Atmo Magic: legacy per-track weather config.
 *
 *        Editing follows the environment floaters: a loaded notecard is the
 *        baseline, local edits sit on top of it and are marked with an
 *        asterisk, and they can be reverted or saved back out to a notecard.
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

#ifndef SS_ATMOTRACK_H
#define SS_ATMOTRACK_H

#include "llassettype.h"
#include "llextendedstatus.h"
#include "llquaternion.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "llviewerparcelmgr.h"
#include "v3math.h"
#include "v3color.h"

#include <array>
#include <string>

class LLInventoryItem;

const S32 SS_TRACK_MIN   = 1;
const S32 SS_TRACK_MAX   = 4;
const S32 SS_TRACK_COUNT = 4;

struct SSAtmoTrackConfig
{
    bool mDefined = false;
    bool mEnabled = false;

    std::string mPreset;
    F32 mPrecipitation = 0.5f;
    F32 mTurbulence    = 0.3f;

    // <SS:Nexii> The weather cube's per-band drop character (SSAtmoEnvWeatherState::mDropletSizeScale / mImpactScale), -1 when no environment provides them. Derived every refresh like the lightning intervals below - never serialised, a v2 notecard has no intensity bands to author.
    F32 mDropletScale = -1.f;
    F32 mImpactScale  = -1.f;

    LLQuaternion mWindRot;
    F32 mWindSpeed = 4.f;

    F32 mTemperatureC = 15.f;

    LLColor3 mLightningColor{0.62f, 0.55f, 1.f};
    F32 mLightningCoreWhite = 0.85f;

    bool mLightning = true;
    bool mLightningCharge = true;
    bool mLightningSparks = true;
    F32 mLightningIntervalMin = -1.f;
    F32 mLightningIntervalMax = -1.f;
    F32 mLightningIntensity = -1.f;

    F32 mGustDepth  = 1.f;
    F32 mGustLength = 140.f;
    F32 mGustVeer   = 14.f;

    bool mHasGround = false;
    F32  mGround    = 0.f;

    F32 mFallThrough = 1.f;

    bool runs() const { return mDefined && mEnabled; }

    LLVector3 windDirection() const;

    F32  heading() const;
    F32  elevation() const;
    void setHeadingElevation(F32 heading_deg, F32 elevation_deg);

    LLSD asLLSD() const;
    void fromLLSD(const LLSD& sd);

    bool operator==(const SSAtmoTrackConfig& rhs) const;
    bool operator!=(const SSAtmoTrackConfig& rhs) const { return !(*this == rhs); }
};

typedef std::array<SSAtmoTrackConfig, SS_TRACK_COUNT> ss_track_set_t;

class SSAtmoTrackManager : public LLSingleton<SSAtmoTrackManager>, public LLParcelObserver
{
    LLSINGLETON(SSAtmoTrackManager);
    ~SSAtmoTrackManager();

public:
    enum ESource
    {
        SOURCE_DEFAULT = 0,
        SOURCE_PARCEL,
        SOURCE_INVENTORY
    };

    void changed() override;

    void idle();

    S32 currentTrack() const;

    const SSAtmoTrackConfig& config(S32 track) const;
    const SSAtmoTrackConfig& active() const { return config(currentTrack()); }

    SSAtmoTrackConfig& editable(S32 track);
    void commit();

    bool isModified() const;
    bool isModified(S32 track) const;

    void revertToBaseline();
    void resetToDefaults();

    F32 trackFloor(S32 track) const;
    F32 trackCeiling(S32 track) const;

    bool isSkyTrack(S32 track) const { return track > SS_TRACK_MIN; }

    ESource source() const { return mSource; }
    const std::string& statusText() const { return mStatus; }
    const std::string& configName() const { return mConfigName; }
    const LLUUID& configAsset() const { return mAssetID; }

    void reload();

    bool importFromInventory(const LLInventoryItem* item);

    void exportToNotecard(const std::string& name);

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd, ss_track_set_t& out) const;

private:
    void requestNotecard(const LLUUID& asset_id, ESource source, const std::string& name);
    void applyNotecardText(const std::string& text);
    void adoptBaseline(const ss_track_set_t& set, ESource source, const std::string& name);
    void loadWorking();
    void saveWorking();

    static void onNotecardLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                 void* user_data, S32 status, LLExtStat ext_status);

    static LLUUID parseDescription(const std::string& desc);

    ss_track_set_t mBaseline;
    ss_track_set_t mWorking;

    ESource     mSource = SOURCE_DEFAULT;
    std::string mStatus = "system defaults";
    std::string mConfigName;

    LLUUID      mAssetID;
    LLUUID      mPendingID;
    ESource     mPendingSource = SOURCE_DEFAULT;
    std::string mPendingName;

    bool mLoaded = false;
};

#endif
