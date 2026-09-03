/**
 * @file ssatmostore.h
 * @brief Persisted Atmo Magic data slots - global sound assignments, the wind
 *        track configuration and the active preset name - kept in a small file
 *        next to the weather presets rather than in the debug settings.
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

#ifndef SS_ATMO_STORE_H
#define SS_ATMO_STORE_H

#include "llsd.h"

#include <string>

// Fixed slot names. The twelve footstep slots are generated from the shared spellings by SSFootstepSounds::globalSettingName(); these six are written here.
namespace SSAtmoStoreKey
{
    extern const std::string THUNDER_CRACK;
    extern const std::string THUNDER_RUMBLE;
    extern const std::string WIND_LIGHT;
    extern const std::string WIND_STRONG;
    extern const std::string TRACK_CONFIG;
    extern const std::string PRESET;
}

// Key-value store for Atmo Magic state that is user data rather than a tunable: comma-separated sound UUID lists, the packed wind track configuration and the active preset name. Persisted to user_settings/ss_weather/atmo_state.xml. The first launch imports whatever the old debug settings held and blanks them, so nothing is lost and the slots stop appearing in the debug settings.
class SSAtmoStore
{
public:
    static std::string getString(const std::string& key);
    static void setString(const std::string& key, const std::string& value);

private:
    SSAtmoStore();

    static SSAtmoStore& store();

    void load();
    bool save() const;
    static std::string filePath();

    LLSD mState;
};

#endif
