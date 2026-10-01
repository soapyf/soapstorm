/**
 * @file ssdeckcellsoftcore.h
 * @brief Atmo Magic: the OBSERVER'S cell-lattice softening - one per-cell quantity read continuously. Header-only core. CONTRACT.
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

#ifndef SS_DECKCELLSOFTCORE_H
#define SS_DECKCELLSOFTCORE_H

// <SS:Nexii> A CORE header (lldefs.h, other ss*core.h, <cmath>, <cstdint> only). THE DEFECT THIS EXISTS FOR (sixth build report, two screenshots: "hard stair-stepped rectangular tiling across the whole cloud field" top-down, and "large hard-edged grey wedges" from deck altitude): the observer's hero frame shift was a STEP FUNCTION on the 260 m cell lattice. ssVolCloudF.glsl read `hero_inf = ss_storm_influence(ss_hero_c, ss_hero.w, ss_storm_samplePoint(air))` - samplePointM floors the fragment's own air position to its cell CENTRE - and then `gate_air = air.xy - ss_frame_heroShift(hero_inf)`. air.xy is continuous and heroShift was piecewise constant per cell, so gate_air JUMPED at every cell wall, and gate_air is the coordinate every base-anchored pattern read takes: the veil's Penrose mottle (SS_SHEET_TILE_M), its de-tiled presence cut, its ss_field_occupancy gate, the puffs' shape_air, and the virga body/streak reads. The jump is bounded by |grad influence| * CELL_M * |shift cap| = (1.5 / (0.6 r)) * 260 * min(0.15 r, 390) - 97 m at r = 2076 m - which is 0.37 of a whole occupancy CELL, so the gate's verdict could flip across one pixel and the veil stepped from present to absent on a straight world-axis line. It is confined to the influence FALLOFF RING (inside INFLUENCE_CORE * r the influence is a saturated 1 and the shift is constant; outside r it is 0), which is exactly the stair-stepped annulus the screenshots show, centred on the hero and not on the camera.
// <SS:Nexii> THE FIX, and why it is this one rather than "stop quantizing": the quantization is not an accident, it is ssdeckframecore.h's producer/observer rule - the CPU builder evaluates the hero's influence ONCE PER CELL, at that cell's own centre, and the observer has to recover the same number or the pattern it reads is not the pattern the puff was built from. Dropping the quantization would break that lockstep everywhere; keeping it breaks continuity everywhere. The bilinear cell-centre blend below does both: it is EXACTLY the per-cell value at every cell centre (t is 0 there, so the blend collapses to the one corner), so the producer/observer agreement is preserved at every point the producer ever evaluated, and it is continuous in between, so no surface can show a wall. It is also not a new idiom in this codebase - ssVolCloudF.glsl's own ss_field_occupancy softens the builder's BINARY per-cell gate over exactly this lattice for exactly this reason ("blending verdicts over exactly one cell ... with no seam at cell walls"), and this is that same stencil applied one step earlier, to the frame the gate itself is read in.
// <SS:Nexii> THE LATTICE, stated because two different cell indices are in play and mixing them is the easy mistake: SSStormCouple::samplePointM's index is floor(air / cellM) - the cell CONTAINING the point - while the 2x2 stencil here is floor(air / cellM - 0.5) - the lower-left of the four cells whose CENTRES bracket the point. They are different indices on purpose. The stencil is the one that can interpolate, and it is the one ss_field_occupancy already uses; at a cell centre the two agree by construction (see cellCentreExact's invariant and the named test).
// Bodies here are formulas; ssVolCloudF.glsl and ssvolcloud.cpp transliterate them and V:\Scratch\atmo\tests\unit_deck_cellsoft.cpp proves the transliteration and every invariant below.
#include "lldefs.h"
#include "ssstormcouplecore.h"

#include <cmath>
#include <cstdint>

namespace SSDeckCellSoft
{
    // The cubic ease GLSL's smoothstep uses on its interior, spelled as ss_field_occupancy spells it (t*t*(3-2t)) so the
    // transliteration is character for character. Invariants: 0 at 0; 1 at 1; monotone non-decreasing on [0,1].
    inline F32 cubicStep(F32 t) { return t * t * (3.f - 2.f * t); }

    // GLSL mix()'s OWN spelling - a*(1-t) + b*t - deliberately not SSDeckFrame::lerpExact's a + (b-a)*t, because the
    // shader side of this formula is a mix() call and the twin test compares the two bit-for-bit. Invariants: equals a
    // bit-exactly at t 0; equals b bit-exactly at t 1.
    inline F32 mixExact(F32 a, F32 b, F32 t) { return a * (1.f - t) + b * t; }

    // The 2x2 cell-centre stencil around an AIR-frame point: the lower-left cell index and the eased fractions.
    // Invariants: at a cell centre (air == (c + 0.5) * cellM) the index is c and both fractions are exactly 0;
    // ix/iy are monotone non-decreasing in air; tx/ty are in [0,1]; identical to ss_field_occupancy's own
    // `q = air / CELL_M - 0.5; i = floor(q); t = t*t*(3-2t)` block.
    struct Stencil
    {
        S32 ix = 0, iy = 0;
        F32 tx = 0.f, ty = 0.f;
    };

    inline Stencil stencilAt(F32 airX, F32 airY, F32 cellM)
    {
        Stencil s;
        const F32 qx = airX / cellM - 0.5f;
        const F32 qy = airY / cellM - 0.5f;
        s.ix = (S32)std::floor(qx);
        s.iy = (S32)std::floor(qy);
        s.tx = cubicStep(qx - (F32)s.ix);
        s.ty = cubicStep(qy - (F32)s.iy);
        return s;
    }

    // The WORLD-frame centre of cell (cx, cy) - the exact point SSStormCouple::samplePointM returns for any air
    // position inside that cell, so the two sides of the producer/observer contract name one point.
    // Invariants: equals samplePointM(air) for every air with floor(air / cellM) == (cx, cy).
    inline void cellCentreM(S32 cx, S32 cy, F32 cellM, F32 driftX, F32 driftY, F32& outX, F32& outY)
    {
        outX = ((F32)cx + 0.5f) * cellM + driftX;
        outY = ((F32)cy + 0.5f) * cellM + driftY;
    }

    // The bilinear blend itself, x first then y - the same order and the same nesting ss_field_occupancy uses.
    // Invariants: equals v00 at (0,0); in [min, max] of the four corners; continuous in tx/ty.
    inline F32 blend(F32 v00, F32 v10, F32 v01, F32 v11, F32 tx, F32 ty)
    {
        return mixExact(mixExact(v00, v10, tx), mixExact(v01, v11, tx), ty);
    }

    // THE ONE FORMULA the fix is: the hero cell's radial influence, read at the FOUR cell centres that bracket this
    // air point and blended, in place of the single read at the point's own quantized cell centre.
    //
    //     heroInfluenceSoft(air) = blend( influence(centre(i+dx, j+dy)) )  over the 2x2 stencil
    //
    // Invariants, all four named in unit_deck_cellsoft.cpp:
    //   * LOCKSTEP: at every cell centre it equals SSStormCouple::influence at that cell's own samplePointM centre,
    //     bit-exactly - so the producer's per-cell number is recovered exactly wherever the producer evaluated it.
    //   * CONTINUOUS: no discontinuity anywhere, in particular none at a cell wall (the shipped quantized read jumps
    //     by up to |grad influence| * cellM there; see the header note for the 97 m figure).
    //   * BOUNDED: in [0, 1], and 0 whenever every stencil corner is at or beyond the radius, 1 whenever every corner
    //     is inside INFLUENCE_CORE * radius - so the saturated plateau and the far field are unchanged.
    //   * radius <= 0 (no hero) returns 0, making every downstream gateAir an identity read exactly as before.
    inline F32 heroInfluenceSoft(F32 airX, F32 airY, F32 cellM, F32 driftX, F32 driftY,
                                 F32 heroCx, F32 heroCy, F32 heroRadius)
    {
        if (heroRadius <= 0.f)
        {
            return 0.f;
        }
        const Stencil s = stencilAt(airX, airY, cellM);
        F32 cx0 = 0.f, cy0 = 0.f;
        cellCentreM(s.ix, s.iy, cellM, driftX, driftY, cx0, cy0);
        const F32 i00 = SSStormCouple::influence(heroCx, heroCy, heroRadius, cx0, cy0);
        const F32 i10 = SSStormCouple::influence(heroCx, heroCy, heroRadius, cx0 + cellM, cy0);
        const F32 i01 = SSStormCouple::influence(heroCx, heroCy, heroRadius, cx0, cy0 + cellM);
        const F32 i11 = SSStormCouple::influence(heroCx, heroCy, heroRadius, cx0 + cellM, cy0 + cellM);
        return blend(i00, i10, i01, i11, s.tx, s.ty);
    }

    // The law this replaces, kept as a named FAILING CONTROL for the ladder (PLAN.md: a rung needs a control that
    // fails on the axis it measures). This is the shipped read - one influence at the point's OWN quantized cell
    // centre, ss_storm_samplePoint's lattice - and its defect is that it is piecewise constant, so every cell wall
    // inside the falloff ring is a step. Never called by shipping code once the fix lands.
    inline F32 heroInfluenceQuantized(F32 airX, F32 airY, F32 cellM, F32 driftX, F32 driftY,
                                      F32 heroCx, F32 heroCy, F32 heroRadius)
    {
        if (heroRadius <= 0.f)
        {
            return 0.f;
        }
        F32 px = 0.f, py = 0.f;
        SSStormCouple::samplePointM(airX, airY, cellM, driftX, driftY, px, py);
        return SSStormCouple::influence(heroCx, heroCy, heroRadius, px, py);
    }
}

#endif
