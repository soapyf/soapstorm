/**
 * @file ssatmostore.cpp
 * @brief See ssatmostore.h.
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

#include "ssatmostore.h"

#include "ssprecippreset.h"

#include "lldir.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llviewercontrol.h"

// ----------------------------------------------------------------------------
// Slot names

const std::string SSAtmoStoreKey::THUNDER_CRACK = "SSAtmoThunderCrack";
const std::string SSAtmoStoreKey::THUNDER_RUMBLE = "SSAtmoThunderRumble";
const std::string SSAtmoStoreKey::WIND_LIGHT = "SSAtmoLoopWindLight";
const std::string SSAtmoStoreKey::WIND_STRONG = "SSAtmoLoopWindStrong";
const std::string SSAtmoStoreKey::TRACK_CONFIG = "SSAtmoTrackConfig";
const std::string SSAtmoStoreKey::PRESET = "SSAtmoPreset";

namespace
{
    // The active preset when nothing was ever chosen; mirrors the old settings.xml value.
    const char* DEFAULT_PRESET = "Rain";
} // namespace

// ----------------------------------------------------------------------------
// SSAtmoStore

SSAtmoStore::SSAtmoStore()
:   mState(LLSD::emptyMap())
{
    load();
}

SSAtmoStore& SSAtmoStore::store()
{
    static SSAtmoStore instance;
    return instance;
}

// The one persisted file, next to the weather presets.
std::string SSAtmoStore::filePath()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ss_weather", "atmo_state.xml");
}

// Reads the state file; where it is missing or unusable, imports whatever the old debug settings held (one-time migration) and stops persisting them.
void SSAtmoStore::load()
{
    std::vector<std::string> keys;
    keys.push_back(SSAtmoStoreKey::THUNDER_CRACK);
    keys.push_back(SSAtmoStoreKey::THUNDER_RUMBLE);
    keys.push_back(SSAtmoStoreKey::WIND_LIGHT);
    keys.push_back(SSAtmoStoreKey::WIND_STRONG);
    keys.push_back(SSAtmoStoreKey::TRACK_CONFIG);
    keys.push_back(SSAtmoStoreKey::PRESET);
    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            if (SSFootstepSounds::surfaceIsGlobal((SSStepSurface)sf))
            {
                keys.push_back(SSFootstepSounds::globalSettingName((SSStepSurface)sf, (SSStepAction)ac));
            }
        }
    }

    bool import_from_settings = false;

    llifstream in(filePath().c_str());
    if (in.is_open())
    {
        LLSD sd;
        const S32 parsed = LLSDSerialize::fromXML(sd, in);
        in.close();
        if (parsed != LLSDParser::PARSE_FAILURE && sd.isMap())
        {
            mState = sd;
        }
        else
        {
            LL_WARNS("AtmoMagic") << "Could not parse atmo_state.xml; re-importing from debug settings" << LL_ENDL;
        }
    }

    // Materialise every slot so reads never miss, importing any value the file lacks but the old debug settings still hold.
    for (const std::string& key : keys)
    {
        if (mState.has(key) && mState[key].isString()) continue;

        std::string value;
        if (gSavedSettings.controlExists(key))
        {
            value = gSavedSettings.getString(key);
            import_from_settings = true;
        }
        if (value.empty() && key == SSAtmoStoreKey::PRESET)
        {
            value = DEFAULT_PRESET;
        }
        mState[key] = value;
    }

    if (import_from_settings && save())
    {
        // Migrated: blank the old slots and keep them out of the settings file.
        for (const std::string& key : keys)
        {
            if (LLControlVariablePtr control = gSavedSettings.getControl(key))
            {
                control->setPersist(LLControlVariable::PERSIST_NO);
                control->setValue(std::string());
            }
        }
    }
}

// Writes the whole state; small enough to rewrite on every change.
bool SSAtmoStore::save() const
{
    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ss_weather");
    if (!gDirUtilp->fileExists(dir))
    {
        LLFile::mkdir(dir);
    }

    llofstream out(filePath().c_str());
    if (!out.is_open())
    {
        LL_WARNS("AtmoMagic") << "Could not write atmo_state.xml" << LL_ENDL;
        return false;
    }
    LLSDSerialize::toPrettyXML(mState, out);
    out.close();
    return true;
}

std::string SSAtmoStore::getString(const std::string& key)
{
    return store().mState.get(key).asString();
}

void SSAtmoStore::setString(const std::string& key, const std::string& value)
{
    SSAtmoStore& self = store();
    self.mState[key] = value;
    self.save();
}
