/**
 * @file ssatmoenvweathergen.cpp
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

#include "llviewerprecompiledheaders.h"

#include "ssatmoenvweathergen.h"

#include "llrand.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace
{
    // <SS:Nexii> Spells are confined to this window rather than allowed to wrap midnight. A spell
    // is four consecutive stretches of time (lead, fall, ease, clear) and wrapping any one of them
    // past phase 1 turns simple arithmetic into modular arithmetic in five places at once. The cost
    // is that no rolled storm runs through midnight; the author can drag one there in a few seconds
    // if they want it, and every other property of the roll survives being dragged.
    const F64 SPELL_WINDOW_START = 0.05;
    const F64 SPELL_WINDOW_END   = 0.90;

    // <SS:Nexii> Every generated key lands on the preview scrubber's grid, through the same
    // ss_atmoenv_snap_phase the sky seeding snaps its measured phases with rather than a local
    // rounding: the grid is 1/SS_ATMOENV_PREVIEW_STEPS and belongs to that constant, not to a
    // hundredth written out here that would quietly stop agreeing with it.
    //
    // The head moves in those steps and nowhere else, and hasKeyframeAt() matches within a tenth
    // of one, so a key at 0.3174 is a key nothing can visit: present in the curve, but the diamond
    // never lights, the prev/next jumps land beside it, and removing it means reaching a mark the
    // scrubber cannot stand on. Snapping in the two lay functions below - the one place every key
    // the generator writes passes through - makes the grid the only rounding there is, which is
    // also what leaves exact duplicates as the only collision left to drop.

    // The diurnal temperature clock: coldest just before dawn, warmest mid-afternoon. Phases
    // rather than hours, because a track's day length is the author's business, not ours.
    const F64 TEMP_TROUGH_PHASE = 0.22;
    const F64 TEMP_PEAK_PHASE   = 0.60;

    // Uniform in [lo, hi].
    F32 rollF(F32 lo, F32 hi)
    {
        if (hi <= lo) return lo;
        return lo + ll_frand(hi - lo);
    }

    // Uniform in [lo, hi], phase-width.
    F64 rollP(F64 lo, F64 hi)
    {
        if (hi <= lo) return lo;
        return lo + (F64)ll_frand((F32)(hi - lo));
    }

    // True with the given probability.
    bool rollChance(F32 probability)
    {
        return ll_frand() < probability;
    }

    // <SS:Nexii> One episode of precipitation, and the whole reason this is a generator rather than
    // a scatter of random keyframes: real weather ARRIVES. The deck thickens over the lead, the
    // rain starts, it eases off, and the sky takes a while to clear again afterwards - four
    // separate stretches of time that "moisture goes up here" collapses into one. The lead is the
    // load-bearing one: it is what puts an overcast sky over the region BEFORE the first drop, and
    // without it the sky snaps from blue to raining in a single keyframe.
    struct Spell
    {
        F64 mStart = 0.30;
        F64 mDuration = 0.10;
        F64 mLead = 0.08;
        F64 mTail = 0.10;
        F32 mPeakMoisture = 0.60f;
        F32 mPeakConvection = 0.35f;
        F32 mWindGain = 1.4f;
    };

    // The season's fair-weather floor and the shape of the day around it.
    struct SeasonBand
    {
        const char* mName;
        F32 mTempLow;         // the dawn trough's range
        F32 mTempHigh;
        F32 mTempSwingLow;    // trough-to-peak, degrees
        F32 mTempSwingHigh;
        F32 mBaseMoistureLow;
        F32 mBaseMoistureHigh;
        F32 mBaseConvectionLow;
        F32 mBaseConvectionHigh;
        F32 mWindLow;
        F32 mWindHigh;
        F32 mSpellChance;     // chance of at least one spell
        F32 mSecondSpellChance;
    };

    const SeasonBand& seasonBand(SSAtmoEnvWeatherSeason season)
    {
        // <SS:Nexii> Bands read off what the resolver does with them rather than off a climate
        // table. Moisture is cloud cover in oktas (moisture * 8) as well as rain intensity, so an
        // autumn baseline of 0.30 is "four oktas standing over you all day" - which is what autumn
        // looks like - while summer's 0.08 is the one wisp that makes a blue sky read as sky and
        // not as a painted dome. Winter's temperature band is the one that matters most: below -1C
        // derivePrecipitationType() stops giving rain at all, so a winter spell IS a snow spell
        // without anything here having to say the word.
        static const SeasonBand BANDS[] = {
            // name      tempLo tempHi swingLo swingHi moistLo moistHi convLo convHi windLo windHi spell second
            {  "Spring",   2.f,  11.f,   6.f,   13.f,   0.12f,  0.34f,  0.08f, 0.30f,  2.f,  8.f,  0.75f, 0.35f },
            {  "Summer",  13.f,  23.f,   8.f,   16.f,   0.03f,  0.18f,  0.05f, 0.24f,  1.f,  6.f,  0.45f, 0.15f },
            {  "Autumn",   3.f,  12.f,   4.f,    9.f,   0.20f,  0.42f,  0.10f, 0.34f,  4.f, 12.f,  0.80f, 0.45f },
            {  "Winter",  -9.f,   1.f,   3.f,    7.f,   0.16f,  0.38f,  0.04f, 0.20f,  3.f, 10.f,  0.70f, 0.35f },
        };
        return BANDS[(size_t)season];
    }

    // <SS:Nexii> Every field's authorable range, so a rolled curve lands somewhere the row that
    // owns it can actually reach. Not decoration: a summer heatwave stacks a raised trough on top
    // of a widened swing and asks for 51C from a slider that stops at 40, and a gale's wind gain
    // multiplies a 27m/s baseline to 59 on a slider that stops at 30. Clamping in the one place
    // every curve passes through beats remembering it at each of the eight places one is built.
    // Keep these in step with the min_val/max_val pairs in the Weather > Conditions panel.
    const F32 MOISTURE_MIN = 0.f,     MOISTURE_MAX = 1.f;
    const F32 CONVECTION_MIN = 0.f,   CONVECTION_MAX = 1.f;
    const F32 TEMPERATURE_MIN = -30.f, TEMPERATURE_MAX = 40.f;
    const F32 HEADING_MIN = 0.f,      HEADING_MAX = 360.f;
    const F32 WIND_MIN = 0.f,         WIND_MAX = 30.f;

    // Snaps to the keyframe grid, sorts, clamps, drops duplicates and lays the curve into a float
    // field. Snapping runs BEFORE the sort: two raw times close enough to swap order under the
    // rounding must not reach the field out of order, and keys that land on the same grid point
    // afterwards collapse to one.
    void layCurve(SSAtmoEnvKeyframed<F32>& field, std::vector<std::pair<F64, F32>>& keys,
                  F32 lo, F32 hi)
    {
        field.reset(keys.empty() ? lo : llclamp(keys.front().second, lo, hi));
        if (keys.size() < 2) return;

        for (std::pair<F64, F32>& key : keys)
        {
            key.first = ss_atmoenv_snap_phase(key.first);
        }

        std::sort(keys.begin(), keys.end(),
                  [](const std::pair<F64, F32>& a, const std::pair<F64, F32>& b)
                  { return a.first < b.first; });

        F64 last_time = -1.0;
        for (const std::pair<F64, F32>& key : keys)
        {
            if (last_time >= 0.0 && key.first <= last_time + 1e-9) continue;
            field.addKeyframe(key.first, llclamp(key.second, lo, hi));
            last_time = key.first;
        }

        // A curve that turned out flat goes back to being a plain value. A dry roll otherwise
        // hands the author eight rows of identical keyframes to step through, which reads as
        // authored intent where there is none - and a plain row is the one you can drag.
        field.collapseIfConstant(0.001f);
    }

    // <SS:Nexii> The precipitation switch: one key on each edge, and nothing between them. Flag
    // keyframes HOLD (ss_atmoenv_default_curve<bool>), so a key stands from its own phase until the
    // next one - the rain starts exactly where the key that turns it on sits and stops exactly
    // where the key that turns it off does. No key is needed at phase 0 either: the wrap segment
    // holds the LAST key's value backwards through midnight, which is the off the last spell ended
    // on. This used to straddle each edge with a false/true pair a hair either side, because an
    // EASE'd flag stepped at the segment midpoint and a lone key would have flipped the rain on
    // halfway between it and whatever preceded it.
    void laySwitch(SSAtmoEnvKeyframed<bool>& field, const std::vector<Spell>& spells)
    {
        field.reset(false);
        if (spells.empty())
        {
            // Nothing falls all cycle. Left as a plain false rather than keyframed: an author who
            // then wants rain flips one checkbox instead of hunting down keys that all say no.
            return;
        }

        for (const Spell& spell : spells)
        {
            field.addKeyframe(ss_atmoenv_snap_phase(spell.mStart), true);
            field.addKeyframe(ss_atmoenv_snap_phase(spell.mStart + spell.mDuration), false);
        }
    }

    // The moisture and convection curves a spell list implies, over a fair-weather baseline.
    void layWeatherCurves(SSAtmoEnvWeather& weather, const std::vector<Spell>& spells,
                          F32 base_moisture, F32 base_convection, F32 base_wind)
    {
        std::vector<std::pair<F64, F32>> moisture;
        std::vector<std::pair<F64, F32>> convection;
        std::vector<std::pair<F64, F32>> wind;

        moisture.emplace_back(0.0, base_moisture);
        moisture.emplace_back(0.98, base_moisture);
        convection.emplace_back(0.0, base_convection);
        convection.emplace_back(0.98, base_convection);
        wind.emplace_back(0.0, base_wind);
        wind.emplace_back(0.98, base_wind);

        for (const Spell& spell : spells)
        {
            const F64 end = spell.mStart + spell.mDuration;

            // The lead. Two keys, not one: a single ramp from baseline to peak reads as a sky
            // sliding steadily wetter, where weather actually thickens slowly and then quickly.
            // The mid key is the point the deck goes properly overcast, well before any rain.
            moisture.emplace_back(spell.mStart - spell.mLead, base_moisture);
            moisture.emplace_back(spell.mStart - spell.mLead * 0.35,
                                  llmax(base_moisture, spell.mPeakMoisture * 0.62f));

            // Falling: nearly there at the first drop, peak in the middle, easing by the last.
            moisture.emplace_back(spell.mStart, spell.mPeakMoisture * 0.88f);
            moisture.emplace_back(spell.mStart + spell.mDuration * 0.5, spell.mPeakMoisture);
            moisture.emplace_back(end, spell.mPeakMoisture * 0.72f);

            // Clearing, which takes longer than the arrival did - the deck has to rain itself out.
            moisture.emplace_back(end + spell.mTail * 0.4, llmax(base_moisture, spell.mPeakMoisture * 0.34f));
            moisture.emplace_back(end + spell.mTail, base_moisture);

            // Convection leads the moisture slightly and outlives it slightly: the air is already
            // stirring before the deck is ready and stays stirred after it has emptied.
            convection.emplace_back(spell.mStart - spell.mLead, base_convection);
            convection.emplace_back(spell.mStart - spell.mLead * 0.25,
                                    llmax(base_convection, spell.mPeakConvection * 0.7f));
            convection.emplace_back(spell.mStart + spell.mDuration * 0.45, spell.mPeakConvection);
            convection.emplace_back(end + spell.mTail * 0.5,
                                    llmax(base_convection, spell.mPeakConvection * 0.3f));
            convection.emplace_back(end + spell.mTail, base_convection);

            // Wind freshens ahead of the front and drops away behind it.
            wind.emplace_back(spell.mStart - spell.mLead, base_wind);
            wind.emplace_back(spell.mStart, base_wind * llmax(1.f, spell.mWindGain * 0.8f));
            wind.emplace_back(spell.mStart + spell.mDuration * 0.4, base_wind * spell.mWindGain);
            wind.emplace_back(end + spell.mTail * 0.6, base_wind);
        }

        layCurve(weather.mMoisture, moisture, MOISTURE_MIN, MOISTURE_MAX);
        layCurve(weather.mConvection, convection, CONVECTION_MIN, CONVECTION_MAX);
        layCurve(weather.mWindSpeed, wind, WIND_MIN, WIND_MAX);
        laySwitch(weather.mPrecipitationFalls, spells);
    }

    // The day's temperature arc: dawn trough, afternoon peak, and back down overnight.
    void layTemperature(SSAtmoEnvWeather& weather, F32 trough_c, F32 swing_c)
    {
        const F32 peak_c = trough_c + swing_c;

        std::vector<std::pair<F64, F32>> keys;
        keys.emplace_back(0.0, trough_c + swing_c * 0.22f);
        keys.emplace_back(TEMP_TROUGH_PHASE, trough_c);
        keys.emplace_back(TEMP_PEAK_PHASE, peak_c);
        keys.emplace_back(0.82, trough_c + swing_c * 0.45f);
        keys.emplace_back(0.98, trough_c + swing_c * 0.24f);

        layCurve(weather.mTemperatureC, keys, TEMPERATURE_MIN, TEMPERATURE_MAX);
    }

    // <SS:Nexii> Wind direction as a slow veer rather than a constant. A fixed heading is the one
    // thing that reads as a simulation running rather than as weather: everything in the scene that
    // rides the flow field - drift, precipitation slant, gust fronts - lines up perfectly and stays
    // there all day. A few tens of degrees over a cycle is enough to break that without ever
    // reading as the wind being indecisive.
    void layWindHeading(SSAtmoEnvWeather& weather, F32 veer_degrees)
    {
        // <SS:Nexii> The starting bearing is picked so the WHOLE veer fits inside 0-360 rather than
        // wrapped afterwards. Heading is a plain number to the resolver and to the slider both, so a
        // curve that crossed north would lerp the long way round the compass - a wind that swings
        // through 350 degrees to move ten. Losing the bearings within a veer's width of the seam
        // costs nothing; a wind that spins the wrong way round the sky is visible from orbit.
        const F32 span = (veer_degrees < 0.f) ? -veer_degrees : veer_degrees;
        const F32 signed_veer = rollChance(0.5f) ? span : -span;
        const F32 start = (signed_veer >= 0.f) ? rollF(0.f, llmax(1.f, 360.f - span))
                                               : rollF(span, 360.f);

        std::vector<std::pair<F64, F32>> keys;
        keys.emplace_back(0.0, start);
        keys.emplace_back(0.35, start + signed_veer * 0.45f);
        keys.emplace_back(0.70, start + signed_veer * 0.80f);
        keys.emplace_back(0.98, start + signed_veer);

        layCurve(weather.mWindHeading, keys, HEADING_MIN, HEADING_MAX);
    }

    // Spells scattered across the window without letting two of them run into each other.
    std::vector<Spell> scatterSpells(S32 count, F32 peak_moisture_low, F32 peak_moisture_high,
                                     F32 peak_convection_low, F32 peak_convection_high)
    {
        std::vector<Spell> spells;
        if (count <= 0) return spells;

        // Each spell gets its own slice of the window, so two never overlap however they roll.
        const F64 slice = (SPELL_WINDOW_END - SPELL_WINDOW_START) / (F64)count;

        for (S32 i = 0; i < count; ++i)
        {
            const F64 slice_start = SPELL_WINDOW_START + slice * (F64)i;

            Spell spell;
            spell.mDuration = rollP(0.05, llmin(0.20, slice * 0.42));
            spell.mLead = rollP(0.05, llmin(0.13, slice * 0.30));
            spell.mTail = rollP(0.06, llmin(0.16, slice * 0.34));

            // The three rolls have independent floors, so their sum can overrun the slice even
            // though each is capped against it - two spells can each roll their longest lead,
            // fall and clear and want 0.45 of a cycle out of the 0.425 they have. Squeezed
            // proportionally rather than clamped, so a spell that has to give ground keeps its
            // shape; the worst squeeze any count can produce is about 6%, so the shortest fall a
            // roll can produce is still around a twentieth of a cycle.
            F64 span = spell.mLead + spell.mDuration + spell.mTail;
            if (span > slice)
            {
                const F64 squeeze = slice / span;
                spell.mLead *= squeeze;
                spell.mDuration *= squeeze;
                spell.mTail *= squeeze;
                span = slice;
            }

            spell.mStart = slice_start + spell.mLead + rollP(0.0, slice - span);

            spell.mPeakMoisture = rollF(peak_moisture_low, peak_moisture_high);
            spell.mPeakConvection = rollF(peak_convection_low, peak_convection_high);
            spell.mWindGain = rollF(1.3f, 2.2f);

            spells.push_back(spell);
        }

        return spells;
    }

    // <SS:Nexii> The extreme events, each expressed as a bend in curves that already exist rather
    // than as a mode of its own. That is the whole trick: because the resolver derives type,
    // intensity, cadence and cover from the cube, "blizzard" is not a switch anywhere - it is cold
    // air, a wet deck and hard wind, and derivePrecipitationType() reaches the word blizzard by
    // itself. Anything that cannot be said in those five curves does not belong in this list.
    enum class Event
    {
        NONE = 0,
        THUNDERSTORM,
        SQUALL_LINE,
        HAILSTORM,
        BLIZZARD,
        COLD_SNAP,
        HEATWAVE,
        GALE,
        STILL_FOG
    };

    const char* eventName(Event event)
    {
        switch (event)
        {
            case Event::THUNDERSTORM: return "thunderstorms";
            case Event::SQUALL_LINE:  return "a squall line";
            case Event::HAILSTORM:    return "a hailstorm";
            case Event::BLIZZARD:     return "a blizzard";
            case Event::COLD_SNAP:    return "a cold snap";
            case Event::HEATWAVE:     return "a heatwave";
            case Event::GALE:         return "gale-force winds";
            case Event::STILL_FOG:    return "a fog-bound morning";
            default:                  return "";
        }
    }

    // Which events a season will admit at all - a heatwave in deep winter is a different world,
    // not a different day, and the fantasy path is where different worlds live.
    Event rollEvent(SSAtmoEnvWeatherSeason season)
    {
        std::vector<Event> pool;
        pool.push_back(Event::THUNDERSTORM);
        pool.push_back(Event::SQUALL_LINE);
        pool.push_back(Event::GALE);
        pool.push_back(Event::STILL_FOG);

        switch (season)
        {
            case SSAtmoEnvWeatherSeason::SPRING:
                pool.push_back(Event::COLD_SNAP);
                pool.push_back(Event::HAILSTORM);
                break;
            case SSAtmoEnvWeatherSeason::SUMMER:
                pool.push_back(Event::HEATWAVE);
                pool.push_back(Event::HEATWAVE);
                pool.push_back(Event::THUNDERSTORM);
                pool.push_back(Event::HAILSTORM);
                break;
            case SSAtmoEnvWeatherSeason::AUTUMN:
                pool.push_back(Event::GALE);
                pool.push_back(Event::COLD_SNAP);
                break;
            case SSAtmoEnvWeatherSeason::WINTER:
                pool.push_back(Event::BLIZZARD);
                pool.push_back(Event::BLIZZARD);
                pool.push_back(Event::COLD_SNAP);
                break;
        }

        return pool[(size_t)ll_rand((S32)pool.size())];
    }

    // The fantasy archetypes: whole worlds rather than days, each deliberately outside the envelope
    // the seasonal path stays inside. Every one still goes through the same spell machinery, so an
    // impossible sky still arrives and clears like a real one.
    struct Fantasy
    {
        const char* mName;
        F32 mTroughC;
        F32 mSwingC;
        F32 mBaseMoisture;
        F32 mBaseConvection;
        F32 mWind;
        F32 mVeer;
        S32 mSpells;
        F32 mPeakMoisture;
        F32 mPeakConvection;
        const char* mForcedType;   // empty to let the temperature decide
    };

    const Fantasy& rollFantasy()
    {
        static const Fantasy WORLDS[] = {
            // name              trough swing moist  conv  wind  veer spells peakM peakC forced
            { "the Stormlands",    9.f, 6.f,  0.62f, 0.55f, 14.f, 140.f, 3,  0.94f, 0.97f, "" },
            { "an Ashen Sky",     31.f, 8.f,  0.44f, 0.90f,  7.f,  40.f, 2,  0.66f, 0.99f, "hail" },
            { "an Endless Winter", -22.f, 5.f, 0.48f, 0.40f, 15.f, 200.f, 2, 0.82f, 0.78f, "" },
            { "a Glass Calm",     22.f, 3.f,  0.00f, 0.00f,  0.f,   0.f, 0,  0.00f, 0.00f, "" },
            { "a Weeping Season",  9.f, 3.f,  0.09f, 0.06f,  2.f,  25.f, 0,  0.00f, 0.00f, "" },
            { "the Tideturn",     16.f, 9.f,  0.06f, 0.10f,  5.f, 300.f, 4,  0.97f, 0.72f, "" },
            { "an Emberfall",     34.f, 6.f,  0.02f, 0.05f,  9.f,  60.f, 1,  0.99f, 0.60f, "" },
        };
        return WORLDS[ll_rand((S32)(sizeof(WORLDS) / sizeof(WORLDS[0])))];
    }
}

// Wipes the cube back to its constructed defaults - a still, dry, clear, temperate sky.
void SSAtmoEnvWeatherGenerator::clear(SSAtmoEnvWeather& weather)
{
    weather = SSAtmoEnvWeather();
}

// <SS:Nexii> One roll of a whole day. Order matters: the theme sets the bands, the event bends them,
// and only then are the curves laid, so an event never has to re-write keyframes a season already
// wrote. Lightning and gusts are deliberately left on auto throughout - convection, moisture and
// temperature already decide the cadence through the resolver, and a generator that also authored
// either would be arguing with itself about what a storm is.
SSAtmoEnvWeatherRoll SSAtmoEnvWeatherGenerator::randomize(SSAtmoEnvWeather& weather)
{
    clear(weather);

    SSAtmoEnvWeatherRoll roll;

    // The fantasy path: a whole archetype, no seasonal band and no event on top. Layering an
    // ordinary cold snap over the Stormlands would only sand the archetype's edges off, and its
    // edges are the entire reason it exists.
    if (rollChance(0.20f))
    {
        const Fantasy& world = rollFantasy();

        roll.mFantasy = true;
        roll.mTheme = world.mName;

        layTemperature(weather, world.mTroughC, world.mSwingC);
        layWindHeading(weather, world.mVeer);

        std::vector<Spell> spells = scatterSpells(world.mSpells,
                                                  world.mPeakMoisture * 0.85f, world.mPeakMoisture,
                                                  world.mPeakConvection * 0.8f, world.mPeakConvection);
        layWeatherCurves(weather, spells, world.mBaseMoisture, world.mBaseConvection, world.mWind);

        if (world.mForcedType && *world.mForcedType)
        {
            weather.mPrecipitationOverride.reset(std::string(world.mForcedType));
        }

        // A world whose baseline is already wet enough to rain leaves the switch simply on: the
        // Weeping Season is not a day with showers in it, it is a place where it is always raining.
        if (spells.empty() && world.mBaseMoisture > 0.02f)
        {
            weather.mPrecipitationFalls.reset(true);
        }

        roll.mSummary = "Fantasy: " + roll.mTheme;
        return roll;
    }

    roll.mSeason = (SSAtmoEnvWeatherSeason)ll_rand(4);
    const SeasonBand& band = seasonBand(roll.mSeason);
    roll.mTheme = band.mName;

    F32 trough_c = rollF(band.mTempLow, band.mTempHigh);
    F32 swing_c = rollF(band.mTempSwingLow, band.mTempSwingHigh);
    F32 base_moisture = rollF(band.mBaseMoistureLow, band.mBaseMoistureHigh);
    F32 base_convection = rollF(band.mBaseConvectionLow, band.mBaseConvectionHigh);
    F32 base_wind = rollF(band.mWindLow, band.mWindHigh);
    F32 veer = rollF(20.f, 90.f);

    S32 spell_count = 0;
    if (rollChance(band.mSpellChance))
    {
        spell_count = rollChance(band.mSecondSpellChance) ? 2 : 1;
    }

    F32 peak_moisture_low = 0.35f;
    F32 peak_moisture_high = 0.70f;
    F32 peak_convection_low = 0.20f;
    F32 peak_convection_high = 0.55f;

    const Event event = rollChance(0.45f) ? rollEvent(roll.mSeason) : Event::NONE;
    roll.mEvent = eventName(event);

    switch (event)
    {
        case Event::THUNDERSTORM:
            // SEVERE convection over a wet deck is what the resolver reads as thunder; the wet gate
            // means the moisture floor here is doing as much work as the convection is.
            spell_count = llmax(spell_count, 1);
            peak_moisture_low = 0.62f;
            peak_moisture_high = 0.92f;
            peak_convection_low = 0.80f;
            peak_convection_high = 0.96f;
            break;

        case Event::SQUALL_LINE:
            // Short, violent and entirely derived: a squall's severe convection over a wet deck is
            // its whole character, and the auto gusts read it straight off the cube.
            spell_count = 1;
            peak_moisture_low = 0.55f;
            peak_moisture_high = 0.85f;
            peak_convection_low = 0.70f;
            peak_convection_high = 0.90f;
            base_wind = rollF(11.f, 19.f);
            veer = rollF(60.f, 120.f);
            break;

        case Event::HAILSTORM:
            // Past 0.95 convection derivePrecipitationType() gives hail on its own, but only above
            // 1.5C - so the trough is lifted rather than left to a spring roll that might be frosty.
            spell_count = 1;
            trough_c = llmax(trough_c, 4.f);
            peak_moisture_low = 0.55f;
            peak_moisture_high = 0.80f;
            peak_convection_low = 0.96f;
            peak_convection_high = 1.00f;
            break;

        case Event::BLIZZARD:
            // Below -1C with convection past 0.7 the type derives as blizzard without the word
            // appearing anywhere here. The whole day is wet, not just the spells.
            trough_c = rollF(-14.f, -5.f);
            swing_c = rollF(2.f, 5.f);
            base_moisture = llmax(base_moisture, 0.45f);
            base_wind = rollF(12.f, 21.f);
            spell_count = llmax(spell_count, 2);
            peak_moisture_low = 0.70f;
            peak_moisture_high = 0.95f;
            peak_convection_low = 0.72f;
            peak_convection_high = 0.88f;
            break;

        case Event::COLD_SNAP:
            // Dropped bodily rather than re-rolled, so whatever the season was doing survives it -
            // an autumn day of showers becomes the same day of sleet.
            trough_c -= rollF(8.f, 16.f);
            swing_c = llmin(swing_c, 6.f);
            base_wind = llmax(base_wind, 5.f);
            break;

        case Event::HEATWAVE:
            trough_c += rollF(6.f, 13.f);
            swing_c = rollF(9.f, 15.f);
            base_moisture = llmin(base_moisture, 0.07f);
            base_convection = llmin(base_convection, 0.12f);
            base_wind = rollF(0.f, 3.f);
            spell_count = 0;
            break;

        case Event::GALE:
            base_wind = rollF(17.f, 27.f);
            base_moisture = llmax(base_moisture, 0.28f);
            veer = rollF(70.f, 150.f);
            break;

        case Event::STILL_FOG:
            // No fog dial in the cube, so a fog-bound morning is what one actually is to everything
            // downstream: a wet, motionless, overcast sky that burns off by mid-afternoon.
            base_moisture = llmax(base_moisture, 0.38f);
            base_convection = 0.01f;
            base_wind = rollF(0.f, 1.2f);
            spell_count = 0;
            veer = rollF(0.f, 15.f);
            break;

        case Event::NONE:
        default:
            break;
    }

    layTemperature(weather, trough_c, swing_c);
    layWindHeading(weather, veer);

    std::vector<Spell> spells = scatterSpells(spell_count,
                                              peak_moisture_low, peak_moisture_high,
                                              peak_convection_low, peak_convection_high);
    layWeatherCurves(weather, spells, base_moisture, base_convection, base_wind);

    // The fog morning is the one case that wants a wet sky curve without a spell in it: heavy at
    // dawn, thinning through the afternoon, and never once raining.
    if (event == Event::STILL_FOG)
    {
        std::vector<std::pair<F64, F32>> fog;
        fog.emplace_back(0.0, base_moisture);
        fog.emplace_back(0.28, llmin(0.62f, base_moisture + 0.2f));
        fog.emplace_back(0.62, base_moisture * 0.45f);
        fog.emplace_back(0.98, base_moisture * 0.8f);
        layCurve(weather.mMoisture, fog, MOISTURE_MIN, MOISTURE_MAX);
    }

    roll.mSummary = roll.mTheme;
    if (!roll.mEvent.empty())
    {
        roll.mSummary += " with " + roll.mEvent;
    }
    if (spells.empty())
    {
        roll.mSummary += " - dry all cycle";
    }
    else
    {
        roll.mSummary += (spells.size() == 1) ? " - one spell of precipitation"
                                              : " - two spells of precipitation";
    }

    return roll;
}
