/**
 * @file ssfloaterpreset.h
 * @brief Atmo Magic: weather preset editor floater.
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

#ifndef SS_FLOATERPRESET_H
#define SS_FLOATERPRESET_H

#include "llfloater.h"
#include "ssprecippreset.h"

class SSFloaterPreset : public LLFloater
{
public:
    SSFloaterPreset(const LLSD& key);

    bool postBuild() override;

    static std::string stepWidgetName(SSStepSurface surface, SSStepAction action);
    void onOpen(const LLSD& key) override;

private:
    void loadPreset(const std::string& name);
    void refreshPresetList();
    void controlsToPreset();
    void presetToControls();
    void applyLive();

    void onSelectPreset();
    void onCommitAny();
    void onClickNew();
    void onClickBlank();
    void onClickRename();
    void onClickDelete();
    void onClickRevert();
    void onClickSave();
    void onClickDiscard();

    void refreshTitle();

    // <SS:Nexii> One editor, two scopes over the same widgets. Viewer scope is the shipped-defaults tool: presets live on disk and the viewer's active one follows the selection. Environment scope edits the loaded Atmo environment's types, so save writes into the asset and the viewer's own precipitation is left alone. Two editors would let the tiers drift apart field by field. See doc/atmo_magic_env_ui.md.
    bool environmentScope() const { return mEnvironmentScope; }
    bool saveToEnvironment();
    bool removeFromEnvironment(const std::string& name);
    void refreshEnvironmentStaging();
    std::vector<std::string> environmentTypeNames() const;

    SSPrecipPreset mEdited;
    bool mUpdating = false;
    bool mEnvironmentScope = false;
};

#endif
