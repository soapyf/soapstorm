/**
 * @file ssatmoenvcloudfieldstate.cpp
 * @brief See ssatmoenvcloudfieldstate.h.
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

#include "ssatmoenvcloudfieldstate.h"

#include "llviewercontrol.h"

namespace
{
    // <SS:Nexii> The seasonal rack: -15C is deep winter, +35C a summer heatwave - the storm deck's base rides between the two rails, and no deck ever grows past a kilometre of thickness. Winter air is dense and squashes the atmosphere down (a winter storm's base hangs low over the ground); summer heat lifts the whole sky. The base follows the temperature NONLINEARLY - a cubic centred on the neutral 10C midpoint that is FLAT in the middle and steepest at the rails: through ordinary spring/autumn weather (and a day's worth of temperature drift) the deck parks near its mid altitude, and only climbs to the summer high or sinks to the winter low as the temperature approaches the season's extremes. The extremes set the rack; the curve keeps everyday weather from dragging the deck around.
    const F32 DECK_BASE_MIN_C = -15.f;
    const F32 DECK_BASE_MAX_C = 35.f;
    const F32 DECK_BASE_WINTER_M = 400.f;
    const F32 DECK_BASE_SUMMER_M = 1200.f;
    const F32 DECK_MID_C = (DECK_BASE_MIN_C + DECK_BASE_MAX_C) * 0.5f;   // 10C
    const F32 DECK_HALF_C = (DECK_BASE_MAX_C - DECK_BASE_MIN_C) * 0.5f;  // 25C
    const F32 DECK_MID_M  = (DECK_BASE_WINTER_M + DECK_BASE_SUMMER_M) * 0.5f; // 800m
    const F32 DECK_LID_M = 1000.f;

    // <SS:Nexii> The gloom's wet gate: numerically the sky modulator's DARKENING_MOIST_MIN/FULL band (ssatmoenvskymodulator.cpp), kept in step by hand so the puffs char exactly when the dome's churn and the thunder say storm.
    const F32 GLOOM_MOIST_MIN  = 0.25f;
    const F32 GLOOM_MOIST_FULL = 0.60f;
}

// Derives the live cloud field (coverage, height, gloom, churn) from the authored tunables and the moisture/convection cube. The authored base height is an offset above the track's floor, so the
// floor rides along here - everything downstream of this resolver is world-frame.
SSAtmoEnvCloudFieldState SSAtmoEnvCloudFieldResolver::resolve(const SSAtmoEnvCloudField& field,
                                                              const SSAtmoEnvWeatherInfluence& influence,
                                                              F32 moisture, F32 convection,
                                                              F32 temperature_c, F64 phase,
                                                              F32 track_floor_z,
                                                              bool auto_owns_geometry)
{
    SSAtmoEnvCloudFieldState state;

    // <SS:Nexii> The seasonal altitude switch: on, the deck's base is temperature-driven - the summer/winter atmosphere rack - and the whole deck is capped at a kilometre of thickness. Off, the old moisture-and-convection base chain stands untouched. One read, shared by the derivation and the cap.
    static LLCachedControl<bool> season(gSavedSettings, "SSAtmoCloudSeason", true);
    const bool seasonal = season;

    F32 base_height, thickness, coverage_scale;
    F32 auto_darkening = -1.f;
    if (field.mAuto)
    {
        deriveAutoBaseline(moisture, convection, temperature_c, seasonal,
                           base_height, thickness, coverage_scale, auto_darkening);

        // The under deck keeps its authored altitude and depth - see the auto_owns_geometry note on the declaration - and takes only the weather half of Auto.
        if (!auto_owns_geometry)
        {
            base_height = field.mBaseHeightM.valueAt(phase);
            thickness   = field.mBaseThicknessM.valueAt(phase);
        }
    }
    else
    {
        base_height    = field.mBaseHeightM.valueAt(phase);
        thickness      = field.mBaseThicknessM.valueAt(phase);
        coverage_scale = field.mCoverageScale.valueAt(phase);
    }

    const F32 m = llclamp(moisture, 0.f, 1.f);
    const F32 dry = 1.f - m;
    state.mCoverage = (1.f - dry * dry * dry) * llmax(0.f, coverage_scale);

    const F32 height_factor = 1.f + llclamp(convection, 0.f, 1.f) * 4.f;
    state.mBaseHeightM = track_floor_z + base_height;
    state.mThicknessM = llmax(0.f, thickness) * height_factor;

    // <SS:Nexii> The storm lid: however fierce the convection, the deck is a 4km-region sky - a kilometre up is a towering cumulonimbus, and nothing grows past it. Applies to authored decks too: the cap is a property of this sky's size, not of one deck's tuning.
    if (seasonal)
    {
        state.mThicknessM = llmin(state.mThicknessM, DECK_LID_M);
    }

    state.mBaseTexture = field.mBaseTexture.valueAt(phase);
    state.mDetailTexture = field.mDetailTexture.valueAt(phase);
    state.mNoiseTexture = field.mNoiseTexture.valueAt(phase);
    state.mProfileTexture = field.mProfileTexture.valueAt(phase);

    // <SS:Nexii> Mid-fade the pair rides along; the partner starts parked on the current map so the renderer can bind it unconditionally. blendAt rewrites the current id with the fade's FROM keyframe - the same answer valueAt held.
    state.mBaseTextureNext = state.mBaseTexture;
    state.mBaseTextureBlend = 0.f;
    field.mBaseTexture.blendAt(phase, state.mBaseTexture, state.mBaseTextureNext, state.mBaseTextureBlend);

    state.mDetailTextureNext = state.mDetailTexture;
    state.mDetailTextureBlend = 0.f;
    field.mDetailTexture.blendAt(phase, state.mDetailTexture, state.mDetailTextureNext, state.mDetailTextureBlend);

    state.mTextureMix = llclamp(field.mTextureMix.valueAt(phase), 0.f, 1.f);
    state.mPuffDensity = llclamp(field.mPuffDensity.valueAt(phase), 0.f, 1.f);
    state.mDetailScale = llmax(0.01f, field.mDetailScale.valueAt(phase));
    state.mNoiseScale = llmax(0.05f, field.mNoiseScale.valueAt(phase));
    state.mDriftRate = llmax(0.f, field.mDriftRate.valueAt(phase));

    const F32 darkening = (auto_darkening >= 0.f)
        ? auto_darkening
        : llclamp(field.mStormDarkening.valueAt(phase), 0.f, 2.f);

    // <SS:Nexii> The puff gloom is the deck's half of "Storm Darkening" and rides the same influence row the dome's churn does: toggled off, the deck keeps its authored albedo whatever the weather. The wet gate holds the sky modulator's rule - a dry heatwave's thermals must not char fair-weather puffs - but it is the ONSET and no longer the whole story, because what makes a cloud dark is what it is carrying, and what it is carrying is water and depth.
    //
    // CONVECTION used to multiply this outright, and it appeared twice - once inside the auto darkening figure and once here - so a deck had to be BOTH soaked and violently rising before it lost a shade. That rules out the one sky most in need of it: a nimbostratus is a moisture regime, not a convective one, and the heaviest rain deck this system can build came out at gloom 1.00, the same albedo as a fair-weather cumulus. It survives as a modifier - a churning deck is deeper and more ragged than a still one - never as a gate.
    //
    // DEPTH is the other half of "its contents": the same water spread through 200 m of cloud and through a kilometre of it are not the same cloud, and the deck already knows its own thickness by here. The two together are the mass of water the light has to cross.
    const F32 wet = llclamp((m - GLOOM_MOIST_MIN) / (GLOOM_MOIST_FULL - GLOOM_MOIST_MIN), 0.f, 1.f);
    const F32 depth = 0.35f + 0.65f * llclamp(state.mThicknessM / DECK_LID_M, 0.f, 1.f);
    const F32 turmoil = 0.6f + 0.4f * llclamp(convection, 0.f, 1.f);
    const F32 gloom_gate = (influence.mEnabled && influence.mStormDarkeningEnabled)
        ? llclamp(influence.mStormDarkeningStrength, 0.f, 1.f) : 0.f;
    state.mGloom = llmax(0.03f, expf(-1.7f * darkening * wet * depth * turmoil * gloom_gate));

    state.mChurn = llclamp(convection, 0.f, 1.f);
    state.mHasAnvil = convection >= 0.75f;

    state.mAnvil = llclamp((convection - 0.6f) / 0.3f, 0.f, 1.f);

    return state;
}

// Auto mode: plausible base height, thickness and darkening straight from moisture and convection when nothing is authored. The base is an offset above the track's floor, not a world altitude -
// the ground track sits at zero, where the two coincide. With the seasonal altitude on (SSAtmoCloudSeason), the BASE is temperature-driven instead: winter air squashes the atmosphere, summer
// heat lifts it, and the base slides between the two rails while moisture and convection shape the thickness above it.
void SSAtmoEnvCloudFieldResolver::deriveAutoBaseline(F32 moisture, F32 convection,
                                                     F32 temperature_c, bool seasonal_altitude,
                                                     F32& out_base_height, F32& out_thickness,
                                                     F32& out_coverage_scale, F32& out_darkening)
{
    const F32 m = llclamp(moisture, 0.f, 1.f);
    const F32 c = llclamp(convection, 0.f, 1.f);

    // <SS:Nexii> Moisture-led, matching the gloom's own drive downstream: a cloud is dark because of what it holds, so the water gets the large coefficient and the convection the small one. Was 0.45 + 1.25c + 0.3mc - convection-led, and multiplied by convection AGAIN at the point of use, which left every stratiform rain deck at its fair-weather albedo. The extremes are held: soaked and violently convective still lands on 2.0, exactly where the old curve topped out.
    out_darkening = 0.45f + 1.05f * m + 0.5f * c;

    if (seasonal_altitude)
    {
        // The cubic: flat at the neutral middle (10C), steepest at the seasonal rails. The
        // deck parks near its mid altitude through ordinary weather - a spring day swinging
        // 8-18C moves it a few metres - and only climbs to the summer high or sinks to the
        // winter low as the temperature approaches the extremes.
        const F32 t = llclamp((temperature_c - DECK_MID_C) / DECK_HALF_C, -1.f, 1.f);
        out_base_height = DECK_MID_M + (DECK_BASE_SUMMER_M - DECK_MID_M) * t * t * t;
    }
    else
    {
        out_base_height = 1400.f - 700.f * m - 200.f * c;
    }

    out_thickness   = 150.f + 350.f * m;

    out_coverage_scale = 1.f;
}
