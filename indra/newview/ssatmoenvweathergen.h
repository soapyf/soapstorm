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
    //
    // <SS:Nexii> severeDay/severeDayStrength: authoring-time-only bias (SSSquall::severeDayBias, the core's own
    // pure function) toward a severe roll - raises the rolled convection/moisture/shear peaks the seasonal path's
    // spell(s) carry, over a window centred on the spell's own peak phase, rather than touching the whole day.
    // Ignored on the fantasy path (rolled 1 time in 5): every fantasy archetype is already outside the envelope
    // this bias reaches toward, and layering it on would only sand its edges rather than sharpen anything. No
    // effect on a dry roll (no spell to bias) or when strength is 0.
    //
    // <SS:Nexii> field/dome: the SAME track's cloud deck and cirrus dome, because the show the severe path authors
    // is a sky and not only a cube. Deck coverage is (1 - dry^3) * mCoverageScale (ssatmoenvcloudfieldstate.cpp),
    // so at the dry baseline moisture a roll leaves half the sky covered and there is no clear sky for the wall to
    // arrive into: the scale, the deck's depth and the dome's own coverage are the three curves that clear it, and
    // none of them lives in the cube. Touched ONLY inside the severe event's rebuild window (see the block in the
    // .cpp) - a roll with no authored cue leaves both structs exactly as it found them, and so does clear().
    // NOTE the deck's own Auto switch still gates the two deck curves downstream: SSAtmoEnvCloudFieldResolver reads
    // mCoverageScale and mBaseThicknessM only when mAuto is off (it derives coverage_scale 1.0 otherwise), and this
    // does not flip that switch - flipping it would hand the deck's altitude and darkening to the authored rows for
    // the whole day, which is not this function's to decide.
    //
    // <SS:Nexii> influence: the track's own SSAtmoEnvWeatherInfluence (V2 legend fix). Randomize Severe Day IS the
    // author opting the track into severe weather, so on a severe roll this turns the influence master and both
    // Allow flags on (mEnabled, mAllowSupercells, mAllowTornadoes - strengths untouched); an ordinary (non-severe)
    // roll leaves it exactly as found, same as clear(). Without this, a Severe Day roll authored a cue and a
    // floor for the pinned cell but left the track's Allow flags off, so SSStormCells::birthMemo's
    // influence_enabled gate (ssstormcells.cpp) always denied a background supercell or a spontaneous hero on a
    // Severe Day track.
    static SSAtmoEnvWeatherRoll randomize(SSAtmoEnvWeather& weather,
                                          SSAtmoEnvCloudField& field,
                                          SSAtmoEnvCloudDome& dome,
                                          bool severeDay, F32 severeDayStrength,
                                          SSAtmoEnvWeatherInfluence& influence);

    // Back to a still, dry, clear sky - the cube's own constructed defaults, no keyframes.
    static void clear(SSAtmoEnvWeather& weather);
};

#endif
