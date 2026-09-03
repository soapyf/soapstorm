/**
 * @file ssatmoenvweathergen.h
 * @brief Atmo Magic: rolls a day of weather into the cube.
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

#ifndef SS_ATMOENVWEATHERGEN_H
#define SS_ATMOENVWEATHERGEN_H

#include "ssatmoenvasset.h"

#include <string>

enum class SSAtmoEnvWeatherSeason
{
    SPRING = 0,
    SUMMER,
    AUTUMN,
    WINTER
};

// <SS:Nexii> What a roll turned out to be, so the editor can say it back. A generator the author
// presses repeatedly has to report what it just did or the button is a slot machine with the reels
// hidden: "Autumn - gale-force winds" tells you whether to keep rolling, where a changed set of
// slider positions does not.
struct SSAtmoEnvWeatherRoll
{
    SSAtmoEnvWeatherSeason mSeason = SSAtmoEnvWeatherSeason::SPRING;

    bool mFantasy = false;

    // The season's name, or the fantasy archetype's.
    std::string mTheme;

    // The extreme event layered on top, empty when the day is an ordinary one.
    std::string mEvent;

    // Theme, event and how many spells of precipitation, as one line for the editor.
    std::string mSummary;
};

// <SS:Nexii> Authors a whole day's weather cube - the keyframed moisture, convection, temperature,
// wind and precipitation switch - rather than a set of constants. Everything downstream (cloud
// cover, storm darkening, lightning cadence, what falls and how hard) already derives from those
// five curves through SSAtmoEnvWeatherResolver, so a generator that gets the CURVES right gets a
// whole day right for free and never has to know what a cloud deck is.
class SSAtmoEnvWeatherGenerator
{
public:
    // Replaces the cube wholesale with a fresh roll. Realistic four times in five; the rest are
    // fantasy archetypes that deliberately leave the envelope real weather stays inside.
    static SSAtmoEnvWeatherRoll randomize(SSAtmoEnvWeather& weather);

    // Back to a still, dry, clear sky - the cube's own constructed defaults, no keyframes.
    static void clear(SSAtmoEnvWeather& weather);
};

#endif
