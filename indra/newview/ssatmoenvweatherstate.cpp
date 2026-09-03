/**
 * @file ssatmoenvweatherstate.cpp
 * @brief See ssatmoenvweatherstate.h.
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

#include "ssatmoenvweatherstate.h"

#include "llviewercontrol.h"

namespace
{
    const F32 CLEAR_MOISTURE_THRESHOLD = 0.02f;

    // <SS:Nexii> The temperature band the lightning season fades over: the SAME season rack the cloud altitudes use (-15C deep winter, +35C summer heatwave). Full frequency only in the real heat - summer discharges freely - throttling to a twentieth by deep winter, when the rare storm still owns its few anvil bolts.
    const F32 LIGHTNING_WARM_C = 35.f;
    const F32 LIGHTNING_COLD_C = -15.f;
    const F32 LIGHTNING_FLOOR  = 0.05f;

    // <SS:Nexii> The wet gate's band: numerically the sky modulator's DARKENING_MOIST_MIN/FULL (ssatmoenvskymodulator.cpp), kept in step by hand so storm darkening and thunder agree about what a storm is.
    const F32 LIGHTNING_MOIST_MIN  = 0.25f;
    const F32 LIGHTNING_MOIST_FULL = 0.60f;

    // Lerp.
    F32 ss_flerp(F32 a, F32 b, F32 t) { return a + (b - a) * t; }

    // Only liquid rain types get the drizzle bands.
    bool isDrizzleCapable(const std::string& type)
    {
        return type == "rain" || type == "freezing_rain";
    }

    // Cloud cover in oktas straight from moisture; below the clear threshold reads 0.
    S32 oktaFromMoisture(F32 moisture)
    {
        if (moisture <= CLEAR_MOISTURE_THRESHOLD) return 0;
        return llclamp(ll_round(moisture * 8.f), 1, 8);
    }

    // Human sky wording per okta band.
    std::string skyTextForOkta(S32 okta)
    {
        if (okta <= 0) return "Clear sky";
        if (okta <= 2) return "Mostly clear";
        if (okta <= 4) return "Partly cloudy";
        if (okta <= 6) return "Cloudy";
        return "Overcast";
    }

    // First-letter capitalisation for forecast wording.
    std::string capitalized(std::string s)
    {
        if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
        return s;
    }
}

// Buckets convection into the four named phases the UI and the lightning defaults key on.
SSAtmoEnvWeatherState::EConvectionPhase SSAtmoEnvWeatherResolver::convectionPhase(F32 convection)
{
    if (convection <= 0.25f) return SSAtmoEnvWeatherState::STABLE;
    if (convection <= 0.55f) return SSAtmoEnvWeatherState::BREEZY;
    if (convection <= 0.75f) return SSAtmoEnvWeatherState::TURBULENT;
    return SSAtmoEnvWeatherState::SEVERE;
}

// Temperature picks the family, convection the severity: snow/blizzard below freezing through hail at extreme convection.
std::string SSAtmoEnvWeatherResolver::derivePrecipitationType(F32 convection, F32 temperature_c)
{
    if (temperature_c < -1.f)
    {
        return (convection > 0.7f) ? "blizzard" : "snow";
    }
    if (temperature_c <= 0.f)
    {
        return (convection > 0.5f) ? "freezing_rain" : "sleet";
    }
    if (temperature_c <= 1.5f)
    {
        return "slush_mix";
    }
    return (convection > 0.95f) ? "hail" : "rain";
}

// <SS:Nexii> Temperature's grip on lightning frequency. Summer heat lets convection discharge freely; winter cold throttles the network to the rare storm - "thundersnow" is a headline, not a Tuesday, and its few strikes are the powerful positive anvil bolts the lightning model favours there. The floor keeps that rare winter storm alive, never zero. With the polarity model off the answer is 1, so the old convection-only intervals stand untouched.
F32 SSAtmoEnvWeatherResolver::lightningTemperatureScale(F32 temperature_c)
{
    static LLCachedControl<bool> polarity(gSavedSettings, "SSAtmoLightningPolarity", true);
    if (!polarity) return 1.f;

    const F32 season = llclamp((temperature_c - LIGHTNING_COLD_C)
                               / (LIGHTNING_WARM_C - LIGHTNING_COLD_C), 0.f, 1.f);
    return LIGHTNING_FLOOR + (1.f - LIGHTNING_FLOOR) * season;
}

// The wet gate: 0 below the band's onset, 1 from its top. No polarity switch here - a dry sky has nothing to charge whichever bolt model runs.
F32 SSAtmoEnvWeatherResolver::lightningMoistureScale(F32 moisture)
{
    return llclamp((moisture - LIGHTNING_MOIST_MIN)
                   / (LIGHTNING_MOIST_FULL - LIGHTNING_MOIST_MIN), 0.f, 1.f);
}

// Moisture into named intensity bands, with the extra drizzle bands only for liquid types.
SSAtmoEnvPrecipIntensity SSAtmoEnvWeatherResolver::classifyIntensity(F32 moisture, const std::string& type)
{
    if (moisture <= CLEAR_MOISTURE_THRESHOLD) return SSAtmoEnvPrecipIntensity::NONE;

    if (isDrizzleCapable(type))
    {
        if (moisture <= 0.06f) return SSAtmoEnvPrecipIntensity::DRIZZLE_LIGHT;
        if (moisture <= 0.10f) return SSAtmoEnvPrecipIntensity::DRIZZLE;
        if (moisture <= 0.15f) return SSAtmoEnvPrecipIntensity::DRIZZLE_HEAVY;
        if (moisture <= 0.35f) return SSAtmoEnvPrecipIntensity::LIGHT;
        if (moisture <= 0.65f) return SSAtmoEnvPrecipIntensity::MODERATE;
        if (moisture <= 0.85f) return SSAtmoEnvPrecipIntensity::HEAVY;
        return SSAtmoEnvPrecipIntensity::TORRENTIAL;
    }

    if (moisture <= 0.35f) return SSAtmoEnvPrecipIntensity::LIGHT;
    if (moisture <= 0.65f) return SSAtmoEnvPrecipIntensity::MODERATE;
    if (moisture <= 0.85f) return SSAtmoEnvPrecipIntensity::HEAVY;
    return SSAtmoEnvPrecipIntensity::TORRENTIAL;
}

// Forecast wording for a type at an intensity band.
std::string SSAtmoEnvWeatherResolver::intensityLabel(const std::string& type, SSAtmoEnvPrecipIntensity band)
{
    using I = SSAtmoEnvPrecipIntensity;
    if (band == I::NONE) return std::string();

    if (type == "rain")
    {
        switch (band)
        {
            case I::DRIZZLE_LIGHT: return "light drizzle";
            case I::DRIZZLE:       return "drizzle";
            case I::DRIZZLE_HEAVY: return "heavy drizzle";
            case I::LIGHT:         return "light rain";
            case I::MODERATE:      return "rain";
            case I::HEAVY:         return "heavy rain";
            case I::TORRENTIAL:    return "torrential rain";
            default: break;
        }
    }
    else if (type == "freezing_rain")
    {
        switch (band)
        {
            case I::DRIZZLE_LIGHT: return "light freezing drizzle";
            case I::DRIZZLE:       return "freezing drizzle";
            case I::DRIZZLE_HEAVY: return "heavy freezing drizzle";
            case I::LIGHT:         return "light freezing rain";
            case I::MODERATE:      return "freezing rain";
            case I::HEAVY:         return "heavy freezing rain";
            case I::TORRENTIAL:    return "severe freezing rain";
            default: break;
        }
    }
    else if (type == "snow")
    {
        switch (band)
        {
            case I::LIGHT:      return "light snow";
            case I::MODERATE:   return "snow";
            case I::HEAVY:      return "heavy snow";
            case I::TORRENTIAL: return "intense snowfall";
            default: return "snow";
        }
    }
    else if (type == "hail")
    {
        switch (band)
        {
            case I::LIGHT:      return "light hail";
            case I::MODERATE:   return "hail";
            case I::HEAVY:      return "heavy hail";
            case I::TORRENTIAL: return "severe hailstorm";
            default: return "hail";
        }
    }
    else if (type == "sleet")
    {
        switch (band)
        {
            case I::LIGHT:      return "light sleet";
            case I::MODERATE:   return "sleet";
            default:            return "heavy sleet";
        }
    }
    else if (type == "slush_mix")
    {
        return (band == I::LIGHT) ? "light wintry mix" : "wintry mix";
    }
    else if (type == "blizzard")
    {
        return "blizzard conditions";
    }

    return type;
}

// One human sentence for the HUD: precipitation (or sky cover) plus wind strength.
std::string SSAtmoEnvWeatherResolver::generateForecastText(const SSAtmoEnvWeatherState& state)
{
    std::string precip;
    // <SS:Nexii> Sky-cover wording whenever nothing is actually falling, resolved state rather than raw moisture: a suppressed sky is wet enough to read SEVERE and used to announce "Thundery showers" over a street where not a drop landed.
    if (state.mPrecipitationType.empty() || state.mIntensityBand == SSAtmoEnvPrecipIntensity::NONE)
    {
        precip = skyTextForOkta(state.mOktaCloudCover);
    }
    else if (state.mConvectionPhase == SSAtmoEnvWeatherState::SEVERE)
    {
        if (state.mPrecipitationType == "hail")
        {
            precip = "Thundery hail";
        }
        else
        {
            const bool snowy = (state.mPrecipitationType == "snow" || state.mPrecipitationType == "blizzard");
            precip = snowy ? "Thundersnow" : "Thundery showers";
        }
    }
    else
    {
        precip = capitalized(intensityLabel(state.mPrecipitationType, state.mIntensityBand));
    }

    std::string wind;
    if (state.mWindSpeed < 1.f)       wind = "still air";
    else if (state.mWindSpeed < 3.f)  wind = "light winds";
    else if (state.mWindSpeed < 7.f)  wind = "a gentle breeze";
    else if (state.mWindSpeed < 12.f) wind = "brisk winds";
    else if (state.mWindSpeed < 20.f) wind = "strong winds";
    else                              wind = "gale-force winds";

    return precip + " and " + wind;
}

// The whole weather cube for one phase: type, intensity, gusts, lightning behaviour and forecast text from moisture/convection/temperature.
SSAtmoEnvWeatherState SSAtmoEnvWeatherResolver::resolve(const SSAtmoEnvWeather& weather, F64 phase)
{
    SSAtmoEnvWeatherState state;

    const F32 moisture    = weather.mMoisture.valueAt(phase);
    const F32 convection  = weather.mConvection.valueAt(phase);
    const F32 temperature = weather.mTemperatureC.valueAt(phase);

    state.mConvectionPhase = convectionPhase(convection);
    state.mWindHeading = weather.mWindHeading.valueAt(phase);
    state.mWindSpeed   = weather.mWindSpeed.valueAt(phase);

    state.mOktaCloudCover = oktaFromMoisture(moisture);

    // <SS:Nexii> The author's switch, read before the moisture test and folded into the same clear branch: moisture alone used to decide, so an overcast stormy sky could not be dry. Off suppresses precipitation and nothing else - the okta cover above is already banked, and the gusts, gloom and lightning below never look at this flag. [interaction: precipitation]
    state.mPrecipitationFalls = weather.mPrecipitationFalls.valueAt(phase);

    if (!state.mPrecipitationFalls || moisture <= CLEAR_MOISTURE_THRESHOLD)
    {
        state.mPrecipitationType = std::string();
        state.mPrecipitationIntensity = 0.f;
        state.mIntensityBand = SSAtmoEnvPrecipIntensity::NONE;
        state.mDropletSizeScale = 0.f;
        state.mImpactScale = 0.f;
    }
    else
    {
        const std::string override_type = weather.mPrecipitationOverride.valueAt(phase);
        state.mPrecipitationType = !override_type.empty()
            ? override_type
            : derivePrecipitationType(convection, temperature);
        state.mPrecipitationIntensity = moisture;
        state.mIntensityBand = classifyIntensity(moisture, state.mPrecipitationType);

        switch (state.mIntensityBand)
        {
            using I = SSAtmoEnvPrecipIntensity;
            case I::DRIZZLE_LIGHT: state.mDropletSizeScale = 0.05f; state.mImpactScale = 0.00f; break;
            case I::DRIZZLE:       state.mDropletSizeScale = 0.12f; state.mImpactScale = 0.00f; break;
            case I::DRIZZLE_HEAVY: state.mDropletSizeScale = 0.20f; state.mImpactScale = 0.05f; break;
            case I::LIGHT:         state.mDropletSizeScale = 0.35f; state.mImpactScale = 0.25f; break;
            case I::MODERATE:      state.mDropletSizeScale = 0.55f; state.mImpactScale = 0.50f; break;
            case I::HEAVY:         state.mDropletSizeScale = 0.75f; state.mImpactScale = 0.80f; break;
            case I::TORRENTIAL:    state.mDropletSizeScale = 1.00f; state.mImpactScale = 1.00f; break;
            default:                state.mDropletSizeScale = 0.f;   state.mImpactScale = 0.f;   break;
        }
    }

    if (weather.mGustAuto)
    {
        state.mGustDepth  = convection;
        state.mGustLength = ss_flerp(220.f, 80.f, convection);
        state.mGustVeer   = ss_flerp(4.f, 35.f, convection);
    }
    else
    {
        state.mGustDepth  = weather.mGustDepth.valueAt(phase);
        state.mGustLength = weather.mGustLength.valueAt(phase);
        state.mGustVeer   = weather.mGustVeer.valueAt(phase);
    }

    state.mLightningColor = weather.mLightningColor.valueAt(phase);
    state.mLightningCoreWhite = llclamp(weather.mLightningCoreWhite.valueAt(phase), 0.f, 1.f);

    state.mLightningEnabled = weather.mLightningEnabled;
    state.mLightningCharge  = weather.mLightningCharge;
    state.mLightningSparks  = weather.mLightningSparks;

    // <SS:Nexii> Cold stretches the intervals: summer convection discharges every few seconds, the same convection under a winter sky a strike a minute at most. The scale divides into the base intervals so "rare in winter, common in summer" holds at every convection; the winter storm's own rare strikes are the powerful positive anvil bolts the model favours there.
    const F32 season = llmax(lightningTemperatureScale(temperature), 0.02f);

    if (weather.mLightningAuto)
    {
        // <SS:Nexii> The wet gate: convection is the engine but the charge needs cloud. Moisture below the band means NO strikes however severe the convection - the dry heatwave whose thermals used to thunder out of a clear blue okta-0 sky - and a marginally wet sky strikes rarely, the wet scale dividing into the base intervals exactly as the winter season does.
        const F32 wet = lightningMoistureScale(moisture);
        if (wet <= 0.f)
        {
            state.mLightningIntervalMinSeconds = 0.f;
            state.mLightningIntervalMaxSeconds = 0.f;
            state.mLightningIntensity = 0.f;
        }
        else
        {
            const F32 scale = season * wet;
            switch (state.mConvectionPhase)
            {
                case SSAtmoEnvWeatherState::TURBULENT:
                    state.mLightningIntervalMinSeconds = 30.f / scale;
                    state.mLightningIntervalMaxSeconds = 60.f / scale;
                    state.mLightningIntensity = convection;
                    break;
                case SSAtmoEnvWeatherState::SEVERE:
                    state.mLightningIntervalMinSeconds = 2.f / scale;
                    state.mLightningIntervalMaxSeconds = 5.f / scale;
                    state.mLightningIntensity = convection;
                    break;
                default:
                    state.mLightningIntervalMinSeconds = 0.f;
                    state.mLightningIntervalMaxSeconds = 0.f;
                    state.mLightningIntensity = 0.f;
                    break;
            }
        }
    }
    else
    {
        // <SS:Nexii> Manual mode: the authored intensity IS the storm. It used to only set fierceness while the intervals still came off the convection phase, so a keyframed intensity under a calm sky never fired a single bolt - dead authoring. Now intensity maps straight to cadence (a whisper of it strikes about once a minute, full intensity matches SEVERE's 2-5s) and only the season still stretches it; no wet gate either - an override is an order, the same call mPrecipitationOverride makes.
        state.mLightningIntensity = weather.mLightningIntensity.valueAt(phase);
        const F32 authored = llclamp(state.mLightningIntensity, 0.f, 1.f);
        if (authored > 0.02f)
        {
            state.mLightningIntervalMinSeconds = ss_flerp(45.f, 2.f, authored) / season;
            state.mLightningIntervalMaxSeconds = ss_flerp(90.f, 5.f, authored) / season;
        }
    }

    state.mForecastText = generateForecastText(state);
    return state;
}
