/**
 * @file ssatmoinfoviewcore.h
 * @brief Atmo Magic: the info-view chart/legend geometry helpers, header-only core.
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

#ifndef SS_ATMOINFOVIEWCORE_H
#define SS_ATMOINFOVIEWCORE_H

// <SS:Nexii> A CORE header (lldefs.h + <cmath> + <cstdint> only; NO llmath.h). Design: doc/atmo_magic_debug_views.md sections 1 and 3. The pure geometry the info views share: the colour ramps of the one colour language (section 1 point 4), the altitude ladder the wind-profile chart samples windAt(z) on, the rung table the in-world wind mast stacks its arrows on, and the pixel mappings of the chart and its hodograph inset. Everything here is a pure function of its arguments so the scratch harness can pin it; nothing here reads a setting, the camera or a system - the shell (ssatmoinfoview.cpp) does the reading and hands the numbers in. None of it positions world content, so it is free of the determinism contract - but it must never be used to.
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSAtmoInfoViewCore
{
    // <SS:Nexii> THE AUTHORITATIVE INFO-VIEW NUMBERING (doc/atmo_magic_debug_views.md section 2). Every V-number in the catalogue is named here - including the ones nobody has built yet - because two docs once handed the same number to two different views (atmo_magic_debug_views.md gave V6 to World Field, atmo_magic_phase8_show.md section 6 item 5 gave V6 to Anatomy) and neither shipped, so nothing in code settled it. Settled 2026-09-06: V6 stays World Field (the older claim, made by the doc that owns the numbering scheme) and Anatomy moves to V8, because V7 is NOT free - the catalogue's V7 is the sync console, referenced as V7 by atmo_magic_phase8_show.md section 1 as well. The console is a stackable SSStatsView sibling and never an exclusive mode, so MODE_RESERVED_CONSOLE exists ONLY to burn the number: assigning 7 to the SSAtmoInfoView setting is a bug, and modeIsBuilt(7) is false. RESERVED entries are declarations of intent, not switch cases - the shell's dispatches fall through to their "not implemented yet" branches until someone builds them. ADD A NEW VIEW ONLY BY TAKING MODE_COUNT's value and moving MODE_COUNT up; never renumber an entry above.
    constexpr U32 MODE_OFF               = 0;
    constexpr U32 MODE_WIND_PROFILE      = 1;
    constexpr U32 MODE_STORM_CELLS       = 2;
    constexpr U32 MODE_DECK_LOD          = 3;
    constexpr U32 MODE_PRECIP_VIRGA      = 4;
    constexpr U32 MODE_WEATHER_CUBE      = 5;
    constexpr U32 MODE_WORLD_FIELD       = 6;   // RESERVED, not built: the existing RENDER_DEBUG_WORLD_FIELD overlay restyled under the shared legend/palette (backlog 5.6)
    constexpr U32 MODE_RESERVED_CONSOLE  = 7;   // NEVER A MODE: catalogue V7 is the sync console, a stackable console beside the info views
    constexpr U32 MODE_ANATOMY           = 8;   // RESERVED, not built: storm anatomy (phase 8b) - moved here from its clashing V6 claim
    constexpr U32 MODE_LIGHTNING         = 9;
    constexpr U32 MODE_ACOUSTICS         = 10;
    constexpr U32 MODE_COUNT             = 11;

    // The catalogue name of a mode, as the legend title and the picker's own combo row print it. Invariants: every constant above (0 .. MODE_COUNT-1) maps to its own distinct, non-empty string; the three unbuilt numbers carry their status in the string itself; any value at or above MODE_COUNT reads "?".
    inline const char* modeLabel(U32 mode)
    {
        switch (mode)
        {
            case MODE_OFF:              return "off";
            case MODE_WIND_PROFILE:     return "V1  WIND PROFILE";
            case MODE_STORM_CELLS:      return "V2  STORM CELLS";
            case MODE_DECK_LOD:         return "V3  DECK LOD";
            case MODE_PRECIP_VIRGA:     return "V4  PRECIP & VIRGA";
            case MODE_WEATHER_CUBE:     return "V5  WEATHER CUBE";
            case MODE_WORLD_FIELD:      return "V6  WORLD FIELD (reserved, not built)";
            case MODE_RESERVED_CONSOLE: return "V7  SYNC CONSOLE (not an info view)";
            case MODE_ANATOMY:          return "V8  ANATOMY (reserved, not built)";
            case MODE_LIGHTNING:        return "V9  LIGHTNING";
            case MODE_ACOUSTICS:        return "V10 ACOUSTICS";
        }
        return "?";
    }

    // Whether a mode has a layer, a legend and a chart behind it today. Invariants: true for MODE_OFF and every built view; false for exactly MODE_WORLD_FIELD, MODE_RESERVED_CONSOLE and MODE_ANATOMY; false at or above MODE_COUNT.
    inline bool modeIsBuilt(U32 mode)
    {
        if (mode >= MODE_COUNT)
        {
            return false;
        }
        return mode != MODE_WORLD_FIELD && mode != MODE_RESERVED_CONSOLE && mode != MODE_ANATOMY;
    }

    // <SS:Nexii> V10 Acoustics helpers (doc/atmo_magic_acoustics.md, the V10 debug
    // ask). Plain S32 constants mirroring SSWorldField::EAirLabel's values one for
    // one, the same idiom the VORTEX_KIND_* mirrors use - the shell casts at the
    // read site and this core keeps its no-llmath rule. The probe label colours
    // reuse the world field overlay's own air-state language (outdoors green,
    // sheltered amber, interior red) so the two readouts agree.
    constexpr S32 AIR_LABEL_SOLID    = 0;
    constexpr S32 AIR_LABEL_OUTDOORS = 1;
    constexpr S32 AIR_LABEL_SHELTERED = 2;
    constexpr S32 AIR_LABEL_INTERIOR = 3;
    constexpr S32 AIR_LABEL_UNKNOWN  = 4;

    // The core's colour type: the ramps below return it and the shell's toColor lifts it into LLColor4.
    struct RGB
    {
        F32 r = 0.f;
        F32 g = 0.f;
        F32 b = 0.f;
    };

    inline RGB airLabelColor(S32 label)
    {
        switch (label)
        {
            case AIR_LABEL_OUTDOORS:  return { 0.30f, 1.00f, 0.40f };
            case AIR_LABEL_SHELTERED: return { 1.00f, 0.80f, 0.20f };
            case AIR_LABEL_INTERIOR:  return { 1.00f, 0.25f, 0.25f };
            default:                  return { 0.55f, 0.60f, 0.70f };
        }
    }

    inline const char* airLabelName(S32 label)
    {
        switch (label)
        {
            case AIR_LABEL_OUTDOORS:  return "outdoors";
            case AIR_LABEL_SHELTERED: return "sheltered";
            case AIR_LABEL_INTERIOR:  return "interior";
            default:                  return "?";
        }
    }

    // Probe marker half-size from the baked room volume: sqrt of the volume, clamped
    // to a readable band in metres, so a closet's probe stays small and a hall's
    // reads big without dominating. Invariants: in [0.35, 2.5]; monotone
    // non-decreasing in volume; non-positive volume gives the floor.
    inline F32 probeMarkerM(F32 volume)
    {
        const F32 v = (volume > 0.f) ? volume : 0.f;
        return llclamp(sqrtf(v) * 0.03f, 0.35f, 2.5f);
    }

    // RT60 colour: the energy ramp (orange -> red) over a 0..3 s band, so a dry
    // studio reads amber and a cathedral reads red. Invariants: monotone in rt60
    // over the band; clamps at both ends; channels in [0,1].
    inline RGB energyRamp(F32 value, F32 max);
    inline RGB rt60Color(F32 rt60)
    {
        return energyRamp(rt60, 3.f);
    }

    // A metre-space 2D vector for the display geometry below - not SSVirga::Vec2 or SSStormCell::Vec2 (neither
    // core may include this one, nor this one theirs); every crossing at a shell read site is a copy, same idiom
    // as ssatmoinfoview.cpp's own toStormVec2.
    struct Vec2M
    {
        F32 x = 0.f;
        F32 y = 0.f;
    };

    struct PixelPoint
    {
        S32 x = 0;
        S32 y = 0;
    };

    // A chart's plotting rectangle in view pixels: left/bottom corner plus size. Axes run right (x) and up (y).
    struct ChartBox
    {
        S32 left   = 0;
        S32 bottom = 0;
        S32 w      = 1;
        S32 h      = 1;
    };

    // Unit fraction of value in [0, max], NaN-safe: a non-positive max reads any positive value as full scale and everything else as zero.
    inline F32 unitOf(F32 value, F32 max)
    {
        if (!(max > 0.f))
        {
            return value > 0.f ? 1.f : 0.f;
        }
        return llclamp(value / max, 0.f, 1.f);
    }

    // Wind speed ramp: dark -> bright teal (design colour language). Invariants: every channel monotone non-decreasing in speed; speed 0 gives the dark end exactly; speed >= max gives the bright end exactly; all channels in [0,1].
    inline RGB speedRamp(F32 speedMS, F32 maxMS)
    {
        const F32 t = unitOf(speedMS, maxMS);
        RGB c;
        c.r = std::lerp(0.02f, 0.55f, t);
        c.g = std::lerp(0.20f, 0.96f, t);
        c.b = std::lerp(0.24f, 0.92f, t);
        return c;
    }

    // Presence / coverage ramp: grey -> white. Same invariants as speedRamp.
    inline RGB presenceRamp(F32 value, F32 max)
    {
        const F32 t = unitOf(value, max);
        RGB c;
        c.r = std::lerp(0.30f, 1.f, t);
        c.g = c.r;
        c.b = c.r;
        return c;
    }

    // Energy / convection ramp: orange -> red. Same invariants as speedRamp (r is constant-bright, g falls, so "monotone" is per channel in either direction: r non-decreasing, g non-increasing, b non-increasing).
    inline RGB energyRamp(F32 value, F32 max)
    {
        const F32 t = unitOf(value, max);
        RGB c;
        c.r = 1.f;
        c.g = std::lerp(0.62f, 0.08f, t);
        c.b = std::lerp(0.10f, 0.05f, t);
        return c;
    }

    // The smallest "nice" axis ceiling (1, 2 or 5 times a power of ten) that is >= v, never below 1. Invariants: result >= v; result >= 1; for v >= 1 result < 2.5 * v; idempotent (niceMax(niceMax(v)) == niceMax(v)); NaN or negative v gives 1.
    inline F32 niceMax(F32 v)
    {
        if (!(v > 1.f))
        {
            return 1.f;
        }
        const F32 e = std::floor(std::log10(v));
        const F32 p = std::pow(10.f, e);
        const F32 m = v / p;
        F32 step;
        if (m <= 1.f)      step = 1.f;
        else if (m <= 2.f) step = 2.f;
        else if (m <= 5.f) step = 5.f;
        else               step = 10.f;
        F32 r = step * p;
        // Guard the floating seam: pow/log10 rounding can land a hair under v.
        if (r < v)
        {
            r = (step >= 10.f) ? 20.f * p : (step >= 5.f ? 10.f * p : (step >= 2.f ? 5.f * p : 2.f * p));
        }
        return r;
    }

    // The chart's altitude ladder: sample i of n from 0 to topAgl, quadratically biased toward the ground where the power law does all its bending. Invariants: i=0 gives 0; i=n-1 gives topAgl exactly; strictly increasing in i for topAgl > 0; n < 2 gives topAgl.
    inline F32 altitudeLadder(F32 topAgl, S32 n, S32 i)
    {
        if (n < 2)
        {
            return topAgl;
        }
        const S32 k = llclamp(i, 0, n - 1);
        if (k == n - 1)
        {
            return topAgl;
        }
        const F32 t = (F32)k / (F32)(n - 1);
        return topAgl * t * t;
    }

    // The in-world mast's rung altitudes AGL: a fixed low table where shear is steep, then MAST_STEP_M steps, then the top itself. mastRungCount(top) rungs, mastRungAgl(i, top) the i-th. Invariants: every rung in (0, top]; strictly increasing in i; the last rung == top; top <= 0 gives zero rungs; rung 0 == MAST_LOW_RUNGS[0] whenever top exceeds it.
    constexpr S32 MAST_LOW_RUNG_COUNT = 10;
    constexpr F32 MAST_LOW_RUNGS[MAST_LOW_RUNG_COUNT] = { 10.f, 25.f, 50.f, 100.f, 200.f, 350.f, 500.f, 750.f, 1000.f, 1500.f };
    constexpr F32 MAST_STEP_M = 500.f;

    inline S32 mastRungCount(F32 topAgl)
    {
        if (!(topAgl > 0.f))
        {
            return 0;
        }
        S32 n = 0;
        for (S32 i = 0; i < MAST_LOW_RUNG_COUNT; ++i)
        {
            if (MAST_LOW_RUNGS[i] < topAgl) ++n;
        }
        const F32 last_low = MAST_LOW_RUNGS[MAST_LOW_RUNG_COUNT - 1];
        if (topAgl > last_low)
        {
            // Steps strictly between the last low rung and the top.
            const S32 k_first = (S32)floor(last_low / MAST_STEP_M) + 1;          // 4 -> 2000m
            const S32 k_last  = (S32)std::ceil(topAgl / MAST_STEP_M) - 1;         // largest k with k*step < top
            if (k_last >= k_first) n += k_last - k_first + 1;
        }
        return n + 1; // the top itself
    }

    inline F32 mastRungAgl(S32 i, F32 topAgl)
    {
        const S32 n = mastRungCount(topAgl);
        if (n <= 0)
        {
            return 0.f;
        }
        const S32 k = llclamp(i, 0, n - 1);
        if (k == n - 1)
        {
            return topAgl;
        }
        S32 low = 0;
        for (S32 j = 0; j < MAST_LOW_RUNG_COUNT; ++j)
        {
            if (MAST_LOW_RUNGS[j] < topAgl) ++low;
        }
        if (k < low)
        {
            return MAST_LOW_RUNGS[k];
        }
        const F32 last_low = MAST_LOW_RUNGS[MAST_LOW_RUNG_COUNT - 1];
        const S32 k_first = (S32)floor(last_low / MAST_STEP_M) + 1;
        return (F32)(k_first + (k - low)) * MAST_STEP_M;
    }

    // Arrow length for a mast rung: a fraction of the gap to the next rung, scaled by speed but floored so a calm rung still shows its heading. Invariants: in [0.08, 1] * gapM * 0.45; monotone in speed.
    inline F32 mastArrowLen(F32 speedMS, F32 maxMS, F32 gapM)
    {
        const F32 t = llclamp(unitOf(speedMS, maxMS), 0.08f, 1.f);
        return llmax(gapM, 0.f) * 0.45f * t;
    }

    // Pixel x of a speed on the chart's horizontal axis. Invariants: 0 -> box.left; >= max -> box.left + box.w; monotone.
    inline S32 chartX(F32 speedMS, F32 maxMS, const ChartBox& box)
    {
        return box.left + (S32)floor(unitOf(speedMS, maxMS) * (F32)box.w + 0.5f);
    }

    // Pixel y of an altitude on the chart's vertical axis. Invariants: 0 -> box.bottom; >= top -> box.bottom + box.h; monotone.
    inline S32 chartY(F32 agl, F32 topAgl, const ChartBox& box)
    {
        return box.bottom + (S32)floor(unitOf(agl, topAgl) * (F32)box.h + 0.5f);
    }

    // Hodograph inset: a wind vector (x east, y north, m/s) as a pixel about (cx, cy) with |v| == maxMS on the radius. Invariants: the zero vector lands on the centre exactly; a vector of length maxMS lands within one pixel of the radius; east is +x, north is +y (up).
    inline PixelPoint hodoPoint(F32 vx, F32 vy, F32 maxMS, S32 cx, S32 cy, S32 radius)
    {
        PixelPoint p;
        const F32 k = (maxMS > 0.f) ? (F32)radius / maxMS : 0.f;
        p.x = cx + (S32)floor(vx * k + 0.5f);
        p.y = cy + (S32)floor(vy * k + 0.5f);
        return p;
    }

    // <SS:Nexii> V2 Storm Cells helpers (doc/atmo_magic_debug_views.md V2). Pure display geometry: the shell hands in the scheduler's numbers and the wall clock; nothing here feeds back into any storm position.

    // Rotation colour (design colour language): purple, sign by hue shift - cyclonic (omega > 0) violet, anticyclonic (omega < 0) magenta - and |omega| brightens the swatch. Invariants: channels in [0,1]; omega 0 gives the dim violet base; the red channel is monotone non-decreasing in |omega|; a positive and a negative omega of equal magnitude differ only in the blue channel (violet carries more blue than magenta).
    inline RGB rotationRamp(F32 omega)
    {
        F32 a = std::fabs(omega);
        if (!(a >= 0.f)) a = 0.f;          // NaN reads as no rotation (llclamp passes NaN through)
        a = llmin(a, 1.f);
        RGB c;
        c.r = std::lerp(0.40f, 0.85f, a);
        c.g = std::lerp(0.12f, 0.25f, a);
        c.b = (omega < 0.f) ? std::lerp(0.45f, 0.60f, a) : std::lerp(0.70f, 1.00f, a);
        return c;
    }

    // Lifecycle colour by stage index (SSStormCell::EStage order: TCU, MATURING, MATURE, ANVIL, DECAY): cool -> warm across the stage bar. Invariants: out-of-range indices clamp to the ends; the red channel is monotone non-decreasing and the blue channel monotone non-increasing across the five stages; channels in [0,1].
    constexpr S32 STAGE_COUNT = 5;
    inline RGB stageColor(S32 stage)
    {
        constexpr RGB kStages[STAGE_COUNT] = {
            { 0.35f, 0.75f, 1.00f },   // towering cumulus: cool blue
            { 0.45f, 0.90f, 0.70f },   // maturing: green-cyan
            { 0.95f, 0.90f, 0.35f },   // mature: yellow
            { 1.00f, 0.60f, 0.20f },   // anvil spread: orange
            { 1.00f, 0.30f, 0.20f }    // decay: red
        };
        return kStages[llclamp(stage, 0, STAGE_COUNT - 1)];
    }

    // The legend's lifecycle ramp: age01 in [0,1] -> the colour of the stage that age falls in (the same bands SSStormCell::lifecycle uses: 0.15 / 0.30 / 0.55 / 0.80). Invariants: piecewise constant; agrees with stageColor(stageOfAge(age01)).
    inline S32 stageOfAge(F32 age01)
    {
        const F32 a = llclamp(age01, 0.f, 1.f);
        return (a < 0.15f) ? 0 : (a < 0.30f) ? 1 : (a < 0.55f) ? 2 : (a < 0.80f) ? 3 : 4;
    }
    inline RGB lifecycleRamp(F32 age01)
    {
        return stageColor(stageOfAge(age01));
    }

    // Distance thinning for an in-world ring of radiusM drawn distM from the camera: the segment count falls with distance and rises with radius so a far ring costs a handful of lines and a near one stays round. Invariants: in [RING_SEGS_MIN, RING_SEGS_MAX]; monotone non-increasing in distM; monotone non-decreasing in radiusM; NaN-safe (NaN inputs give RING_SEGS_MIN).
    constexpr S32 RING_SEGS_MIN = 8;
    constexpr S32 RING_SEGS_MAX = 64;
    inline S32 ringSegments(F32 radiusM, F32 distM)
    {
        if (!(radiusM > 0.f) || !(distM >= 0.f))
        {
            return RING_SEGS_MIN;
        }
        // Apparent size: radius over distance, 1 at "the ring fills the view".
        const F32 apparent = radiusM / llmax(distM, 1.f);
        const F32 t = llclamp(apparent * 2.f, 0.f, 1.f);
        return RING_SEGS_MIN + (S32)floor(t * (F32)(RING_SEGS_MAX - RING_SEGS_MIN) + 0.5f);
    }

    // Age-tick interval (seconds) along the hero ribbon: the smallest of 60 / 120 / 300 / 600 / 900 s that keeps the tick count at or under RIBBON_TICKS_MAX for the lifetime. Invariants: result is one of the table values; lifetimeS / result <= RIBBON_TICKS_MAX whenever any table value achieves that (else the largest); monotone non-decreasing in lifetimeS; non-positive or NaN lifetime gives the smallest interval.
    constexpr S32 RIBBON_TICKS_MAX = 24;
    inline F32 ribbonTickIntervalS(F32 lifetimeS)
    {
        constexpr F32 kTable[] = { 60.f, 120.f, 300.f, 600.f, 900.f };
        constexpr S32 kCount = 5;
        if (!(lifetimeS > 0.f))
        {
            return kTable[0];
        }
        for (S32 i = 0; i < kCount; ++i)
        {
            if (lifetimeS / kTable[i] <= (F32)RIBBON_TICKS_MAX)
            {
                return kTable[i];
            }
        }
        return kTable[kCount - 1];
    }

    // Orbiting glyph angle (radians, in [0, 2pi)) for arrow i of n around a rotating cell: evenly spaced, advancing with the WALL CLOCK (never a frame counter) at one revolution per periodS, clockwise for negative omega (anticyclonic) and anticlockwise for positive. Invariants: in [0, 2pi); n glyphs are 2pi/n apart at any instant; angle(now + periodS) == angle(now) to float precision; omega 0 does not advance.
    constexpr F32 TWO_PI_F = 6.283185307179586f;
    inline F32 orbitAngle(F64 now, F32 omega, S32 i, S32 n, F32 periodS)
    {
        const S32 count = llmax(n, 1);
        const F32 base = (F32)(llclamp(i, 0, count - 1)) / (F32)count * TWO_PI_F;
        F32 spin = 0.f;
        if (omega != 0.f && periodS > 0.f)
        {
            // Reduce the clock modulo the period in double first so a large wall clock keeps sub-degree resolution.
            const F64 turns = now / (F64)periodS;
            const F64 frac = turns - floor(turns);
            spin = (F32)frac * TWO_PI_F * ((omega < 0.f) ? -1.f : 1.f);
        }
        F32 a = std::fmod(base + spin, TWO_PI_F);
        if (a < 0.f) a += TWO_PI_F;
        if (a >= TWO_PI_F) a -= TWO_PI_F;
        return a;
    }

    // Lattice-tile alpha from storm potential against the spawn threshold: tiles below the threshold's floor read faint, tiles above read solid, so the threshold shows as the tint's floor. potential and threshold in [0,1]. Invariants: in [TILE_ALPHA_MIN, TILE_ALPHA_MAX]; monotone non-decreasing in potential; exactly TILE_ALPHA_MIN at potential 0; reaches TILE_ALPHA_MAX at potential 1.
    constexpr F32 TILE_ALPHA_MIN = 0.06f;
    constexpr F32 TILE_ALPHA_MAX = 0.55f;
    inline F32 latticeTileAlpha(F32 potential, F32 threshold)
    {
        const F32 p = (potential >= 0.f) ? llmin(potential, 1.f) : 0.f;   // NaN reads as 0 (llclamp passes NaN through)
        const F32 th = (threshold >= 0.f) ? llmin(threshold, 1.f) : 0.f;
        // Below the threshold: a shallow ramp to the knee; above: the rest of the range, steeper, so the threshold reads as the tint's floor.
        constexpr F32 KNEE_FRAC = 0.15f;
        const F32 knee = std::lerp(TILE_ALPHA_MIN, TILE_ALPHA_MAX, KNEE_FRAC);
        if (p <= th)
        {
            return (th > 0.f) ? std::lerp(TILE_ALPHA_MIN, knee, p / th) : TILE_ALPHA_MIN;
        }
        return (th < 1.f) ? std::lerp(knee, TILE_ALPHA_MAX, (p - th) / (1.f - th)) : TILE_ALPHA_MAX;
    }

    // Compass heading (degrees, 0 north, clockwise, in [0, 360)) the vector points TOWARD. Invariants: (0,1)->0, (1,0)->90, (0,-1)->180, (-1,0)->270 (to float precision); the zero vector gives 0; never NaN.
    inline F32 headingOfVec(F32 vx, F32 vy)
    {
        if (vx == 0.f && vy == 0.f)
        {
            return 0.f;
        }
        constexpr F32 kRadToDeg = 57.29577951308232f;
        F32 h = std::atan2(vx, vy) * kRadToDeg;
        if (h < 0.f) h += 360.f;
        if (h >= 360.f) h -= 360.f;
        return h;
    }

    // <SS:Nexii> V2 vortex-icon helpers (DEBUG task: vortex icons at each active vortex's contact point, labelled
    // by kind). Plain S32 constants mirroring SSVortex::EKind's own values one for one, rather than including
    // ssvortexcore.h here, so this stays the same shape as stageColor(S32)/stageOfAge(S32) above (a CORE never
    // takes a dependency on another core's enum type when a mirrored int does the job) - the shell casts
    // SSVortex::EKind to S32 at the read site exactly as it already does for SSStormCell::EStage.
    constexpr S32 VORTEX_KIND_NONE         = 0;
    constexpr S32 VORTEX_KIND_MESOCYCLONIC = 1;
    constexpr S32 VORTEX_KIND_LANDSPOUT    = 2;
    constexpr S32 VORTEX_KIND_WATERSPOUT   = 3;
    constexpr S32 VORTEX_KIND_SATELLITE    = 4;
    constexpr S32 VORTEX_KIND_ANTICYCLONIC = 5;
    constexpr S32 VORTEX_KIND_QLCS         = 6;
    constexpr S32 VORTEX_KIND_GUSTNADO     = 7;
    constexpr S32 VORTEX_KIND_DUST_DEVIL   = 8;

    // Kind label: the taxonomy name an icon/legend row prints. Invariants: every VORTEX_KIND_* constant above maps
    // to its own distinct, non-empty, lower-case string; any other value (including negative and the unimplemented
    // QLCS reservation's neighbours) reads "?".
    inline const char* vortexKindLabel(S32 kind)
    {
        switch (kind)
        {
            case VORTEX_KIND_NONE:         return "none";
            case VORTEX_KIND_MESOCYCLONIC: return "mesocyclonic";
            case VORTEX_KIND_LANDSPOUT:    return "landspout";
            case VORTEX_KIND_WATERSPOUT:   return "waterspout";
            case VORTEX_KIND_SATELLITE:    return "satellite";
            case VORTEX_KIND_ANTICYCLONIC: return "anticyclonic";
            case VORTEX_KIND_QLCS:         return "qlcs";
            case VORTEX_KIND_GUSTNADO:     return "gustnado";
            case VORTEX_KIND_DUST_DEVIL:   return "dust devil";
        }
        return "?";
    }

    // Vortex icon colour: the same violet (cyclonic) / magenta (anticyclonic) rotation language cell rotation
    // glyphs already use (rotationRamp above), keyed on SIGN alone at full magnitude - an icon needs to read its
    // spin sense at a glance, not the live |omega|, which the caller already encodes separately (condensation bar,
    // alpha). A named wrapper rather than a bare rotationRamp(sign) call at each icon site so "vortex kind colour"
    // reads as its own concept. Invariants: vortexKindColor(s) == rotationRamp(s < 0.f ? -1.f : 1.f) for every s
    // (sign-only, saturated); therefore channels in [0,1]; positive and negative differ only in the blue channel.
    inline RGB vortexKindColor(F32 rotationSign)
    {
        return rotationRamp(rotationSign < 0.f ? -1.f : 1.f);
    }

    // <SS:Nexii> V4 Precip & Virga helpers (doc/atmo_magic_debug_views.md V4). Display geometry only - the shell
    // hands in the numbers it already reads off SSVolCloud/SSWindProfile/the precip preset; nothing here decides
    // where a shaft's own cards actually go (ssvirgacore.h owns that). F7 (2026-09-06 review), stale claim
    // corrected: a curtain is NOT a plain vertical stack any more - phase 8e (ssvirgacore.h skewOffsetM) gave it a
    // real per-altitude wind-fall lean. This function's line is a SEPARATE, independent comparison, unaffected by
    // that change: where a raindrop released from the same column would land if it drifted with the GROUND wind
    // the way precipitation's own spawner tilts it (ssprecipitation.cpp's spawnTierCell: entry = hit - windAt(hit)
    // * fallTime) - a display approximation the shaft's own actual skew (drawn as a separate line, see
    // SSAtmoInfoView::renderVirga) is read against, not a claim that the two describe the same geometry.

    // The horizontal drift (metres, downwind) a drop released dropHeightM above its landing point accumulates
    // falling at fallSpeedMS through a wind of (windX, windY): landing = entry + offset, so entry = landing -
    // offset is precip's own tilt, transliterated. Invariants: the zero vector for zero wind or zero/negative
    // dropHeightM; magnitude scales linearly with both wind speed and dropHeightM; fallSpeedMS is floored so a
    // stalled preset (0 or negative) never divides by zero; NaN-free for any finite input.
    inline Vec2M fallTiltOffsetM(F32 windX, F32 windY, F32 fallSpeedMS, F32 dropHeightM)
    {
        const F32 h = llmax(dropHeightM, 0.f);
        const F32 fallT = h / llmax(fallSpeedMS, 0.1f);
        Vec2M v;
        v.x = windX * fallT;
        v.y = windY * fallT;
        return v;
    }

    // <SS:Nexii> V5 Weather Cube helpers (doc/atmo_magic_debug_views.md V5). Pure display geometry only: the shell
    // samples the day-cycle curves/gates through the existing resolvers (SSAtmoEnvWeatherResolver,
    // SSAtmoEnvCloudFieldResolver, SSStormCell's own gateScore, ...) and hands the numbers in; nothing here reads a
    // cube, a phase clock or a setting.

    // Splits an outer chart box into laneCount equal-height lanes stacked vertically with a fixed pixel gap between
    // them, lane 0 at the TOP (the reading order the design doc gives: the day-cycle curves lane above, the derived
    // gates lane below). Invariants: every lane's left/w equals the outer box's; lane 0's top (bottom + h) equals
    // the outer box's top; the last lane's bottom equals the outer box's bottom; lanes never overlap and the gaps
    // between consecutive lanes are exactly gapPx (to integer rounding); laneCount < 1 reads as 1 (the whole box,
    // one lane); laneIndex clamps to [0, laneCount).
    inline ChartBox cubeLaneBox(const ChartBox& outer, S32 laneIndex, S32 laneCount, S32 gapPx)
    {
        const S32 n = llmax(laneCount, 1);
        const S32 i = llclamp(laneIndex, 0, n - 1);
        const S32 gap = llmax(gapPx, 0);
        const S32 total_gap = gap * (n - 1);
        const S32 content = llmax(outer.h - total_gap, n);   // every lane keeps at least 1px even when squeezed
        const S32 base_h = content / n;
        const S32 rem = content - base_h * n;                // the first `rem` lanes (top-down) take one extra pixel

        // Walk from the outer box's own top down through lane 0..i, so floor division's remainder is spent before
        // reaching the last lane - the last lane's bottom then lands exactly on the outer box's own bottom, which
        // a fixed per-lane height (dropping the remainder) cannot guarantee.
        S32 bottom = outer.bottom + outer.h;
        S32 h = base_h;
        for (S32 k = 0; k <= i; ++k)
        {
            h = base_h + (k < rem ? 1 : 0);
            bottom -= h;
            if (k < i) bottom -= gap;
        }

        ChartBox box;
        box.left = outer.left;
        box.w = outer.w;
        box.h = h;
        box.bottom = bottom;
        return box;
    }

    // The day-cycle sample ladder: sample i of n across one full cycle, i=0 at phase 0 and i=n-1 at phase 1 (the
    // curve's own wrap point - the day cycle wraps 1 back to 0, so plotting BOTH ends closes the loop visually).
    // Invariants: i=0 gives 0; i=n-1 gives 1; strictly increasing in i; n < 2 gives 0.
    inline F32 cubePhaseSample(S32 n, S32 i)
    {
        if (n < 2)
        {
            return 0.f;
        }
        const S32 k = llclamp(i, 0, n - 1);
        return (F32)k / (F32)(n - 1);
    }

    // Wraps an arbitrary phase (an authored cue's own keyframe time, or a preview override, neither range-checked
    // at authoring) into [0,1) the way the day cycle itself does. Invariants: a phase already in [0,1) is
    // unchanged; wrapPhase(p + k) == wrapPhase(p) for any integer k (to float precision); NaN reads as 0.
    inline F32 cubeWrapPhase(F64 phase)
    {
        if (!(phase == phase)) return 0.f; // NaN
        F64 p = std::fmod(phase, 1.0);
        if (p < 0.0) p += 1.0;
        return (F32)p;
    }

    // A cue marker's (or the "now" cursor's) pixel x on a phase-axis chart box: cubeWrapPhase then the same
    // chartX(unit, 1, box) mapping every other chart in this file uses. Invariants: 0 -> box.left; a phase just
    // under 1 -> just under box.left + box.w (never AT it, post-wrap); monotone within one wrap.
    inline S32 cubePhaseX(F64 phase, const ChartBox& box)
    {
        return chartX(cubeWrapPhase(phase), 1.f, box);
    }

    // <SS:Nexii> V4's CHART arithmetic (doc/atmo_magic_debug_views.md V4; backlog 5.3 - V4 shipped its in-world
    // layer with no panel beside it). Pure display geometry, exactly like every helper above: the shell hands in
    // the counts and radii it already reads off SSVolCloud::virgaDebug() and the precip preset, and nothing here
    // decides which cell qualifies, which the hash trim keeps, or where a card goes - ssvirgacore.h owns all
    // three. The one formula quoted from that core (keepProbability) is quoted deliberately and cross-checked
    // against it by a named test rather than re-derived by eye; see its own comment.

    // The drive histogram's bucket for a drive in [0,1]: nBuckets equal-width buckets, bucket i covering
    // [i/n, (i+1)/n) and the top bucket closed at 1. Invariants: in [0, nBuckets-1]; 0 for drive <= 0 and for
    // NaN; nBuckets-1 for drive >= 1; monotone non-decreasing in drive; nBuckets < 1 reads as 1 (one bucket).
    constexpr S32 DRIVE_BUCKETS = 10;
    inline S32 driveBucket(F32 drive, S32 nBuckets)
    {
        const S32 n = llmax(nBuckets, 1);
        if (!(drive > 0.f))            // NaN and every non-positive drive fall in the first bucket
        {
            return 0;
        }
        if (drive >= 1.f)
        {
            return n - 1;
        }
        return llclamp((S32)floor(drive * (F32)n), 0, n - 1);
    }

    // The pixel column of histogram bar `bucket` inside a lane, and its height for `count` against the tallest
    // bucket in the same histogram. Bars are laid edge to edge across the lane with gapPx between them, so the
    // lane's whole width is spent. Invariants: every bar's left/right lies inside the lane; bar i's right edge is
    // at or left of bar i+1's left edge (never overlapping); the last bar's right edge is the lane's right edge;
    // a zero count gives h == 0 and a count equal to maxCount gives h == lane.h; maxCount <= 0 gives h == 0;
    // bucket and nBuckets clamp the same way driveBucket's do.
    inline ChartBox histogramBarBox(const ChartBox& lane, S32 bucket, S32 nBuckets, S32 count, S32 maxCount, S32 gapPx)
    {
        const S32 n = llmax(nBuckets, 1);
        const S32 i = llclamp(bucket, 0, n - 1);
        const S32 gap = llmax(gapPx, 0);
        // Edge positions by exact division from the lane's own left, so rounding never leaves a seam or an
        // overlap and the last bar lands on the lane's right edge.
        const S32 x0 = lane.left + (lane.w * i) / n;
        const S32 x1 = lane.left + (lane.w * (i + 1)) / n;
        ChartBox bar;
        bar.left = x0;
        bar.w = llmax(x1 - x0 - (i + 1 < n ? gap : 0), 0);
        bar.bottom = lane.bottom;
        bar.h = (maxCount > 0) ? (S32)floor(unitOf((F32)count, (F32)maxCount) * (F32)lane.h + 0.5f) : 0;
        return bar;
    }

    // The shaft budget's fill as a fraction of the cap: kept / cap, DELIBERATELY unclamped above 1, because the
    // shaft trim's own rank-cut backstop (SSVirga::hardCap) permits a kept set above MAX_SHAFTS and a bar that
    // silently clamped would hide exactly the overshoot the panel exists to show. Invariants: 0 at kept 0; 1 at
    // kept == cap; monotone non-decreasing in kept; never negative; a non-positive cap reads any positive kept
    // count as 1 (full) and 0 as 0, the same convention unitOf uses.
    inline F32 budgetFrac(S32 kept, S32 cap)
    {
        const F32 k = (F32)llmax(kept, 0);
        if (cap <= 0)
        {
            return k > 0.f ? 1.f : 0.f;
        }
        return k / (F32)cap;
    }

    // The filled width, in pixels, of a budget bar barWpx wide - budgetFrac clamped INTO the bar, since a bar
    // cannot draw past its own end (the overshoot is reported by budgetFrac and printed as a number instead).
    // Invariants: in [0, barWpx]; 0 at kept 0; barWpx at and above kept == cap; monotone non-decreasing in kept;
    // a non-positive barWpx gives 0.
    inline S32 budgetFillPx(S32 kept, S32 cap, S32 barWpx)
    {
        const S32 w = llmax(barWpx, 0);
        const F32 f = llclamp(budgetFrac(kept, cap), 0.f, 1.f);
        return (S32)floor(f * (F32)w + 0.5f);
    }

    // The stable hash trim's own keep probability for a candidate set: min(1, cap / quantisedN), where quantisedN
    // is SSVirga::quantiseCount(n) - the shell passes that core's OWN result in rather than this core quantising
    // anything, so the only thing quoted here is the min/divide the panel prints as a percentage. Quoted rather
    // than called because ssvirgacore.h is not includable from this core (see the file header); the quote is held
    // to SSVirga::keepHash's measured behaviour by tests/xsection_infoview_virga_budget.cpp, which drives the real
    // keepHash over 20000 cells and compares the kept share to this number. Invariants: in [0,1]; exactly 1 when
    // quantisedN <= cap; monotone non-increasing in quantisedN; monotone non-decreasing in cap; a non-positive or
    // NaN quantisedN reads as 1 (nothing to trim).
    inline F32 keepProbability(S32 cap, F32 quantisedN)
    {
        if (!(quantisedN > 0.f))
        {
            return 1.f;
        }
        const F32 c = (F32)llmax(cap, 0);
        return llmin(1.f, c / quantisedN);
    }

    // The distance axis ceiling for the handoff chart: a nice round number comfortably past the end of the ramp
    // it has to show, so the "full shaft alpha" rail never sits on the axis' own right edge. Invariants: >= 1;
    // >= rampEndM (strictly greater whenever rampEndM > 0); monotone non-decreasing in rampEndM; a NaN or
    // non-positive rampEndM gives 1.
    constexpr F32 DIST_AXIS_MARGIN = 1.25f;
    inline F32 distanceAxisMaxM(F32 rampEndM)
    {
        if (!(rampEndM > 0.f))
        {
            return 1.f;
        }
        return niceMax(rampEndM * DIST_AXIS_MARGIN);
    }

    // <SS:Nexii> V9 LIGHTNING helpers (doc/atmo_magic_debug_views.md V9). Same rule as every block above: the
    // shell reads SSLightning's own resident strike list and hands the numbers in; nothing here schedules,
    // advances or places a strike, and none of it is on any determinism path (a strike's own geometry is rolled
    // from its fire time in sslightning.cpp and this file never sees that stream). Plain S32 kind constants
    // mirroring SSStrikeKind's values one for one, the same idiom the VORTEX_KIND_* mirrors use - a core never
    // includes a shell header for an enum when a mirrored int does the job, and the shell casts at the read site.
    constexpr S32 STRIKE_KIND_SHEET  = 0;
    constexpr S32 STRIKE_KIND_FORK   = 1;
    constexpr S32 STRIKE_KIND_GROUND = 2;
    constexpr S32 STRIKE_KIND_COUNT  = 3;

    // Kind label for an icon or legend row. Invariants: every STRIKE_KIND_* constant maps to its own distinct,
    // non-empty, lower-case string; any other value reads "?".
    inline const char* strikeKindLabel(S32 kind)
    {
        switch (kind)
        {
            case STRIKE_KIND_SHEET:  return "sheet";
            case STRIKE_KIND_FORK:   return "fork";
            case STRIKE_KIND_GROUND: return "ground";
        }
        return "?";
    }

    // A live strike's lifecycle stage, read off the fields SSLightning::advance() already maintains on the strike:
    // tSec is its own clock (SSStrike::mT, negative before contact), leaderProgress its stepped leader front
    // (SSStrike::mLeaderProgress, 0 until the leader starts), plasmaSinceS the seconds since the LATEST return
    // stroke (SSStrike::mPlasmaSince, negative when no stroke has fired), and the two windows are the caller's own
    // (the stroke decay constant and SSDissolve::PLASMA_S) so this core takes no dependency on sslightning.h.
    // Invariants: always in [0, STRIKE_STAGE_COUNT); before contact it is CHARGE while the leader has not started
    // and LEADER once it has; after contact it is monotone non-decreasing in plasmaSinceS whenever
    // strokeWindowS <= plasmaWindowS; a negative plasmaSinceS after contact reads AFTERGLOW (no stroke is lit);
    // NaN in any argument reads as the earliest stage the other arguments allow, never as a crash or a wrap.
    constexpr S32 STRIKE_STAGE_CHARGE    = 0;
    constexpr S32 STRIKE_STAGE_LEADER    = 1;
    constexpr S32 STRIKE_STAGE_STROKE    = 2;
    constexpr S32 STRIKE_STAGE_PLASMA    = 3;
    constexpr S32 STRIKE_STAGE_AFTERGLOW = 4;
    constexpr S32 STRIKE_STAGE_COUNT     = 5;
    inline S32 strikeStage(F32 tSec, F32 leaderProgress, F32 plasmaSinceS, F32 strokeWindowS, F32 plasmaWindowS)
    {
        if (!(tSec == tSec))                       // NaN clock: the strike has not been advanced yet
        {
            return STRIKE_STAGE_CHARGE;
        }
        if (tSec < 0.f)
        {
            return (leaderProgress > 0.f) ? STRIKE_STAGE_LEADER : STRIKE_STAGE_CHARGE;
        }
        if (!(plasmaSinceS >= 0.f))                // no stroke is lit (mPlasmaSince -1), or NaN
        {
            return STRIKE_STAGE_AFTERGLOW;
        }
        if (plasmaSinceS <= llmax(strokeWindowS, 0.f))
        {
            return STRIKE_STAGE_STROKE;
        }
        if (plasmaSinceS <= llmax(plasmaWindowS, 0.f))
        {
            return STRIKE_STAGE_PLASMA;
        }
        return STRIKE_STAGE_AFTERGLOW;
    }

    // Stage label for the legend's stage bar and the in-world tag. Invariants: every STRIKE_STAGE_* constant maps
    // to its own distinct, non-empty, lower-case string; any other value reads "?".
    inline const char* strikeStageLabel(S32 stage)
    {
        switch (stage)
        {
            case STRIKE_STAGE_CHARGE:    return "charge";
            case STRIKE_STAGE_LEADER:    return "leader";
            case STRIKE_STAGE_STROKE:    return "stroke";
            case STRIKE_STAGE_PLASMA:    return "plasma";
            case STRIKE_STAGE_AFTERGLOW: return "afterglow";
        }
        return "?";
    }

    // Stage colour: the design's lifecycle language (cool -> warm across the stage bar), which is the SAME five-
    // step ramp the storm cells' own stages already use - a named wrapper rather than a bare stageColor() call at
    // each site so "strike stage colour" reads as its own concept, exactly as vortexKindColor wraps rotationRamp.
    // Invariants: strikeStageColor(s) == stageColor(s) for every s (STRIKE_STAGE_COUNT == STAGE_COUNT, so the
    // clamping at both ends agrees too); therefore channels in [0,1], red non-decreasing and blue non-increasing
    // across the five stages.
    inline RGB strikeStageColor(S32 stage)
    {
        return stageColor(stage);
    }

    // Channel LOD: the minimum node width a channel segment must carry to be drawn at this camera distance, so a
    // far bolt reduces to its trunk instead of spending hundreds of lines on branches a pixel wide. The bracket is
    // the channel builder's own taper (sslightning.cpp growPath: a trunk runs 1.0 -> 0.6 wide, and growBranches
    // starts a branch at 0.55x its parent, so 0.55 is the widest a non-trunk node can ever be) - CUTOFF_MAX sits
    // strictly between the two, which is what makes "the far reading is the trunk and nothing else" true rather
    // than approximate. LOCKSTEP with those two literals: if the taper changes, these change with it.
    // Invariants: 0 at and below CHANNEL_FULL_M (everything near the camera draws); CHANNEL_CUTOFF_MAX at and
    // above CHANNEL_TRUNK_M; monotone non-decreasing; always in [0, CHANNEL_CUTOFF_MAX]; NaN gives
    // CHANNEL_CUTOFF_MAX (draw the least, never the most).
    constexpr F32 CHANNEL_TRUNK_MIN_WIDTH  = 0.6f;    // LOCKSTEP sslightning.cpp buildChannel growPath(..., 1.f, 0.6f, ...)
    constexpr F32 CHANNEL_BRANCH_MAX_WIDTH = 0.55f;   // LOCKSTEP sslightning.cpp growBranches from_node.mWidth * 0.55f
    constexpr F32 CHANNEL_CUTOFF_MAX       = 0.58f;
    constexpr F32 CHANNEL_FULL_M           = 400.f;
    constexpr F32 CHANNEL_TRUNK_M          = 4000.f;
    inline F32 channelWidthCutoff(F32 distM)
    {
        if (!(distM == distM))
        {
            return CHANNEL_CUTOFF_MAX;
        }
        if (distM <= CHANNEL_FULL_M)
        {
            return 0.f;
        }
        if (distM >= CHANNEL_TRUNK_M)
        {
            return CHANNEL_CUTOFF_MAX;
        }
        return CHANNEL_CUTOFF_MAX * ((distM - CHANNEL_FULL_M) / (CHANNEL_TRUNK_M - CHANNEL_FULL_M));
    }

    // The weight a strike carries as a light on the cloud deck, and whether it clears the cut that decides which
    // strikes are uploaded to the cloud shader at all: ssvolcloud.cpp's own uploader takes b = channel brightness
    // x intensity and skips anything at or below STRIKE_LIGHT_CUT, then stops at SS_MAX_STRIKE_LIGHTS. Quoted here
    // (LOCKSTEP ssvolcloud.cpp update(), the mStrikeLights loop) so the view can mark which live strikes the cloud
    // shader is actually lit by - the uploaded array itself is private to SSVolCloud, so the panel reports this
    // reading of the same inputs and says so, rather than claiming to show the array. Invariants: the weight is
    // never negative and is 0 whenever either input is non-positive or NaN; monotone non-decreasing in each input;
    // strikeLightsCloud is exactly weight > STRIKE_LIGHT_CUT.
    constexpr F32 STRIKE_LIGHT_CUT = 0.004f;
    inline F32 strikeLightWeight(F32 channelBrightness, F32 intensity)
    {
        if (!(channelBrightness > 0.f) || !(intensity > 0.f))
        {
            return 0.f;
        }
        return channelBrightness * intensity;
    }
    inline bool strikeLightsCloud(F32 channelBrightness, F32 intensity)
    {
        return strikeLightWeight(channelBrightness, intensity) > STRIKE_LIGHT_CUT;
    }
}

#endif
