/**
 * @file ssatmomagic.cpp
 * @brief See ssatmomagic.h.
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

#include "ssatmomagic.h"
#include "ssatmotrack.h"
#include "ssatmoenvapplier.h"
#include "ssatmoenvbridge.h"
#include "ssatmoenvmanager.h"
#include "ssgranular.h"
#include "ssrainshadow.h"
#include "ssavatarwet.h"
#include "ssvolcloud.h"
#include "sslightning.h"
#include "sslightningrender.h"
#include "sssurfacefield.h"
#include "sswhiteout.h"
#include "sswindflow.h"
#include "ssworldfield.h"

#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "ssprecipitation.h"
#include "ssprecippreset.h"
#include "sssoundscape.h"
#include "sssoundmeta.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llaudioengine.h"
#include "lldate.h"
#include "llfetchedgltfmaterial.h"
#include "llgltfmateriallist.h"
#include "llfasttimer.h"
#include "llfontgl.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "pipeline.h"

#include <algorithm>

static const F32 IMPACT_SEE_RADIUS  = 32.f;
static const F64 ASSET_POLL_PERIOD  = 2.0;

static const U32 SS_ATMO_SEED = 0x5EED1337u;

// <SS:Nexii> Regime derivation. Enter thresholds are the whole story - the hysteresis both ways is the dwell time: a regime must hold its candidate the entire dwell before the switch fires, and a lull must last just as long to climb back down. See section 14 of doc/atmo_magic_snow.md: regimes direct, the field decides.
static const F32 REGIME_ENTER_SALTATION = 4.0f;
static const F32 REGIME_ENTER_DRIFT     = 6.5f;
static const F32 REGIME_ENTER_BLIZZARD  = 8.5f;
static const F32 REGIME_SQUALL_WIND     = 7.5f;
static const F32 REGIME_SQUALL_PRECIP   = 0.55f;
static const F32 REGIME_SQUALL_GUST     = 0.35f;
static const F32 REGIME_DWELL           = 20.f;
static const F32 REGIME_DWELL_SQUALL    = 30.f;

static LLTrace::BlockTimerStatHandle FTM_SS_ATMO("Atmo Magic");
static LLTrace::BlockTimerStatHandle FTM_SS_ATMO_IMPACTS("Impacts");

namespace SSAtmoNoise
{

// 1D lattice hash to [-1,1].
static F32 latticeGrad(U32 seed, S32 ix)
{
    return hash01(combine(seed, (U32)ix)) * 2.f - 1.f;
}

// 2D lattice hash to [-1,1].
static F32 latticeGrad2(U32 seed, S32 ix, S32 iy)
{
    return hash01(combine(seed, combine((U32)ix, (U32)iy * 0x27d4eb2fu))) * 2.f - 1.f;
}

// Quintic fade.
static inline F32 quintic(F32 t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

// 1D value noise.
F32 value1(F32 x, U32 seed)
{
    const S32 ix = llfloor(x);
    const F32 fx = (F32)ix;
    F32 t = quintic(x - fx);
    return lerp(latticeGrad(seed, ix), latticeGrad(seed, ix + 1), t);
}

// 2D value noise.
F32 value2(F32 x, F32 y, U32 seed)
{
    const S32 ix = llfloor(x);
    const S32 iy = llfloor(y);
    const F32 fx = (F32)ix;
    const F32 fy = (F32)iy;
    F32 tx = quintic(x - fx);
    F32 ty = quintic(y - fy);
    F32 a = lerp(latticeGrad2(seed, ix, iy),     latticeGrad2(seed, ix + 1, iy),     tx);
    F32 b = lerp(latticeGrad2(seed, ix, iy + 1), latticeGrad2(seed, ix + 1, iy + 1), tx);
    return lerp(a, b, ty);
}

// 1D fractal noise - the deterministic wobble everything shares.
F32 fbm1(F32 x, U32 seed, S32 octaves)
{
    F32 sum = 0.f, amp = 0.5f, freq = 1.f, norm = 0.f;
    for (S32 i = 0; i < octaves; ++i)
    {
        sum += amp * value1(x * freq, combine(seed, (U32)i));
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;
    }
    return sum / norm;
}

// 2D fractal noise.
F32 fbm2(F32 x, F32 y, U32 seed, S32 octaves)
{
    F32 sum = 0.f, amp = 0.5f, freq = 1.f, norm = 0.f;
    for (S32 i = 0; i < octaves; ++i)
    {
        sum += amp * value2(x * freq, y * freq, combine(seed, (U32)i));
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;
    }
    return sum / norm;
}

}

static const F32 TRACK_FADE_RATE = 0.45f;
static const F32 WIND_FADE_RATE  = 0.8f;

static const F64 WIND_DRIFT_WRAP = 1048576.0;

// Singleton shell; state arrives via refreshParams.
SSAtmoMagic::SSAtmoMagic()
{
}

SSAtmoMagic::~SSAtmoMagic()
{
}

// The static seed all deterministic weather derives from.
U32 SSAtmoMagic::seed() const
{
    return SS_ATMO_SEED;
}

// Water height used where no region answers.
F32 SSAtmoMagic::voidWaterHeight()
{
    LLViewerRegion* regionp = gAgent.getRegion();
    return regionp ? regionp->getWaterHeight() : 20.f;
}

// Re-derives the whole running weather from the active track config: preset, intensity easing, wind, gusts, lightning handoff.
void SSAtmoMagic::refreshParams()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoEnabled", false);

    SSAtmoTrackConfig v3_cfg;
    bool v3_is_ground_track = true;
    const F32 world_z = gAgent.getPositionAgent().mV[VZ];
    LLViewerRegion* agent_region = gAgent.getRegion();
    const LLUUID region_id = agent_region ? agent_region->getRegionID() : LLUUID::null;

    const bool teleported = !mV3PrevWorldZValid
        || region_id != mV3PrevRegionID
        || llabs(world_z - mV3PrevWorldZ) > 60.f;

    const bool v3_active = SSAtmoEnvBridge::resolveActiveTrack(
        world_z, mV3PrevWorldZValid ? mV3PrevWorldZ : world_z, teleported, v3_cfg, v3_is_ground_track);

    mV3PrevWorldZ = world_z;
    mV3PrevWorldZValid = true;
    mV3PrevRegionID = region_id;

    SSAtmoTrackManager* tracks = SSAtmoTrackManager::getInstance();
    mTrack = tracks->currentTrack();

    // <SS:Nexii> An environment is the whole system's master gate, not just the sky's: with none resolved, every Atmo Magic feature - precipitation, wind, lightning, the sound engine, footsteps - stands down and the viewer runs pristine. The deprecated legacy track layer no longer drives anything here; ssatmoenvapplier.cpp's want_active is the sky-side twin.
    static const SSAtmoTrackConfig no_env_cfg;
    const SSAtmoTrackConfig& cfg = v3_active ? v3_cfg : no_env_cfg;

    const bool track_runs = enabled && cfg.runs();

    SSPrecipPresetManager& mgr = SSPrecipPresetManager::instance();
    const SSPrecipPreset* named = cfg.mPreset.empty() ? nullptr : mgr.find(cfg.mPreset);
    const SSPrecipPreset& target_preset = named ? *named : mgr.active();

    const F32 dt = llclamp((F32)gFrameIntervalSeconds, 0.f, 0.25f);

    // <SS:Nexii> The blend tracks whether weather RUNS at all - an environment arriving or leaving fades precipitation in and out. A preset swap is not a weather change: rain becoming snow as temperature crosses zero is the same sky handing over different particles, and dipping intensity through zero for it read as the storm dying and restarting (observed at the crossover). Swap in place, immediately, every time.
    const F32 blend_target = track_runs ? 1.f : 0.f;
    mBlend += (blend_target - mBlend) * llclamp(TRACK_FADE_RATE * dt, 0.f, 1.f);

    if (track_runs)
    {
        mPreset = target_preset;
        mPresetName = target_preset.mName;
    }

    mEnabled = enabled && (cfg.runs() || mBlend > 0.01f);

    // <SS:Nexii> Switched-on means the setting AND a live environment: the footstep picker and sound-meta analyser read this directly, and both must fall back to stock behaviour (stock step sounds, no analysis) the moment no environment answers.
    mSwitchedOn = enabled && v3_active;

    mTemperatureC = cfg.mTemperatureC;

    // <SS:Nexii> The bolt-from-the-blue look-ahead: when the weather cube's next keyframe is stormier than now and the day phase has run most of the way toward it, a thunderstorm approaches from upwind - lightning starts arriving from that direction before the storm itself does (SSLightning::idle's blue scheduler). Zero without a live environment.
    mStormApproach = 0.f;
    mStormApproachHeading = -1.f;
    if (v3_active)
    {
        mStormApproach = SSAtmoEnvBridge::stormApproach(
            world_z, mV3PrevWorldZValid ? mV3PrevWorldZ : world_z, teleported,
            mStormApproachHeading);
    }

    mLightningColor = cfg.mLightningColor;
    mLightningCoreWhite = cfg.mLightningCoreWhite;

    mLightning = cfg.mLightning;
    mLightningCharge = cfg.mLightningCharge;
    mLightningSparks = cfg.mLightningSparks;
    mLightningIntervalMin = cfg.mLightningIntervalMin;
    mLightningIntervalMax = cfg.mLightningIntervalMax;
    mLightningIntensity = cfg.mLightningIntensity;

    mPrecipitation = llclamp(cfg.mPrecipitation, 0.f, 1.f) * mBlend;
    mTurbulence = llclamp(cfg.mTurbulence, 0.f, 1.f);
    mDropletScale = cfg.mDropletScale;
    mImpactScale = cfg.mImpactScale;

    mGustDepth = llclamp(cfg.mGustDepth, 0.f, 3.f) * mTurbulence * mBlend;
    mGustLength = llclamp(cfg.mGustLength, 8.f, 2000.f);
    mGustVeer = llclamp(cfg.mGustVeer, 0.f, 90.f) * DEG_TO_RAD;

    const F32 target_speed = track_runs ? llmax(0.f, cfg.mWindSpeed) : 0.f;
    const LLVector3 target_wind = cfg.windDirection() * target_speed;
    mWind += (target_wind - mWind) * llclamp(WIND_FADE_RATE * dt, 0.f, 1.f);
    mWindXY.set(mWind.mV[VX], mWind.mV[VY], 0.f);
    mWindSpeed = mWind.magVec();

    if (mWindDriftSeeded)
    {
        mWindDrift = fmod(mWindDrift + (F64)mWindSpeed * dt, WIND_DRIFT_WRAP);
    }
    else if (target_speed > 0.f)
    {
        mWindDrift = fmod(mNow * (F64)target_speed, WIND_DRIFT_WRAP);
        mWindDriftSeeded = true;
    }

    mSkyTrack = v3_active ? !v3_is_ground_track : tracks->isSkyTrack(mTrack);
    mGroundZero = cfg.mHasGround ? cfg.mGround : tracks->trackFloor(mTrack);
    mFallThrough = llclamp(cfg.mFallThrough, 0.f, 1.f);

    mHasWeather = mEnabled && mPrecipitation > 0.02f;

    const F32 fall = mPreset.mFallSpeed;
    if (!mHasWeather || fall <= 0.f)
    {
        mRainDirection.set(0.f, 0.f, -1.f);
    }
    else if (mPreset.risesFromGround())
    {
        mRainDirection.set(mWindXY.mV[0] * 0.02f, mWindXY.mV[1] * 0.02f, -1.f);
        mRainDirection.normVec();
    }
    else
    {
        const F32 tilt = llclamp(mWindXY.magVec() * mPreset.mWindResponse / fall, 0.f, 2.f);
        LLVector3 dir_xy = mWindXY;
        dir_xy.normVec();
        mRainDirection = dir_xy * tilt;
        mRainDirection.mV[VZ] = -1.f;
        mRainDirection.normVec();
    }
}

// The gust strength envelope at a moment - shared time, so every client agrees.
F32 SSAtmoMagic::gustEnvelopeAt(F64 time) const
{
    if (mTurbulence <= 0.f) return 1.f;

    const F32 t = (F32)fmod(time, 4096.0);
    F32 wave = SSAtmoNoise::fbm1(t * (0.05f + 0.15f * mTurbulence), SS_ATMO_SEED ^ 0xA17C0FEEu);
    wave = llclamp(wave * 2.2f + 0.5f, 0.f, 1.f);
    wave = cubic_step(wave);

    F32 burst = llclamp(SSAtmoNoise::fbm1(t * 0.7f, SS_ATMO_SEED ^ 0x00B57A9Du, 2) * 2.5f, 0.f, 1.f);
    burst = burst * burst * burst * 2.f;

    return lerp(1.f, 0.15f + 1.7f * wave + burst, mTurbulence);
}

// Slow spatial modulation of intensity over global position - showers have edges.
F32 SSAtmoMagic::areaFactorAt(F64 global_x, F64 global_y) const
{
    const F64 drift = mNow * 0.35;
    const F32 x = (F32)fmod(global_x - mWindXY.mV[0] * drift, 8192.0) * 0.02f;
    const F32 y = (F32)fmod(global_y - mWindXY.mV[1] * drift, 8192.0) * 0.02f;
    const F32 n = 0.5f + 0.5f * SSAtmoNoise::fbm2(x, y, SS_ATMO_SEED ^ 0x5EED0A2Bu);
    return 0.35f + 1.3f * n;
}

// Splits a CSV setting into asset entries.
static void parseAssetList(const std::string& value, std::vector<SSAtmoAsset>& out)
{
    out.clear();
    std::string::size_type pos = 0;
    while (pos < value.size())
    {
        std::string::size_type end = value.find(',', pos);
        if (end == std::string::npos) end = value.size();
        std::string token = value.substr(pos, end - pos);
        pos = end + 1;

        LLStringUtil::trim(token);
        if (token.empty()) continue;

        SSAtmoAsset asset;
        if (token.compare(0, 4, "pbr:") == 0)
        {
            asset.mIsPBR = true;
            token = token.substr(4);
            LLStringUtil::trim(token);
        }
        if (LLUUID::validate(token))
        {
            asset.mID.set(token);
            out.push_back(asset);
        }
    }
}

// Reloads the texture and asset lists from settings.
void SSAtmoMagic::refreshAssets()
{
    const std::string fingerprint = mPreset.mName + "|" + mPreset.mTextures + "|"
                                  + mPreset.mRippleTexture;
    if (fingerprint == mAssetsFingerprint) return;
    mAssetsFingerprint = fingerprint;

    parseAssetList(mPreset.mTextures, mTextureAssets);

    std::vector<SSAtmoAsset> ripple;
    parseAssetList(mPreset.mRippleTexture, ripple);
    mRippleTexture = nullptr;
    if (!ripple.empty() && !ripple[0].mIsPBR)
    {
        mRippleTexture = LLViewerTextureManager::getFetchedTexture(ripple[0].mID);
    }

}

// Fetches an asset's texture, tint and glow.
LLViewerTexture* SSAtmoMagic::textureFor(const SSAtmoAsset& asset, LLColor4& tint, F32& glow)
{
    if (asset.mIsPBR)
    {
        LLFetchedGLTFMaterial* mat = gGLTFMaterialList.getMaterial(asset.mID);
        if (mat)
        {
            tint = mat->mBaseColor;
            glow = llclamp((mat->mEmissiveColor.mV[0] + mat->mEmissiveColor.mV[1] + mat->mEmissiveColor.mV[2]) / 3.f, 0.f, 1.f);
            if (mat->mBaseColorTexture.notNull()) return mat->mBaseColorTexture;
            if (mat->mEmissiveTexture.notNull()) return mat->mEmissiveTexture;
        }
        return nullptr;
    }
    return LLViewerTextureManager::getFetchedTexture(asset.mID);
}

// Rolls a particle texture from the configured set.
LLViewerTexture* SSAtmoMagic::pickParticleTexture(SSRandStream& rng, LLColor4& tint, F32& glow)
{
    tint = LLColor4::white;
    glow = 0.f;
    if (mTextureAssets.empty()) return nullptr;
    return textureFor(mTextureAssets[rng.rand((S32)mTextureAssets.size())], tint, glow);
}

// First usable texture from a CSV list.
LLViewerTexture* SSAtmoMagic::textureFromList(const std::string& csv)
{
    if (csv.empty()) return nullptr;

    std::vector<SSAtmoAsset> assets;
    parseAssetList(csv, assets);
    if (assets.empty() || assets[0].mIsPBR) return nullptr;
    return LLViewerTextureManager::getFetchedTexture(assets[0].mID);
}

// The ripple ring texture.
LLViewerTexture* SSAtmoMagic::rippleTexture()
{
    return mRippleTexture;
}

// Creates the particle sim on first need.
void SSAtmoMagic::ensureSim()
{
    if (mEnabled)
    {
        if (!mSim)
        {
            mSim = std::make_unique<SSPrecipSim>();
        }
    }
    else if (mSim)
    {
        mSim.reset();
        mImpacts.clear();
    }
}

// Keeps agent-space state coherent across region origin shifts.
void SSAtmoMagic::shift(const LLVector3& offset)
{
    if (mSim)
    {
        mSim->shift(offset);
    }
    for (auto& impact : mImpacts)
    {
        impact.second.mPosAgent += offset;
    }

    for (auto& entry : mPendingEdits)
    {
        entry.second.mPos += offset;
    }
}

// Queues a drop impact to be processed at its own arrival time.
void SSAtmoMagic::queueImpact(F64 time, const LLVector3& pos_agent, F32 strength, bool on_water,
                              const LLVector3& normal, const LLVector3& velocity, bool shatter,
                              bool from_runoff)
{
    if (mImpacts.size() > 16384) return;
    mImpacts.emplace(time, Impact{ pos_agent, normal, velocity, strength, on_water, shatter, from_runoff });
}

// Plays due impacts: sounds and effects, whatever the preset says an arrival does.
void SSAtmoMagic::processImpacts()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_ATMO_IMPACTS);

    static LLCachedControl<bool> ripples(gSavedSettings, "SSAtmoRipples", true);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    while (!mImpacts.empty() && mImpacts.begin()->first <= mNow)
    {
        const Impact impact = mImpacts.begin()->second;
        mImpacts.erase(mImpacts.begin());

        const F32 dist = (impact.mPosAgent - cam).magVec();
        if (dist > IMPACT_SEE_RADIUS) continue;

        SSSoundscape::getInstance()->notifyImpact(
            impact.mStrength * (1.f - dist / IMPACT_SEE_RADIUS));

        SSRandStream rng(SSAtmoNoise::combine(SS_ATMO_SEED,
            SSAtmoNoise::combine((U32)(S32)(impact.mPosAgent.mV[VX] * 16.f),
                                 (U32)(S32)(impact.mPosAgent.mV[VY] * 16.f))));
        rng.next();

        // <SS:Nexii> Granular runoff lands as mass, not water: a cascade clump credits the cell it lands on, and the eave drift pile forms there. The from_runoff flag marks exactly these; ordinary falling snow still makes no ripples (FLAKE presets carry mImpactStrength 0, so they never queue impacts).
        if (impact.mRunoff && granularWeather() && !impact.mOnWater)
        {
            SSSurfaceField::getInstance()->depositAt(impact.mPosAgent,
                                                     0.0006f * llmax(impact.mStrength, 0.4f));
            continue;
        }

        if (ripples && mSim)
        {
            mSim->spawnRipple(impact.mPosAgent, impact.mStrength, impact.mOnWater, impact.mNormal, rng);
            if (impact.mShatter)
            {
                mSim->spawnShatter(impact.mPosAgent, impact.mNormal, impact.mVelocity,
                                   impact.mStrength, rng);
            }
        }

    }
}

// <SS:Nexii> Granular weather: lift authority, transport bundle, regime machine.

// Is snow lifting here, and how hard - 0 to 1, physical, no preset rate or gust in the figure. Callers scale by the preset's rate and apply gustEnvelopeAt() once; the transport gets both as scalars in its bundle. The threshold band evaluates against the flowmap's ground slab, not the ambient - the alley jets reach it before the open ground does, the whole point of the capture stack.
F32 SSAtmoMagic::liftAt(const LLVector3& pos_agent) const
{
    const SSPrecipPreset& p = preset();
    if (!hasWeather() || !p.isGranular() || p.mSnowLiftRate <= 0.f) return 0.f;

    static LLCachedControl<F32> lift_lo(gSavedSettings, "SSAtmoSnowLiftLo", 3.5f);
    static LLCachedControl<F32> lift_hi(gSavedSettings, "SSAtmoSnowLiftHi", 8.0f);
    static LLCachedControl<F32> lift_temp(gSavedSettings, "SSAtmoSnowLiftTemp", 1.5f);

    const F32 temp_scale = llclamp(1.f - mTemperatureC / llmax((F32)lift_temp, 0.1f), 0.f, 1.f);
    if (temp_scale <= 0.f) return 0.f;

    const LLVector3 flow = SSWindFlowMap::getInstance()->sampleGround(pos_agent);
    const F32 speed = sqrtf(flow.mV[VX] * flow.mV[VX] + flow.mV[VY] * flow.mV[VY]);

    const F32 lo = llmax((F32)lift_lo, 0.1f);
    const F32 hi = llmax((F32)lift_hi, lo + 0.1f);
    if (speed <= lo) return 0.f;

    const F32 band = llclamp((speed - lo) / (hi - lo), 0.f, 1.f);
    return band * band * (3.f - 2.f * band) * temp_scale;
}

bool SSAtmoMagic::granularWeather() const
{
    return hasWeather() && mPreset.isGranular();
}

// The transport's bundle for this tick. Plain floats only; the gust envelope rides in as one scalar, applied once per tick rather than per cell.
void SSAtmoMagic::fillTransportParams(SSGranularParams& params) const
{
    const SSPrecipPreset& p = preset();

    static LLCachedControl<F32> lift_lo(gSavedSettings, "SSAtmoSnowLiftLo", 3.5f);
    static LLCachedControl<F32> lift_hi(gSavedSettings, "SSAtmoSnowLiftHi", 8.0f);
    static LLCachedControl<F32> lift_temp(gSavedSettings, "SSAtmoSnowLiftTemp", 1.5f);
    static LLCachedControl<F32> deposit_gap(gSavedSettings, "SSAtmoSnowDepositGap", 0.7f);
    static LLCachedControl<F32> creep_scale(gSavedSettings, "SSAtmoSnowCreep", 1.f);

    params.mLiftLo = llmax((F32)lift_lo, 0.1f);
    params.mLiftHi = llmax((F32)lift_hi, params.mLiftLo + 0.1f);
    params.mLiftRate = granularWeather() ? llmax(p.mSnowLiftRate, 0.f) : 0.f;
    params.mDepositRate = granularWeather() ? llmax(p.mSnowDepositRate, 0.f) : 0.f;
    params.mCreepRate = granularWeather() ? llmax(p.mSnowCreepRate, 0.f) * llmax((F32)creep_scale, 0.f) : 0.f;
    params.mDepositGap = llclamp((F32)deposit_gap, 0.1f, 1.f);
    params.mLiftTemp = llmax((F32)lift_temp, 0.1f);
    params.mTemperatureC = mTemperatureC;
    params.mSnowDepth = llmax(p.mSnowDepth, 0.f);
    params.mReposeRad = llclamp(p.mSnowRepose, 5.f, 89.f) * DEG_TO_RAD;
    params.mGust = llclamp(gustEnvelopeAt(mNow), 0.f, 2.5f);
    params.mFlow = nullptr;
}

const char* SSAtmoMagic::regimeName(ERegime r)
{
    switch (r)
    {
        case ERegime::SALTATION: return "saltation";
        case ERegime::DRIFT:     return "drift";
        case ERegime::BLIZZARD:  return "blizzard";
        case ERegime::SQUALL:    return "squall";
        case ERegime::CALM:      return "calm";
        default:                 return "?";
    }
}

// The regime director: derived from the same params the weather resolver already produces, with the dwell time as hysteresis in both directions. Fixed-step discipline - the dwell accumulates only real elapsed time, and the initial regime is derived, so a viewer joining mid-storm starts right without replaying history.
void SSAtmoMagic::updateRegime(F32 dt)
{
    static LLCachedControl<S32> override_regime(gSavedSettings, "SSAtmoSnowRegimeOverride", -1);

    const F32 wind = mWindXY.magVec();
    const bool cold = mTemperatureC <= 1.5f;
    const bool snow_present = hasWeather() && mPreset.isGranular() && mPreset.mSnowRate > 0.f;
    const bool falling = snow_present && mPrecipitation > 0.05f;

    ERegime candidate = ERegime::CALM;
    if (cold && snow_present)
    {
        if (falling && mPrecipitation >= REGIME_SQUALL_PRECIP && wind >= REGIME_SQUALL_WIND
            && mGustDepth >= REGIME_SQUALL_GUST)
        {
            candidate = ERegime::SQUALL;
        }
        else if (wind >= REGIME_ENTER_BLIZZARD)
        {
            candidate = ERegime::BLIZZARD;
        }
        else if (wind >= REGIME_ENTER_DRIFT)
        {
            candidate = ERegime::DRIFT;
        }
        else if (wind >= REGIME_ENTER_SALTATION)
        {
            candidate = ERegime::SALTATION;
        }
    }

    if ((S32)override_regime >= 0 && (S32)override_regime < (S32)ERegime::COUNT)
    {
        candidate = (ERegime)(S32)override_regime;
    }

    if (!mRegimeReady)
    {
        mRegime = candidate;
        mRegimeReady = true;
        mRegimeCandidateTime = 0.f;
        return;
    }

    if (candidate == mRegime)
    {
        mRegimeCandidateTime = 0.f;
        return;
    }

    mRegimeCandidateTime += dt;
    const F32 dwell = (mRegime == ERegime::SQUALL || candidate == ERegime::SQUALL)
                          ? REGIME_DWELL_SQUALL : REGIME_DWELL;
    if (mRegimeCandidateTime >= dwell)
    {
        const ERegime previous = mRegime;
        mRegime = candidate;
        mRegimeCandidateTime = 0.f;
        mRegimeSignal(previous, mRegime);
    }
}


// The per-frame heartbeat: params, sim, sounds, fields, lightning - everything driven from here.
void SSAtmoMagic::idle()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_ATMO);

    mNow = LLDate::now().secondsSinceEpoch();

    SSAtmoTrackManager::getInstance()->idle();

    refreshParams();

    // <SS:Nexii> Regime evaluation on the frame clock (the dwell is seconds of real time; the transport below is fixed-step) and the squall figure the whiteout ramp reads.
    updateRegime(gFrameIntervalSeconds);
    {
        const bool falling = hasWeather() && mPreset.isGranular() && mPreset.mSnowRate > 0.f
                          && mPrecipitation > 0.05f;
        const F32 target = (falling && mTemperatureC <= 0.f && mGustDepth >= 0.3f)
                               ? llmin(mPrecipitation, mGustDepth * 2.f) : 0.f;
        const F32 blend = 1.f - expf(-gFrameIntervalSeconds * 1.5f);
        mSquallFactor = lerp(mSquallFactor, llclamp(target, 0.f, 1.f), blend);
    }

    if (mEnabled && mNow - mLastAssetPoll > ASSET_POLL_PERIOD)
    {
        mLastAssetPoll = mNow;
        refreshAssets();
    }

    settleEdits();

    ensureSim();

    if (mSim)
    {
        mSim->update(gFrameIntervalSeconds);
    }

    SSSurfaceField::getInstance()->idle(gFrameIntervalSeconds);

    // <SS:Nexii> The whiteout layer's intensity state: regime ramps applied CPU-side, the pass itself draws in the pool loop after the haze.
    SSWhiteout::getInstance()->idle(gFrameIntervalSeconds);

    SSAvatarWet::getInstance()->idle(gFrameIntervalSeconds);

    SSLightning::getInstance()->idle(gFrameIntervalSeconds);

    SSVolCloud::getInstance()->update(gFrameIntervalSeconds);

    if (mEnabled)
    {
        processImpacts();
    }

    SSSoundscape::getInstance()->idle();
}

static const F64 EDIT_SETTLE_SECONDS = 4.0;

static const size_t MAX_PENDING_EDITS = 4096;

static const size_t MAX_SETTLE_PER_FRAME = 64;

static const F32 EDIT_SETTLE_SLOP = 0.25f;

// Feeds the settle queue: geometry must hold still a while before the maps spend a recapture on it.
void SSAtmoMagic::onObjectUpdate(LLViewerObject* objectp)
{
    SSAtmoMagic* self = getInstance();
    if (!self->mEnabled) return;
    if (!objectp || objectp->isDead() || objectp->isAvatar() || objectp->isAttachment()) return;

    const LLVector3 scale = objectp->getScale();
    const F32 dim = llmax(scale.mV[VX], scale.mV[VY], scale.mV[VZ]);
    if (dim < 0.5f) return;

    if (objectp->isLikelyProjectileBullet()) return;

    const LLVector3 pos = objectp->getRenderPosition();
    const F32 radius = scale.magVec() * 0.5f;

    const bool moving = objectp->getVelocity().magVecSquared() > 0.25f
                     || objectp->getAngularVelocity().magVecSquared() > 0.25f;

    auto it = self->mPendingEdits.find(objectp->getID());
    if (it != self->mPendingEdits.end())
    {
        PendingEdit& edit = it->second;

        if (moving || (edit.mPos - pos).magVecSquared() > EDIT_SETTLE_SLOP * EDIT_SETTLE_SLOP)
        {
            edit.mPos = pos;
            edit.mRadius = radius;
            edit.mSettleAt = self->mNow + EDIT_SETTLE_SECONDS;
            ++edit.mResets;
        }
        return;
    }

    if (self->mPendingEdits.size() >= MAX_PENDING_EDITS)
    {
        auto worst = self->mPendingEdits.begin();
        for (auto e = self->mPendingEdits.begin(); e != self->mPendingEdits.end(); ++e)
        {
            if (e->second.mSettleAt > worst->second.mSettleAt) worst = e;
        }
        if (worst->second.mSettleAt <= self->mNow + EDIT_SETTLE_SECONDS) return;
        self->mPendingEdits.erase(worst);
    }

    PendingEdit edit;
    edit.mPos = pos;
    edit.mRadius = radius;
    edit.mSettleAt = self->mNow + EDIT_SETTLE_SECONDS;
    edit.mFirstSeen = self->mNow;
    self->mPendingEdits[objectp->getID()] = edit;
}

// Promotes settled edits into rain-shadow dirty marks.
void SSAtmoMagic::settleEdits()
{
    if (mPendingEdits.empty()) return;

    size_t confirmed = 0;

    for (auto it = mPendingEdits.begin(); it != mPendingEdits.end(); )
    {
        if (mNow < it->second.mSettleAt)
        {
            ++it;
            continue;
        }

        LLViewerObject* objectp = gObjectList.findObject(it->first);

        if (!objectp || objectp->isDead())
        {
            it = mPendingEdits.erase(it);
            continue;
        }

        const LLVector3 pos = objectp->getRenderPosition();
        if ((pos - it->second.mPos).magVecSquared() > EDIT_SETTLE_SLOP * EDIT_SETTLE_SLOP)
        {
            it->second.mPos = pos;
            it->second.mRadius = objectp->getScale().magVec() * 0.5f;
            it->second.mSettleAt = mNow + EDIT_SETTLE_SECONDS;
            ++it->second.mResets;
            ++it;
            continue;
        }

        SSRainShadowMap::getInstance()->markDirty(it->second.mPos, it->second.mRadius);
        SSWindFlowMap::markDirty(it->second.mPos, it->second.mRadius);
        SSWorldField::markDirty(it->second.mPos, it->second.mRadius);
        ++mSettledEdits;

        it = mPendingEdits.erase(it);

        if (++confirmed >= MAX_SETTLE_PER_FRAME) break;
    }
}

// Draws pending settle edits as boxes.
void SSAtmoMagic::renderDebug()
{
    if (mPendingEdits.empty()) return;

    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    static const F32 BEACON_HEIGHT = 24.f;

    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        gGL.begin(LLRender::LINES);
        for (const auto& entry : mPendingEdits)
        {
            const PendingEdit& edit = entry.second;

            LLColor4 color = colorForEdit(edit);

            const LLVector3& p = edit.mPos;
            const F32 r = llmax(edit.mRadius, 0.25f);

            gGL.color4fv(color.mV);
            const LLVector3 lo = p - LLVector3(r, r, r);
            const LLVector3 hi = p + LLVector3(r, r, r);
            const F32 xs[2] = { lo.mV[VX], hi.mV[VX] };
            const F32 ys[2] = { lo.mV[VY], hi.mV[VY] };
            const F32 zs[2] = { lo.mV[VZ], hi.mV[VZ] };
            for (S32 a = 0; a < 2; ++a)
            {
                for (S32 b = 0; b < 2; ++b)
                {
                    gGL.vertex3f(xs[0], ys[a], zs[b]);  gGL.vertex3f(xs[1], ys[a], zs[b]);
                    gGL.vertex3f(xs[a], ys[0], zs[b]);  gGL.vertex3f(xs[a], ys[1], zs[b]);
                    gGL.vertex3f(xs[a], ys[b], zs[0]);  gGL.vertex3f(xs[a], ys[b], zs[1]);
                }
            }
        }
        gGL.end();
    }

    {
        LLGLDepthTest depth(GL_FALSE);
        gGL.begin(LLRender::LINES);
        for (const auto& entry : mPendingEdits)
        {
            const PendingEdit& edit = entry.second;
            LLColor4 color = colorForEdit(edit);

            const LLVector3& p = edit.mPos;
            const S32 steps = 6;
            for (S32 i = 0; i < steps; ++i)
            {
                const F32 t0 = (F32)i / (F32)steps;
                const F32 t1 = (F32)(i + 1) / (F32)steps;
                gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE],
                            color.mV[VALPHA] * (1.f - t0));
                gGL.vertex3f(p.mV[VX], p.mV[VY], p.mV[VZ] + BEACON_HEIGHT * t0);
                gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE],
                            color.mV[VALPHA] * (1.f - t1));
                gGL.vertex3f(p.mV[VX], p.mV[VY], p.mV[VZ] + BEACON_HEIGHT * t1);
            }
        }
        gGL.end();
    }

    gGL.flush();
}

// Settle state to overlay colour.
LLColor4 SSAtmoMagic::colorForEdit(const PendingEdit& edit)
{
    SSAtmoMagic* self = getInstance();

    const F32 remaining = (F32)llclamp((edit.mSettleAt - self->mNow) / EDIT_SETTLE_SECONDS, 0.0, 1.0);
    const F32 alpha = 0.35f + 0.55f * (1.f - remaining);

    if (edit.mResets >= 10)  return LLColor4(1.f, 0.2f, 0.15f, alpha);
    if (edit.mResets >= 2)   return LLColor4(1.f, 0.7f, 0.15f, alpha);
    return LLColor4(0.3f, 1.f, 0.4f, alpha);
}

// The debug overlay text block: weather, sim, maps, sea, lightning, audio.
// Sections are data-driven so every orange heading is a click target that
// collapses its own body (see handleInfoClick) - lightning got its own heading
// rather than riding under -- audio --, since its draw stats are render debug.
void SSAtmoMagic::drawInfo()
{
    static LLCachedControl<bool> show_info(gSavedSettings, "SSAtmoShowInfo", false);
    if (!show_info) return;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSSoundscape* audio = SSSoundscape::getInstance();
    SSPrecipSim* sim = atmo->sim();

    const SSPrecipPreset& preset = atmo->preset();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3d cam_global = gAgent.getPosGlobalFromAgent(cam);
    const LLVector3 dir = atmo->rainDirection();

    static LLCachedControl<bool> use_rain_shader(gSavedSettings, "SSAtmoRainShader", true);

    struct InfoSection
    {
        std::string key;
        std::string heading;
        std::vector<std::string> lines;
    };
    std::vector<InfoSection> sections;
    // A section's reference is only good until the next open_section - each one
    // is filled completely before the next opens.
    auto open_section = [&sections](std::string key, std::string heading) -> InfoSection&
    {
        sections.emplace_back(InfoSection{ std::move(key), std::move(heading), {} });
        return sections.back();
    };

    InfoSection& atmo_section = open_section("atmo",
        llformat("ATMO MAGIC  %s", atmo->isEnabled() ? "[enabled]" : "[disabled]"));
    atmo_section.lines.push_back(llformat("track      %d of 4   %s   ground zero %.0fm%s",
                             atmo->track(), SSAtmoEnvManager::getInstance()->statusText().c_str(),
                             atmo->groundZero(), atmo->isSkyTrack() ? " (sky)" : ""));
    if (atmo->trackBlend() < 0.99f)
    {
        atmo_section.lines.push_back(llformat("crossfade  %.2f", atmo->trackBlend()));
    }
    atmo_section.lines.push_back(llformat("preset     %s (%s)", preset.mName.c_str(),
                             SSPrecipPreset::archetypeName(preset.mArchetype)));
    atmo_section.lines.push_back(llformat("precip     %.2f    turbulence %.2f",
                             atmo->precipitation(), atmo->turbulence()));
    atmo_section.lines.push_back(llformat("wind       %.1f m/s   fall %.1f m/s   dir %.2f %.2f %.2f",
                             atmo->windSpeed(), preset.mFallSpeed,
                             dir.mV[VX], dir.mV[VY], dir.mV[VZ]));
    atmo_section.lines.push_back(llformat("gust x%.2f   area x%.2f   %s",
                             atmo->gustEnvelopeAt(atmo->sharedTime()),
                             atmo->areaFactorAt(cam_global.mdV[VX], cam_global.mdV[VY]),
                             atmo->hasWeather() ? "active" : "idle"));

    if (sim)
    {
        atmo_section.lines.push_back(llformat("particles  drops %d  clusters %d  sheets %d  ripples %d",
                                 sim->tierCount(TIER_DROPS), sim->tierCount(TIER_CLUSTERS),
                                 sim->tierCount(TIER_SHEETS), (S32)sim->ripples().size()));
    }
    else
    {
        atmo_section.lines.push_back("particles  (sim idle)");
    }

    const bool shader_live = use_rain_shader && preset.mWaterShading && gSSPrecipRainProgram.isComplete();
    atmo_section.lines.push_back(llformat("shading    rain %s   lit %s   SSR %s",
                             shader_live ? "water" : "fallback",
                             gSSPrecipLitProgram.isComplete() ? "on" : "fallback",
                             gPipeline.mSceneMap.getWidth() > 0 ? "on" : "off"));

    InfoSection& edits_section = open_section("geometry edits", "-- geometry edits --");
    edits_section.lines.push_back(llformat("queue      %d settling   %u believed",
                             (S32)atmo->pendingEdits(), atmo->settledEdits()));
    {
        U32 rearming = 0;
        for (const auto& entry : atmo->mPendingEdits)
        {
            if (entry.second.mResets >= 2) ++rearming;
        }
        edits_section.lines.push_back(llformat("re-arming  %u of %d never settling",
                                 rearming, (S32)atmo->pendingEdits()));
    }

    InfoSection& flow_section = open_section("wind flow", "-- wind flow --");
    SSWindFlowMap* flow = SSWindFlowMap::getInstance();
    if (!SSWindFlowMap::isSupported())
    {
        flow_section.lines.push_back("flowmap    unavailable (needs OpenGL 4.3)");
    }
    else if (!flow->isValid())
    {
        flow_section.lines.push_back("flowmap    idle");
    }
    else
    {
        flow_section.lines.push_back(llformat("domain     %.0fm at %d texels (%.1fm/cell)  %d tiles",
                                 flow->extent(), flow->resolution(),
                                 flow->cellSize(), flow->tileCount()));
        flow_section.lines.push_back(llformat("solved     %.0fs ago   builds %u",
                                 (F32)flow->age(), flow->buildCount()));

        // <SS:Nexii> Rebuild telemetry: is the solve off the main thread, and how well the smarter rebuilds are doing.
        flow_section.lines.push_back(llformat("worker     %s   solve %s",
                                 flow->workerActive() ? "GL thread" : "main thread",
                                 flow->lastSolveOnWorker() ? "on" : "in-frame"));
        flow_section.lines.push_back(llformat("rebuilds   %u full / %u partial   box avg %.0f%% of tile",
                                 flow->fullBuildCount(), flow->partialBuildCount(),
                                 flow->partialBoxShare() * 100.f));

        std::string slabs;
        for (S32 i = 0; i <= flow->sliceCount(); ++i)
        {
            slabs += llformat("%s%.0f", i ? " / " : "", flow->sliceAltitude(i));
        }
        flow_section.lines.push_back(llformat("slabs      %d   %s", flow->sliceCount(), slabs.c_str()));

        const LLVector3 local = flow->sample(cam);
        flow_section.lines.push_back(llformat("local wind %.1f %.1f %.1f  (%.1f m/s)  exposure %.2f",
                                 local.mV[VX], local.mV[VY], local.mV[VZ],
                                 local.magVec(), flow->exposure(cam)));

        static LLCachedControl<F32> gust_travel(gSavedSettings, "SSAtmoWindGustTravel", 1.f);

        F32 gust_scale = 1.f, gust_veer = 0.f;
        flow->gustAt(cam, atmo->sharedTime(), gust_scale, gust_veer);

        const F32 gust_length = atmo->gustLength();
        const F32 gust_speed = llmax(0.1f, atmo->windSpeed()
                                           * llclamp((F32)gust_travel, 0.01f, 4.f));
        flow_section.lines.push_back(llformat("gust wave  x%.2f   veer %+.0f deg   fronts every %.1fs",
                                 gust_scale, gust_veer * RAD_TO_DEG,
                                 gust_length / gust_speed));
        flow_section.lines.push_back(llformat("build      %.1f ms%s",
                                 flow->lastSolveOnWorker() ? flow->workerSolveMS() : flow->lastSolveMS(),
                                 flow->lastBuildPartial() ? "  (last partial)" : ""));

        flow_section.lines.push_back(llformat("solver     %.0f MB VRAM",
                                 flow->vramMB()));

        F32 top = 0.f;
        if (flow->surfaceAt(cam, top))
        {
            flow_section.lines.push_back(llformat("surface    %.1fm top   %.0f%% solid   %.1f%% under the surface",
                                     top, flow->solidFill() * 100.f,
                                     flow->carvedFraction() * 100.f));
        }
        else
        {
            flow_section.lines.push_back(llformat("surface    open column, nothing captured   %.0f%% solid",
                                     flow->solidFill() * 100.f));
        }
    }

    InfoSection& shadow_section = open_section("rain shadow", "-- rain shadow --");

    {
        LLVector3 hit;
        bool on_water = false;
        const bool mapped = SSRainShadowMap::getInstance()->resolveColumn(cam, hit, on_water);
        shadow_section.lines.push_back(llformat("column     %s at %.1fm (%+.1fm)%s",
                                 mapped ? "mapped surface" : "heightmap guess",
                                 hit.mV[VZ], hit.mV[VZ] - cam.mV[VZ],
                                 on_water ? ", water" : ""));
    }

    {
        SSRainShadowMap* shadow = SSRainShadowMap::getInstance();
        shadow_section.lines.push_back(llformat("shadow     %d regions + %d void at %d texels   %d dirty",
                                 shadow->tileCount(), shadow->voidTileCount(),
                                 (S32)shadow->resolution(),
                                 (S32)shadow->dirtyTileCount()));
        shadow_section.lines.push_back(llformat("captures   %u total, %u forced by edits   last %.1fs ago, %.1f ms",
                                 shadow->captureCount(), shadow->dirtyCaptureCount(),
                                 (F32)shadow->lastCaptureAge(), shadow->lastCaptureMS()));
    }

    InfoSection& surface_section = open_section("surface", "-- surface --");

    {
        SSSurfaceField* surface = SSSurfaceField::getInstance();
        surface_section.lines.push_back(llformat("surface    %d fields   wet %.2f   snow %.0f mm   puddle %.0f mm   %.1f ms",
                                 surface->fieldCount(), surface->peakWet(),
                                 surface->peakSnow() * 1000.f,
                                 surface->peakPuddle() * 1000.f,
                                 surface->lastTickMS()));
    }

    // <SS:Nexii> The shared world field: capture health, the state of the air flood, and what its labels say about the camera's own cell.
    InfoSection& field_section = open_section("world field", "-- world field --");
    {
        SSWorldField* field = SSWorldField::getInstance();
        const LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);

        field_section.lines.push_back(llformat("capture    %d tiles, %d cells/axis, %.0fm bands, %d cap",
                                 field->tileCount(), field->resolution(),
                                 (F32)field->bandHeight(), field->bandCount()));
        field_section.lines.push_back(llformat("builds     %u total, %u dirty rects, last %.1f ms",
                                 field->captureCount(), field->dirtyCaptureCount(),
                                 field->lastCaptureMS()));

        const F64 age = field->tileAge(cam);
        if (age >= 0.0)
        {
            field_section.lines.push_back(llformat("tile       %.0fs old, %d bands live",
                                     age, field->effectiveBands(cam)));
        }

        const U8 air = field->airLabelAt(cam);
        static const char* AIR_NAME[] = { "solid", "outside", "interior", "unknown" };
        const U32 air_depth = field->airDepthAt(cam);
        field_section.lines.push_back(llformat("air        %s, depth %s",
                                 AIR_NAME[llclamp((S32)air, 0, 3)],
                                 air_depth == SSWorldField::AIR_DEPTH_UNREACHED
                                     ? "n/a" : llformat("%u cells", air_depth).c_str()));
        if (cam_region)
        {
            field_section.lines.push_back(llformat("flood      %.0f%% of cells labelled",
                                     field->airCoverage(cam_region->getHandle()) * 100.f));
        }
    }

    // <SS:Nexii> Snow: every link of the chain in one look - type and temperature (the gate that turns the whole system off), the regime, the lift the transport computed at the camera, and the drift pool's standing population.
    InfoSection& snow_section = open_section("snow", "-- snow --");
    {
        const LLVector3 ground_flow = SSWindFlowMap::getInstance()->sampleGround(cam);
        const F32 ground_speed = sqrtf(ground_flow.mV[VX] * ground_flow.mV[VX]
                                     + ground_flow.mV[VY] * ground_flow.mV[VY]);

        snow_section.lines.push_back(llformat("type       %s   temp %.1f C   %s",
                                 preset.mName.c_str(), atmo->temperatureC(),
                                 atmo->granularWeather() ? "granular"
                                     : "NOT granular (warm or liquid type - nothing will blow)"));
        snow_section.lines.push_back(llformat("regime     %s   squall %.2f   ground wind %.1f m/s   lift here %.2f",
                                 regimeName(atmo->regime()), atmo->squallFactor(),
                                 ground_speed, atmo->liftAt(cam)));

        SSSurfaceField* surface = SSSurfaceField::getInstance();
        S32 lift_cells = 0;
        F32 peak_lift = 0.f;
        surface->forEachLiftCell(cam, 64.f,
            [&](const LLVector3&, F32, F32 lift)
            {
                ++lift_cells;
                peak_lift = llmax(peak_lift, lift);
            });

        const S32 drift_count = atmo->sim() ? atmo->sim()->driftCount() : 0;
        snow_section.lines.push_back(llformat("drift      %d live   lift cells %d within 64m   peak %.2f",
                                 drift_count, lift_cells, peak_lift));
        if (!atmo->granularWeather())
        {
            snow_section.lines.push_back("           no drift: the active type is not granular");
        }
        else if (lift_cells == 0 && drift_count == 0)
        {
            snow_section.lines.push_back("           no lift: is snow settled, is the wind over SSAtmoSnowLiftLo?");
        }

        SSWhiteout* whiteout = SSWhiteout::getInstance();
        snow_section.lines.push_back(llformat("whiteout  in %.2f   squall %.2f   drift %.2f   falloff %.0fm",
                                 whiteout->intensity(), whiteout->squallPart(),
                                 whiteout->liftPart(), whiteout->falloff()));
    }

    // <SS:Nexii> Lightning on its own heading: the draw, segment and charge stats are render debug, not audio.
    InfoSection& lightning_section = open_section("lightning", "-- lightning --");
    {
        SSLightning* lit = SSLightning::getInstance();
        const F64 next = lit->nextStrikeIn();
        lightning_section.lines.push_back(llformat("strikes    %d live   flash %.2f   %s   %d thunder pending",
                                 lit->liveCount(), lit->flash(),
                                 next < 0.0 ? "not thundery"
                                            : llformat("next in %.0fs", next).c_str(),
                                 audio->pendingThunder()));
        const F32 skew = SSLightning::positiveSkew(atmo->temperatureC());
        lightning_section.lines.push_back(llformat("charge     %s (%.0f%%)   storm approach %.0f%%",
                                 skew >= 0.5f ? "positive" : "negative", skew * 100.f,
                                 atmo->stormApproach() * 100.f));
        const SSLightningRender::DrawStats& ds = SSLightningRender::getInstance()->stats();
        lightning_section.lines.push_back(llformat("draw      %d live / %d bright / %d offscreen / %d occluded   %d quads%s",
                                 ds.mStrikes, ds.mBright, ds.mOffScreen, ds.mOccluded, ds.mQuads,
                                 ds.mDepthCopy ? "  depth copy" : ""));
        lightning_section.lines.push_back(llformat("segs      %d bolt / %d plasma / %d sparks / %d discs / %d steam",
                                 ds.mSegments, ds.mPlasma, ds.mSparks, ds.mDiscs, ds.mSteam));
    }

    InfoSection& audio_section = open_section("audio", "-- audio --");
    audio_section.lines.push_back(llformat("analysis   %d sounds ready   %d pending",
                             SSSoundMeta::getInstance()->readyCount(),
                             SSSoundMeta::getInstance()->pendingCount()));
    audio_section.lines.push_back(llformat("cover      %s   space %s%s%s",
                             audio->isCovered() ? "ROOFED" : "open sky",
                             SSSoundscape::spaceName(audio->space()),
                             audio->isCovered() ? ""
                                 : (std::string(" / ") + SSSoundscape::sizeName(audio->outdoorSize())).c_str(),
                             audio->isInterior() ? "   SEALED" : ""));
    if (audio->isCovered())
    {
        audio_section.lines.push_back(llformat("roof       %.1fm above   buried %.1fm   occlusion %.2f",
                                 audio->roofDistance(), audio->burialDepth(),
                                 audio->burialOcclusion()));
    }
    audio_section.lines.push_back(llformat("walls      %d hit   avg %.1fm   blend %.2f",
                             audio->wallCount(), audio->wallDistance(), audio->coverBlend()));
    for (S32 self = 1; self >= 0; --self)
    {
        const SSSoundscape::StepDebug& st = audio->lastStep(self != 0);
        const char* who = self ? "you " : "them";

        if (st.mWhen < 0.0)
        {
            audio_section.lines.push_back(llformat("step %s  none yet", who));
            continue;
        }

        static const char* ACTION[] = { "walk", "run", "jump", "land" };
        const char* act = (st.mAction >= 0 && st.mAction < 4) ? ACTION[st.mAction] : "?";

        std::string surface("(not reached)");
        if (st.mSurface >= 0)
        {
            surface = SSFootstepSounds::surfaceKey((SSStepSurface)st.mSurface);
        }

        audio_section.lines.push_back(llformat("step %s  %.1fs ago   %s / %s   %s(%c)   wet %.2f%s",
                                 who,
                                 (F32)(atmo->sharedTime() - st.mWhen),
                                 surface.c_str(), act,
                                 st.mIndoors ? "in" : "out", st.mIndoorsFrom,
                                 st.mWet,
                                 st.mFieldValid ? "" : "   [field: NO ANSWER]"));
        if (st.mWhyNot[0])
        {
            audio_section.lines.push_back(llformat("  SILENT   %s   [%s]",
                                    st.mWhyNot, st.mSource.c_str()));
        }
        else
        {
            audio_section.lines.push_back(llformat("  played   %s of %d   [%s]",
                                     st.mPicked.asString().substr(0, 8).c_str(),
                                     st.mListSize, st.mSource.c_str()));
        }
        audio_section.lines.push_back(llformat("  mode     %s",
                                 st.mMode == 'S' ? "per-impact segments" :
                                 st.mMode == 'L' ? "attached loop" : "-"));
        if (st.mMode == 'S')
        {
            // The number to eyeball against the gait: SL walks a step roughly every 0.5s and runs one roughly every 0.3s, so a gap near double that means footfalls are being missed rather than played per step. Any drops at all mean the anti-spam gate is firing, which it should not during a steady walk.
            audio_section.lines.push_back(llformat("  cadence  %.2fs between steps   %.1f/s   %d dropped",
                                    st.mStepGap,
                                    st.mStepGap > 0.01f ? 1.f / st.mStepGap : 0.f,
                                    st.mStepDropped));
        }
    }

    audio_section.lines.push_back(llformat("impacts    %.1f/s   %d queued   loops %d",
                             audio->impactRate(), (S32)atmo->pendingImpacts(), audio->activeLoops()));
    audio_section.lines.push_back(llformat("probe age  %.2fs", (F32)audio->lastProbeAge()));

    // Lay the sections out top down, recording each heading row as a click
    // target for the next frame's hit test.
    const LLFontGL* font = LLFontGL::getFontMonospace();
    const S32 line_h = font->getLineHeight();
    const S32 left = 12;
    const LLRect world_view = gViewerWindow->getWorldViewRectScaled();
    S32 top = world_view.getHeight() - 32;

    static const LLColor4 heading_color(1.f, 0.75f, 0.3f, 1.f);
    static const LLColor4 body_color(0.9f, 0.95f, 1.f, 1.f);

    atmo->mInfoHeadingRects.clear();

    gGL.pushMatrix();
    for (const InfoSection& section : sections)
    {
        const bool collapsed = atmo->mInfoCollapsed.count(section.key) > 0;
        const std::string heading = section.heading + (collapsed ? "  +" : "");
        atmo->mInfoHeadingRects.emplace_back(section.key,
            LLRect(left, top, world_view.getWidth(), top - line_h));
        font->renderUTF8(heading, 0, (F32)left, (F32)top, heading_color,
                         LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
        top -= line_h;
        if (!collapsed)
        {
            for (const std::string& line : section.lines)
            {
                font->renderUTF8(line, 0, (F32)left, (F32)top, body_color,
                                 LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW_SOFT);
                top -= line_h;
            }
        }
    }
    gGL.popMatrix();
}

// Click handling for the info overlay's headings: x/y are scaled window
// coordinates, origin bottom left - the same space drawInfo lays out in. A hit
// toggles that section's collapse; anything else falls through untouched.
bool SSAtmoMagic::handleInfoClick(S32 x, S32 y)
{
    static LLCachedControl<bool> show_info(gSavedSettings, "SSAtmoShowInfo", false);
    if (!show_info) return false;

    SSAtmoMagic* atmo = getInstance();
    for (const auto& heading : atmo->mInfoHeadingRects)
    {
        if (heading.second.pointInRect(x, y))
        {
            if (!atmo->mInfoCollapsed.insert(heading.first).second)
            {
                atmo->mInfoCollapsed.erase(heading.first);
            }
            return true;
        }
    }
    return false;
}
