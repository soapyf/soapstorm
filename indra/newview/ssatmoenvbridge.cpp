/**
 * @file ssatmoenvbridge.cpp
 * @brief See ssatmoenvbridge.h.
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

#include "ssatmoenvbridge.h"

#include "ssatmotrack.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvtrackstate.h"
#include "ssatmoenvweatherstate.h"

#include "llviewercontrol.h"

// <SS:Nexii> The storm-approach window: a storm barely registers against the next keyframe until the day phase has run most of the way toward it, and a keyframe level (or a drop) is not a storm on the way at all - only a rise counts. The rising margin guards against noise.
namespace
{
    const F64 APPROACH_RAMP_FROM = 0.55;
    const F64 APPROACH_RAMP_TO   = 0.95;
    const F32 APPROACH_MIN_RISE  = 0.05f;
    const F32 APPROACH_MIN       = 0.02f;
    const F32 APPROACH_STORM_C   = 0.55f;
}

// Maps a v3 precipitation type to the v2 preset name the legacy renderer keys on.
std::string SSAtmoEnvBridge::presetNameForType(const std::string& v3_type)
{
    if (v3_type.empty())          return std::string();
    if (v3_type == "rain")        return "Rain";
    if (v3_type == "snow")        return "Snow";
    if (v3_type == "hail")        return "Hail";
    if (v3_type == "blizzard")    return "Blizzard";
    if (v3_type == "sleet")       return "Sleet";
    if (v3_type == "freezing_rain") return "Freezing Rain";
    if (v3_type == "slush_mix")   return "Wintry Mix";

    // <SS:Nexii> Anything else is a type the environment carries under its own authored name - the derivation vocabulary above is only the shipped set. Handing the name straight through lets SSPrecipPresetManager::find() decide: it resolves an environment type staged by ssAtmoEnvStagePrecipTypes(), and a name that resolves to nothing falls back to the active preset exactly as an empty string used to. See doc/atmo_magic_env_ui.md.
    return v3_type;
}

// Translates the active v3 track's resolved weather into a v2 SSAtmoTrackConfig so the existing renderer runs unmodified.
bool SSAtmoEnvBridge::resolveActiveTrack(F32 world_z, F32 prev_world_z, bool teleported,
                                               SSAtmoTrackConfig& out_cfg, bool& out_is_ground_track)
{
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    // <SS:Nexii> An asset with no tracks is no environment: the applier's want_active refuses it and so does this resolver, rather than indexing an empty track vector below.
    if (!mgr->hasAsset() || mgr->asset().mTracks.empty()) return false;

    const SSAtmoEnvAsset& asset = mgr->asset();

    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(asset, world_z, prev_world_z, teleported);
    const SSAtmoEnvTrack& track = asset.mTracks[blend.mPrimaryTrack];
    out_is_ground_track = (blend.mPrimaryTrack == 0);

    const F64 phase = mgr->hasPreviewPhaseOverride() ? mgr->previewPhaseOverride()
                                                     : track.currentDayCyclePhase();
    const SSAtmoEnvWeatherState state = SSAtmoEnvWeatherResolver::resolve(track.mWeather, phase);

    out_cfg = SSAtmoTrackConfig();
    out_cfg.mDefined = true;
    out_cfg.mEnabled = true;

    out_cfg.mPreset = presetNameForType(state.mPrecipitationType);

    const F32 neighbor_fade = (blend.mNeighborTrack >= 0) ? (1.f - blend.mNeighborWeight) : 1.f;

    out_cfg.mPrecipitation = llclamp(state.mPrecipitationIntensity, 0.f, 1.f) * neighbor_fade;
    out_cfg.mTurbulence = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);

    // <SS:Nexii> The band tables ride along unfaded: they grade what one drop is like (its size, how hard it lands), not how many fall - the neighbor fade already thins the count through mPrecipitation.
    out_cfg.mDropletScale = llclamp(state.mDropletSizeScale, 0.f, 1.f);
    out_cfg.mImpactScale  = llclamp(state.mImpactScale, 0.f, 1.f);

    out_cfg.mTemperatureC = llclamp(track.mWeather.mTemperatureC.valueAt(phase), -60.f, 60.f);

    out_cfg.mLightningColor = state.mLightningColor;
    out_cfg.mLightningCoreWhite = state.mLightningCoreWhite;

    out_cfg.mLightning = state.mLightningEnabled;
    out_cfg.mLightningCharge = state.mLightningCharge;
    out_cfg.mLightningSparks = state.mLightningSparks;
    out_cfg.mLightningIntervalMin = state.mLightningIntervalMinSeconds;
    out_cfg.mLightningIntervalMax = state.mLightningIntervalMaxSeconds;
    out_cfg.mLightningIntensity = state.mLightningIntensity;

    out_cfg.mGustDepth = llclamp(state.mGustDepth, 0.f, 3.f);
    out_cfg.mGustLength = llclamp(state.mGustLength, 8.f, 2000.f);
    out_cfg.mGustVeer = llclamp(state.mGustVeer, 0.f, 90.f);

    out_cfg.setHeadingElevation(state.mWindHeading, 0.f);
    out_cfg.mWindSpeed = llmax(0.f, state.mWindSpeed);

    out_cfg.mHasGround = true;
    out_cfg.mGround = track.mFloorZ;

    out_cfg.mFallThrough = 1.f;

    return true;
}

// <SS:Nexii> The bolt-from-the-blue storm look-ahead. The weather cube is a forecast: its next convection keyframe is where the sky is heading, and when that is stormier than now - and the day phase has run most of the way toward it - a thunderstorm is on its way. How imminent (the approach, 0..1) scales how early and how often the lightning ahead of it arrives, and the storm comes FROM upwind: the weather travels with the wind, so its source is on the far side of where the wind is blowing. Moisture is the second opinion: a dry high-convection sky is turbulence, not a storm.
F32 SSAtmoEnvBridge::stormApproach(F32 world_z, F32 prev_world_z, bool teleported,
                                   F32& out_heading_deg)
{
    static LLCachedControl<bool> blue(gSavedSettings, "SSAtmoLightningBlue", true);
    out_heading_deg = -1.f;
    if (!blue) return 0.f;

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!mgr->hasAsset() || mgr->asset().mTracks.empty()) return 0.f;

    const SSAtmoEnvAsset& asset = mgr->asset();
    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(
        asset, world_z, prev_world_z, teleported);
    const SSAtmoEnvTrack& track = asset.mTracks[blend.mPrimaryTrack];

    const SSAtmoEnvWeather& weather = track.mWeather;
    if (!weather.mConvection.hasKeyframes()) return 0.f;

    const F64 phase = mgr->hasPreviewPhaseOverride() ? mgr->previewPhaseOverride()
                                                     : track.currentDayCyclePhase();

    // Storminess at a moment: convection is the thunder engine, moisture the moisture the charge
    // needs - a wet high-convection sky is a storm, the same convection dry is clear-air.
    auto stormy = [&](F64 p) -> F32
    {
        const F32 c = llclamp(weather.mConvection.valueAt(p), 0.f, 1.f);
        const F32 m = llclamp(weather.mMoisture.valueAt(p), 0.f, 1.f);
        return llclamp(llmax(c, m * APPROACH_STORM_C), 0.f, 1.f);
    };

    const F32 storm_now  = stormy(phase);
    const F32 storm_next = stormy(weather.mConvection.nextKeyframeTime(phase));
    const F32 rising = llclamp(storm_next - storm_now, 0.f, 1.f);
    if (rising < APPROACH_MIN_RISE) return 0.f;

    // How far the day phase has travelled between this keyframe and the next - the storm's
    // approach within its own segment, wrap-safe.
    const F64 prev_t = weather.mConvection.prevKeyframeTime(phase);
    F64 seg = weather.mConvection.nextKeyframeTime(phase) - prev_t;
    if (seg < 0.0) seg += 1.0;
    F64 pos = phase - prev_t;
    if (pos < 0.0) pos += 1.0;
    if (seg <= 0.01) return 0.f;

    const F64 along = pos / seg;
    if (along <= APPROACH_RAMP_FROM) return 0.f;

    const F32 imminent = cubic_step((F32)llclamp(
        (along - APPROACH_RAMP_FROM) / (APPROACH_RAMP_TO - APPROACH_RAMP_FROM), 0.0, 1.0));
    const F32 approach = imminent * rising;
    if (approach < APPROACH_MIN) return 0.f;

    // The storm comes from upwind: the heading the wind blows TOWARD, plus 180.
    out_heading_deg = fmodf(weather.mWindHeading.valueAt(phase) + 180.f, 360.f);
    return approach;
}
