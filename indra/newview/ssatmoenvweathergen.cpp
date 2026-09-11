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

#include "sssquallcore.h" // <SS:Nexii> SSSquall::severeDayBias, ::Onset/::onsetValue, ::KIND_SQUALL - the severe-day option's and the 8a authored-event's numeric formulas, core-side
#include "sswindprofilecore.h" // <SS:Nexii> SSWindProfile::fromHeading - the authored event's upwind-offset formula, core-side

#include "llrand.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    const F32 SHEAR_MIN = 0.f,        SHEAR_MAX = 1.f;

    // <SS:Nexii> The two cloud-deck rows and the one dome row the authored event reaches, same rule as the five
    // above: keep in step with the Clouds > Deck and Clouds > Dome panels (cloud_coverage_* 0..2,
    // cloud_thickness_* 10..2000, dome_coverage_* 0..1).
    const F32 COVERAGE_SCALE_MIN = 0.f,  COVERAGE_SCALE_MAX = 2.f;
    const F32 THICKNESS_MIN = 10.f,      THICKNESS_MAX = 2000.f;
    const F32 DOME_COVERAGE_MIN = 0.f,   DOME_COVERAGE_MAX = 1.f;

    // <SS:Nexii> The severe-day option's own baseline: what an ordinary roll's shear would have been had it
    // rolled one at all. The generator otherwise leaves mShearAuto on throughout - shear derives from moisture/
    // convection/temperature the same as gusts do - so there is no rolled shear peak of the roll's own to hand
    // SSSquall::severeDayBias; this nominal floor stands in for it, matching the mild jet SSWindProfile::autoShear
    // itself derives on an ordinary wet day. Severe day authors the curve explicitly (mShearAuto off) only when
    // it fires, so an unchecked roll never touches shear at all.
    const F32 SHEAR_DAY_BASELINE = 0.30f;

    // <SS:Nexii> Phase 8a: how far AHEAD of the cue each of the authored squall's curves starts moving. The
    // deck leads the wall - moisture first, convection just behind it, both wide enough to read as a sky going
    // over before anything arrives - while the wind and the rain itself step at the cue on the core's own
    // ONSET_RAMP_PHASE, which is the abrupt one (~5 min of a 4 h day). That contrast IS the show: an hour of
    // thickening cloud, then a wall, then rain in the same instant. Ramps, not the shape - the shape is
    // SSSquall::onsetValue's, sampled through layOnsetCurve below.
    const F32 ONSET_RAMP_MOISTURE = 0.06f;
    const F32 ONSET_RAMP_CONVECTION = 0.05f;

    // <SS:Nexii> Phase 8a, the clear sky the wall arrives INTO. The deck's coverage is
    // (1 - (1 - moisture)^3) * mCoverageScale, and the cube's dry baseline moisture is around 0.22, which is still
    // 0.53 of the sky covered before the storm has done anything - a wall arriving into an overcast is not an
    // arrival. So the scale itself is authored down to CLEAR_SCALE for the approach and stepped back up at the cue:
    // the sky OPENS while the moisture ramp is already thickening what is left of it, and then shuts. Thickness
    // does the same in the vertical (a low thin deck before, a tall one at the cue - the wall is what is tall), and
    // the cirrus dome thins to DOME_CLEAR_FRACTION so the blue is blue rather than milky. Multipliers, not
    // absolutes, everywhere the author's own value can stand in for "ordinary": only the cleared scale is a number,
    // because "clear" is a property of the sky and not of what this deck was dialled to.
    const F32 CLEAR_SCALE = 0.15f;
    const F32 CUE_SCALE = 1.f;
    const F32 THIN_BEFORE_MUL = 0.5f;
    const F32 TALL_AT_CUE_MUL = 1.6f;
    const F32 DOME_CLEAR_FRACTION = 0.3f;

    // <SS:Nexii> Lays one field as the core's onset curve: four keys at the four phases the shape itself has -
    // ramp start, cue, hold end, taper end - each carrying what SSSquall::onsetValue reads AT the phase the key
    // lands on, so this file never respells the curve and a snapped key never disagrees with the core about the
    // value there. Keys go down on the preview grid like every other generated key (ss_atmoenv_snap_phase), and
    // a phase that snaps onto its predecessor is dropped rather than inserted twice.
    // <SS:Nexii> `after` is what the TAPER returns to, which is not always what the ramp left: the deck's coverage
    // scale is dropped to a cleared value for the approach and has to come back to the author's own scale rather
    // than to the clearing, or every authored squall would leave the sky permanently open behind it. It is still
    // one onset curve - the ramp reads `before`, the taper reads `after`, and the two branches agree everywhere
    // they overlap because onsetValue returns `peak` across the whole hold whatever baseline it is handed. Pass
    // `before` again for the symmetric case (moisture, convection, wind: back to the day's baseline).
    void layOnsetCurve(SSAtmoEnvKeyframed<F32>& field, const SSSquall::Onset& onset,
                       F32 before, F32 peak, F32 after, F32 lo, F32 hi, SSAtmoEnvCurve curve)
    {
        const F64 cue = (F64)onset.cuePhase;
        const F64 phases[4] = { cue - (F64)onset.rampPhase,
                                cue,
                                cue + (F64)onset.holdPhase,
                                cue + (F64)onset.holdPhase + (F64)onset.taperPhase };

        F64 last = -1.0;
        for (const F64 phase : phases)
        {
            const F64 at = ss_atmoenv_snap_phase(phase);
            if (last >= 0.0 && llabs(at - last) < 1e-9) continue;
            const F32 baseline = (phase > cue) ? after : before;
            field.addKeyframe(at, llclamp(SSSquall::onsetValue((F32)at, onset, baseline, peak), lo, hi), curve);
            last = at;
        }
    }

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
SSAtmoEnvWeatherRoll SSAtmoEnvWeatherGenerator::randomize(SSAtmoEnvWeather& weather,
                                                          SSAtmoEnvCloudField& field,
                                                          SSAtmoEnvCloudDome& dome,
                                                          bool severeDay, F32 severeDayStrength,
                                                          SSAtmoEnvWeatherInfluence& influence)
{
    clear(weather);

    SSAtmoEnvWeatherRoll roll;

    // The fantasy path: a whole archetype, no seasonal band and no event on top. Layering an
    // ordinary cold snap over the Stormlands would only sand the archetype's edges off, and its
    // edges are the entire reason it exists. A severe day skips it: the checkbox promises an authored arrival
    // (8a), and the fantasy path lays no event to arrive.
    if (!severeDay && rollChance(0.20f))
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

    Event event = rollChance(0.45f) ? rollEvent(roll.mSeason) : Event::NONE;
    // <SS:Nexii> Severe Day promises an arrival, so the two events that dry the day out (both zero the spell count) are
    // re-rolled away; whatever else the season admits may carry the severe spell.
    while (severeDay && (event == Event::HEATWAVE || event == Event::STILL_FOG))
    {
        event = rollEvent(roll.mSeason);
    }
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

    // <SS:Nexii> Severe day: at least one spell (the arrival the checkbox promises needs a spell to be cued from -
    // before 8f-b a quarter of severe rolls missed the plain spell chance and authored nothing), then lifts the peak
    // RANGE a spell may roll into before any spell is actually rolled - the bias reaches the ceiling scatterSpells
    // draws from, same as an event's own peak_*_high overrides above, rather than editing a spell after the fact.
    if (severeDay)
    {
        spell_count = llmax(spell_count, 1);
    }
    if (severeDay && spell_count > 0)
    {
        const SSSquall::DayPeaks rolled{ peak_moisture_high, peak_convection_high, SHEAR_DAY_BASELINE };
        const SSSquall::DayPeaks biased = SSSquall::severeDayBias(rolled, severeDayStrength);
        peak_moisture_high   = llmax(peak_moisture_high,   biased.mMoisture);
        peak_moisture_low    = llmax(peak_moisture_low,    biased.mMoisture * 0.85f);
        peak_convection_high = llmax(peak_convection_high, biased.mConvection);
        peak_convection_low  = llmax(peak_convection_low,  biased.mConvection * 0.85f);
    }

    std::vector<Spell> spells = scatterSpells(spell_count,
                                              peak_moisture_low, peak_moisture_high,
                                              peak_convection_low, peak_convection_high);
    layWeatherCurves(weather, spells, base_moisture, base_convection, base_wind);

    // <SS:Nexii> Severe day's shear half: authors mShearStrength explicitly (auto stays off) over a window
    // shaped exactly like the rolled peak's own lead/fall/tail - centred on the same phase moisture and
    // convection already peak at - rather than the whole cycle, so a severe SPELL gets severe shear and the
    // fair-weather rest of the day is untouched. Picks the most severe of the day's spells (highest combined
    // moisture+convection peak) when more than one rolled; the core's severeDayBias is the only formula here.
    if (severeDay && !spells.empty())
    {
        const Spell* peak_spell = &spells.front();
        for (const Spell& spell : spells)
        {
            const F32 severity = spell.mPeakMoisture + spell.mPeakConvection;
            const F32 peak_severity = peak_spell->mPeakMoisture + peak_spell->mPeakConvection;
            if (severity > peak_severity) peak_spell = &spell;
        }

        const SSSquall::DayPeaks rolled{ peak_spell->mPeakMoisture, peak_spell->mPeakConvection, SHEAR_DAY_BASELINE };
        const SSSquall::DayPeaks biased = SSSquall::severeDayBias(rolled, severeDayStrength);

        const F64 peak_phase = peak_spell->mStart + peak_spell->mDuration * 0.5;

        std::vector<std::pair<F64, F32>> shear;
        shear.emplace_back(0.0, SHEAR_DAY_BASELINE);
        shear.emplace_back(llclamp(peak_phase - peak_spell->mLead, 0.0, 1.0), SHEAR_DAY_BASELINE);
        shear.emplace_back(peak_phase, biased.mShear);
        shear.emplace_back(llclamp(peak_phase + peak_spell->mDuration * 0.5 + peak_spell->mTail, 0.0, 0.98),
                           SHEAR_DAY_BASELINE);
        shear.emplace_back(0.98, SHEAR_DAY_BASELINE);

        weather.mShearAuto = false;
        layCurve(weather.mShearStrength, shear, SHEAR_MIN, SHEAR_MAX);
    }

    // <SS:Nexii> Phase 8a (doc/atmo_magic_phase8_show.md section 3): a severe day AUTHORS the event, not just
    // its curves. "A generated squall line IS the weather change" - so this writes the forced-storm keyframes
    // (mStormOverride idiom, HOLD like every other field in that group) and then RESHAPES moisture, convection
    // and wind speed around the cue with SSSquall::onsetValue, superseding the spell's own natural onset shape
    // laid by layWeatherCurves above. Kind: squall line 40%, tornado day 60% (ll_frand, authoring-time only -
    // no runtime randomness). Cue: the rolled peak spell's own mStart - its ONSET, the phase its rain was going
    // to begin at, not mStart + mLead: mLead is the ramp BEFORE mStart everywhere else in this file (see
    // layWeatherCurves, whose lead keys sit at mStart - mLead), so adding it put the wall's arrival a lead's
    // width INTO the rain it was supposed to be bringing. Offset: 1500 m upwind of the anchor at the wind of
    // that phase - the CUBE's own wind speed/heading keyframes at the cue, through
    // SSWindProfile::fromHeading, because SSAtmoEnvApplier::windProfileAt needs a full SSAtmoEnvTrack and this
    // generator only ever sees the SSAtmoEnvWeather cube (no track in scope) - never callable here.
    std::string severe_event_summary; // appended to roll.mSummary after its own assembly below, never overwritten by it
    if (severeDay && !spells.empty())
    {
        const Spell* peak_spell = &spells.front();
        for (const Spell& spell : spells)
        {
            const F32 severity = spell.mPeakMoisture + spell.mPeakConvection;
            const F32 peak_severity = peak_spell->mPeakMoisture + peak_spell->mPeakConvection;
            if (severity > peak_severity) peak_spell = &spell;
        }

        const bool is_squall = rollChance(0.40f); // squall line 40%, tornado day 60%
        const F64 cue_phase = ss_atmoenv_snap_phase(peak_spell->mStart);

        // Upwind unit vector at the cue: the wind vector points where the air MOVES (SSWindProfile::fromHeading's
        // own contract), so upwind is its negation; a degenerate (zero-speed) wind falls back to due south, the
        // same "unit north" degenerate convention SSSquall::unitOrNorth uses but negated (upwind, not downwind).
        const F32 heading_at_cue = weather.mWindHeading.valueAt(cue_phase);
        const F32 speed_at_cue = weather.mWindSpeed.valueAt(cue_phase);
        const SSWindProfile::Vec2 wind_at_cue = SSWindProfile::fromHeading(heading_at_cue, speed_at_cue);
        const F32 wind_len = std::sqrt(wind_at_cue.x * wind_at_cue.x + wind_at_cue.y * wind_at_cue.y);
        F32 upwind_x = 0.f, upwind_y = -1.f;
        if (wind_len >= 1e-4f)
        {
            upwind_x = -wind_at_cue.x / wind_len;
            upwind_y = -wind_at_cue.y / wind_len;
        }
        constexpr F32 STORM_OVERRIDE_OFFSET_M = 1500.f;

        weather.mStormOverride.reset(is_squall ? std::string("squall") : std::string("tornado"));
        weather.mStormOverridePhase.reset((F32)cue_phase);
        weather.mStormOverrideOffsetXM.reset(upwind_x * STORM_OVERRIDE_OFFSET_M);
        weather.mStormOverrideOffsetYM.reset(upwind_y * STORM_OVERRIDE_OFFSET_M);
        // 7b F4 idiom: these three curves are forced HOLD on every write, same as ssatmoenvasset.cpp's fromLLSD -
        // a piecewise-constant cue that cannot slide under EASE/LINEAR interpolation.
        weather.mStormOverride.forceCurve(SSAtmoEnvCurve::HOLD);
        weather.mStormOverridePhase.forceCurve(SSAtmoEnvCurve::HOLD);
        weather.mStormOverrideOffsetXM.forceCurve(SSAtmoEnvCurve::HOLD);
        weather.mStormOverrideOffsetYM.forceCurve(SSAtmoEnvCurve::HOLD);

        // The onset: dry/calm before the ramp, a rise INTO the cue, held for the peak spell's own duration,
        // then a taper back down - SSSquall::Onset's own default taper width, only cuePhase, holdPhase and
        // each curve's own ramp are the roll's.
        SSSquall::Onset onset;
        onset.cuePhase = (F32)cue_phase;
        onset.holdPhase = (F32)peak_spell->mDuration;

        const F64 hold_end_at = cue_phase + (F64)onset.holdPhase;
        const F64 taper_end_at = hold_end_at + (F64)onset.taperPhase;

        // <SS:Nexii> The window is REBUILT, not written over. layWeatherCurves has already laid this same
        // spell's natural arrival across it - a lead that thickens from mStart - mLead, a peak at mid-spell, a
        // tail out to mStart + mDuration + mTail - and adding the cue's own keys on top of those leaves one
        // storm with two humps: the sky wets up, rains, dries part way, and rains again for the same event.
        // So every keyframe of the four curves the cue owns is dropped over [start of the spell's own lead
        // minus the widest onset ramp, taper end] first, and only then laid back in the cue's shape. Wrapping
        // is the container's (removeKeyframesInPhaseRange); spells never wrap midnight (SPELL_WINDOW_END) but
        // a taper past 0.98 does. The window's low edge is clamped to the peak spell's OWN lead start (mStart -
        // mLead, which scatterSpells keeps inside the spell's slice), so a second spell earlier in the day keeps
        // every key of its tail (8a re-audit: unclamped, the moisture ramp's reach-back ate the other spell's
        // back-to-base key on ~10% of two-spell rolls) - the cue reshapes one spell, not the roll.
        const F64 window_lo = llmax(cue_phase - peak_spell->mLead - (F64)ONSET_RAMP_MOISTURE,
                                    peak_spell->mStart - peak_spell->mLead);
        weather.mMoisture.removeKeyframesInPhaseRange(window_lo, taper_end_at);
        weather.mConvection.removeKeyframesInPhaseRange(window_lo, taper_end_at);
        weather.mWindSpeed.removeKeyframesInPhaseRange(window_lo, taper_end_at);
        weather.mPrecipitationFalls.removeKeyframesInPhaseRange(window_lo, taper_end_at);

        // Precipitation intensity derives from moisture alone (SSAtmoEnvWeatherResolver::classifyIntensity) -
        // the cube has no separate intensity field - so mMoisture IS the precipitation intensity curve, and it
        // leads the cue by the widest ramp of the four: the deck is already heavy when the wall shows up.
        SSSquall::Onset moisture_onset = onset;
        moisture_onset.rampPhase = ONSET_RAMP_MOISTURE;
        layOnsetCurve(weather.mMoisture, moisture_onset, base_moisture, peak_spell->mPeakMoisture, base_moisture,
                      MOISTURE_MIN, MOISTURE_MAX, SSAtmoEnvCurve::EASE);

        // Convection follows moisture in a fractionally tighter ramp - the air stirs once the deck is on its way.
        SSSquall::Onset convection_onset = onset;
        convection_onset.rampPhase = ONSET_RAMP_CONVECTION;
        layOnsetCurve(weather.mConvection, convection_onset, base_convection, peak_spell->mPeakConvection,
                      base_convection, CONVECTION_MIN, CONVECTION_MAX, SSAtmoEnvCurve::EASE);

        // The gust front: wind speed JUMPS at the cue rather than easing into it. Same four keys, but on the
        // core's abrupt ONSET_RAMP_PHASE and every key HOLD, so the segment before the cue stands at the base
        // wind and the cue itself is the step. Clamped like every other generated key - a 2.2 gain on a gale's
        // 27 m/s baseline asks for 59 from a slider that stops at 30 (see WIND_MAX above).
        SSSquall::Onset wind_onset = onset;
        wind_onset.rampPhase = SSSquall::ONSET_RAMP_PHASE;
        layOnsetCurve(weather.mWindSpeed, wind_onset, base_wind, base_wind * peak_spell->mWindGain, base_wind,
                      WIND_MIN, WIND_MAX, SSAtmoEnvCurve::HOLD);

        // <SS:Nexii> The rain is a switch, not a curve, so it gets the shape rather than the values: dry
        // through the ramp the deck is thickening over, falling AT the cue - the wall's arrival IS the first
        // drop - and off again where the moisture taper has it back at the day's baseline. Three keys, not
        // four: a flag holds from its own key to the next (ss_atmoenv_default_curve<bool>), so a second key
        // saying "still true" at the hold end would be a mark the author can delete with nothing changing.
        // The pre-cue sky this leaves is deliberately DRY, which is only safe because the line the cue
        // authors carries its own weather floor (SSSquall::forcedLine's mHasFloor) - its members no longer
        // gate on the sky they are arriving into.
        weather.mPrecipitationFalls.addKeyframe(ss_atmoenv_snap_phase(cue_phase - (F64)SSSquall::ONSET_RAMP_PHASE), false);
        weather.mPrecipitationFalls.addKeyframe(cue_phase, true);
        weather.mPrecipitationFalls.addKeyframe(ss_atmoenv_snap_phase(taper_end_at), false);

        // <SS:Nexii> The clear sky the wall arrives into. Everything above this reshapes the CUBE, and the cube
        // alone cannot open the sky: deck coverage is (1 - (1 - moisture)^3) * mCoverageScale
        // (ssatmoenvcloudfieldstate.cpp), so even with the moisture curve driven down to the day's dry baseline
        // (~0.22) the deck still stands over 0.53 of the sky, and the dome's cirrus is on its own dial that no
        // weather curve reaches at all. The author's report - no clear sky before the squall - is those two facts.
        // So the same rebuild-window discipline is applied to three more curves, all of them the SKY's rather than
        // the weather's: the deck's coverage scale and depth, and the dome's coverage. Same window, same shape
        // through SSSquall::onsetValue, same snapped phases, EASE so the opening and the shutting are both gradual
        // reads rather than cuts, and nothing laid outside [window_lo, taper_end_at].
        //
        // These are the only writes this generator makes outside the weather cube, and they happen ONLY here, on
        // an authored severe event - an ordinary roll never touches the deck or the dome.
        //
        // The values the taper returns to are read BEFORE the removals, at the taper end, so a deck the author has
        // keyframed gets its own value back rather than a constant: the event borrows the sky for its window and
        // hands it back. [interaction: SSAtmoEnvCloudFieldResolver::resolve now reads mCoverageScale under Auto
        // too (8f-1) - it derives a baseline of 1.0 and multiplies the authored curve onto it, so these onset keys
        // open the sky whether or not the deck's Auto is on. mBaseThicknessM is still Auto's alone (for a deck
        // that owns its own geometry - the primary deck does) and these keys sit unread there; that split is 8b-1,
        // not this generator's business to close. Not flipped here: Auto also owns the deck's seasonal altitude
        // and its moisture-led darkening, and a weather roll has no business taking those off an author who chose
        // Auto.]
        {
            const F32 prior_scale_at_cue   = field.mCoverageScale.valueAt(cue_phase);
            const F32 prior_scale_after    = field.mCoverageScale.valueAt(taper_end_at);
            const F32 prior_thick_at_cue   = field.mBaseThicknessM.valueAt(cue_phase);
            const F32 prior_thick_after    = field.mBaseThicknessM.valueAt(taper_end_at);
            const F32 prior_dome_at_cue    = dome.mCoverage.valueAt(cue_phase);
            const F32 prior_dome_after     = dome.mCoverage.valueAt(taper_end_at);

            field.mCoverageScale.removeKeyframesInPhaseRange(window_lo, taper_end_at);
            field.mBaseThicknessM.removeKeyframesInPhaseRange(window_lo, taper_end_at);
            dome.mCoverage.removeKeyframesInPhaseRange(window_lo, taper_end_at);

            // All three step AT the cue on the core's abrupt ramp rather than leading it: the deck's own lead is
            // already spent on the moisture and convection curves above, and what arrives at the cue is a wall -
            // the sky is open one step before it and shut on it. The ramp key's own value is the cleared one, so
            // the approach to it (the wrap segment back from the taper end, EASE) is the sky visibly opening over
            // the hours before the event rather than a jump.
            SSSquall::Onset sky_onset = onset;
            sky_onset.rampPhase = SSSquall::ONSET_RAMP_PHASE;

            // Coverage scale: cleared to CLEAR_SCALE, full at the cue, back to the author's own scale. The cue
            // value takes the larger of CUE_SCALE and what was there, so a deck already dialled past 1 is never
            // REDUCED by a storm arriving over it.
            layOnsetCurve(field.mCoverageScale, sky_onset, CLEAR_SCALE,
                          llmax(CUE_SCALE, prior_scale_at_cue), prior_scale_after,
                          COVERAGE_SCALE_MIN, COVERAGE_SCALE_MAX, SSAtmoEnvCurve::EASE);

            // Depth: a thin remnant deck before, a tall one at the cue. Convection multiplies this downstream
            // (height_factor) and the storm lid caps the product, so the multipliers are deliberately modest.
            layOnsetCurve(field.mBaseThicknessM, sky_onset, prior_thick_at_cue * THIN_BEFORE_MUL,
                          prior_thick_at_cue * TALL_AT_CUE_MUL, prior_thick_after,
                          THICKNESS_MIN, THICKNESS_MAX, SSAtmoEnvCurve::EASE);

            // The cirrus band thins to a fraction of itself for the approach and comes straight back at the cue -
            // the dome is the anvil-level cloud, so it belongs to the storm, not to the clearing.
            layOnsetCurve(dome.mCoverage, sky_onset, prior_dome_at_cue * DOME_CLEAR_FRACTION,
                          prior_dome_at_cue, prior_dome_after,
                          DOME_COVERAGE_MIN, DOME_COVERAGE_MAX, SSAtmoEnvCurve::EASE);
        }

        const S32 cue_minutes_of_day = (S32)ll_round(llclamp(cue_phase, 0.0, 1.0) * 24.0 * 60.0);
        char cue_hhmm[8];
        std::snprintf(cue_hhmm, sizeof(cue_hhmm), "%02d:%02d", (cue_minutes_of_day / 60) % 24, cue_minutes_of_day % 60);
        severe_event_summary = (is_squall ? ", squall line arriving at " : ", tornado day, cue at ") + std::string(cue_hhmm);
    }

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

    // <SS:Nexii> V2 legend fix: Randomize Severe Day IS the author opting the track into severe weather - a roll
    // that authors a cue and a floor for the pinned cell but leaves the track's Allow flags off means no
    // background supercell and no spontaneous hero can ever form on the track (SSStormCells::birthMemo's
    // influence_enabled gate, ssstormcells.cpp). So a severe roll turns the master and both Allow flags on here;
    // strengths are untouched, and an ordinary (non-severe) roll leaves the track's permissions exactly as it
    // found them - flipping permissions is not something an ordinary roll of the day's weather does.
    if (severeDay)
    {
        influence.mEnabled = true;
        influence.mAllowSupercells = true;
        influence.mAllowTornadoes = true;
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
    if (severeDay && !spells.empty())
    {
        roll.mSummary += ", severe day";
    }
    roll.mSummary += severe_event_summary;
    if (severeDay)
    {
        roll.mSummary += ", supercells and tornadoes allowed";
    }

    return roll;
}
