/**
 * @file sswindprofilecore.h
 * @brief Atmo Magic: the altitude wind profile (hodograph), header-only core. CONTRACT.
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

#ifndef SS_WINDPROFILECORE_H
#define SS_WINDPROFILECORE_H

// <SS:Nexii> A CORE header (lldefs.h + <cmath> only; NO llmath.h - see ssatmonoisecore.h for why). Design: doc/atmo_magic_wind_profile.md sections 3-4. Every function here is a PURE function of its arguments: no dt, no time, no camera, no singleton, no setting - the same inputs give bit-identical outputs on every client, which is the whole determinism story. The surface wind handed in must be the CURVE-RESOLVED weather-cube value at phase, never the eased SSAtmoMagic::mWind (Euler-integrated per frame, so framerate-dependent). The shear exponent must come from the weather cube, never from the flowmap's region-roughness alpha (camera-region dependent). Bodies marked STUB are the implementer's to fill; the invariants in each comment are the test author's spec.
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSWindProfile
{
    // ---- Constants. Ones marked LOCKSTEP mirror a value owned elsewhere and must change together. ----
    constexpr F32 REF_M          = 10.f;     // LOCKSTEP sswindflow.cpp SS_WIND_REF_M: the altitude AGL the wind is authored at
    constexpr F32 BL_TOP_M       = 1500.f;   // LOCKSTEP sswindflow.cpp SS_WIND_GRADIENT_CEIL_M: boundary-layer top, power law plateaus here
    constexpr F32 SCALE_MIN      = 0.35f;    // LOCKSTEP sswindflow.cpp windGradientScale clamp floor
    constexpr F32 SCALE_BL_MAX   = 3.f;      // LOCKSTEP sswindflow.cpp windGradientScale clamp ceiling (the plateau value at strong exponents)
    constexpr F32 SCALE_JET_MAX  = 6.f;      // absolute ceiling once the jet continuation is added above BL_TOP_M
    constexpr F32 JET_GAIN       = 1.5f;     // scale above BL_TOP_M = plateau * (1 + S * JET_GAIN * smoothstep(BL_TOP_M, anvilAgl, z))
    constexpr F32 VEER_SPLIT_B   = 0.35f;    // share of the total veer spent in the friction layer (Ekman-ish); the rest is the deep-layer leg
    constexpr F32 SHEAR_LEAN_S   = 600.f;    // O(z) = (windAt(z) - windAt(base)) * SHEAR_LEAN_S, then capped: a fixed lean, NOT an integral
    constexpr F32 SHEAR_CAP_M    = 2500.f;   // |O(z)| never exceeds this (~10 x CELL_M); the bounded-offset rule from the design review
    constexpr F32 EXPONENT_DEFAULT = 0.16f;  // LOCKSTEP sswindflow.cpp SS_WIND_ALPHA_FALLBACK: exponent when nothing is authored
    constexpr F32 EXPONENT_MIN   = 0.f;
    constexpr F32 EXPONENT_MAX   = 0.6f;     // LOCKSTEP sswindflow.cpp windAlpha() setting clamp
    constexpr F32 CELL_M         = 260.f;    // LOCKSTEP ssvolcloud.cpp CELL_M and ssVolCloudF.glsl SS_CELL_M [interaction: SSVolCloud]
    constexpr F32 WRAP_MIN_M     = 100000.f; // the drift wrap span is the smallest common lattice period at or above this
    constexpr F32 WRAP_MAX_M     = 8000000.f;// and never above this: F32 ulp at 8e6 m is 0.5 m, the most the shader's air.xy read tolerates
    constexpr F32 NOISE_M        = 880.f;    // LOCKSTEP ssVolCloudF.glsl SS_NOISE_M and SS_SHEET_TILE_M: detail-octave and veil periods on air.xy
    constexpr F32 SKEW_M         = 1400.f;   // LOCKSTEP ssVolCloudF.glsl SS_SKEW_M: the triplanar skew period on air.xy

    // x east, y north. A heading h (degrees, 0 = north, clockwise) becomes (sin h, cos h) - the same convention as
    // SSAtmoEnvSkyWeatherModulator's drift velocity and SSAtmoTrackConfig::setHeadingElevation. The vector points the
    // way the air MOVES (a drift velocity), not where the wind comes from.
    struct Vec2
    {
        F32 x = 0.f;
        F32 y = 0.f;
    };

    struct Params
    {
        F32 mHeading10Deg  = 0.f;               // curve-resolved surface heading at phase
        F32 mSpeed10MS     = 0.f;               // curve-resolved surface speed at phase (m/s, >= 0)
        F32 mExponent      = EXPONENT_DEFAULT;  // boundary-layer shear exponent, clamped to [EXPONENT_MIN, EXPONENT_MAX] by the callee
        F32 mShearStrength = 0.f;               // S in [0,1]: how much jet and deep-layer veer the day carries
        F32 mVeerDeg       = 0.f;               // total heading turn from surface to anvil, degrees; positive = veers clockwise with height
        F32 mAnvilAglM     = 6000.f;            // the altitude AGL the jet leg and the veer both complete at (cirrus/anvil level); > BL_TOP_M
    };

    struct AutoShear
    {
        F32 mStrength = 0.f;
        F32 mVeerDeg  = 0.f;
    };

    // GLSL-semantics smoothstep: clamp((x-e0)/(e1-e0),0,1) then t*t*(3-2t). Invariants: returns 0 for x<=e0, 1 for x>=e1,
    // monotone non-decreasing in x; if e1 <= e0 it degenerates to a step at e0 (never divides by zero, never NaN).
    inline F32 smoothstep(F32 e0, F32 e1, F32 x)
    {
        if (e1 <= e0)
        {
            return (x <= e0) ? 0.f : 1.f;
        }
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // (sin h, cos h) * speed. Invariants: fromHeading(0, s) == (0, s); fromHeading(90, s) == (s, 0) to float precision;
    // speed 0 gives the zero vector for any heading.
    inline Vec2 fromHeading(F32 headingDeg, F32 speed)
    {
        constexpr F32 kDegToRad = 0.017453292519943295f;
        if (speed == 0.f)
        {
            return Vec2();
        }
        const F32 h = headingDeg * kDegToRad;
        Vec2 v;
        v.x = std::sin(h) * speed;
        v.y = std::cos(h) * speed;
        return v;
    }

    // Blend two wind VECTORS. This is the only sanctioned way to interpolate wind between two altitudes/bands - bearings
    // must never be lerped (350 -> 10 degrees through 180 is the bug this exists to prevent). Invariants: t=0 gives a
    // bit-exact copy of a, t=1 of b; blending equal-speed headings 350 and 10 yields a vector pointing within 1 degree
    // of north with speed >= cos(10deg) * s.
    inline Vec2 lerpVec(const Vec2& a, const Vec2& b, F32 t)
    {
        Vec2 v;
        v.x = std::lerp(a.x, b.x, t);
        v.y = std::lerp(a.y, b.y, t);
        return v;
    }

    // Speed factor at zAgl relative to the 10m authored speed. Below BL_TOP_M: clamp((max(zAgl,0.5)/REF_M)^exponent,
    // SCALE_MIN, SCALE_BL_MAX) - byte-for-byte the existing windGradientScale shape, so a calm day (shearStrength 0)
    // renders exactly as today. Above BL_TOP_M: plateau * (1 + S * JET_GAIN * smoothstep(BL_TOP_M, anvilAgl, zAgl)),
    // clamped to [SCALE_MIN, SCALE_JET_MAX], where plateau is the value at BL_TOP_M. Invariants: continuous at BL_TOP_M;
    // monotone non-decreasing in zAgl for exponent >= 0; S=0 gives a flat plateau above BL_TOP_M; exponent 0 gives 1.0
    // everywhere below the plateau; never NaN for zAgl in [-1000, 20000].
    inline F32 speedScale(F32 zAgl, const Params& p)
    {
        const F32 exponent = llclamp(p.mExponent, EXPONENT_MIN, EXPONENT_MAX);
        if (zAgl <= BL_TOP_M)
        {
            const F32 h = llmax(zAgl, 0.5f);
            return llclamp(std::pow(h / REF_M, exponent), SCALE_MIN, SCALE_BL_MAX);
        }
        const F32 plateau = llclamp(std::pow(BL_TOP_M / REF_M, exponent), SCALE_MIN, SCALE_BL_MAX);
        const F32 s = llclamp(p.mShearStrength, 0.f, 1.f);
        const F32 jet = plateau * (1.f + s * JET_GAIN * smoothstep(BL_TOP_M, p.mAnvilAglM, zAgl));
        return llclamp(jet, SCALE_MIN, SCALE_JET_MAX);
    }

    // Heading at zAgl: heading10 + veer * (B * smoothstep(0, BL_TOP_M, z) + (1-B) * smoothstep(BL_TOP_M, anvilAgl, z)),
    // B = VEER_SPLIT_B, veer = mVeerDeg * mShearStrength. Returned in degrees, NOT wrapped (callers convert to a vector;
    // wrapping bearings is exactly what fromHeading absorbs). Invariants: equals heading10 at z<=0; equals heading10 +
    // veer at z >= anvilAgl; monotone in z; shearStrength 0 or veer 0 gives heading10 at every altitude.
    inline F32 headingDeg(F32 zAgl, const Params& p)
    {
        const F32 veer = p.mVeerDeg * p.mShearStrength;
        const F32 frac = VEER_SPLIT_B * smoothstep(0.f, BL_TOP_M, zAgl)
                       + (1.f - VEER_SPLIT_B) * smoothstep(BL_TOP_M, p.mAnvilAglM, zAgl);
        return p.mHeading10Deg + veer * frac;
    }

    // The wind vector at zAgl = fromHeading(headingDeg(z), mSpeed10MS * speedScale(z)). Invariants: pure; bit-identical for
    // equal inputs; speed10 0 gives the zero vector at every altitude; a calm-day Params (S=0, veer=0) gives a vector
    // whose heading equals heading10 at every altitude.
    inline Vec2 windAt(F32 zAgl, const Params& p)
    {
        return fromHeading(headingDeg(zAgl, p), p.mSpeed10MS * speedScale(zAgl, p));
    }

    // The BOUNDED shear offset of altitude zAgl relative to the deck base at baseAgl: d = windAt(z) - windAt(base);
    // O = d * SHEAR_LEAN_S, and if |O| > SHEAR_CAP_M it is scaled down to exactly SHEAR_CAP_M along the same direction.
    // This is a frame TRANSFORM (design section 4, revised - see ssdeckframecore.h's PRODUCER/OBSERVER rule, corrected
    // 2026-09-05 after the phase-4 opus review found the first contract unsound). CORRECTED rule (the rule below used
    // to invert this - it named exactly two things and specifically excluded the n_map column read, which was the bug):
    // O(z) is a PLACEMENT transform - the CPU builder places a puff of column c at altitude z at c + O(z)
    // (SSDeckFrame::placeWorld) - and it is undone by EVERY fragment read of that puff (SSDeckFrame::frameAir):
    // the n_map column read (tower window, anvil, floor, fill), the detail octaves, the triplanar skew, the cap band -
    // all of it, not a named subset. The veil sheet, the shadow bake and precipNoiseAt never see O(z) at all, but not
    // because they are exceptions carved out of this rule - they are base-anchored BY DEFINITION (the sheet's own
    // reads are gateAir unconditionally, whatever height it is drawn at) and so never route through frameAir in the
    // first place. ssdeckframecore.h is the authority for this contract; do not re-derive it here. It is NOT an
    // integral and carries no time - two accumulators at different velocities diverge without bound and
    // wrap apart, which is the bug this formulation exists to avoid. Invariants: O(base, base) is the zero vector;
    // |O| <= SHEAR_CAP_M for every z in [-1000, 20000] and every Params; continuous in z; a calm-day Params gives
    // |O| == 0 above BL_TOP_M (flat plateau) while below it the speed difference alone leans the column, as intended.
    inline Vec2 shearOffset(F32 zAgl, F32 baseAgl, const Params& p)
    {
        const Vec2 wz = windAt(zAgl, p);
        const Vec2 wb = windAt(baseAgl, p);
        Vec2 o;
        o.x = (wz.x - wb.x) * SHEAR_LEAN_S;
        o.y = (wz.y - wb.y) * SHEAR_LEAN_S;
        const F32 len = std::sqrt(o.x * o.x + o.y * o.y);
        if (len > SHEAR_CAP_M)
        {
            const F32 k = SHEAR_CAP_M / len;
            o.x *= k;
            o.y *= k;
        }
        return o;
    }

    // The storm consolidation product - smoothstep(0.55,0.85,moisture) * smoothstep(0.45,0.75,convection) - the ONE
    // definition (phase-3c F4: previously respelled in ssvolcloud.cpp's builder, its V2 overlay AND this file's own
    // autoShear; SSStormCouple::consolidation, in the storm core that includes this one, now forwards here instead
    // of carrying a fourth copy - a wind core must never include a storm core, so this is the direction the single
    // definition has to live in). Invariants: 0 when either input is at or below its window floor; 1 at (0.85, 0.75)
    // and above; monotone in both; in [0,1].
    inline F32 consolidation(F32 moisture, F32 convection)
    {
        return smoothstep(0.55f, 0.85f, moisture) * smoothstep(0.45f, 0.75f, convection);
    }

    // Auto-derivation when nothing is authored (design section 3, mirrors the mGustAuto idiom):
    //   storm    = consolidation(moisture, convection)
    //   strength = clamp(lerp(0.15, 1.0, max(convection*0.4, storm)), 0, 1)
    //   veerDeg  = lerp(8, 65, strength) + clamp((15 - temperatureC)*0.4, -10, 15)
    // Invariants: dry calm (0,0,20C) gives strength 0.15 and veer in [7, 12]; a consolidated storm (0.9, 0.8, 20C) gives
    // strength 1.0 and veer 65 +- 3; strength monotone non-decreasing in convection and in moisture; colder air adds veer.
    inline AutoShear autoShear(F32 moisture, F32 convection, F32 temperatureC)
    {
        const F32 storm = consolidation(moisture, convection);
        AutoShear r;
        r.mStrength = llclamp(std::lerp(0.15f, 1.f, llmax(convection * 0.4f, storm)), 0.f, 1.f);
        r.mVeerDeg  = std::lerp(8.f, 65.f, r.mStrength) + llclamp((15.f - temperatureC) * 0.4f, -10.f, 15.f);
        return r;
    }

    // The drift wrap span: the smallest multiple of BOTH cellM and noiseTileM that is >= WRAP_MIN_M. Precondition: noiseTileM
    // is an exact positive integer multiple of cellM (the shell quantizes the authored noise tile to the cell lattice so this
    // holds - 2048 becomes 2080 = 8 cells). Wrapping the drift accumulator on this span keeps every frame-consuming
    // pattern (cell gate, cluster noise, noise map) exactly where it was, where fmodf(drift, 1e6) repopped the field.
    // Invariants: result is a multiple of cellM and of noiseTileM; result >= WRAP_MIN_M; result < WRAP_MIN_M + noiseTileM
    // when noiseTileM >= cellM; if the precondition fails, falls back to the smallest multiple of cellM >= WRAP_MIN_M.
    inline F32 wrapSpanM(F32 cellM, F32 noiseTileM)
    {
        if (!(cellM > 0.f))
        {
            return WRAP_MIN_M;
        }
        // The lattice period: noiseTileM when it is an exact positive integer multiple of cellM, else cellM alone.
        F32 period = cellM;
        if (noiseTileM > 0.f)
        {
            const F32 ratio = noiseTileM / cellM;
            const S32 n = (S32)floor(ratio + 0.5f);
            if (n >= 1 && std::fabs(noiseTileM - (F32)n * cellM) <= 1e-3f * cellM)
            {
                period = noiseTileM;
            }
        }
        S32 k = (S32)floor(WRAP_MIN_M / period);
        if ((F32)k * period < WRAP_MIN_M)
        {
            ++k;
        }
        return (F32)k * period;
    }

    // The drift wrap span over EVERY period the field draws with on air.xy: the cell lattice, both decks' (cell-quantised)
    // noise tiles and the shader's NOISE_M / SKEW_M constants - the smallest multiple of their lcm at or above minM. A
    // period is skipped when it is not a positive whole number of metres, or when folding it in would push the lcm past
    // maxM (F32 precision guard) - the shell passes the shader constants first so they are never the ones dropped.
    // Invariants: the result is a multiple of every period it kept; minM <= result <= maxM (or the largest kept lcm
    // multiple below maxM); {260, 2080, 880, 1400} gives 800800 exactly; a lone 260 gives the smallest multiple of 260
    // >= minM. NOTE, widened (phase 6b de-tile, review F4): alignment keeps only THESE listed periods' tiling
    // textures where they were - it is not honest to say "every" tiling texture. The de-tile's second octave
    // (ssdecknoisecore.h: detileCoord rotates by 37 degrees and scales by 1/phi before re-reading the same map) has
    // no axis-aligned translation that realigns it at any span - the rotation is irrational relative to the primary
    // grid, so the second read re-samples fresh at every wrap regardless of the lcm chosen here, stacking on top of
    // the already-accepted hash re-roll below. And the hash-based cell gate and cluster noise are still NOT
    // periodic in the cell index, so they too re-roll at a wrap until the builder/shader/bake hash the cell index
    // modulo (span / CELL_M) - scheduled with the phase-6b gate work (doc/atmo_magic_wind_profile.md section 4);
    // that scheduled fix covers the gate hash only, not the de-tile's second octave, which has no cell-index
    // modulus to take in the first place.
    inline F32 lcmSpanM(const F32* periodsM, S32 count, F32 minM, F32 maxM)
    {
        uint64_t l = 1;
        for (S32 i = 0; i < count; ++i)
        {
            const F32 p = periodsM[i];
            if (!(p > 0.f)) continue;
            const F32 r = std::floor(p + 0.5f);
            if (std::fabs(p - r) > 1e-3f) continue;          // not a whole number of metres
            const uint64_t q = (uint64_t)r;
            uint64_t a = l, b = q;
            while (b != 0) { const uint64_t t = a % b; a = b; b = t; }   // gcd
            const uint64_t cand = (l / a) * q;
            if ((F32)cand > maxM) continue;                   // precision guard: drop this period
            l = cand;
        }
        const F32 lf = (F32)l;
        if (!(lf > 0.f)) return minM;
        F32 k = std::floor(minM / lf);
        if (k * lf < minM) k += 1.f;
        F32 span = k * lf;
        if (span > maxM) span = std::floor(maxM / lf) * lf;
        if (!(span > 0.f)) span = lf;
        return span;
    }

    // v wrapped into [0, spanM): v - floor(v/spanM)*spanM, computed so the result is bit-identical for equal inputs and
    // never negative or >= spanM (guard the floating rounding at the seam). Invariants: wrapDrift(v + k*spanM, spanM) ==
    // wrapDrift(v, spanM) to within one ulp for integer k; wrapDrift(v, spanM) == v for v in [0, spanM).
    inline F32 wrapDrift(F32 v, F32 spanM)
    {
        if (!(spanM > 0.f))
        {
            return 0.f;
        }
        F32 r = v - std::floor(v / spanM) * spanM;
        if (r < 0.f)
        {
            r += spanM;
        }
        if (r >= spanM)
        {
            r -= spanM;
        }
        if (r < 0.f || r >= spanM)
        {
            r = 0.f;
        }
        return r;
    }

    // F64 OVERLOAD (the accumulator's own precision - doc/atmo_magic_wind_profile.md section 4, F64 accumulator fix,
    // 2026-09-05): identical shape to wrapDrift above, computed entirely in F64 so the accumulator that carries this
    // wrap never round-trips through F32 before it is wrapped. lcmSpanM stays F32 (the span itself is a small fixed
    // constant - {260, 880, 1400, 2080} lands on 800800 exactly in F32 - so only the accumulator's own running sum
    // and its wrap need the wider type). Invariants: same as wrapDrift, F64 throughout; wrapDriftD((F64)v, (F64)spanM)
    // equals (F64)wrapDrift(v, spanM) to within F32 rounding for inputs that fit F32 exactly.
    inline F64 wrapDriftD(F64 v, F64 spanM)
    {
        if (!(spanM > 0.0))
        {
            return 0.0;
        }
        F64 r = v - std::floor(v / spanM) * spanM;
        if (r < 0.0)
        {
            r += spanM;
        }
        if (r >= spanM)
        {
            r -= spanM;
        }
        if (r < 0.0 || r >= spanM)
        {
            r = 0.0;
        }
        return r;
    }
}

#endif
