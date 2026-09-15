/**
 * @file ssdeckboilcore.h
 * @brief Atmo Magic: the deck's BOIL CLOCK - the advected detail octave's phase and the rain curtain's fall, as integrated distances rather than rate x wall clock. Header-only core. CONTRACT.
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

#ifndef SS_DECKBOILCORE_H
#define SS_DECKBOILCORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath>, <cstdint> only). THE FINDING IT EXISTS FOR (fourth build report, D3 - "every puff's appearance jumps in a sudden step about once per second, and the flow motion is far too exaggerated"): ssVolCloudF.glsl computed the advected octave's phase as `turn = ss_time * SS_OCT_LAPS * ss_drift_rate * (SS_OCT_DRIFT_FLOOR + ss_churn) * 2pi`, i.e. a RATE multiplied by an ABSOLUTE clock (LLFrameTimer::getElapsedSeconds, seconds since the viewer started). Both factors of that rate vary with time: deck.mDriftRate and deck.mChurn are resolved from the day-cycle phase every frame (ssvolcloud.cpp update() -> SSAtmoEnvCloudFieldResolver::resolve), and that phase comes from SSAtmoEnvTrack::currentDayCyclePhase(), which at the time was dayCyclePhaseAt((F64)time(nullptr)) - WHOLE SECONDS (that root clock was made continuous in the D3 follow-up, 2026-09-07: it now reads SSAtmoMagic::sharedTime(); this core is unaffected and still required, because integrating a rate is what makes ANY rate change invisible in the phase and what keeps the uniforms in F32 range at long uptimes - see tests/unit_phase_clock.cpp). So the rate is a staircase with a 1 Hz tread, and phase = rate x t turns each tread into a JUMP of t x delta-rate radians: at an hour of uptime a rate change of one part in ten thousand moves the detail phase by a fifth of a cycle, in one frame. The same multiplication also makes the APPARENT rate d(phase)/dt = rate + t x d(rate)/dt, whose second term grows without bound with viewer uptime - the "far too exaggerated" half of the same report. The shaft's fall coordinate (`world_true.z - ss_time * ss_drift_rate * SS_SHAFT_FALL_MPS`) had the identical shape and is fixed the same way.
// <SS:Nexii> THE RULE, stated once: a time-varying rate is INTEGRATED, never multiplied by an absolute clock. A step in the rate then changes only the DERIVATIVE of the animation, which is invisible; multiplied by t it changes the VALUE, which is a jump. Everything here is that integral - `advanceLaps` and `advanceFallM` - plus the wraps that keep the accumulators exact in an F32 uniform forever.
// <SS:Nexii> WHICH CLOCK (PLAN.md lesson 31, the user's rule): ANIMATION runs on the wall clock, position and STATE on the day-cycle timeline. Everything in this header is animation - it advances a texture lookup's phase and a streak's sample point and positions nothing, so no world state depends on it and no two clients need to agree on it. That was already true of what it replaces (ss_time was this client's own elapsed-seconds counter), so this changes nothing about replication; it is recorded here because an accumulator is frame-rate dependent in its last bits where a closed-form clock read is not, and PLAN.md lesson 13's F64 rule is what bounds that: both accumulators are F64 internally and F32 only at the uniform boundary.
// Bodies here are formulas; ssvolcloud.cpp drives them and ssVolCloudF.glsl consumes their two results as uniforms.
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSDeckBoil
{
    // THE RATE, and its retune. LAPS_PER_S is the shader's old SS_OCT_LAPS, moved here because it is the number the
    // report asks to change and a shader const is not a place a rate can be reasoned about. It was 0.06 laps/s: at
    // drift rate 1 and full convection that is 0.075 laps/s, a complete advection cycle every 13.3 s, and the
    // uptime-dependent second term above ran several times faster still. It is 0.012 now - one fifth - so a full
    // cycle takes 66.7 s at maximum convection and 333 s in dead calm (the DRIFT_FLOOR end). The target is the
    // user's own: "meant to be very slow change to a cloud's appearance". A cumulus turret's visible morphology
    // turns over in one to three minutes, so 67 s sits at the fast end of the real range and the calm end is a
    // five-minute evolution; nothing here is faster than the weather it is standing in for.
    // DRIFT_FLOOR is the shader's SS_OCT_DRIFT_FLOOR, unchanged: the share of the rate a dead-calm sky keeps,
    // because even still air is not static. FALL_MPS is the shader's SS_SHAFT_FALL_MPS, unchanged.
    constexpr F32 LAPS_PER_S  = 0.012f;
    constexpr F32 DRIFT_FLOOR = 0.25f;
    constexpr F32 FALL_MPS    = 6.0f;

    // The longest frame that counts as elapsed animation. A hitch, an alt-tab or a teleport hands the caller a dt of
    // seconds to minutes; integrating that whole would jump the phase exactly the way the bug being fixed does. The
    // cap is well above any real frame interval (5 fps) and well below the shortest hitch worth noticing.
    constexpr F32 MAX_DT_S = 0.2f;

    // drive(driftRate, churn): the deck's own animation multiplier - the wind's drift rate times the convection the
    // sky is running, floored so a calm sky still moves. Invariants: >= 0; 0 only at driftRate 0; monotone
    // non-decreasing in both arguments; equals driftRate * DRIFT_FLOOR at churn 0 and driftRate * (DRIFT_FLOOR + 1)
    // at churn 1.
    inline F32 drive(F32 driftRate, F32 churn)
    {
        return llmax(0.f, driftRate) * (DRIFT_FLOOR + llclamp(churn, 0.f, 1.f));
    }

    // lapsRate(driftRate, churn): laps of the advection cycle per second, at this deck's current dials. This is the
    // DERIVATIVE the accumulator integrates; nothing multiplies it by a clock.
    // Invariants: >= 0; <= LAPS_PER_S * (DRIFT_FLOOR + 1) * driftRate.
    inline F32 lapsRate(F32 driftRate, F32 churn)
    {
        return LAPS_PER_S * drive(driftRate, churn);
    }

    // fallRateMPS(driftRate): metres per second the rain curtain's streak sample point travels DOWN. The curtain's
    // fall follows the deck's own drift rate exactly as the boil does, so a still sky's virga hangs and a driven
    // one's pours. Invariants: >= 0; linear in driftRate.
    inline F32 fallRateMPS(F32 driftRate)
    {
        return FALL_MPS * llmax(0.f, driftRate);
    }

    // clampDt(dtS): the integrator's own step, bounded by MAX_DT_S and floored at 0 (a clock that goes backwards -
    // an NTP correction, a wrapped counter - must not run the animation backwards).
    // Invariants: in [0, MAX_DT_S]; the identity on every real frame interval.
    inline F32 clampDt(F32 dtS)
    {
        return llclamp(dtS, 0.f, MAX_DT_S);
    }

    // advanceLaps(laps, driftRate, churn, dtS): ONE integration step of the boil phase, in laps. F64 because a phase
    // that is only ever consumed modulo 1 still has to be summed without drift over a session (PLAN.md lesson 13).
    // Invariants: monotone non-decreasing in dtS; equals laps exactly at dtS 0; the increment is
    // lapsRate(driftRate, churn) * clampDt(dtS).
    inline F64 advanceLaps(F64 laps, F32 driftRate, F32 churn, F32 dtS)
    {
        return laps + (F64)lapsRate(driftRate, churn) * (F64)clampDt(dtS);
    }

    // advanceFallM(fallM, driftRate, dtS): the same step for the curtain's fall, in metres.
    // Invariants: monotone non-decreasing in dtS; equals fallM exactly at dtS 0.
    inline F64 advanceFallM(F64 fallM, F32 driftRate, F32 dtS)
    {
        return fallM + (F64)fallRateMPS(driftRate) * (F64)clampDt(dtS);
    }

    // wrapLaps(laps): the value handed to the shader. The fragment stage only ever takes fract() of the phase, so
    // folding the accumulator into [0, 1) before it crosses into F32 is EXACT - it removes no information and it
    // means the uniform keeps its full 24 bits of mantissa however long the viewer has been open, which the raw
    // `ss_time` product never did (at ten hours of uptime an F32 second-count has ~4 ms of resolution and the
    // product's phase had correspondingly less).
    // Invariants: in [0, 1); wrapLaps(x + n) == wrapLaps(x) for integer n; 0 at 0.
    inline F32 wrapLaps(F64 laps)
    {
        const F64 w = laps - std::floor(laps);
        return (F32)llclamp(w, 0.0, 0.9999999);
    }

    // wrapFallM(fallM, tileM): the same for the curtain's fall, folded on the noise map's own tile PERIOD. The streak
    // coordinate is (world z - fall) / tileM into a wrapping texture, so subtracting a whole number of tileM metres
    // is exact and the shader cannot tell the fold happened. A tileM of zero or less returns 0 rather than dividing.
    // Invariants: in [0, tileM) for tileM > 0; wrapFallM(m + k * tileM, tileM) == wrapFallM(m, tileM) for integer k;
    // 0 for tileM <= 0.
    inline F32 wrapFallM(F64 fallM, F32 tileM)
    {
        if (!(tileM > 0.f))
        {
            return 0.f;
        }
        const F64 t = (F64)tileM;
        const F64 w = fallM - std::floor(fallM / t) * t;
        return (F32)llclamp(w, 0.0, t - 1e-6);
    }

    // <SS:Nexii> THE FAILING CONTROL, kept here rather than in the test because it is the SHIPPED spelling and the
    // rung's job is to show in numbers what it did: phase = rate x absolute clock. oldLaps is what
    // ssVolCloudF.glsl's `turn / 6.2831853` evaluated to before this header existed, with OLD_LAPS_PER_S the
    // constant it used. Any test that wants to demonstrate the 1 Hz tread calls this at two clock values a
    // quantised phase apart and measures the step.
    constexpr F32 OLD_LAPS_PER_S = 0.06f;
    inline F64 oldLaps(F32 elapsedS, F32 driftRate, F32 churn)
    {
        return (F64)elapsedS * (F64)OLD_LAPS_PER_S * (F64)driftRate * (F64)(DRIFT_FLOOR + llclamp(churn, 0.f, 1.f));
    }
}

#endif
