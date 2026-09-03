/**
 * @file ssfloateratmoskyimport.h
 * @brief Atmo Magic: EEP sky import sub-floater.
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

#ifndef SS_FLOATERATMOSKYIMPORT_H
#define SS_FLOATERATMOSKYIMPORT_H

#include "llfloater.h"
#include "llhandle.h"

#include "llsettingssky.h"

#include <string>

// <SS:Nexii> Atmo Magic: pops on an EEP sky drop onto the environment editor, asking which groupings to import - atmosphere, lighting, celestial, cloud dome - before anything is stamped. SSFloaterAtmoEnv::handleSettingsDrop fetches the asset (the item alone cannot say sky vs water vs day cycle), then hands the sky here together with the track and preview phase the author dropped onto.
class SSFloaterAtmoSkyImport : public LLFloater
{
public:
    SSFloaterAtmoSkyImport(const LLSD& key);

    bool postBuild() override;

    // Brings the floater up armed with a fetched sky and where it will land. The track and
    // phase are what the author saw at drop time - resolved at OK against whatever the
    // manager then holds, in case the asset changed while the dialog sat open.
    static void show(LLSettingsSky::ptr_t sky, S32 track_index, F64 phase,
                     const std::string& item_name, LLHandle<LLFloater> parent);

private:
    void setPayload(LLSettingsSky::ptr_t sky, S32 track_index, F64 phase,
                    const std::string& item_name, LLHandle<LLFloater> parent);

    U32 checkedGroups() const;

    void refresh();
    void onClickImport();

    LLSettingsSky::ptr_t mSky;
    S32 mTrackIndex = 0;
    F64 mPhase = 0.0;
    std::string mItemName;
    LLHandle<LLFloater> mParent;
};

#endif
