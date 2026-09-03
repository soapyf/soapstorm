/**
 * @file ssprecippreset.h
 * @brief Atmo Magic: editable weather presets.
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

#ifndef SS_PRECIPPRESET_H
#define SS_PRECIPPRESET_H

#include "llsd.h"
#include "llsingleton.h"
#include "v4color.h"

#include <string>
#include <vector>

enum SSPrecipTier : U8
{
    TIER_DROPS = 0,
    TIER_CLUSTERS,
    TIER_SHEETS,
    TIER_COUNT
};

enum SSPrecipKind : U8
{
    KIND_STREAK = 0,
    KIND_ROUND,
    KIND_SHEET,
    KIND_FLAT,
    KIND_STREAM
};

enum SSPrecipMaterial : U8
{
    MAT_LIT = 0,
    MAT_WATER,
    MAT_EMISSIVE,
    MAT_DECAL,
    MAT_GRANULAR,   // <SS:Nexii> sand/snow-family particles: screen-door dithered near the camera, so a cascade reads as grains rather than a liquid sheet
    MAT_COUNT
};

enum SSPrecipDropShape : U8
{
    DROP_DOT = 0,
    DROP_TEARDROP,
    DROP_SLIVER
};

enum class SSPrecipArchetype : S32
{
    LIQUID = 0,
    FLAKE,
    SOLID,
    RISER,
    COUNT
};

struct SSPrecipTierParams
{
    bool mEnabled = true;
    U8   mKind = KIND_ROUND;
    F32  mSizeX = 0.05f;
    F32  mSizeY = 0.05f;
    F32  mAlpha = 0.5f;
    F32  mRadius = 28.f;
};

struct SSPrecipSounds
{
    std::string mAmbientLight;
    std::string mAmbientMedium;
    std::string mAmbientHeavy;
    std::string mRoofOpen;
    std::string mRoofSmall;
    std::string mRoofMedium;
    std::string mRoofBig;
};

enum SSStepSurface : U8
{
    STEP_TERRAIN_DRY = 0,
    STEP_TERRAIN_WET,
    STEP_TERRAIN_PUDDLE,
    STEP_OUTSIDE_DRY,
    STEP_OUTSIDE_WET,
    STEP_OUTSIDE_PUDDLE,
    STEP_INSIDE_DRY,
    STEP_SURFACE_COUNT
};

enum SSStepAction : U8
{
    STEP_WALK = 0,
    STEP_RUN,
    STEP_JUMP,
    STEP_LAND,
    STEP_ACTION_COUNT
};

struct SSFootstepSounds
{
    std::string mSounds[STEP_SURFACE_COUNT][STEP_ACTION_COUNT];

    std::string& at(SSStepSurface surface, SSStepAction action) { return mSounds[surface][action]; }
    const std::string& at(SSStepSurface surface, SSStepAction action) const { return mSounds[surface][action]; }

    static const char* surfaceName(SSStepSurface s);
    static const char* actionName(SSStepAction a);
    static bool surfaceIsGlobal(SSStepSurface s);

    static std::string globalSettingName(SSStepSurface s, SSStepAction a);

    static const char* surfaceKey(SSStepSurface s);
    static const char* actionKey(SSStepAction a);
};

static const F32 SS_STREAM_LENGTH_MAX = 20.f;

struct SSPrecipPreset
{
    std::string mName;
    bool mBuiltIn = false;

    // <SS:Nexii> Set when the type came from the loaded Atmo environment rather than disk. Not serialised: it records where this copy arrived from, not what the type is, and the same definition in an environment and on disk is the same type either way.
    bool mFromEnvironment = false;

    SSPrecipArchetype mArchetype = SSPrecipArchetype::LIQUID;

    F32 mFallSpeed = 9.5f;
    F32 mFallLo = 16.f;
    F32 mFallHi = 26.f;
    F32 mSway = 0.f;
    F32 mWindResponse = 1.f;

    F32 mRate = 0.16f;

    F32 mIntensitySize = 0.f;

    // <SS:Nexii> Weather-cube opt-in: sizes drops from the cube's graded intensity band (drizzle 0.05 .. torrential 1.0, SSAtmoEnvWeatherState::mDropletSizeScale) instead of the raw precipitation number, through the same 0.55-1.45 multiplier window - never outside what the old ramp could reach. Still needs mIntensitySize > 0 to have any reach; off (or no environment), the raw-number ramp stands.
    bool mWeatherSize = true;

    LLColor4 mTint = LLColor4::white;
    F32 mGlow = 0.f;
    U8 mDropShape = DROP_DOT;
    bool mEmissive = false;
    bool mWaterShading = false;

    F32 mDropScale = 1.f;

    SSPrecipTierParams mTiers[TIER_COUNT];

    F32 mImpactStrength = 0.7f;

    // <SS:Nexii> Weather-cube opt-in: scales the authored impact strength by the cube's band impact scale (SSAtmoEnvWeatherState::mImpactScale - drizzle 0, torrential 1), so splashes graduate as the storm builds instead of drizzle hammering at the full authored strength. Off (or no environment), the flat authored value stands.
    bool mWeatherImpact = true;
    bool mShatter = false;

    F32 mRippleSize = 0.35f;
    F32 mRippleAlpha = 0.4f;
    F32 mRippleLife = 0.45f;

    F32 mCrownSize = 0.05f;
    F32 mCrownAlpha = 0.35f;
    F32 mCrownSpeed = 0.6f;
    F32 mCrownLife = 0.3f;

    F32 mDarkMix = 0.f;
    F32 mPuffMix = 0.f;

    F32 mStreamSpan = 0.f;
    F32 mStreamAlpha = 1.f;
    F32 mStreamLength = 6.f;
    F32 mStreamStretch = 1.f;
    F32 mStreamWind = 0.35f;
    F32 mStreamScale = 1.f;
    F32 mDripScale = 1.f;

    F32 mWetRate = 0.f;
    F32 mDryRate = 0.f;
    F32 mSnowRate = 0.f;
    F32 mSnowMelt = 0.f;
    F32 mSnowDepth = 0.f;
    F32 mSnowRepose = 45.f;

    // <SS:Nexii> Granular transport: what the wind does to the settled snow this type leaves. The threshold band is global settings (physics, not art); these are the per-type rates. mSnowCreepRate 0 leaves cascades off - the shed store fills only from creep spill.
    F32 mSnowLiftRate = 0.f;     // metres of depth per second eroded at full lift; 0 = never blows
    F32 mSnowDepositRate = 0.f;  // metres of depth per second banking in a lee
    F32 mSnowCreepRate = 0.f;    // creep advection strength feeding drifts and eave spill
    F32 mSnowDriftAge = 2.5f;    // drift particle life cap, seconds

    F32 mPuddleRate = 0.f;
    F32 mPuddleDepth = 0.f;
    F32 mPuddleDrain = 0.f;

    std::string mTextures;
    std::string mRippleTexture;
    std::string mDarkTexture;
    std::string mPuffTexture;
    SSPrecipSounds mSounds;
    SSFootstepSounds mFootsteps;

    LLSD asLLSD() const;
    void fromLLSD(const LLSD& sd);

    bool risesFromGround() const { return mArchetype == SSPrecipArchetype::RISER; }
    // <SS:Nexii> FLAKE and SOLID are granular - the settled field is a drift surface the wind works on, runoff cascades dithered grains rather than water sheets. LIQUID and RISER are not. The code keeps its mSnow* names; the channel is the granular channel.
    bool isGranular() const
    {
        return mArchetype == SSPrecipArchetype::FLAKE || mArchetype == SSPrecipArchetype::SOLID;
    }
    bool makesImpacts() const { return mImpactStrength > 0.f && !risesFromGround(); }
    bool makesRipples() const { return mRippleSize > 0.f && mRippleAlpha > 0.f && mRippleLife > 0.f; }
    bool makesCrowns() const { return mCrownSize > 0.f && mCrownAlpha > 0.f && mCrownLife > 0.f; }

    bool marksSurface() const
    {
        return mWetRate > 0.f
            || (mSnowRate > 0.f && mSnowDepth > 0.f)
            || (mPuddleRate > 0.f && mPuddleDepth > 0.f);
    }
    U8 material() const { return mEmissive ? MAT_EMISSIVE : (mWaterShading ? MAT_WATER : MAT_LIT); }

    static const char* archetypeName(SSPrecipArchetype a);
};

class SSPrecipPresetManager : public LLSingleton<SSPrecipPresetManager>
{
    LLSINGLETON(SSPrecipPresetManager);

public:
    void refresh();

    const std::vector<SSPrecipPreset>& presets() const { return mPresets; }
    const SSPrecipPreset* find(const std::string& name) const;

    const SSPrecipPreset& active() const;

    bool save(const SSPrecipPreset& preset);
    bool remove(const std::string& name);

    void stage(const SSPrecipPreset& preset);

    // <SS:Nexii> The environment tier. Types authored into an Atmo environment are staged into the live list so everything resolving a precipitation by name finds them unchanged, and dropped again when the environment unloads. Replacing the whole set at once is what makes an editor rename or deletion take.
    void setEnvironmentPresets(const std::vector<SSPrecipPreset>& presets);
    void clearEnvironmentPresets();

    bool isModified(const std::string& name) const;

    const SSPrecipPreset* findSaved(const std::string& name) const;

    static std::string presetDir();

private:
    void buildDefaults();
    void loadUserPresets();

    std::vector<SSPrecipPreset> mPresets;

    std::vector<SSPrecipPreset> mSaved;
};

#endif
