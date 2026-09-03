/**
 * @file ssfloatersim.cpp
 * @brief See ssfloatersim.h.
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

#include "ssfloatersim.h"

#include "llfloaterreg.h"

#include "ssrainshadow.h"
#include "sswindflow.h"

#include "llbutton.h"
#include "llcontrol.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

// Floater shell; all content is wired in postBuild.
SSFloaterSimulation::SSFloaterSimulation(const LLSD& key) :
    LLFloater(key)
{
}

// Wires rebuild buttons and the setting watchers that invalidate the right map.
// The debug views that used to live here moved to the debug floater; what stays
// is the capture/solve tuning a change to which must force a rebuild.
bool SSFloaterSimulation::postBuild()
{
    getChild<LLButton>("shadow_rebuild_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRecaptureShadow(); });
    getChild<LLButton>("flow_rebuild_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRebuildFlow(); });

    watch("SSAtmoShadowRes", EInvalidate::SHADOW);
    watch("SSAtmoShadowMaxAge", EInvalidate::SHADOW);

    const char* flow_controls[] = {
        "SSAtmoWindFlow", "SSAtmoWindFlowCell", "SSAtmoWindFlowRes",
        "SSAtmoWindFlowMargin", "SSAtmoWindFlowHeight",
        "SSAtmoWindFlowIterations", "SSAtmoWindFlowSliceMax",
        "SSAtmoWindFlowSliceMin", "SSAtmoWindFlowShelterSteps",
        "SSAtmoWindFlowGradient"
    };
    for (const char* name : flow_controls)
    {
        watch(name, EInvalidate::FLOW);
    }

    return true;
}

// Invalidates the owning map (shadow cache or flow solve) whenever one of its settings changes.
void SSFloaterSimulation::watch(const std::string& control, EInvalidate what)
{
    LLControlVariable* var = gSavedSettings.getControl(control);
    if (!var)
    {
        LL_WARNS("AtmoMagic") << "Simulation floater has no setting named "
                              << control << LL_ENDL;
        return;
    }

    mConnections.emplace_back(var->getSignal()->connect(
        [this, what](LLControlVariable*, const LLSD&, const LLSD&)
        {
            if (what == EInvalidate::SHADOW)
            {
                SSRainShadowMap::getInstance()->clearCache();
            }
            else
            {
                SSWindFlowMap::getInstance()->rebuildAll();
            }
        }));
}

// Explicit rain-shadow recapture.
void SSFloaterSimulation::onClickRecaptureShadow()
{
    SSRainShadowMap::getInstance()->clearCache();
}

// Explicit flowmap re-solve.
void SSFloaterSimulation::onClickRebuildFlow()
{
    SSWindFlowMap::getInstance()->rebuildAll();
}
