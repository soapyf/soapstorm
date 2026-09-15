/**
 * @file ssfloaterworldfield.cpp
 * @brief See ssfloaterworldfield.h.
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

#include "ssfloaterworldfield.h"

#include "ssworldfield.h"

#include "llbutton.h"
#include "llcontrol.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

SSFloaterWorldField::SSFloaterWorldField(const LLSD& key) :
    LLFloater(key)
{
}

// Wires the reclassify button and the tuning watchers. Every dial here is baked
// into the published grid, so a change has to drop the cached grids to take
// effect - the cell size is part of the sheet-set stamp and would rebuild on its
// own, but the sky angle and the ground reach are read at classify time and at
// query time respectively and nothing else would notice them move. The field's
// debug views live in the debug floater.
bool SSFloaterWorldField::postBuild()
{
    const char* tuning_controls[] = {
        "SSWorldFieldCell", "SSWorldFieldOpenAngle", "SSWorldFieldGroundReach"
    };
    for (const char* name : tuning_controls)
    {
        watch(name);
    }

    getChild<LLButton>("recapture_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&)
        {
            SSWorldField::getInstance()->clear();
        });

    return true;
}

// A tuning change drops the cached grids so every region reclassifies under the
// new geometry; without this a dial the classification baked in would never be
// picked up downstream.
void SSFloaterWorldField::watch(const std::string& control)
{
    LLControlVariable* var = gSavedSettings.getControl(control);
    if (!var)
    {
        LL_WARNS("AtmoMagic") << "World field floater has no setting named "
                              << control << LL_ENDL;
        return;
    }

    mConnections.emplace_back(var->getSignal()->connect(
        [](LLControlVariable*, const LLSD&, const LLSD&)
        {
            SSWorldField::getInstance()->clear();
        }));
}
