/**
 * @file ssdeckshadecore.h
 * @brief Atmo Magic: the puff deck's structural shading (facing / through-layer shade / form / buried) - the ONE formula site the veil sheet, every fine puff and every Tier B macro body share. Header-only core.
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

#ifndef SS_DECKSHADECORE_H
#define SS_DECKSHADECORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath> only). Phase 6d review finding 3: the deck's structural shading block (facing term, exponential shade through the layer, beam gate, rim ease, buried depth) had been spelled three times in ssvolcloud.cpp - the base veil, the fine puff loop, the Tier B macro body - as shell arithmetic the harness could not compile (PLAN.md lesson 2). It lives here once - VERBATIM from the fine puff loop's copy, whose constants are now the LEGACY set the SSAtmoCloudShadeCalibration FALSE branch still reproduces bit-for-bit (see Calibration below for the retune and the measurement behind it) - and the three call sites pass their own inputs: the fine puff and the macro body pass (up, max(cellHeight - up, 0), up, coreness, rim); the veil passes its fixed representative depths (facingUp 0.15, aboveDepth 0.65, belowDepth 0.65, coreness 1, rim 0). V:/Scratch/atmo/tests/deckshadecore.cpp transliterates the three OLD shell blocks and pins the fine/macro results bit-identical and the veil's within 1e-6 (the veil's old facing term associated its multiplications differently, so a last-bit difference is possible; measured 0 over the test grid). Pure functions of their arguments - no camera, no time. The one setting that reaches them is the `calibrated` flag the call sites pass (SSAtmoCloudShadeCalibration, an in-build A/B against the pre-retune constants exactly as SSAtmoCloudLightVariant is): it selects a constant set and nothing else, it is stated at every call site, and like every other per-client render dial it changes only what this client's own deck LOOKS like - no position, no gate, no hash, nothing replicated depends on it.
#include "lldefs.h"

#include <cmath>

namespace SSDeckShade
{
    // <SS:Nexii> The SHADE CALIBRATION, and the measurement that forced it. Every number in this comment is printed by V:/Scratch/atmo/tests/unit_deckshade_calibration.cpp, which tabulates the DIRECT-SUN SHARE of a puff's light - sun_body / (ambient + sun_body) with ssVolCloudF.glsl's own sun_body = vary_ss_sunlit * vary_color.r - across the sun, deck, height and coreness ranges the field can build. Read it before touching a constant here. The headline in doc/atmo_magic_backlog.md 7.4 (form 0.033, 'the deck is ambient-dominated') is TRUE BUT LOCAL and the retune is scoped to what was actually measured: over a real deck (thicknessTerm 1.0-1.5, which is any convective or moist sky - ssatmoenvcloudfieldstate.cpp saturates the thickness half at 500 m and coverage runs 0.7-1.0) in daylight the old mean share was 0.582 at the LID of a column, 0.371 half way up and 0.209 at the floor - and 0.083 at the backlog's own corner (full coverage, coreness 1, sun at 17 degrees, mid column, form 0.024). So the crush is not uniform; it is concentrated where coreness, coverage and airmass are all high, and it has one pathological shape. Two things were wrong and neither was the exponential itself. (1) The neighbour term coreness * (1 - sunZ) * 1.5 is DEPTH-FREE, so it is 100% of the exponent at every LID point: a puff sitting on top of its column with nothing at all above it wore shade 0.207 on a storm deck, and 250 of 1800 daylight lid samples read under a 0.50 direct share, worst 0.254. A lid that reads at a quarter direct is the flat-deck complaint's own mechanism. It keeps its shape - neighbour shading really does grow as the sun drops - at a quadratic falloff and a third of the scale, worth 0.02 to 0.27 of the exponent at working sun elevations and approaching its old weight only at a horizon sun. (2) The slant floor 0.35 traversed the column's OWN depth along a 2.857x airmass, but a cell is 260 m wide (ssvolcloud.cpp CELL_M) against a several-hundred-metre column, so a real ray leaves its own column long before that and enters the NEIGHBOURS - term (1)'s job, not the slant's. The floor moves to 0.6 (the slant caps at 1.667, elevation ~37 deg, between the 51 deg a 320 m column over a 260 m cell geometrically implies and the 20 deg the old floor implied) and the coefficient rises with it, so what the column term does over the sun's range is FLATTEN: slantK / max(sunZ, floor) ran 0.9 (zenith) to 2.571 (below 20 deg) and now runs 1.3 to 2.167 - deeper at a high sun, where the beam really does cross the whole column, shallower at a low one, where it does not. The facing spread widens 0.2 -> 0.3 with them: it is the column's own top-to-bottom cue and at 0.2 a zenith sun could only ask for 0.7:0.3 between a lid and a floor. Two laws fix the four constants, both measured: the LID law (nothing above the puff -> share >= 0.50 everywhere in the daylight set; met at 1800/1800, worst 0.534) fixes coreK and the quadratic falloff, and the BASE law (the floor of the thickest full-coverage deck -> share <= 0.10 at every sun elevation; measured worst 0.0968) fixes slantK against the floor the aspect ratio gives. MEASURED CONSEQUENCES, since the A/B exists to judge them: over a real deck in daylight the lid gains about a quarter of its total light (share 0.582 -> 0.662), the middle about 3% (0.371 -> 0.391) and the FLOOR LOSES about 4% (0.209 -> 0.172) - this is vertical contrast, not a brightness dial; the backlog corner goes 0.083 -> 0.245; the base veil's sheet brightens at a low sun (share 0.124 -> 0.260 at sunZ 0.3) and dims slightly at a high one (0.361 -> 0.317), which reads as the deck's underside catching a low sun instead of being uniformly grey. HONEST LIMIT, and the reason this is not billed as the fix for SSAtmoCloudLightVariant's invisibility: the same rung measures POWDER's own authority over a puff's light (share * (1 - ss_powder)) at a mean of 0.356 BEFORE this retune and 0.367 after. The dial already had roughly a third of a puff's light to spend, so if it still shows nothing on a build, the cause is downstream of the form term and not this calibration. [interaction: ssVolCloudF.glsl's sun_body/puff_light, SSAtmoCloudLightVariant]
    struct Calibration
    {
        F32 mSlantK;     // the column-above term's coefficient
        F32 mSlantFloor; // the sunZ floor that caps the slant - the aspect ratio at which the ray leaves its own column
        F32 mCoreK;      // the neighbour-shading term's coefficient
        F32 mCoreSq;     // 0 = the neighbour term falls off as (1 - sunZ), 1 = as (1 - sunZ)^2
        F32 mFacingAmp;  // the facing term's half-spread about 0.5
    };
    // The pre-retune constants, bit-for-bit: SSAtmoCloudShadeCalibration FALSE restores exactly this look, and
    // V:/Scratch/atmo/tests/deckshadecore.cpp pins the whole FALSE branch against the old shell text with SS_CHECK_BITS.
    constexpr Calibration LEGACY  { 0.9f, 0.35f, 1.5f,  0.f, 0.2f };
    constexpr Calibration RETUNED { 1.3f, 0.60f, 0.55f, 1.f, 0.3f };
    // The one selector: TRUE is the retuned calibration (the shipping default), FALSE the pre-retune constants.
    inline Calibration calibration(bool calibrated)
    {
        return calibrated ? RETUNED : LEGACY;
    }

    // The layer's optical thickness term th: (0.5 + clamp(thicknessM / 500)) * (0.35 + 0.65 * coverage). Invariants: in
    // [0.175, 1.5] for coverage in [0,1]; monotone in both arguments.
    inline F32 thicknessTerm(F32 thicknessM, F32 coverage)
    {
        return (0.5f + llclamp(thicknessM / 500.f, 0.f, 1.f)) * (0.35f + 0.65f * coverage);
    }

    // The facing term: clamp(0.5 + amp * (sunZ * (facingUp - 0.5) * 2)), amp 0.2 legacy / 0.3 retuned. A puff high in its
    // column facing a high sun brightens, one low in it darkens. Invariants: in [0.5 - amp, 0.5 + amp]; 0.5 at sunZ 0 or
    // facingUp 0.5.
    inline F32 facing(F32 sunZ, F32 facingUp, bool calibrated = true)
    {
        return llclamp(0.5f + calibration(calibrated).mFacingAmp * (sunZ * (facingUp - 0.5f) * 2.f), 0.f, 1.f);
    }

    // The exponential shade through the layer. Sun above the horizon: the column standing ABOVE the point (aboveDepth,
    // in layer fractions) attenuates along the slant 1/max(sunZ, slantFloor) plus a coreness-weighted low-sun neighbour
    // term; sun below: the column BELOW the point (belowDepth) does, plus a flat coreness term. Invariants: in (0, 1]; 1
    // when both depths and coreness are 0; monotone non-increasing in each depth, in coreness and in th; monotone
    // non-decreasing in sunZ above the horizon. The two constant sets and why they differ are in Calibration above.
    inline F32 shade(F32 sunZ, F32 th, F32 aboveDepth, F32 belowDepth, F32 coreness, bool calibrated = true)
    {
        const Calibration c = calibration(calibrated);
        if (sunZ >= 0.f)
        {
            const F32 low = 1.f - sunZ;
            return expf(-(aboveDepth / llmax(sunZ, c.mSlantFloor) * c.mSlantK + coreness * (low * std::lerp(1.f, low, c.mCoreSq)) * c.mCoreK) * th);
        }
        return expf(-(belowDepth / llmax(-sunZ, c.mSlantFloor) * c.mSlantK + coreness * c.mCoreK) * th);
    }

    // The form the fragment stage multiplies in: lerp(0.65, lerp(facing, 0.65, rim), beam) * lerp(1, shade, beam) - the
    // beam gates both terms (a sunless sky wears the flat 0.65), the rim (cubic-eased edge fraction, 0 inside the field)
    // flattens the facing toward 0.65 so the last rows meet the dome band's flat painting. Invariants: 0.65 at beam 0;
    // equals facing * shade at beam 1, rim 0; in (0, 1].
    inline F32 form(F32 facingTerm, F32 shadeTerm, F32 rim, F32 beam)
    {
        return std::lerp(0.65f, std::lerp(facingTerm, 0.65f, rim), beam) * std::lerp(1.f, shadeTerm, beam);
    }

    // The buried depth (Puff::mBuried): how much of the column stands over the point, normalised by the column's own
    // height and NOT beam-gated (a storm deck is dark underneath at midnight too), eased to 0.5 at the rim. Invariants:
    // in [0,1]; 0.5 at rim 1; 0 at up >= cellHeight with rim 0; 1 at up 0 with rim 0.
    inline F32 buried(F32 cellHeight, F32 up, F32 rim)
    {
        return std::lerp(llclamp((cellHeight - up) / llmax(cellHeight, 0.01f), 0.f, 1.f), 0.5f, rim);
    }

    // The fine-puff / macro-body composite: form at (sunZ, th) for a point at height fraction up in a column of height
    // cellHeight with the given coreness, rim and beam. aboveDepth = max(cellHeight - up, 0), belowDepth = up.
    inline F32 puffForm(F32 sunZ, F32 th, F32 up, F32 cellHeight, F32 coreness, F32 rim, F32 beam, bool calibrated = true)
    {
        const F32 above = llmax(cellHeight - up, 0.f);
        return form(facing(sunZ, up, calibrated), shade(sunZ, th, above, up, coreness, calibrated), rim, beam);
    }

    // The base veil's composite: the shade a puff at the deck's floor would wear - a representative low facing height
    // (0.15), a fixed 0.65 of layer over and under it, full coreness, no rim.
    constexpr F32 VEIL_FACING_UP = 0.15f;
    constexpr F32 VEIL_DEPTH     = 0.65f;
    inline F32 veilForm(F32 sunZ, F32 th, F32 beam, bool calibrated = true)
    {
        return form(facing(sunZ, VEIL_FACING_UP, calibrated), shade(sunZ, th, VEIL_DEPTH, VEIL_DEPTH, 1.f, calibrated), 0.f, beam);
    }
}

#endif
