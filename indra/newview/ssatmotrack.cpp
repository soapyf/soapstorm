/**
 * @file ssatmotrack.cpp
 * @brief See ssatmotrack.h.
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

#include "ssatmotrack.h"

#include "ssatmoenvmanager.h"
#include "ssatmostore.h"

#include <sstream>

#include "llnotecard.h"
#include "llnotificationsutil.h"
#include "llpermissionsflags.h"
#include "roles_constants.h"
#include "llsdserialize.h"
#include "llsdutil_math.h"

#include "llagent.h"
#include "llassetstorage.h"
#include "llenvironment.h"
#include "llfilesystem.h"
#include "llfloaterperms.h"
#include "llinventorymodel.h"
#include "llparcel.h"
#include "llviewerassetupload.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewermessage.h"
#include "llviewerregion.h"

static const char* CONFIG_TAG = "atmo:";
static const F32   FIELD_EPSILON = 1e-4f;

static const SSAtmoTrackConfig sEmptyConfig;

// North rotated by the stored orientation, normalised so a hand-authored quaternion still behaves.
LLVector3 SSAtmoTrackConfig::windDirection() const
{
    LLVector3 dir = LLVector3(0.f, 1.f, 0.f) * mWindRot;
    if (dir.magVecSquared() < F_APPROXIMATELY_ZERO)
    {
        return LLVector3(0.f, 1.f, 0.f);
    }
    dir.normVec();
    return dir;
}

// Wind heading in degrees, back-derived from the quaternion.
F32 SSAtmoTrackConfig::heading() const
{
    const LLVector3 dir = windDirection();
    F32 deg = atan2f(dir.mV[VX], dir.mV[VY]) * RAD_TO_DEG;
    if (deg < 0.f) deg += 360.f;
    return deg;
}

// Wind elevation in degrees, back-derived from the quaternion.
F32 SSAtmoTrackConfig::elevation() const
{
    return asinf(llclamp(windDirection().mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
}

// Builds the wind quaternion from the two angles a person can author.
void SSAtmoTrackConfig::setHeadingElevation(F32 heading_deg, F32 elevation_deg)
{
    const F32 h = heading_deg * DEG_TO_RAD;
    const F32 e = llclamp(elevation_deg, -89.f, 89.f) * DEG_TO_RAD;
    const F32 ce = cosf(e);

    const LLVector3 dir(sinf(h) * ce, cosf(h) * ce, sinf(e));
    mWindRot.shortestArc(LLVector3(0.f, 1.f, 0.f), dir);
}

// Serialised as heading/elevation, not a raw quaternion - the angles a notecard author can write.
LLSD SSAtmoTrackConfig::asLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["enabled"]        = mEnabled;
    sd["preset"]         = mPreset;
    sd["precipitation"]  = (LLSD::Real)mPrecipitation;
    sd["turbulence"]     = (LLSD::Real)mTurbulence;
    sd["wind_heading"]   = (LLSD::Real)heading();
    sd["wind_elevation"] = (LLSD::Real)elevation();
    sd["wind_speed"]     = (LLSD::Real)mWindSpeed;
    sd["gust_depth"]     = (LLSD::Real)mGustDepth;
    sd["gust_length"]    = (LLSD::Real)mGustLength;
    sd["gust_veer"]      = (LLSD::Real)mGustVeer;
    sd["fallthrough"]    = (LLSD::Real)mFallThrough;
    if (mHasGround)
    {
        sd["ground"] = (LLSD::Real)mGround;
    }
    return sd;
}

// Reads a track's fields tolerantly; a track present with no 'enabled' key still counts as weather.
void SSAtmoTrackConfig::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return;

    mDefined = true;
    mEnabled = sd.has("enabled") ? sd["enabled"].asBoolean() : true;

    if (sd.has("preset"))        mPreset        = sd["preset"].asString();
    if (sd.has("precipitation")) mPrecipitation = llclamp((F32)sd["precipitation"].asReal(), 0.f, 1.f);
    if (sd.has("turbulence"))    mTurbulence    = llclamp((F32)sd["turbulence"].asReal(), 0.f, 1.f);
    if (sd.has("wind_speed"))    mWindSpeed     = llmax(0.f, (F32)sd["wind_speed"].asReal());
    if (sd.has("gust_depth"))    mGustDepth     = llclamp((F32)sd["gust_depth"].asReal(), 0.f, 3.f);
    if (sd.has("gust_length"))   mGustLength    = llclamp((F32)sd["gust_length"].asReal(), 8.f, 2000.f);
    if (sd.has("gust_veer"))     mGustVeer      = llclamp((F32)sd["gust_veer"].asReal(), 0.f, 90.f);
    if (sd.has("fallthrough"))   mFallThrough   = llclamp((F32)sd["fallthrough"].asReal(), 0.f, 1.f);

    if (sd.has("wind_rot"))
    {
        mWindRot = ll_quaternion_from_sd(sd["wind_rot"]);
    }
    else if (sd.has("wind_heading") || sd.has("wind_elevation"))
    {
        setHeadingElevation((F32)sd["wind_heading"].asReal(),
                            (F32)sd["wind_elevation"].asReal());
    }

    if (sd.has("ground"))
    {
        mHasGround = true;
        mGround = (F32)sd["ground"].asReal();
    }
}

bool SSAtmoTrackConfig::operator==(const SSAtmoTrackConfig& rhs) const
{
    auto alike = [](F32 a, F32 b) { return llabs(a - b) < FIELD_EPSILON; };

    return mDefined == rhs.mDefined
        && mEnabled == rhs.mEnabled
        && mPreset == rhs.mPreset
        && alike(mPrecipitation, rhs.mPrecipitation)
        && alike(mTurbulence, rhs.mTurbulence)
        && alike(mWindSpeed, rhs.mWindSpeed)
        && alike(mGustDepth, rhs.mGustDepth)
        && alike(mGustLength, rhs.mGustLength)
        && alike(mGustVeer, rhs.mGustVeer)
        && alike(mFallThrough, rhs.mFallThrough)
        && mHasGround == rhs.mHasGround
        && (!mHasGround || alike(mGround, rhs.mGround))
        && alike(heading(), rhs.heading())
        && alike(elevation(), rhs.elevation());
}

// Observes parcel changes from construction - crossings and description edits both fire it.
SSAtmoTrackManager::SSAtmoTrackManager()
{
    LLViewerParcelMgr::getInstance()->addObserver(this);
}

SSAtmoTrackManager::~SSAtmoTrackManager()
{
    if (LLViewerParcelMgr::instanceExists())
    {
        LLViewerParcelMgr::getInstance()->removeObserver(this);
    }
}

// Which sky track the CAMERA's altitude is in - weather is what you look at.
S32 SSAtmoTrackManager::currentTrack() const
{
    const F32 z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    return llclamp(LLEnvironment::instance().calculateSkyTrackForAltitude((F64)z),
                   SS_TRACK_MIN, SS_TRACK_MAX);
}

// The band's base altitude.
F32 SSAtmoTrackManager::trackFloor(S32 track) const
{
    const LLEnvironment::altitude_list_t& alts = LLEnvironment::instance().getRegionAltitudes();
    return alts[llclamp(track, SS_TRACK_MIN, SS_TRACK_MAX) - 1];
}

// The band's top; the highest band is open ended.
F32 SSAtmoTrackManager::trackCeiling(S32 track) const
{
    const LLEnvironment::altitude_list_t& alts = LLEnvironment::instance().getRegionAltitudes();
    const S32 t = llclamp(track, SS_TRACK_MIN, SS_TRACK_MAX);
    if (t >= SS_TRACK_MAX) return F32_MAX;
    return alts[t];
}

// Read access to a track's config.
const SSAtmoTrackConfig& SSAtmoTrackManager::config(S32 track) const
{
    if (track < SS_TRACK_MIN || track > SS_TRACK_MAX) return sEmptyConfig;
    return mWorking[track - SS_TRACK_MIN];
}

// Write access to a track's working config.
SSAtmoTrackConfig& SSAtmoTrackManager::editable(S32 track)
{
    return mWorking[llclamp(track, SS_TRACK_MIN, SS_TRACK_MAX) - SS_TRACK_MIN];
}

// The working set becomes the baseline.
void SSAtmoTrackManager::commit()
{
    saveWorking();
}

// Whether one track differs from baseline - epsilon compare, since LLSD round-trips wobble floats.
bool SSAtmoTrackManager::isModified(S32 track) const
{
    if (track < SS_TRACK_MIN || track > SS_TRACK_MAX) return false;
    const S32 i = track - SS_TRACK_MIN;
    return mWorking[i] != mBaseline[i];
}

// Whether any track differs from baseline.
bool SSAtmoTrackManager::isModified() const
{
    for (S32 i = 0; i < SS_TRACK_COUNT; ++i)
    {
        if (mWorking[i] != mBaseline[i]) return true;
    }
    return false;
}

// Working set back to the baseline.
void SSAtmoTrackManager::revertToBaseline()
{
    mWorking = mBaseline;
    saveWorking();
}

// All tracks back to defaults.
void SSAtmoTrackManager::resetToDefaults()
{
    mBaseline.fill(SSAtmoTrackConfig());
    mWorking = mBaseline;
    mSource = SOURCE_DEFAULT;
    mAssetID.setNull();
    mPendingID.setNull();
    mConfigName.clear();
    mStatus = "system defaults";
    saveWorking();
}

// Installs a track set as the new baseline and records where it came from.
void SSAtmoTrackManager::adoptBaseline(const ss_track_set_t& set, ESource source, const std::string& name)
{
    mBaseline = set;
    mWorking = set;
    mSource = source;
    mConfigName = name;

    S32 defined = 0;
    for (const SSAtmoTrackConfig& cfg : set)
    {
        if (cfg.mDefined) ++defined;
    }

    const char* origin = (source == SOURCE_PARCEL) ? "parcel" : "notecard";
    mStatus = defined ? llformat("%s: %s (%d track%s)", origin, name.c_str(), defined,
                                 defined == 1 ? "" : "s")
                      : llformat("%s: %s (no tracks)", origin, name.c_str());

    saveWorking();
}

// The whole config as the notecard document.
LLSD SSAtmoTrackManager::asLLSD() const
{
    LLSD tracks = LLSD::emptyMap();
    for (S32 track = SS_TRACK_MIN; track <= SS_TRACK_MAX; ++track)
    {
        const SSAtmoTrackConfig& cfg = mWorking[track - SS_TRACK_MIN];
        if (!cfg.mDefined) continue;
        tracks[llformat("%d", track)] = cfg.asLLSD();
    }

    LLSD sd = LLSD::emptyMap();
    sd["version"] = 1;
    if (!mConfigName.empty()) sd["name"] = mConfigName;
    sd["tracks"] = tracks;
    return sd;
}

// Parses a config document, tolerant of bare track maps and loose 'track N' spellings.
bool SSAtmoTrackManager::fromLLSD(const LLSD& sd, ss_track_set_t& out) const
{
    out.fill(SSAtmoTrackConfig());
    if (!sd.isMap()) return false;

    const LLSD& tracks = sd.has("tracks") ? sd["tracks"] : sd;
    if (!tracks.isMap()) return false;

    bool any = false;
    for (S32 track = SS_TRACK_MIN; track <= SS_TRACK_MAX; ++track)
    {
        const std::string key = llformat("%d", track);
        const LLSD* entry = nullptr;

        if (tracks.has(key))
        {
            entry = &tracks[key];
        }
        else
        {
            const std::string alt = llformat("track%d", track);
            const std::string alt_spaced = llformat("track %d", track);
            if (tracks.has(alt)) entry = &tracks[alt];
            else if (tracks.has(alt_spaced)) entry = &tracks[alt_spaced];
        }

        if (!entry || !entry->isMap()) continue;

        out[track - SS_TRACK_MIN].fromLLSD(*entry);
        any = true;
    }

    return any;
}

// Parses notecard text (XML or notation, whichever works) into the working set.
void SSAtmoTrackManager::applyNotecardText(const std::string& text)
{
    LLSD sd;

    // <SS:Nexii> Notecard saving and drag-and-drop now write the environment in the compressed v3 format - an SS-ATMO-ENV-COMPRESSED magic header around deflate+base64 binary LLSD - and a parcel can advertise exactly such a card. That reader is shared (ssatmoenvmanager.h), so decode through it, keeping the XML/notation path below for hand-authored cards. A decoded v3 environment is not a legacy track set - the environment discovery system applies it - so stand aside, don't reject the binary payload.
    if (ss_atmo_env_payload_is_compressed(text))
    {
        std::string error;
        if (!ss_atmo_env_from_notecard_text(text, sd, error))
        {
            LL_WARNS("AtmoMagic") << "Atmo config notecard is a compressed payload but unreadable: "
                                  << error << LL_ENDL;
            mStatus = "notecard is not valid LLSD";
            return;
        }
    }
    else
    {
        std::istringstream stream(text);

        bool parsed = false;
        if (text.find("<llsd") != std::string::npos)
        {
            parsed = (LLSDSerialize::fromXML(sd, stream) != LLSDParser::PARSE_FAILURE);
        }
        if (!parsed)
        {
            std::istringstream retry(text);
            parsed = (LLSDSerialize::fromNotation(sd, retry, (S32)text.size()) != LLSDParser::PARSE_FAILURE);
        }

        if (!parsed)
        {
            LL_WARNS("AtmoMagic") << "Atmo config notecard is not valid LLSD" << LL_ENDL;
            mStatus = "notecard is not valid LLSD";
            return;
        }
    }

    ss_track_set_t set;
    if (!fromLLSD(sd, set))
    {
        // <SS:Nexii> The document may be a clean v3 environment card, not garbage: its tracks are an array, not the legacy track map this reader expects. The environment system owns applying it, so name that rather than a generic "no tracks".
        if (sd.isMap() && sd.has("tracks") && sd["tracks"].isArray())
        {
            mStatus = "notecard is a v3 environment; the environment system applies it";
        }
        else
        {
            mStatus = "notecard defined no tracks";
        }
        return;
    }

    std::string name = mPendingName;
    if (sd.isMap() && sd.has("name")) name = sd["name"].asString();

    adoptBaseline(set, mPendingSource, name);
}

// Finds 'atmo:<uuid>' in a parcel description - case-insensitive, allowed inside prose.
LLUUID SSAtmoTrackManager::parseDescription(const std::string& desc)
{
    const std::string lower = utf8str_tolower(desc);
    const std::string::size_type tag_len = strlen(CONFIG_TAG);
    std::string::size_type pos = 0;

    while ((pos = lower.find(CONFIG_TAG, pos)) != std::string::npos)
    {
        std::string::size_type start = pos + tag_len;
        while (start < desc.size() && isspace((unsigned char)desc[start])) ++start;

        if (start + UUID_STR_SIZE <= desc.size())
        {
            const std::string candidate = desc.substr(start, UUID_STR_SIZE);
            if (LLUUID::validate(candidate))
            {
                return LLUUID(candidate);
            }
        }
        pos = start;
    }

    return LLUUID::null;
}

// Parcel changed: fetch, refetch or drop the parcel config; hand-loaded configs survive crossings.
void SSAtmoTrackManager::changed()
{
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    const std::string desc = parcel ? parcel->getDesc() : LLStringUtil::null;
    const LLUUID asset_id = parseDescription(desc);

    if (asset_id.isNull())
    {
        if (mSource == SOURCE_PARCEL)
        {
            resetToDefaults();
        }
        return;
    }

    if (asset_id == mAssetID || asset_id == mPendingID) return;

    const std::string name = parcel ? parcel->getName() : std::string("parcel");
    requestNotecard(asset_id, SOURCE_PARCEL, name);
}

// Refetches the current source's notecard.
void SSAtmoTrackManager::reload()
{
    mAssetID.setNull();
    mPendingID.setNull();
    changed();
}

// Async notecard fetch.
void SSAtmoTrackManager::requestNotecard(const LLUUID& asset_id, ESource source, const std::string& name)
{
    if (!gAssetStorage)
    {
        mStatus = "asset system unavailable";
        return;
    }

    mPendingID = asset_id;
    mPendingSource = source;
    mPendingName = name;
    mStatus = "loading notecard...";

    gAssetStorage->getAssetData(asset_id, LLAssetType::AT_NOTECARD,
                                &SSAtmoTrackManager::onNotecardLoaded, nullptr, true);
}

// Loads a config notecard from inventory by hand.
bool SSAtmoTrackManager::importFromInventory(const LLInventoryItem* item)
{
    if (!item || item->getAssetUUID().isNull()) return false;

    if (!gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE)
        && !gAgent.isGodlike())
    {
        mStatus = "no permission to read that notecard";
        return false;
    }

    requestNotecard(item->getAssetUUID(), SOURCE_INVENTORY, item->getName());
    return true;
}

// Fetch arrival: unwrap the notecard, parse, adopt.
void SSAtmoTrackManager::onNotecardLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                      void* user_data, S32 status, LLExtStat ext_status)
{
    SSAtmoTrackManager* self = SSAtmoTrackManager::getInstance();

    if (asset_id != self->mPendingID) return;
    self->mPendingID.setNull();

    if (status != 0)
    {
        LL_WARNS("AtmoMagic") << "Atmo config notecard " << asset_id
                              << " failed to load, status " << status << LL_ENDL;
        self->mStatus = "notecard unavailable";
        return;
    }

    LLFileSystem file(asset_id, type, LLFileSystem::READ);
    const S32 length = file.getSize();
    if (length <= 0)
    {
        self->mStatus = "notecard empty";
        return;
    }

    std::vector<char> buffer(length + 1);
    file.read((U8*)buffer.data(), length);
    buffer[length] = '\0';

    std::string text(buffer.data(), length);
    if (length > 19 && strncmp(buffer.data(), "Linden text version", 19) == 0)
    {
        LLNotecard notecard;
        std::istringstream stream(text);
        if (!notecard.importStream(stream))
        {
            LL_WARNS("AtmoMagic") << "Could not parse Atmo notecard " << asset_id << LL_ENDL;
            self->mStatus = "notecard unreadable";
            return;
        }
        text = notecard.getText();
    }

    self->mAssetID = asset_id;
    self->applyNotecardText(text);

    LL_INFOS("AtmoMagic") << "Applied Atmo config notecard " << asset_id
                          << ": " << self->mStatus << LL_ENDL;
}

// Writes the working set as a notecard into inventory.
void SSAtmoTrackManager::exportToNotecard(const std::string& name)
{
    LLSD sd = asLLSD();
    sd["name"] = name;

    std::ostringstream body;
    LLSDSerialize::toPrettyXML(sd, body);

    LLNotecard nc(LLNotecard::MAX_SIZE);
    nc.setText(body.str());
    std::ostringstream wrapped;
    nc.exportStream(wrapped);

    const std::string asset_text = wrapped.str();
    const LLUUID folder_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD);

    LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback(
        [asset_text, name, folder_id](const LLUUID& new_item_id)
        {
            suppress_inventory_auto_open_for_folder(folder_id, false);

            LLViewerRegion* region = gAgent.getRegion();
            const std::string url = region ? region->getCapability("UpdateNotecardAgentInventory")
                                           : std::string();
            if (new_item_id.isNull() || url.empty())
            {
                LL_WARNS("AtmoMagic") << "Could not create Atmo notecard item" << LL_ENDL;
                return;
            }

            LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
                new_item_id, LLAssetType::AT_NOTECARD, asset_text,
                [name](LLUUID, LLUUID new_asset_id, LLUUID, LLSD)
                {
                    LL_INFOS("AtmoMagic") << "Saved Atmo config notecard '" << name
                                          << "' as asset " << new_asset_id << LL_ENDL;
                },
                nullptr);

            LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
        });

    suppress_inventory_auto_open_for_folder(folder_id, true);

    create_inventory_item(gAgentID, gAgentSessionID, folder_id, LLTransactionID::tnull,
                          name, "Atmo Magic weather configuration",
                          LLAssetType::AT_NOTECARD, LLInventoryType::IT_NOTECARD,
                          NO_INV_SUBTYPE, PERM_ALL, cb);
}

// Restores the working set from the atmo state file.
void SSAtmoTrackManager::loadWorking()
{
    mLoaded = true;

    const std::string packed = SSAtmoStore::getString(SSAtmoStoreKey::TRACK_CONFIG);
    if (packed.empty()) return;

    LLSD sd;
    std::istringstream stream(packed);
    if (LLSDSerialize::fromNotation(sd, stream, (S32)packed.size()) == LLSDParser::PARSE_FAILURE)
    {
        LL_WARNS("AtmoMagic") << "Could not parse SSAtmoTrackConfig; ignoring" << LL_ENDL;
        return;
    }

    ss_track_set_t set;
    if (!fromLLSD(sd, set)) return;

    mWorking = set;

    mBaseline = set;

    if (sd.isMap() && sd.has("name")) mConfigName = sd["name"].asString();
    if (sd.isMap() && sd.has("source"))
    {
        mSource = (ESource)sd["source"].asInteger();
        if (mSource == SOURCE_INVENTORY && !mConfigName.empty())
        {
            mStatus = "notecard: " + mConfigName;
        }
        else if (mSource != SOURCE_DEFAULT)
        {
            mSource = SOURCE_DEFAULT;
            mStatus = "system defaults";
        }
    }
}

// Persists the working set to the atmo state file.
void SSAtmoTrackManager::saveWorking()
{
    LLSD sd = asLLSD();
    sd["source"] = (S32)mSource;

    std::ostringstream stream;
    LLSDSerialize::toNotation(sd, stream);
    SSAtmoStore::setString(SSAtmoStoreKey::TRACK_CONFIG, stream.str());
}

// First-tick lazy load of the working set, then a parcel check.
void SSAtmoTrackManager::idle()
{
    if (!mLoaded)
    {
        loadWorking();
        changed();
    }
}
