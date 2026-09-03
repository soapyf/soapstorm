/**
 * @file ssfloaterpreset.cpp
 * @brief See ssfloaterpreset.h.
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

#include "ssfloaterpreset.h"
#include "llsdutil.h"
#include "ssatmoenvmanager.h"

#include "ssatmostore.h"
#include "ssfloatersoundlist.h"
#include "ssatmotrack.h"
#include "ssprecipvariants.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llsliderctrl.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"

static const char* TIER_PREFIX[TIER_COUNT] = { "drops", "clusters", "sheets" };

// Floater shell; all content is wired in postBuild.
SSFloaterPreset::SSFloaterPreset(const LLSD& key) :
    LLFloater(key)
{
}

// Widget name for a surface/action footstep slot.
std::string SSFloaterPreset::stepWidgetName(SSStepSurface surface, SSStepAction action)
{
    return std::string("step_") + SSFootstepSounds::surfaceKey(surface)
         + "_" + SSFootstepSounds::actionKey(action);
}

// Wires every editor control to the shared commit path plus the toolbar buttons.
bool SSFloaterPreset::postBuild()
{
    getChild<LLComboBox>("preset_combo")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { onSelectPreset(); });

    getChild<LLButton>("new_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickNew(); });
    getChild<LLButton>("blank_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickBlank(); });
    getChild<LLButton>("rename_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRename(); });
    getChild<LLButton>("delete_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickDelete(); });
    getChild<LLButton>("revert_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickRevert(); });
    getChild<LLButton>("save_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickSave(); });
    getChild<LLButton>("discard_button")->setClickedCallback(
        [this](LLUICtrl*, const LLSD&) { onClickDiscard(); });

    static const char* widgets[] = {
        "archetype_combo", "fall_speed", "fall_lo", "fall_hi", "sway",
        "wind_response", "rate", "intensity_size", "weather_size", "tint", "glow", "drop_shape",
        "drop_scale", "emissive", "water_shading", "impact_strength", "weather_impact", "shatter",
        "ripple_size", "ripple_alpha", "ripple_life",
        "crown_size", "crown_alpha", "crown_speed", "crown_life",
        "dark_mix", "puff_mix",
        "stream_alpha", "stream_span", "stream_length", "stream_wind",
        "stream_scale", "stream_stretch", "drip_scale",
        "textures", "ripple_texture",
        "dark_texture", "puff_texture",
        "snd_light", "snd_medium", "snd_heavy",
        "snd_roof_open", "snd_roof_small", "snd_roof_medium", "snd_roof_big",
    };
    for (const char* name : widgets)
    {
        if (LLUICtrl* ctrl = findChild<LLUICtrl>(name))
        {
            ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitAny(); });
        }
    }

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            if (SSFootstepSounds::surfaceIsGlobal((SSStepSurface)sf)) continue;

            const std::string name = stepWidgetName((SSStepSurface)sf, (SSStepAction)ac);
            if (LLUICtrl* ctrl = findChild<LLUICtrl>(name))
            {
                ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitAny(); });
            }
        }
    }

    for (S32 t = 0; t < TIER_COUNT; ++t)
    {
        static const char* suffixes[] = { "enabled", "kind", "size_x", "size_y", "alpha", "radius" };
        for (const char* suffix : suffixes)
        {
            const std::string name = std::string(TIER_PREFIX[t]) + "_" + suffix;
            if (LLUICtrl* ctrl = findChild<LLUICtrl>(name))
            {
                ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitAny(); });
            }
        }
    }

    refreshPresetList();
    return true;
}

// Opens on the named preset (or the active one).
void SSFloaterPreset::onOpen(const LLSD& key)
{
    // <SS:Nexii> A map key carries the scope; a bare string is the viewer-scope call this floater has always taken, so existing callers are unchanged.
    std::string want;
    if (key.isMap())
    {
        mEnvironmentScope = (key["scope"].asString() == "environment");
        want = key["name"].asString();
    }
    else
    {
        mEnvironmentScope = false;
        want = key.isString() ? key.asString() : std::string();
    }

    refreshPresetList();

    if (want.empty() && mEnvironmentScope)
    {
        const std::vector<std::string> names = environmentTypeNames();
        if (!names.empty()) want = names.front();
    }
    if (want.empty() && !mEnvironmentScope)
    {
        want = SSAtmoStore::getString(SSAtmoStoreKey::PRESET);
    }
    if (want.empty() && !mEnvironmentScope)
    {
        want = SSAtmoMagic::getInstance()->preset().mName;
    }
    if (!want.empty())
    {
        loadPreset(want);
    }
    refreshTitle();
}

// The environment's type names, from the asset rather than the staged list, so a type staged
// by something else cannot masquerade as one this environment owns.
std::vector<std::string> SSFloaterPreset::environmentTypeNames() const
{
    std::vector<std::string> names;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return names;

    for (const auto& entry : mgr->asset().mPrecipitationTypes)
    {
        names.push_back(entry.first);
    }
    return names;
}

// Writes the working copy into the loaded environment and restages it.
bool SSFloaterPreset::saveToEnvironment()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset() || mEdited.mName.empty()) return false;

    mgr->editable().mPrecipitationTypes[mEdited.mName] = mEdited.asLLSD();
    refreshEnvironmentStaging();
    return true;
}

bool SSFloaterPreset::removeFromEnvironment(const std::string& name)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return false;

    if (!mgr->editable().mPrecipitationTypes.erase(name)) return false;
    refreshEnvironmentStaging();
    return true;
}

void SSFloaterPreset::refreshEnvironmentStaging()
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset()) return;

    ssAtmoEnvStagePrecipTypes(mgr->asset());
    SSPrecipVariants::instance().clearCache();
}

// Rebuilds the preset combo around the current selection.
void SSFloaterPreset::refreshPresetList()
{
    LLComboBox* combo = getChild<LLComboBox>("preset_combo");
    const std::string selected = combo->getSelectedItemLabel();

    combo->removeall();
    if (mEnvironmentScope)
    {
        for (const std::string& name : environmentTypeNames())
        {
            combo->add(name, name);
        }
    }
    else
    {
        for (const SSPrecipPreset& p : SSPrecipPresetManager::instance().presets())
        {
            combo->add(p.mName, p.mName);
        }
    }
    if (!selected.empty())
    {
        combo->selectByValue(selected);
    }
}

// Loads a preset into the working copy and the controls.
void SSFloaterPreset::loadPreset(const std::string& name)
{
    const SSPrecipPreset* found = SSPrecipPresetManager::instance().find(name);
    if (!found) return;

    mEdited = *found;
    getChild<LLComboBox>("preset_combo")->selectByValue(mEdited.mName);
    getChild<LLLineEditor>("preset_name_editor")->setText(mEdited.mName);
    presetToControls();
    refreshTitle();
}

// Working copy out to every widget.
void SSFloaterPreset::presetToControls()
{
    mUpdating = true;

    getChild<LLUICtrl>("archetype_combo")->setValue((S32)mEdited.mArchetype);
    getChild<LLUICtrl>("fall_speed")->setValue(mEdited.mFallSpeed);
    getChild<LLUICtrl>("fall_lo")->setValue(mEdited.mFallLo);
    getChild<LLUICtrl>("fall_hi")->setValue(mEdited.mFallHi);
    getChild<LLUICtrl>("sway")->setValue(mEdited.mSway);
    getChild<LLUICtrl>("wind_response")->setValue(mEdited.mWindResponse);
    getChild<LLUICtrl>("rate")->setValue(mEdited.mRate);
    getChild<LLUICtrl>("intensity_size")->setValue(mEdited.mIntensitySize);
    getChild<LLUICtrl>("weather_size")->setValue(mEdited.mWeatherSize);

    getChild<LLColorSwatchCtrl>("tint")->set(mEdited.mTint);
    getChild<LLUICtrl>("glow")->setValue(mEdited.mGlow);
    getChild<LLUICtrl>("drop_shape")->setValue((S32)mEdited.mDropShape);
    getChild<LLUICtrl>("drop_scale")->setValue(mEdited.mDropScale);
    getChild<LLUICtrl>("emissive")->setValue(mEdited.mEmissive);
    getChild<LLUICtrl>("water_shading")->setValue(mEdited.mWaterShading);

    getChild<LLUICtrl>("impact_strength")->setValue(mEdited.mImpactStrength);
    getChild<LLUICtrl>("weather_impact")->setValue(mEdited.mWeatherImpact);
    getChild<LLUICtrl>("shatter")->setValue(mEdited.mShatter);
    getChild<LLUICtrl>("ripple_size")->setValue(mEdited.mRippleSize);
    getChild<LLUICtrl>("ripple_alpha")->setValue(mEdited.mRippleAlpha);
    getChild<LLUICtrl>("ripple_life")->setValue(mEdited.mRippleLife);
    getChild<LLUICtrl>("crown_size")->setValue(mEdited.mCrownSize);
    getChild<LLUICtrl>("crown_alpha")->setValue(mEdited.mCrownAlpha);
    getChild<LLUICtrl>("crown_speed")->setValue(mEdited.mCrownSpeed);
    getChild<LLUICtrl>("crown_life")->setValue(mEdited.mCrownLife);
    getChild<LLUICtrl>("dark_mix")->setValue(mEdited.mDarkMix);
    getChild<LLUICtrl>("puff_mix")->setValue(mEdited.mPuffMix);

    getChild<LLUICtrl>("stream_alpha")->setValue(mEdited.mStreamAlpha);
    getChild<LLUICtrl>("stream_span")->setValue(mEdited.mStreamSpan);
    getChild<LLUICtrl>("stream_length")->setValue(mEdited.mStreamLength);
    getChild<LLUICtrl>("stream_stretch")->setValue(mEdited.mStreamStretch);
    getChild<LLUICtrl>("stream_wind")->setValue(mEdited.mStreamWind);
    getChild<LLUICtrl>("stream_scale")->setValue(mEdited.mStreamScale);
    getChild<LLUICtrl>("drip_scale")->setValue(mEdited.mDripScale);

    for (S32 t = 0; t < TIER_COUNT; ++t)
    {
        const std::string prefix = TIER_PREFIX[t];
        getChild<LLUICtrl>(prefix + "_enabled")->setValue(mEdited.mTiers[t].mEnabled);
        getChild<LLUICtrl>(prefix + "_kind")->setValue((S32)mEdited.mTiers[t].mKind);
        getChild<LLUICtrl>(prefix + "_size_x")->setValue(mEdited.mTiers[t].mSizeX);
        getChild<LLUICtrl>(prefix + "_size_y")->setValue(mEdited.mTiers[t].mSizeY);
        getChild<LLUICtrl>(prefix + "_alpha")->setValue(mEdited.mTiers[t].mAlpha);
        getChild<LLUICtrl>(prefix + "_radius")->setValue(mEdited.mTiers[t].mRadius);
    }

    getChild<LLUICtrl>("textures")->setValue(mEdited.mTextures);
    getChild<LLUICtrl>("ripple_texture")->setValue(mEdited.mRippleTexture);
    getChild<LLUICtrl>("dark_texture")->setValue(mEdited.mDarkTexture);
    getChild<LLUICtrl>("puff_texture")->setValue(mEdited.mPuffTexture);

    getChild<SSSoundListCtrl>("snd_light")->setList(ss_asset_list_parse(mEdited.mSounds.mAmbientLight));
    getChild<SSSoundListCtrl>("snd_medium")->setList(ss_asset_list_parse(mEdited.mSounds.mAmbientMedium));
    getChild<SSSoundListCtrl>("snd_heavy")->setList(ss_asset_list_parse(mEdited.mSounds.mAmbientHeavy));
    getChild<SSSoundListCtrl>("snd_roof_open")->setList(ss_asset_list_parse(mEdited.mSounds.mRoofOpen));
    getChild<SSSoundListCtrl>("snd_roof_small")->setList(ss_asset_list_parse(mEdited.mSounds.mRoofSmall));
    getChild<SSSoundListCtrl>("snd_roof_medium")->setList(ss_asset_list_parse(mEdited.mSounds.mRoofMedium));
    getChild<SSSoundListCtrl>("snd_roof_big")->setList(ss_asset_list_parse(mEdited.mSounds.mRoofBig));

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            const SSStepSurface surface = (SSStepSurface)sf;
            const SSStepAction action = (SSStepAction)ac;

            if (SSFootstepSounds::surfaceIsGlobal(surface)) continue;

            if (SSSoundListCtrl* ctrl =
                    findChild<SSSoundListCtrl>(stepWidgetName(surface, action)))
            {
                ctrl->setList(ss_asset_list_parse(mEdited.mFootsteps.at(surface, action)));
                ctrl->setSlotLabel(std::string(SSFootstepSounds::surfaceName(surface))
                                   + " - " + SSFootstepSounds::actionName(action));
            }
        }
    }

    mUpdating = false;
}

// Every widget back into the working copy.
void SSFloaterPreset::controlsToPreset()
{
    mEdited.mArchetype = (SSPrecipArchetype)llclamp(getChild<LLUICtrl>("archetype_combo")->getValue().asInteger(),
                                                    0, (S32)SSPrecipArchetype::COUNT - 1);
    mEdited.mFallSpeed = (F32)getChild<LLUICtrl>("fall_speed")->getValue().asReal();
    mEdited.mFallLo = (F32)getChild<LLUICtrl>("fall_lo")->getValue().asReal();
    mEdited.mFallHi = llmax(mEdited.mFallLo, (F32)getChild<LLUICtrl>("fall_hi")->getValue().asReal());
    mEdited.mSway = (F32)getChild<LLUICtrl>("sway")->getValue().asReal();
    mEdited.mWindResponse = (F32)getChild<LLUICtrl>("wind_response")->getValue().asReal();
    mEdited.mRate = (F32)getChild<LLUICtrl>("rate")->getValue().asReal();
    mEdited.mIntensitySize = (F32)getChild<LLUICtrl>("intensity_size")->getValue().asReal();
    mEdited.mWeatherSize = getChild<LLUICtrl>("weather_size")->getValue().asBoolean();

    mEdited.mTint = getChild<LLColorSwatchCtrl>("tint")->get();
    mEdited.mGlow = (F32)getChild<LLUICtrl>("glow")->getValue().asReal();
    mEdited.mDropShape = (U8)llclamp(getChild<LLUICtrl>("drop_shape")->getValue().asInteger(), 0, 2);
    mEdited.mDropScale = (F32)getChild<LLUICtrl>("drop_scale")->getValue().asReal();
    mEdited.mEmissive = getChild<LLUICtrl>("emissive")->getValue().asBoolean();
    mEdited.mWaterShading = getChild<LLUICtrl>("water_shading")->getValue().asBoolean();

    mEdited.mImpactStrength = (F32)getChild<LLUICtrl>("impact_strength")->getValue().asReal();
    mEdited.mWeatherImpact = getChild<LLUICtrl>("weather_impact")->getValue().asBoolean();
    mEdited.mShatter = getChild<LLUICtrl>("shatter")->getValue().asBoolean();
    mEdited.mRippleSize = (F32)getChild<LLUICtrl>("ripple_size")->getValue().asReal();
    mEdited.mRippleAlpha = (F32)getChild<LLUICtrl>("ripple_alpha")->getValue().asReal();
    mEdited.mRippleLife = (F32)getChild<LLUICtrl>("ripple_life")->getValue().asReal();
    mEdited.mCrownSize = (F32)getChild<LLUICtrl>("crown_size")->getValue().asReal();
    mEdited.mCrownAlpha = (F32)getChild<LLUICtrl>("crown_alpha")->getValue().asReal();
    mEdited.mCrownSpeed = (F32)getChild<LLUICtrl>("crown_speed")->getValue().asReal();
    mEdited.mCrownLife = (F32)getChild<LLUICtrl>("crown_life")->getValue().asReal();
    mEdited.mDarkMix = (F32)getChild<LLUICtrl>("dark_mix")->getValue().asReal();
    mEdited.mPuffMix = (F32)getChild<LLUICtrl>("puff_mix")->getValue().asReal();

    mEdited.mStreamAlpha = (F32)getChild<LLUICtrl>("stream_alpha")->getValue().asReal();
    mEdited.mStreamSpan = (F32)getChild<LLUICtrl>("stream_span")->getValue().asReal();
    mEdited.mStreamLength = (F32)getChild<LLUICtrl>("stream_length")->getValue().asReal();
    mEdited.mStreamStretch = (F32)getChild<LLUICtrl>("stream_stretch")->getValue().asReal();
    mEdited.mStreamWind = (F32)getChild<LLUICtrl>("stream_wind")->getValue().asReal();
    mEdited.mStreamScale = (F32)getChild<LLUICtrl>("stream_scale")->getValue().asReal();
    mEdited.mDripScale = (F32)getChild<LLUICtrl>("drip_scale")->getValue().asReal();

    for (S32 t = 0; t < TIER_COUNT; ++t)
    {
        const std::string prefix = TIER_PREFIX[t];
        mEdited.mTiers[t].mEnabled = getChild<LLUICtrl>(prefix + "_enabled")->getValue().asBoolean();
        mEdited.mTiers[t].mKind = (U8)llclamp(getChild<LLUICtrl>(prefix + "_kind")->getValue().asInteger(), 0, 3);
        mEdited.mTiers[t].mSizeX = (F32)getChild<LLUICtrl>(prefix + "_size_x")->getValue().asReal();
        mEdited.mTiers[t].mSizeY = (F32)getChild<LLUICtrl>(prefix + "_size_y")->getValue().asReal();
        mEdited.mTiers[t].mAlpha = (F32)getChild<LLUICtrl>(prefix + "_alpha")->getValue().asReal();
        mEdited.mTiers[t].mRadius = (F32)getChild<LLUICtrl>(prefix + "_radius")->getValue().asReal();
    }

    mEdited.mTextures = getChild<LLUICtrl>("textures")->getValue().asString();
    mEdited.mRippleTexture = getChild<LLUICtrl>("ripple_texture")->getValue().asString();
    mEdited.mDarkTexture = getChild<LLUICtrl>("dark_texture")->getValue().asString();
    mEdited.mPuffTexture = getChild<LLUICtrl>("puff_texture")->getValue().asString();

    mEdited.mSounds.mAmbientLight = ss_asset_list_str(getChild<SSSoundListCtrl>("snd_light")->getList());
    mEdited.mSounds.mAmbientMedium = ss_asset_list_str(getChild<SSSoundListCtrl>("snd_medium")->getList());
    mEdited.mSounds.mAmbientHeavy = ss_asset_list_str(getChild<SSSoundListCtrl>("snd_heavy")->getList());
    mEdited.mSounds.mRoofOpen = ss_asset_list_str(getChild<SSSoundListCtrl>("snd_roof_open")->getList());
    mEdited.mSounds.mRoofSmall = ss_asset_list_str(getChild<SSSoundListCtrl>("snd_roof_small")->getList());
    mEdited.mSounds.mRoofMedium = ss_asset_list_str(getChild<SSSoundListCtrl>("snd_roof_medium")->getList());
    mEdited.mSounds.mRoofBig = ss_asset_list_str(getChild<SSSoundListCtrl>("snd_roof_big")->getList());

    for (S32 sf = 0; sf < STEP_SURFACE_COUNT; ++sf)
    {
        for (S32 ac = 0; ac < STEP_ACTION_COUNT; ++ac)
        {
            const SSStepSurface surface = (SSStepSurface)sf;
            const SSStepAction action = (SSStepAction)ac;
            if (SSFootstepSounds::surfaceIsGlobal(surface)) continue;

            if (SSSoundListCtrl* ctrl =
                    findChild<SSSoundListCtrl>(stepWidgetName(surface, action)))
            {
                mEdited.mFootsteps.at(surface, action) = ss_asset_list_str(ctrl->getList());
            }
        }
    }
}

// Stages the edit into the running weather (and clears baked textures) so it can be dialled in while watching it fall.
void SSFloaterPreset::applyLive()
{
    SSPrecipPresetManager::instance().stage(mEdited);

    SSPrecipVariants::instance().clearCache();

    // Only the viewer-scope editor sets what the viewer is running. In environment scope the type
    // is one the region offers, and picking it is the environment's job, not the editor's.
    if (!mEnvironmentScope)
    {
        SSAtmoStore::setString(SSAtmoStoreKey::PRESET, mEdited.mName);
    }

    refreshTitle();
}

// Title shows the preset name and its modified state.
void SSFloaterPreset::refreshTitle()
{
    if (mEnvironmentScope)
    {
        SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
        bool modified = true;
        if (mgr->hasAsset())
        {
            const auto& types = mgr->asset().mPrecipitationTypes;
            const auto it = types.find(mEdited.mName);
            modified = (it == types.end()) || !llsd_equals(it->second, mEdited.asLLSD());
        }
        setTitle("Edit Environment Precipitation - " + mEdited.mName
                 + (modified ? " - Unsaved changes*" : ""));
        getChild<LLUICtrl>("save_button")->setEnabled(modified);
        return;
    }

    const bool modified = SSPrecipPresetManager::instance().isModified(mEdited.mName);
    setTitle("Edit Atmo Magic Preset - " + mEdited.mName + (modified ? " - Unsaved changes*" : ""));
    getChild<LLUICtrl>("save_button")->setEnabled(modified);
}

// Persists the working copy.
void SSFloaterPreset::onClickSave()
{
    if (mEnvironmentScope)
    {
        if (!saveToEnvironment())
        {
            LLNotificationsUtil::add("GenericAlert", LLSD().with(
                "MESSAGE", "There is no Atmo environment loaded to save this type into."));
            return;
        }
    }
    else
    {
        SSPrecipPresetManager::instance().save(mEdited);
    }
    refreshPresetList();
    refreshTitle();
}

// Any control commit: read the widgets, apply live.
void SSFloaterPreset::onCommitAny()
{
    if (mUpdating) return;
    controlsToPreset();
    applyLive();
}

// Switches the editor (and the running weather) to another preset.
void SSFloaterPreset::onSelectPreset()
{
    loadPreset(getChild<LLComboBox>("preset_combo")->getValue().asString());
    if (!mEnvironmentScope)
    {
        SSAtmoStore::setString(SSAtmoStoreKey::PRESET, mEdited.mName);
    }
}

// New preset as a copy of the current one.
void SSFloaterPreset::onClickNew()
{
    std::string base = mEdited.mName + " copy";
    std::string name = base;
    for (S32 i = 2; SSPrecipPresetManager::instance().find(name) && i < 100; ++i)
    {
        name = base + " " + llformat("%d", i);
    }

    mEdited.mName = name;
    mEdited.mBuiltIn = false;
    // Deriving carries a full copy, not a reference to the parent, so a viewer update that
    // retunes the shipped type it came from cannot silently change a region shipping this one.
    mEdited.mFromEnvironment = mEnvironmentScope;
    if (mEnvironmentScope) saveToEnvironment();
    applyLive();
    refreshPresetList();
    getChild<LLComboBox>("preset_combo")->selectByValue(name);
}

// First free 'name N' variant.
static std::string uniquePresetName(const std::string& base)
{
    std::string name = base;
    for (S32 i = 2; SSPrecipPresetManager::instance().find(name) && i < 1000; ++i)
    {
        name = base + " " + llformat("%d", i);
    }
    return name;
}

// New preset from defaults.
void SSFloaterPreset::onClickBlank()
{
    mEdited = SSPrecipPreset();
    mEdited.mName = uniquePresetName("New preset");
    mEdited.mBuiltIn = false;
    mEdited.mFromEnvironment = mEnvironmentScope;
    if (mEnvironmentScope) saveToEnvironment();

    applyLive();
    refreshPresetList();
    getChild<LLComboBox>("preset_combo")->selectByValue(mEdited.mName);
    presetToControls();
}

// Renames the working preset (delete-and-resave under the new name).
void SSFloaterPreset::onClickRename()
{
    std::string name = getChild<LLLineEditor>("preset_name_editor")->getText();
    LLStringUtil::trim(name);

    if (name.empty() || name == mEdited.mName) return;

    if (SSPrecipPresetManager::instance().find(name))
    {
        LLNotificationsUtil::add("GenericAlert",
            LLSD().with("MESSAGE", "A preset with that name already exists."));
        return;
    }

    const std::string old_name = mEdited.mName;

    mEdited.mName = name;
    mEdited.mBuiltIn = false;

    if (mEnvironmentScope)
    {
        // Rekey the asset, then follow the reference through every keyframe that named the old
        // type - otherwise the rename leaves those keyframes pointing at nothing.
        SSAtmoEnvManager* env = SSAtmoEnvManager::getInstance();
        if (env->hasAsset())
        {
            SSAtmoEnvAsset& asset = env->editable();
            asset.mPrecipitationTypes.erase(old_name);
            asset.mPrecipitationTypes[name] = mEdited.asLLSD();

            for (SSAtmoEnvTrack& track : asset.mTracks)
            {
                track.mWeather.mPrecipitationOverride.renameValue(old_name, name);
            }
        }
        refreshEnvironmentStaging();
        refreshPresetList();
        getChild<LLComboBox>("preset_combo")->selectByValue(name);
        refreshTitle();
        return;
    }

    SSPrecipPresetManager::instance().save(mEdited);
    SSPrecipPresetManager::instance().remove(old_name);

    SSAtmoTrackManager* tracks = SSAtmoTrackManager::getInstance();
    bool touched = false;
    for (S32 track = SS_TRACK_MIN; track <= SS_TRACK_MAX; ++track)
    {
        SSAtmoTrackConfig& cfg = tracks->editable(track);
        if (cfg.mPreset == old_name)
        {
            cfg.mPreset = name;
            touched = true;
        }
    }
    if (touched) tracks->commit();

    if (SSAtmoStore::getString(SSAtmoStoreKey::PRESET) == old_name)
    {
        SSAtmoStore::setString(SSAtmoStoreKey::PRESET, name);
    }

    SSPrecipVariants::instance().clearCache();
    refreshPresetList();
    getChild<LLComboBox>("preset_combo")->selectByValue(name);
}

// Deletes the preset and falls back to whatever is first.
void SSFloaterPreset::onClickDelete()
{
    const std::string name = mEdited.mName;

    if (mEnvironmentScope)
    {
        if (!removeFromEnvironment(name))
        {
            LLNotificationsUtil::add("GenericAlert",
                LLSD().with("MESSAGE", "This environment does not carry a type by that name."));
            return;
        }

        refreshPresetList();
        const std::vector<std::string> remaining = environmentTypeNames();
        if (!remaining.empty()) loadPreset(remaining.front());
        refreshTitle();
        return;
    }

    if (!SSPrecipPresetManager::instance().remove(name))
    {
        LLNotificationsUtil::add("GenericAlert",
            LLSD().with("MESSAGE", "This preset has no saved copy to delete."));
        return;
    }

    refreshPresetList();
    const auto& presets = SSPrecipPresetManager::instance().presets();
    if (!presets.empty())
    {
        loadPreset(presets.front().mName);
    }
}

// Drops staged edits, back to the saved version.
void SSFloaterPreset::onClickDiscard()
{
    if (mEnvironmentScope)
    {
        // The asset is the saved copy in this scope.
        loadPreset(mEdited.mName);
        return;
    }

    const SSPrecipPreset* saved = SSPrecipPresetManager::instance().findSaved(mEdited.mName);
    if (!saved) return;

    SSPrecipPresetManager::instance().stage(*saved);
    SSPrecipVariants::instance().clearCache();
    loadPreset(mEdited.mName);
}

// Reverts the controls to the saved preset.
void SSFloaterPreset::onClickRevert()
{
    if (mEnvironmentScope)
    {
        loadPreset(mEdited.mName);
        return;
    }

    SSPrecipPresetManager::instance().remove(mEdited.mName);
    SSPrecipVariants::instance().clearCache();
    refreshPresetList();
    loadPreset(mEdited.mName);
}
