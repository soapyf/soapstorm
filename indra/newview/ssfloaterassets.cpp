/**
 * @file ssfloaterassets.cpp
 * @brief See ssfloaterassets.h.
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

#include "ssfloaterassets.h"

#include "ssfloatersoundlist.h"

#include "ssatmostore.h"

#include "ssprecippreset.h"

// Floater shell; all content is wired in postBuild.
SSFloaterAssets::SSFloaterAssets(const LLSD& key)
:   LLFloater(key)
{
}

// Wires commit callbacks for the wind loops, thunder lists and every global footstep slot.
bool SSFloaterAssets::postBuild()
{
    getChild<LLUICtrl>("loop_wind_light")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitWind(); });
    getChild<LLUICtrl>("loop_wind_strong")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitWind(); });

    getChild<SSSoundListCtrl>("thunder_crack")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitThunder(); });
    getChild<SSSoundListCtrl>("thunder_rumble")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onCommitThunder(); });

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        if (!SSFootstepSounds::surfaceIsGlobal((SSStepSurface)sf)) continue;

        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            const std::string name = stepWidgetName((SSStepSurface)sf, (SSStepAction)ac);
            if (LLUICtrl* ctrl = findChild<LLUICtrl>(name))
            {
                ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitSteps(); });
            }
        }
    }

    return true;
}

// Widget name for a surface/action footstep slot.
std::string SSFloaterAssets::stepWidgetName(SSStepSurface surface, SSStepAction action)
{
    return std::string("step_") + SSFootstepSounds::surfaceKey(surface)
         + "_" + SSFootstepSounds::actionKey(action);
}

// Persists both thunder lists.
void SSFloaterAssets::onCommitThunder()
{
    SSAtmoStore::setString(SSAtmoStoreKey::THUNDER_CRACK,
        ss_asset_list_str(getChild<SSSoundListCtrl>("thunder_crack")->getList()));
    SSAtmoStore::setString(SSAtmoStoreKey::THUNDER_RUMBLE,
        ss_asset_list_str(getChild<SSSoundListCtrl>("thunder_rumble")->getList()));
}

// Persists every global footstep slot list (and re-reads the thunder lists from the store).
void SSFloaterAssets::onCommitSteps()
{
    getChild<SSSoundListCtrl>("thunder_crack")->setList(
        ss_asset_list_parse(SSAtmoStore::getString(SSAtmoStoreKey::THUNDER_CRACK)));
    getChild<SSSoundListCtrl>("thunder_rumble")->setList(
        ss_asset_list_parse(SSAtmoStore::getString(SSAtmoStoreKey::THUNDER_RUMBLE)));

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        const SSStepSurface surface = (SSStepSurface)sf;
        if (!SSFootstepSounds::surfaceIsGlobal(surface)) continue;

        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            const SSStepAction action = (SSStepAction)ac;
            if (SSSoundListCtrl* ctrl =
                    findChild<SSSoundListCtrl>(stepWidgetName(surface, action)))
            {
                SSAtmoStore::setString(
                    SSFootstepSounds::globalSettingName(surface, action),
                    ss_asset_list_str(ctrl->getList()));
            }
        }
    }
}

// Loads all lists from the store into their controls and labels the footstep slots.
void SSFloaterAssets::onOpen(const LLSD& key)
{
    getChild<SSSoundListCtrl>("loop_wind_light")->setList(
        ss_asset_list_parse(SSAtmoStore::getString(SSAtmoStoreKey::WIND_LIGHT)));
    getChild<SSSoundListCtrl>("loop_wind_strong")->setList(
        ss_asset_list_parse(SSAtmoStore::getString(SSAtmoStoreKey::WIND_STRONG)));

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        const SSStepSurface surface = (SSStepSurface)sf;
        if (!SSFootstepSounds::surfaceIsGlobal(surface)) continue;

        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            const SSStepAction action = (SSStepAction)ac;
            if (SSSoundListCtrl* ctrl =
                    findChild<SSSoundListCtrl>(stepWidgetName(surface, action)))
            {
                ctrl->setList(ss_asset_list_parse(SSAtmoStore::getString(
                    SSFootstepSounds::globalSettingName(surface, action))));
                ctrl->setSlotLabel(std::string(SSFootstepSounds::surfaceName(surface))
                                   + " - " + SSFootstepSounds::actionName(action));
            }
        }
    }
}

// Persists both wind loop lists.
void SSFloaterAssets::onCommitWind()
{
    SSAtmoStore::setString(SSAtmoStoreKey::WIND_LIGHT,
        ss_asset_list_str(getChild<SSSoundListCtrl>("loop_wind_light")->getList()));
    SSAtmoStore::setString(SSAtmoStoreKey::WIND_STRONG,
        ss_asset_list_str(getChild<SSSoundListCtrl>("loop_wind_strong")->getList()));
}
