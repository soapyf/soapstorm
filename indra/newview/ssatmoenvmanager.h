/**
 * @file ssatmoenvmanager.h
 * @brief Atmo Magic: environment asset load/hold/persist.
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

#ifndef SS_ATMOENVMGR_H
#define SS_ATMOENVMGR_H

#include "llassettype.h"
#include "llextendedstatus.h"
#include "llsingleton.h"
#include "lluuid.h"

#include <functional>
#include <string>
#include <vector>

#include "ssatmoenvasset.h"

class LLInventoryItem;

// <SS:Nexii> The notecard text codec behind every Atmo Magic read path. A saved environment is deflate+base64 in an SS-ATMO-ENV-COMPRESSED magic-header wrapper, not readable LLSD; ss_atmo_env_payload_is_compressed() asks whether a body is that wrapper, and ss_atmo_env_from_notecard_text() decodes either it or a plain XML/notation document into LLSD. The environment manager uses these directly and the legacy per-track layer routes parcel notecards through them too, so a parcel card written by this save path never reads as "not valid LLSD".
bool ss_atmo_env_payload_is_compressed(const std::string& text);
bool ss_atmo_env_from_notecard_text(const std::string& text, LLSD& out_sd, std::string& out_error);

class SSAtmoEnvManager : public LLSingleton<SSAtmoEnvManager>
{
    LLSINGLETON(SSAtmoEnvManager);
    ~SSAtmoEnvManager() = default;

public:
    bool hasAsset() const { return mHasAsset; }

    void unload();

    const LLUUID& sourceAssetId() const { return mSourceAssetId; }
    bool cameFromParcel() const { return mFromParcel; }
    void noteSource(const LLUUID& asset_id, bool from_parcel)
    {
        mSourceAssetId = asset_id;
        mFromParcel = from_parcel;
    }

    const SSAtmoEnvAsset& asset() const { return mWorking; }
    SSAtmoEnvAsset& editable() { return mWorking; }

    bool isModified() const;
    void revertToBaseline();

    // <SS:Nexii> The creation seeds behind the floater's Create Environment chooser. All write a fresh notecard into the Atmo Magic folder and hand it back through on_created. The stock day cycle: the four shipped seed skies, each measured against the track's own sun and stamped as keyframes. Also the generic inventory "New Atmo Environment" path.
    static void createDefaultNotecard(const LLUUID& parent_id,
                                       std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created);

    // Empty: the midday defaults and nothing else - no fetching, no seeding.
    static void createEmptyNotecard(const LLUUID& parent_id,
                                    std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created);

    // A list of skies of the author's choosing, run through the same measure-and-stamp algorithm as the stock day cycle. Every sky lands at the phase where its dominant body reaches the height the sky drew it - the order of the day falls out of the heights. The inventory name sorts the placement to the right side of noon and names the fixed anchors: Sunrise, Sunset, Noon and Midnight pin their phases outright ("Daylight" implies Noon, "Night" implies Midnight - a night sky is never parked in daylight even without a moon). A "Morning Umbra" with its sun below the horizon lands in the pre-dawn hours, not in daylight. Names are log-only otherwise and may be empty. An empty list makes the empty environment.
    static void createFromSkies(const std::vector<LLUUID>& sky_asset_ids,
                                const std::vector<std::string>& sky_names,
                                const LLUUID& parent_id,
                                std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created);

    // An EEP day cycle asset, mapped over: every sky keyframe on its ground-level track is stamped at the day cycle's own keyframe time, so the authored timings carry across rather than being re-derived from the suns. The water track is not mapped - the v3 water block is a different vocabulary from the EEP water preset set.
    static void createFromDayCycle(const LLUUID& day_cycle_asset_id,
                                   const LLUUID& parent_id,
                                   std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created);

    // An EEP water preset: the empty (midday defaults) environment with the ground track's water block stamped from the preset. The block's height and emissive stay at the defaults, and no sky work is fetched - the water maps synchronously after the asset arrives.
    static void createFromWater(const LLUUID& water_asset_id,
                                const LLUUID& parent_id,
                                std::function<void(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset)> on_created);

    // <SS:Nexii> Stamps a group of dropped EEP skies ACROSS one track of a LIVE asset as a day cycle - the loaded-environment counterpart to createFromSkies. Each sky lands at the same measured phase the seeding uses (name-pinned anchors sketch the sun path, the other skies sit on it by their dominant body's height), and every field grouping is taken - a holistic re-skin of the track, not the single-point, grouping-choice import the one-sky drop offers. on_done(false) only for a bad track index or when no sky could be fetched. The asset mutates via the live mWorking reference; the caller keeps it stable (the floater holds busy state across the fetch).
    static void stampSkiesOnTrack(SSAtmoEnvAsset& asset, S32 track_index,
                                  const std::vector<LLUUID>& sky_asset_ids,
                                  const std::vector<std::string>& sky_names,
                                  std::function<void(bool success)> on_done);

    // <SS:Nexii> Seeds a world template onto ONE track of an existing asset. The template's world settings overwrite wholesale, and the track's sky reseeds as the stock four-sky day cycle (the Default create path's measured-phase stamp) with the template's atmosphere columns tinted over every stamped keyframe, so the archetype's mood rides the whole day instead of flattening it. Falls back to the plain constant template when the seed skies cannot be fetched. on_done(false) only for an unknown key or a bad track index.
    static void applyTemplateToTrack(SSAtmoEnvAsset& asset, S32 track_index, const std::string& key,
                                     std::function<void(bool success)> on_done);

    void adoptCreated(const LLUUID& item_id, const LLUUID& asset_id, const SSAtmoEnvAsset& asset);

    static void atmoFolderId(std::function<void(const LLUUID&)> on_ready);

    bool loadFromInventory(const LLInventoryItem* item, std::function<void(bool success)> on_complete = nullptr);

    void loadFromAssetId(const LLUUID& asset_id);

    bool applyExternalLLSD(const LLUUID& source_id, const LLSD& sd);
    bool applyExternalNotecardText(const LLUUID& source_id, const std::string& text);

    void saveNotecard(const std::string& name);

    const std::string& statusText() const { return mStatus; }
    const std::string& configName() const { return mHasAsset ? mWorking.mName : mNoAssetName; }
    const LLUUID& configAssetId() const { return mAssetID; }

    void setPreviewPhaseOverride(F64 phase) { mPreviewOverrideActive = true; mPreviewOverridePhase = phase; }
    void clearPreviewPhaseOverride() { mPreviewOverrideActive = false; }
    bool hasPreviewPhaseOverride() const { return mPreviewOverrideActive; }
    F64 previewPhaseOverride() const { return mPreviewOverridePhase; }

private:
    static void onAssetLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                               void* user_data, S32 status, LLExtStat ext_status);
    bool applyNotecardText(const std::string& text, bool from_inventory_permission_check);

    bool adoptParsedAsset(const LLSD& sd);

    void updateExistingNotecard(const std::string& name);

    void finishLoad(bool success);

    SSAtmoEnvAsset mBaseline;
    SSAtmoEnvAsset mWorking;
    bool mHasAsset = false;

    LLUUID mSourceAssetId;
    bool mFromParcel = false;

    LLUUID mAssetID;
    LLUUID mItemID;
    LLUUID mPendingID;
    LLUUID mPendingItemID;
    std::function<void(bool)> mLoadCompleteCallback;

    std::string mStatus = "no environment loaded";
    const std::string mNoAssetName = "(none)";

    bool mPreviewOverrideActive = false;
    F64 mPreviewOverridePhase = 0.0;
};

#endif
