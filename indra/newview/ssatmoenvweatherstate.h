/**
 * @file ssatmoenvweatherstate.h
 * @brief Atmo Magic: weather cube resolver.
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

#ifndef SS_ATMOENVWEATHERSTATE_H
#define SS_ATMOENVWEATHERSTATE_H

#include "ssatmoenvasset.h"

#include <string>

enum class SSAtmoEnvPrecipIntensity
{
    NONE = 0,
    DRIZZLE_LIGHT,
    DRIZZLE,
    DRIZZLE_HEAVY,
    LIGHT,
    MODERATE,
    HEAVY,
    TORRENTIAL
};

struct SSAtmoEnvWeatherState
{
    // <SS:Nexii> The author's precipitation switch as resolved for this instant. Type and intensity already read clear when it is off, so nothing downstream has to consult it; it is here so the editor can say "Off" where it would otherwise say "Clear", which are different authoring states. [interaction: precipitation]
    bool mPrecipitationFalls = true;

    std::string mPrecipitationType;
    F32 mPrecipitationIntensity = 0.f;
    SSAtmoEnvPrecipIntensity mIntensityBand = SSAtmoEnvPrecipIntensity::NONE;

    F32 mDropletSizeScale = 0.f;

    F32 mImpactScale = 0.f;

    S32 mOktaCloudCover = 0;

    enum EConvectionPhase
    {
        STABLE = 0,
        BREEZY = 1,
        TURBULENT = 2,
        SEVERE = 3
    };
    EConvectionPhase mConvectionPhase = STABLE;

    F32 mWindHeading = 0.f;
    F32 mWindSpeed   = 0.f;

    F32 mGustDepth  = 0.f;
    F32 mGustLength = 140.f;
    F32 mGustVeer   = 0.f;

    F32 mLightningIntervalMinSeconds = 0.f;
    F32 mLightningIntervalMaxSeconds = 0.f;
    F32 mLightningIntensity = 0.f;

    LLColor3 mLightningColor{0.62f, 0.55f, 1.f};
    F32 mLightningCoreWhite = 0.85f;

    bool mLightningEnabled = true;
    bool mLightningCharge = true;
    bool mLightningSparks = true;

    std::string mForecastText;
};

class SSAtmoEnvWeatherResolver
{
public:
    static SSAtmoEnvWeatherState resolve(const SSAtmoEnvWeather& weather, F64 phase);

    static SSAtmoEnvWeatherState::EConvectionPhase convectionPhase(F32 convection);

    static std::string derivePrecipitationType(F32 convection, F32 temperature_c);

    static SSAtmoEnvPrecipIntensity classifyIntensity(F32 moisture, const std::string& type);

    static std::string intensityLabel(const std::string& type, SSAtmoEnvPrecipIntensity band);

    // <SS:Nexii> Temperature's grip on lightning frequency: the same season rack the cloud altitudes use - full frequency in the summer heatwave (+35C), throttled to a twentieth by deep winter (-15C, never zero - the winter storm still owns a few anvil bolts). Off with SSAtmoLightningPolarity, the answer is 1 and the old convection-only intervals stand.
    static F32 lightningTemperatureScale(F32 temperature_c);

    // <SS:Nexii> Moisture's grip on lightning: charge separation needs cloud, so the wet gate rides the SAME 0.25-0.60 moisture band the sky modulator's storm darkening does - 0 below the onset (a dry high-convection sky is clear-air turbulence and fires nothing), ramping to 1, dividing into the base intervals like the winter season does. The sky that flattens into a storm is the sky that thunders.
    static F32 lightningMoistureScale(F32 moisture);

private:
    static std::string generateForecastText(const SSAtmoEnvWeatherState& state);
};

#endif
