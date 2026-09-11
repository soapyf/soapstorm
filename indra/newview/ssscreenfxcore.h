/**
 * @file ssscreenfxcore.h
 * @brief Atmo Magic: screen-space weather effects state - thermal shock and mirage strength, lens drops and condensation, the height fog demand laws and the density profile the fog shader marches. Header-only core. CONTRACT.
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

#ifndef SS_SCREENFXCORE_H
#define SS_SCREENFXCORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_surface_weather.md sections 7-9. Three
// screen-space effects share it: the heat shimmer (a mirage post pass whose strength comes from a THERMAL SHOCK accumulator - heat
// turned up quickly from any temperature - plus a sunny-day baseline, both suppressed by wind and rain), the lens drops (a rain-on-window
// post pass whose demand follows the camera's pitch against the rain's direction, with a wet-lens pulse when the camera surfaces from
// water and a condensation layer that trails clear), and the height fog (one screen-space layer marched against the surface field,
// summing several demand sources: ground fog from humidity, a precipitation veil, the squall whiteout and the drift band the whiteout
// pass already had). These are PRESENTATION: they run on the frame's dt (wall clock), never touch world state, and are allowed to differ
// per client. Still pure: no settings, no singletons, no clock but the dt handed in; every closed form below is what the twin test
// transliterates from the GLSL. Bodies marked STUB are the implementer's.
#include "lldefs.h"
#include "sssheltercore.h"

#include <cmath>
#include <cstdint>

namespace SSScreenFX
{
    // <SS:Nexii> GLSL-semantics smoothstep, local so this core needs nothing beyond lldefs.h: 0 for x <= e0, 1 for x >= e1, monotone.
    inline F32 smoothstep(F32 e0, F32 e1, F32 x)
    {
        if (e1 <= e0)
        {
            return (x <= e0) ? 0.f : 1.f;
        }
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // ------------------------------------------------------------------ thermal shock and mirage

    constexpr F32 SHOCK_PER_C        = 0.12f;   // shock gained per degree of temperature RISE (a 10 C jump gives ~1.2 -> clamps to SHOCK_MAX)
    constexpr F32 SHOCK_MAX          = 1.5f;    // accumulator ceiling
    constexpr F32 SHOCK_TAU_S        = 180.f;   // e-folding time of the shock: three minutes to fall to 37%
    constexpr F32 SHOCK_RATE_MIN_CPS = 0.02f;   // rises slower than this (degrees per second) are weather, not shock, and add nothing
    constexpr F32 BASELINE_T_LO_C    = 24.f;    // sunny-day baseline shimmer starts here...
    constexpr F32 BASELINE_T_HI_C    = 40.f;    // ... and is full here
    constexpr F32 WIND_SUPPRESS_LO   = 3.f;     // m/s: wind starts flattening the shimmer...
    constexpr F32 WIND_SUPPRESS_HI   = 9.f;     // ... and kills it here
    constexpr F32 ZOOM_REF_FOV_DEG   = 60.f;    // the mirage multiplies by ZOOM_REF_FOV_DEG / fov (narrow lens = more shimmer)
    constexpr F32 ZOOM_MAX           = 4.f;     // ... capped
    constexpr F32 MIRAGE_NEAR_M      = 6.f;     // no distortion inside this depth (foreground stays crisp), LOCKSTEP ssPostHeatF.glsl
    constexpr F32 MIRAGE_FULL_M      = 60.f;    // full distortion weight from this depth on, LOCKSTEP ssPostHeatF.glsl

    struct Thermal
    {
        F32 mShock = 0.f;
        F32 mLastTempC = 0.f;
        bool mValid = false;   // false until the first step seeds mLastTempC (the first observed temperature is not a jump)
    };

    // One frame of the shock accumulator. rise = tempC - mLastTempC (only positive rises count); if rise / dt >= SHOCK_RATE_MIN_CPS the
    // shock gains rise * SHOCK_PER_C; then the shock decays by exp(-dt / SHOCK_TAU_S); clamped to [0, SHOCK_MAX]; mLastTempC = tempC.
    // The first call (mValid false) only seeds. Invariants: a constant temperature decays the shock toward 0 and never below; a FALL
    // in temperature adds nothing; the gain from a jump depends on the jump size, not on dt (a 10 C jump adds the same at 30 and 144
    // fps); decay over two steps of dt equals decay over one step of 2 dt within 1e-5 (closed-form exp); dt 0 changes nothing at all -
    // not even mLastTempC - so a jump landing on a paused frame fires on the next real one.
    inline void stepThermal(Thermal& t, F32 tempC, F32 dt)
    {
        if (!t.mValid)
        {
            t.mLastTempC = tempC;
            t.mValid = true;
            return;
        }
        if (dt <= 0.f) return;   // <SS:Nexii> a paused/zero-length frame must not consume the temperature sample: mLastTempC stays put so the real jump is still visible next frame.

        const F32 rise = tempC - t.mLastTempC;
        if (rise > 0.f && dt > 0.f && (rise / dt) >= SHOCK_RATE_MIN_CPS)
        {
            t.mShock += rise * SHOCK_PER_C;
        }
        t.mShock *= std::exp(-dt / SHOCK_TAU_S);
        t.mShock = llclamp(t.mShock, 0.f, SHOCK_MAX);
        t.mLastTempC = tempC;
    }

    // The sunny-day baseline: clamp((tempC - BASELINE_T_LO_C) / (BASELINE_T_HI_C - BASELINE_T_LO_C), 0, 1) * sunUp01. Invariants: 0 below
    // BASELINE_T_LO_C; 0 at sunUp 0; monotone in both; <= 1.
    inline F32 mirageBaseline(F32 tempC, F32 sunUp01)
    {
        return llclamp((tempC - BASELINE_T_LO_C) / (BASELINE_T_HI_C - BASELINE_T_LO_C), 0.f, 1.f) * sunUp01;
    }

    // Wind suppression: 1 - smoothstep(WIND_SUPPRESS_LO, WIND_SUPPRESS_HI, windMs). Rain suppression: 1 - rain01. Zoom: clamp(ZOOM_REF_FOV_DEG /
    // max(fovDeg, 1), 1, ZOOM_MAX). strength = clamp(baseline + shock, 0, SHOCK_MAX) * windSuppress * rainSuppress * zoom. Invariants: 0 when
    // both baseline and shock are 0; 0 at windMs >= WIND_SUPPRESS_HI; 0 at rain01 1; monotone in shock and baseline; non-increasing in
    // wind and rain; the zoom factor is 1 at fov >= 60 and never above ZOOM_MAX; result <= SHOCK_MAX * ZOOM_MAX (6.0) - the heat shader
    // is told this ceiling.
    inline F32 mirageStrength(F32 baseline, F32 shock, F32 windMs, F32 rain01, F32 fovDeg)
    {
        const F32 windSuppress = 1.f - smoothstep(WIND_SUPPRESS_LO, WIND_SUPPRESS_HI, windMs);
        const F32 rainSuppress = 1.f - rain01;
        const F32 zoom = llclamp(ZOOM_REF_FOV_DEG / llmax(fovDeg, 1.f), 1.f, ZOOM_MAX);
        return llclamp(baseline + shock, 0.f, SHOCK_MAX) * windSuppress * rainSuppress * zoom;
    }

    // ------------------------------------------------------------------ lens drops

    constexpr F32 LENS_UP_LO         = 0.05f;   // camera pitch (sin of elevation, -1 down .. 1 up) where the looking-up accumulation starts...
    constexpr F32 LENS_UP_HI         = 0.70f;   // ... and is full
    constexpr F32 LENS_DOWN_CUT      = -0.35f;  // below this pitch nothing lands on the lens
    constexpr F32 LENS_AHEAD_GAIN    = 0.5f;    // looking level, drops land in proportion to how much the rain comes AT the lens, up to this
    constexpr F32 LENS_WET_IN_S      = 1.5f;    // demand fills the lens over this
    constexpr F32 LENS_WET_OUT_S     = 6.f;     // ... and it drains over this once demand stops
    constexpr F32 LENS_SURFACE_PULSE = 1.f;     // wet set to this when the camera surfaces from water
    constexpr F32 LENS_FOG_IN_S      = 4.f;     // condensation grows over this...
    constexpr F32 LENS_FOG_OUT_S     = 12.f;    // ... and clears over this
    constexpr F32 LENS_FOG_COLD_C    = 8.f;     // condensation demand is full at or below this air temperature...
    constexpr F32 LENS_FOG_WARM_C    = 20.f;    // ... and 0 at or above this
    constexpr F32 LENS_WIND_SLIDE    = 0.15f;   // screen-space slide direction = gravity + LENS_WIND_SLIDE * wind (m/s), see slideDir
    // <SS:Nexii> SHELTER COLLAPSE (2026-09-07). This was the last of three duplicated spellings of "is this point under cover" -
    // ssprecipitation.cpp is already collapsed onto SSShelter; this was the remaining one, in the lens's own fence. LENS_COVER_TOL_M is
    // gone as a local literal; SSShelter::COVER_TOL_M is the one source of the threshold, and lensExposure below now calls
    // SSShelter::coverExposure directly rather than spelling its own comparison.
    //
    // NOT A BIT-IDENTICAL NO-OP, and the collapse also exposed that this consumer needs its own THRESHOLD. The R1-R5 build's
    // lensExposure had two independently-tuned numbers, a 2 m threshold and a separate 1.5 m soft width, ramping over 2.0-3.5 m.
    // SSShelter::coverExposure has one number and its ramp is always exactly tol_m wide starting at tol_m. Collapsing onto it with
    // the SIM's 2 m default was wrong for the lens and is corrected below - see LENS_COVER_TOL_M.
    // Invariants: 1 when unresolved; 1 when hitZ <= camZ (nothing above); 0 when hitZ - camZ >= 2 * LENS_COVER_TOL_M; monotone
    // non-increasing in hitZ - camZ; in [0,1]; C1 at both ends of the band.
    // <SS:Nexii> The lens's OWN cover tolerance, and why it cannot be the sim's. SSShelter::coverExposure remains the one formula -
    // only this argument differs, which is exactly what its tol_m parameter is for - but SSShelter::COVER_TOL_M's 2 m is calibrated
    // for the PRECIPITATION SIM's geometry, where the question is whether a hit above a falling drop is a roof or the ground that
    // drop is heading for, and 2 m separates those cleanly. The lens asks a different question about a different pair of heights:
    // hitZ is the column's top and camZ is the EYE, so indoors the gap is a ceiling's CLEARANCE above your head - about 1.3 m under
    // a 3 m ceiling with the camera at 1.7 m. Against a 2 m tolerance that read as NOT COVER, so the shelter gate did nothing in any
    // ordinary interior, and full shelter would have needed 4 m of clearance - a 5.7 m ceiling. 0.25 m instead: the ramp runs
    // 0.25 -> 0.50 m of clearance, so anything meaningfully overhead shelters while a surface at or below the eye still does not, and
    // a camera clipped inside geometry reads sheltered, which is the right answer. OWED: a rung pinning exposure across a clearance
    // sweep at named interior heights (2.4 / 3.0 / 3.5 m ceilings against a 1.7 m eye must all read 0) with the 2 m tolerance as the
    // failing control - this constant is stated here, not yet demonstrated. [interaction: SSShelter, sssheltercore.h]
    constexpr F32 LENS_COVER_TOL_M = 0.25f;

    inline F32 lensExposure(F32 hitZ, F32 camZ, bool resolved)
    {
        return SSShelter::coverExposure(resolved, hitZ, camZ, LENS_COVER_TOL_M);
    }

    // <SS:Nexii> forward declaration - full definition (and contract comment) lives with the height-fog relax below; stepLens shares it.
    inline F32 relax(F32 x, F32 target, F32 tauIn, F32 tauOut, F32 dt);

    struct Lens
    {
        F32 mWet = 0.f;   // how many drops sit on the lens, 0..1
        F32 mFog = 0.f;   // condensation, 0..1
    };

    // Demand from the view: pitch = sin of the camera's elevation (-1 straight down, 1 straight up); rainAlign = dot(-rainDir, viewDir)
    // in [-1,1] (1 when the rain flies straight at the lens); intensity 0..1. up = smoothstep(LENS_UP_LO, LENS_UP_HI, pitch);
    // ahead = clamp(rainAlign, 0, 1) * LENS_AHEAD_GAIN * (1 - up); cut = smoothstep(LENS_DOWN_CUT, LENS_DOWN_CUT + 0.25, pitch);
    // demand = intensity * (up + ahead) * cut, clamped to [0,1]. Invariants: 0 at intensity 0; 0 at pitch <= LENS_DOWN_CUT; 1 at pitch 1,
    // intensity 1; level view with rain from behind (rainAlign -1) gives 0; monotone in pitch for pitch >= LENS_DOWN_CUT at fixed
    // rainAlign >= 0; monotone in rainAlign; <= 1.
    inline F32 lensDemand(F32 pitch, F32 rainAlign, F32 intensity)
    {
        const F32 up = smoothstep(LENS_UP_LO, LENS_UP_HI, pitch);
        const F32 ahead = llclamp(rainAlign, 0.f, 1.f) * LENS_AHEAD_GAIN * (1.f - up);
        const F32 cut = smoothstep(LENS_DOWN_CUT, LENS_DOWN_CUT + 0.25f, pitch);
        return llclamp(intensity * (up + ahead) * cut, 0.f, 1.f);
    }

    // One frame of the lens state. underwater: both channels snap to 0 (water clears the lens). surfaced (the frame the camera came
    // out of water): mWet = max(mWet, LENS_SURFACE_PULSE). Otherwise mWet relaxes toward demand with tau LENS_WET_IN_S when demand >
    // mWet, LENS_WET_OUT_S when below (exponential relax: x += (target - x) * (1 - exp(-dt / tau))). Condensation target = cold01 *
    // max(mWet, surfaced ? 1 : 0), where mWet is the value AFTER this frame's wet relax above (not the raw demand) and cold01 =
    // clamp((LENS_FOG_WARM_C - tempC) / (LENS_FOG_WARM_C - LENS_FOG_COLD_C), 0, 1); mFog relaxes
    // toward it with LENS_FOG_IN_S up, LENS_FOG_OUT_S down. Invariants: underwater -> both 0 regardless of demand; surfaced -> mWet >=
    // LENS_SURFACE_PULSE that frame; demand 0 for long enough -> mWet -> 0 (below 0.01 after 10 * LENS_WET_OUT_S); a warm lens (tempC >=
    // LENS_FOG_WARM_C) never fogs; both in [0,1]; the relax is frame-rate independent in the limit (two steps of dt land within 1e-3 of
    // one step of 2 dt for the same constant demand).
    inline void stepLens(Lens& l, F32 demand, F32 tempC, bool underwater, bool surfaced, F32 dt)
    {
        if (underwater)
        {
            l.mWet = 0.f;
            l.mFog = 0.f;
            return;
        }

        if (surfaced)
        {
            l.mWet = llmax(l.mWet, LENS_SURFACE_PULSE);
        }
        else
        {
            l.mWet = relax(l.mWet, demand, LENS_WET_IN_S, LENS_WET_OUT_S, dt);
        }

        const F32 cold01 = llclamp((LENS_FOG_WARM_C - tempC) / (LENS_FOG_WARM_C - LENS_FOG_COLD_C), 0.f, 1.f);
        const F32 fogTarget = cold01 * llmax(l.mWet, surfaced ? 1.f : 0.f);
        l.mFog = relax(l.mFog, fogTarget, LENS_FOG_IN_S, LENS_FOG_OUT_S, dt);
    }

    // The screen-space direction drops slide in: gravity projected into the screen (gx, gy: world -Z through the view, already in screen
    // units, length 0 when looking straight up or down) plus LENS_WIND_SLIDE times the wind's screen projection (wx, wy in m/s screen
    // units). Normalised; if the sum's length is below 1e-4 the result is (0, -1) (straight down the screen). Out: dx, dy. Invariants: unit
    // length; with zero wind and a level view the direction is (0, -1) within 1e-5 when gravity projects to (0, -g); with gravity's
    // projection 0 the direction is the wind's; pure.
    inline void slideDir(F32 gx, F32 gy, F32 wx, F32 wy, F32& dx, F32& dy)
    {
        const F32 sx = gx + LENS_WIND_SLIDE * wx;
        const F32 sy = gy + LENS_WIND_SLIDE * wy;
        const F32 len = std::sqrt(sx * sx + sy * sy);
        if (len < 1.0e-4f)
        {
            dx = 0.f;
            dy = -1.f;
            return;
        }
        dx = sx / len;
        dy = sy / len;
    }

    // ------------------------------------------------------------------ height fog

    constexpr F32 FOG_HUMIDITY_LO    = 0.70f;   // ground fog demand starts at this humidity...
    constexpr F32 FOG_HUMIDITY_HI    = 0.95f;   // ... and is full here
    constexpr F32 FOG_WIND_LO        = 2.f;     // wind starts blowing ground fog away...
    constexpr F32 FOG_WIND_HI        = 6.f;     // ... gone here
    constexpr F32 FOG_SUN_BURN       = 0.6f;    // full sun burns off this share of the ground fog demand
    constexpr F32 FOG_GROUND_SCALE_M = 6.f;     // ground fog scale height (density e-folds over this)
    constexpr F32 FOG_PRECIP_SCALE_M = 60.f;    // precipitation veil scale height (it fills the column)
    constexpr F32 FOG_PRECIP_RAIN    = 0.25f;   // veil demand at rain intensity 1
    constexpr F32 FOG_PRECIP_SNOW    = 0.60f;   // ... at snow intensity 1 (snow hides more)
    constexpr F32 FOG_PRECIP_POW     = 1.5f;    // veil demand = intensity^FOG_PRECIP_POW * (rain or snow figure)
    constexpr F32 FOG_RAMP_IN_S      = 20.f;    // demand curves ramp in over this...
    constexpr F32 FOG_RAMP_OUT_S     = 45.f;    // ... and out over this (the shell relaxes every source with these)

    // Ground fog demand: smoothstep(FOG_HUMIDITY_LO, FOG_HUMIDITY_HI, humidity) * (1 - smoothstep(FOG_WIND_LO, FOG_WIND_HI, windMs)) *
    // (1 - FOG_SUN_BURN * sunUp01). Invariants: 0 below FOG_HUMIDITY_LO; 0 at windMs >= FOG_WIND_HI; monotone in humidity; non-increasing
    // in wind and sun; in [0,1].
    inline F32 groundFogDemand(F32 humidity, F32 windMs, F32 sunUp01)
    {
        return smoothstep(FOG_HUMIDITY_LO, FOG_HUMIDITY_HI, humidity)
             * (1.f - smoothstep(FOG_WIND_LO, FOG_WIND_HI, windMs))
             * (1.f - FOG_SUN_BURN * sunUp01);
    }

    // Precipitation veil demand: pow(intensity, FOG_PRECIP_POW) * (granular ? FOG_PRECIP_SNOW : FOG_PRECIP_RAIN). Invariants: 0 at
    // intensity 0; monotone; snow >= rain at equal intensity; <= FOG_PRECIP_SNOW.
    inline F32 precipVeilDemand(F32 intensity, bool granular)
    {
        return std::pow(intensity, FOG_PRECIP_POW) * (granular ? FOG_PRECIP_SNOW : FOG_PRECIP_RAIN);
    }

    // The density profile the fog shader marches, LOCKSTEP ssPostFogF.glsl ssFogDensityAt: h = height above the surface (metres, >= 0),
    // ground/precip/squall are the ramped demand scalars, squallScaleM the squall's own depth scale (10..100 m, set by the shell), lift
    // the drift-band demand and bandM its height: density = ground * exp(-h / FOG_GROUND_SCALE_M) + precip * exp(-h / FOG_PRECIP_SCALE_M) +
    // squall * exp(-h / max(squallScaleM, 4)) + (h < bandM ? lift : 0). Invariants: 0 when every demand is 0; non-increasing in h for the
    // three exponential terms (the band term is a step); at h 0 the sum equals ground + precip + squall + lift; linear in each demand.
    inline F32 fogDensityAt(F32 h, F32 ground, F32 precip, F32 squall, F32 squallScaleM, F32 lift, F32 bandM)
    {
        return ground * std::exp(-h / FOG_GROUND_SCALE_M)
             + precip * std::exp(-h / FOG_PRECIP_SCALE_M)
             + squall * std::exp(-h / llmax(squallScaleM, 4.f))
             + (h < bandM ? lift : 0.f);
    }

    // Exponential relax toward a target with asymmetric time constants: x += (target - x) * (1 - exp(-dt / (target > x ? tauIn : tauOut))).
    // Invariants: never overshoots; dt 0 -> unchanged; monotone toward the target; two steps of dt within 1e-5 of one step of 2 dt.
    inline F32 relax(F32 x, F32 target, F32 tauIn, F32 tauOut, F32 dt)
    {
        const F32 tau = (target > x) ? tauIn : tauOut;
        return x + (target - x) * (1.f - std::exp(-dt / tau));
    }
}

#endif
