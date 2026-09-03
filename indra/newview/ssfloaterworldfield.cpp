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

// Wires the recapture button and tuning watchers. A cell/band/ceiling change
// must rebuild the cached tiles to take effect - staleness checks compare cell
// and band against the live settings, but no recent tile is forced to retouch,
// so the watchers drop them. MaxAge is deliberately unwatched: consumed live by
// every needsBuild check; a clear on a timing tweak would force an immediate
// full recapture the new age would have waited out on its own. The field's
// debug views moved to the debug floater.
bool SSFloaterWorldField::postBuild()
{
    const char* tuning_controls[] = {
        "SSWorldFieldCell", "SSWorldFieldBand", "SSWorldFieldCeiling"
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

// A tuning change drops the cached tiles so the field rebuilds under the new
// geometry; without this a cell/band/ceiling change would never be picked up
// downstream - staleness checks compare against the live
// settings but nothing forces an already-recent tile to retouch.
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
