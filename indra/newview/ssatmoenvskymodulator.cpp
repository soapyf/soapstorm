/**
 * @file ssatmoenvskymodulator.cpp
 * @brief See ssatmoenvskymodulator.h.
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

#include "ssatmoenvskymodulator.h"

#include <cmath>

namespace
{

    const F32 SCROLL_FULL_WIND_MS = 25.f;

    const F32 CHURN_FULL_ADD = 0.5f;

    const F32 WATERFOG_FULL_BOOST = 0.5f;

    const F32 DARKENING_ONSET = 0.55f;
    const F32 DARKENING_MOIST_MIN  = 0.25f;
    const F32 DARKENING_MOIST_FULL = 0.60f;

    const F32 BLUE_FULL_BOOST     = 0.20f;
    const F32 RED_FULL_CUT        = 0.10f;
    const F32 COLD_FULL_BELOW_C   = 15.f;
    const F32 COLD_CLEAR_MOISTURE = 0.5f;

    const F32 RAINBOW_WINDOW_S      = 240.f;
    const F32 RAINBOW_MIN_MOISTURE  = 0.3f;
    const F32 RAINBOW_FULL_MOISTURE = 0.75f;

    const F32 RAINBOW_SUN_FULL_DEG = 42.f;
    const F32 RAINBOW_SUN_GONE_DEG = 60.f;

    // <SS:Nexii> Water-drop optics (corona). Liquid droplets must hang in the air along the sight line - mist, droplets lingering in the rain's wake, or light drizzle - and they freeze out below about -4C. Heavy precipitation hides the disc itself, so it suppresses the ring.
    const F32 CORONA_MIST_MIN       = 0.30f;
    const F32 CORONA_MIST_FULL      = 0.75f;
    const F32 CORONA_AFTER_RAIN_S   = 240.f;
    const F32 CORONA_DRIZZLE_AT     = 0.35f;
    const F32 CORONA_HIDE_MIN       = 0.5f;
    const F32 CORONA_FREEZE_C       = -4.f;
    const F32 CORONA_MELT_C         = 1.f;

    // <SS:Nexii> Ice-crystal optics (halos). Crystals need BOTH sub-zero air and moisture - nothing to freeze in a dry -30C sky drives NO ice. Small platelets form a thin cirrus veil (22° family); deep cold plus a little convection lofts large plates and columns (46° family); still air lets plates settle horizontal, making sundogs and the circumzenithal arc sharp instead of smeared.
    const F32 ICE_MOIST_MIN         = 0.05f;
    const F32 ICE_MOIST_FULL        = 0.45f;
    const F32 ICE_FROST_FIRST_C     = 0.f;
    const F32 ICE_FROST_FULL_C      = -20.f;
    const F32 ICE_DEEP_FULL_C       = -38.f;
    const F32 ICE_CONVECTION_MIN    = 0.05f;
    const F32 ICE_CONVECTION_FULL   = 0.30f;
    const F32 ICE_ALIGN_CALM_MS     = 2.f;
    const F32 ICE_ALIGN_FULL_MS     = 7.f;
    const F32 ICE_LEVEL_FULL_ADD    = 0.35f;

    // Linear ramp of value across [lo, hi] to 0..1.
    F32 ss_ramp(F32 value, F32 lo, F32 hi)
    {
        if (hi <= lo) return (value >= hi) ? 1.f : 0.f;
        return llclamp((value - lo) / (hi - lo), 0.f, 1.f);
    }

    // A drive gated by its influence toggle and scaled by its strength.
    F32 ss_effect(F32 drive, bool enabled, F32 strength)
    {
        if (!enabled) return 0.f;
        return llclamp(drive, 0.f, 1.f) * llclamp(strength, 0.f, 1.f);
    }
}

// Turns the weather cube plus the author's influence settings into this frame's set of sky transforms.
SSAtmoEnvSkyModulation SSAtmoEnvSkyWeatherModulator::compute(const SSAtmoEnvSkyWeatherInput& in,
                                                             const SSAtmoEnvWeatherInfluence& influence)
{
    SSAtmoEnvSkyModulation mod;
    if (!influence.mEnabled) return mod;

    // <SS:Nexii> The dome's overcast band is its own layer again. It used to track the main deck's live coverage (the same number the puffs render with - one coverage, two layers), but that wired moisture into the sky dome: whatever lifted the deck's coverage (its moisture floor, its convection consolidation) lifted the dome's overcast with it, and the band no longer sat separately with the authored deck. Tracking is removed for now - the dome draws at its authored coverage alone, and the two layers only match when the author says so. mCoverTarget/mCoverBlend stay on the modulation struct (frozen at 0) so cloudCoverage is identity until the coupling is reworked around the horizon and colour issues that drove it off.
    mod.mCoverTarget = 0.f;
    mod.mCoverBlend = 0.f;

    {
        const F32 blend = ss_effect(1.f, influence.mWindScrollEnabled,
                                    influence.mWindScrollStrength);

        mod.mWind = ss_effect(ss_ramp(in.mWindSpeedMS, 0.f, SCROLL_FULL_WIND_MS),
                              influence.mWindScrollEnabled, influence.mWindScrollStrength);

        const F32 heading_rad = in.mWindHeadingDeg * DEG_TO_RAD;
        mod.mDriftVelocity = LLVector2(sinf(heading_rad), cosf(heading_rad))
            * (in.mWindSpeedMS * blend);
    }

    // <SS:Nexii> Precipitation -> water fog. The one atmosphere-adjacent mapping left, and it reaches the Water tab's fog, not the sky: moisture's haze mapping (haze density up, distance multiplier down) is retired - moisture's +1.5 haze drove the fog term's airlight past what custom skies with heavy haze_horizon/glow could hold, blowing the scene out under dynamic exposure, and muggy-by-numbers was never worth that. The authored haze density and distance multiplier now render exactly as keyframed; rain still thickens the underwater fog on its own toggle.
    mod.mPrecip = ss_effect(in.mPrecipitationIntensity, influence.mWaterFogEnabled, influence.mWaterFogStrength);

    // <SS:Nexii> Storm churn needs a wet sky - convection alone is clear-air turbulence, and a dry heatwave's thermals must not move the dome band. The storm's old scene darkening (gamma/ambient cuts off this same drive) is retired: it was global and darkened the sky above the deck too - the deck's ground shadow (moisture-driven through coverage) and the authored cloud_shadow own the scene darkening now.
    mod.mDarkening = ss_effect(ss_ramp(in.mConvection, DARKENING_ONSET, 1.f)
                             * ss_ramp(in.mMoisture, DARKENING_MOIST_MIN, DARKENING_MOIST_FULL),
                               influence.mStormDarkeningEnabled, influence.mStormDarkeningStrength);

    {
        const F32 heading_rad = in.mWindHeadingDeg * DEG_TO_RAD;
        LLVector2 along(sinf(heading_rad), cosf(heading_rad));
        if (in.mWindSpeedMS < 0.05f) along = LLVector2(1.f, 0.f);
        mod.setChurn(along);
    }

    {
        const F32 cold  = ss_ramp(-in.mTemperatureC, 0.f, COLD_FULL_BELOW_C);
        const F32 clear = 1.f - ss_ramp(in.mMoisture, 0.f, COLD_CLEAR_MOISTURE);
        mod.mCold = ss_effect(cold * clear, influence.mColdSkyEnabled, influence.mColdSkyStrength);
    }

    // <SS:Nexii> Corona: diffraction rings around the light from liquid water drops in the air. Mist (moisture) is the steady source; droplets lingering in the rain's wake give the same ring for a few minutes, and light drizzle works too. Heavy fall washes the disc out behind the drops, and sub-freezing air freezes the droplets out entirely - what forms the crystal halos below is NOT what forms this.
    {
        const F32 mist       = ss_ramp(in.mMoisture, CORONA_MIST_MIN, CORONA_MIST_FULL);
        F32 lingering        = 0.f;
        if (in.mSecondsSinceRainStopped >= 0.f)
        {
            lingering = 1.f - ss_ramp(in.mSecondsSinceRainStopped, 0.f, CORONA_AFTER_RAIN_S);
        }
        const F32 drizzle    = ss_ramp(in.mPrecipitationIntensity, 0.f, CORONA_DRIZZLE_AT);
        const F32 uncovered  = 1.f - ss_ramp(in.mPrecipitationIntensity, CORONA_HIDE_MIN, 1.f);
        const F32 liquid     = ss_ramp(in.mTemperatureC, CORONA_FREEZE_C, CORONA_MELT_C);

        F32 drops = llmax(mist, llmax(lingering * 0.7f, drizzle * 0.6f));
        drops *= uncovered * liquid;

        mod.mCorona = ss_effect(drops, influence.mCoronaEnabled, influence.mCoronaStrength);
    }

    // <SS:Nexii> Ice-crystal optics. The base veil of small platelets (22° halo family) wants cold AND moisture - frost ramps from freezing down to -20C, nothing above freezing - and the deep-cold 46° family additionally wants a trace of convection to loft big plates/columns; plate ALIGNMENT for sundogs and the circumzenithal arc wants the still, settling air the wind row churns away. All three sub-channels ride the single ice halo influence so authors flip "halos on" as one decision.
    {
        const F32 frost    = ss_ramp(-in.mTemperatureC, -ICE_FROST_FIRST_C, -ICE_FROST_FULL_C);
        const F32 vapour   = ss_ramp(in.mMoisture, ICE_MOIST_MIN, ICE_MOIST_FULL);
        const F32 crystals = frost * vapour;

        const F32 deep     = ss_ramp(-in.mTemperatureC, -ICE_FROST_FULL_C, -ICE_DEEP_FULL_C);
        const F32 loft     = ss_ramp(in.mConvection, ICE_CONVECTION_MIN, ICE_CONVECTION_FULL);
        const F32 large    = crystals * deep * loft;

        const F32 calm     = 1.f - ss_ramp(in.mWindSpeedMS, ICE_ALIGN_CALM_MS, ICE_ALIGN_FULL_MS);
        const F32 aligned  = crystals * calm;

        mod.mIceHalo      = ss_effect(crystals, influence.mIceHaloEnabled, influence.mIceHaloStrength);
        mod.mIceHalo46    = ss_effect(large, influence.mIceHaloEnabled, influence.mIceHaloStrength);
        mod.mCrystalAlign = ss_effect(aligned, influence.mIceHaloEnabled, influence.mIceHaloStrength);
    }

    if (in.mSecondsSinceRainStopped >= 0.f && in.mSunElevationSin > 0.f)
    {
        const F32 decay = 1.f - ss_ramp(in.mSecondsSinceRainStopped, 0.f, RAINBOW_WINDOW_S);
        const F32 wet   = ss_ramp(in.mMoisture, RAINBOW_MIN_MOISTURE * 0.5f, RAINBOW_MIN_MOISTURE);
        const F32 elevation_deg = RAD_TO_DEG * asinf(llclamp(in.mSunElevationSin, -1.f, 1.f));
        const F32 low_sun = 1.f - ss_ramp(elevation_deg, RAINBOW_SUN_FULL_DEG, RAINBOW_SUN_GONE_DEG);

        mod.mRainbow = ss_effect(decay * wet * low_sun,
                                 influence.mRainbowEnabled, influence.mRainbowStrength);
    }

    return mod;
}

// Blends authored coverage toward the modulation's target. Frozen at 0 while the dome band sits apart from the deck again (see compute), so this passes authored coverage through untouched - the dome draws exactly as authored. When the coupling is reworked, the old "weather can pile cloud on, but an authored overcast stays overcast" lift is the shape to bring back.
F32 SSAtmoEnvSkyModulation::cloudCoverage(F32 base) const
{
    return base + (llmax(base, mCoverTarget) - base) * mCoverBlend;
}

// Authored scroll plus the churn delta.
LLVector2 SSAtmoEnvSkyModulation::cloudScrollRate(const LLVector2& base) const
{
    return base + mScrollDelta;
}

// Storm churn: extra scroll along the wind, scaled by darkening.
void SSAtmoEnvSkyModulation::setChurn(const LLVector2& along)
{
    mScrollDelta = along * (mDarkening * CHURN_FULL_ADD);
}

// Cold clear sky shifts blue density up and red down.
LLColor3 SSAtmoEnvSkyModulation::blueDensity(const LLColor3& base) const
{
    if (mCold <= 0.f) return base;

    LLColor3 out = base;
    out.mV[0] *= (1.f - mCold * RED_FULL_CUT);
    out.mV[2] *= (1.f + mCold * BLUE_FULL_BOOST);
    return out;
}

// <SS:Nexii> Sky ice mirrors the moisture-gated crystal drive - from the ice halo mapping, never cold alone. A clear dry -30C sky has no crystals to form, so it renders exactly as authored instead of frosting over.
F32 SSAtmoEnvSkyModulation::skyIceLevel(F32 base) const
{
    return llclamp(base + mIceHalo * ICE_LEVEL_FULL_ADD, 0.f, 1.f);
}

// A live rainbow raises sky moisture toward its full value.
F32 SSAtmoEnvSkyModulation::skyMoistureLevel(F32 base) const
{
    return llmax(base, base + (RAINBOW_FULL_MOISTURE - base) * mRainbow);
}

// Precipitation thickens water fog.
F32 SSAtmoEnvSkyModulation::waterFogModifier(F32 base) const
{
    return base * (1.f + mPrecip * WATERFOG_FULL_BOOST);
}
