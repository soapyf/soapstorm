/**
 * @file ssfloateratmo.cpp
 * @brief See ssfloateratmo.h.
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

#include "ssfloateratmo.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "ssprecippreset.h"

static const F64 PRESET_POLL_INTERVAL = 1.0;

// Deprecated legacy floater; only the preset picker and the sub-floater launchers remain.
SSFloaterAtmoMagic::SSFloaterAtmoMagic(const LLSD& key) :
    LLFloater(key)
{
}

// Wires the sub-floater launcher buttons and the preset editor.
bool SSFloaterAtmoMagic::postBuild()
{
    getChild<LLButton>("fx_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_fx"); });
    getChild<LLButton>("assets_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_assets"); });
    getChild<LLButton>("audio_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_audio"); });
    getChild<LLButton>("sim_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_sim"); });
    getChild<LLButton>("worldfield_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_worldfield"); });
    getChild<LLButton>("debug_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_atmo_debug"); });
    getChild<LLButton>("sound_analysis_button")->setClickedCallback(
        [](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("ss_sound_analysis"); });
    getChild<LLButton>("edit_preset_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickEditPreset(); });

    refreshPresets();
    return true;
}

// Re-lists presets on open.
void SSFloaterAtmoMagic::onOpen(const LLSD& key)
{
    refreshPresets();
}

// Polls the preset store once a second so external edits show up without a reopen.
void SSFloaterAtmoMagic::draw()
{
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mLastPoll > PRESET_POLL_INTERVAL)
    {
        mLastPoll = now;
        refreshPresets();
    }

    LLFloater::draw();
}

// Rebuilds the preset combo, keeping the current selection when it survives.
void SSFloaterAtmoMagic::refreshPresets()
{
    LLComboBox* combo = getChild<LLComboBox>("preset_combo");
    const std::string current = combo->getSelectedValue().asString();

    combo->removeall();
    for (const SSPrecipPreset& preset : SSPrecipPresetManager::instance().presets())
    {
        combo->add(preset.mName, LLSD(preset.mName));
    }

    if (!combo->selectByValue(LLSD(current)) && combo->getItemCount() > 0)
    {
        combo->selectFirstItem();
    }
}

// Opens the preset editor on the selected preset.
void SSFloaterAtmoMagic::onClickEditPreset()
{
    LLFloater* editor = LLFloaterReg::showInstance("ss_atmo_preset");
    const std::string name = getChild<LLComboBox>("preset_combo")->getValue().asString();
    if (editor && !name.empty())
    {
        editor->onOpen(LLSD(name));
    }
}
