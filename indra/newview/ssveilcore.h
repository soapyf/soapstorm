/**
 * @file ssveilcore.h
 * @brief Atmo Magic: the BASE VEIL's own law - base amplitude, coverage holes, rim fade-IN, outward reach. Header-only core. CONTRACT.
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

#ifndef SS_VEILCORE_H
#define SS_VEILCORE_H

// <SS:Nexii> A CORE header (lldefs.h, other ss*core.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_far_clouds.md section 2 steps 2-3 and section 4, doc/atmo_magic_phase8_show.md section 5. THE FINDING THIS EXISTS FOR (third build report, PLAN.md section 25 item 3): the veil was invisible in a top-down build shot because its density was `base * presence * occupancy * edgeFade * near_fade` - occupancy is the PUFFS' own per-cell coverage gate and edgeFade is the PUFFS' outward-only rail (8000 -> 0 at 9800), so the sheet was a strict SUBSET of the puff footprint that died exactly where the puffs died. It had no fade-IN term at all, and therefore could not do the one job the user asks of it: "strongly fade in to help with the sparse puffs at the outer rim (but also continue to support the existing volumetric cloud field at the bottom of the deck)". This header is that law, in one place, transliterated by ssVolCloudF.glsl's SHEET branch and driven by ssvolcloud.cpp's sheet draw.
// <SS:Nexii> The split the law rests on: the veil KEEPS the sky's genuine geography (the coverage gate's holes, softened bilinearly - `holesSoft`, ss_field_occupancy) because a hole in the deck must stay a hole all the way through, and DROPS the deck's rendering economy (the LOD's far thinning and the puffs' edge fade) because those are camera-distance decisions about how many bodies to draw, not statements about where cloud exists (ssdecklodcore.h's own header: "LOD here is a function of CAMERA DISTANCE ONLY ... never the field"). The old law confused the two: it multiplied the veil by a RENDER decision. The new one inverts that decision instead - the veil rises exactly as the puffs are given up.
// <SS:Nexii> [interaction: doc/atmo_magic_phase8_show.md section 5, the horizon deck] reachOut is a PLACEHOLDER for 8c and says so: 8c is design text only (no HORIZON_* symbol exists anywhere in the tree as of 2026-09-06). When the horizon deck's far annulus (HORIZON_INNER_M ~8 km to HORIZON_OUTER_M ~200 km, shaded from the same de-tiled presence field) is built, that annulus REPLACES reachOut - the veil goes back to ending at its rim and the annulus carries the sky from there to the geometric horizon, with the crossfade living on the annulus's inner edge instead of here. Until then reachOut buys the user's "strongly fade in ... at the outer rim" a few kilometres of sky past the puffs' 9800 m for the price of one coarse tile ring.
// Bodies here are formulas; ssVolCloudF.glsl and ssvolcloud.cpp transliterate them and twin_veil_law.cpp proves the transliteration.
#include "lldefs.h"
#include "ssdecklodcore.h"

#include <cmath>
#include <cstdint>

namespace SSVeil
{
    // The veil's base amplitude ramp against its own mottle read (the Penrose five-way sample of the puff texture,
    // `sheet_n` in the shader). UNCHANGED numbers - these are the literals the sheet branch has always clamped with
    // (`clamp(0.30 + 0.40 * sheet_n, 0.0, 0.75)`), lifted here so one place owns them. The cap stays 0.75 for the
    // reason the shader's own comment gives: the veil is THIN cloud and its thin half is where the shared sun-through
    // fringe lives; run denser and it reads as a black slab under a gloom-crushed deck.
    constexpr F32 BASE_MIN  = 0.30f;
    constexpr F32 BASE_SPAN = 0.40f;
    constexpr F32 BASE_CAP  = 0.75f;

    // The veil's OWN outward rails, past the puffs' DECK_EDGE_M (9800). The puffs stop at 9800 because that is where
    // the dome band's horizon melt takes over for THEM (ssdecklodcore.h DECK_EDGE_M, lldrawpoolwlsky.cpp
    // SS_DECK_EDGE_M); the veil is a flat sheet of thin cloud with no bodies to run out of, so it keeps going and
    // dissolves on rails of its own. REACH_START_M is FIELD_DRAW_M exactly, not a new number: the veil reaches full
    // rimIn at DECK_EDGE_M and holds it across the 9800->10000 stub before the reach fade starts, so there is no
    // distance at which both the rim fade-in and the reach fade-out are partial (the one place a "fade in then
    // immediately out" could pinch the veil to a thin bright ring).
    constexpr F32 REACH_START_M = SSDeckLod::FIELD_DRAW_M; // 10000
    constexpr F32 REACH_END_M   = 14000.f;                 // the veil's last metre; VEIL_REACH_M in the design text
    constexpr F32 VEIL_REACH_M  = REACH_END_M;             // the design doc's name for the same number

    // Tiling beyond the puff field. The sheet is emitted as quads on the deck's own CELL_M lattice (see ssvolcloud.cpp's
    // sheet draw for why a single big rect cannot work: the squash bends the plane, so a wide triangle's interpolated
    // world position misses its true plane point by hundreds of metres and the veil swims). Out to FINE_REACH_M the
    // tile stays CELL_M so the veil's frame is bit-identical to the puffs'. Beyond it the tile doubles: a 520 m quad
    // at 10-14 km subtends under a third of a degree of extra interpolation error, and the alternative - CELL_M tiles
    // all the way to 14 km - nearly doubles a draw the far-clouds doc already flags as one of the two dominant
    // per-frame costs ("the veil's ~4900-quad immediate-mode emission every frame").
    // <SS:Nexii> 8g F1 (2026-09-06 review): the two tile sizes are ONE grid subdivided, never two independently culled rings. The draw walks COARSE_TILE_M blocks once, culls a block on its own centre against REACH_END_M, and subdivides the blocks whose centre is inside FINE_REACH_M into SUB_PER_SIDE^2 CELL_M children; every surviving block is therefore tiled exactly once. The shipped code used to run two loops with two centre culls on two tile sizes and lost a 260 m radial notch at ~10 km on some bearings while double-blending on others (unit_veil_sheet_partition.cpp measures both, new and control). SUB_PER_SIDE is the reason a child corner is still on the puffs' own lattice: the block origin is an integer multiple of COARSE_TILE_M in the drift-slid air frame, and COARSE_TILE_M is SUB_PER_SIDE * CELL_M, so every child corner is k * CELL_M + drift for an integer k. Retuning COARSE_TILE_M without SUB_PER_SIDE breaks that and the static_assert says so.
    constexpr F32 FINE_REACH_M  = SSDeckLod::FIELD_DRAW_M;   // 10000, blocks whose centre is inside this subdivide to CELL_M (260 m)
    constexpr S32 SUB_PER_SIDE  = 2;                         // CELL_M children per block side inside FINE_REACH_M
    constexpr F32 COARSE_TILE_M = 2.f * SSDeckLod::CELL_M;   // 520 m, the ONE block the sheet draw's single loop walks

    static_assert(COARSE_TILE_M == (F32)SUB_PER_SIDE * SSDeckLod::CELL_M,
                  "SSVeil: the coarse block must be a whole number of CELL_M cells on a side, or the subdivided "
                  "children stop landing on the puffs' own lattice");

    // Local smoothstep, spelled exactly as GLSL's is (and as ssdecklodcore.h's own eases are), so the transliteration
    // in the sheet branch is a character-for-character match rather than a same-shape approximation.
    inline F32 smooth01(F32 e0, F32 e1, F32 x)
    {
        const F32 t = llclamp((x - e0) / llmax(e1 - e0, 1e-6f), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // base(coverage): the veil's amplitude from its own mottle. Invariants: BASE_MIN at coverage 0; monotone
    // non-decreasing; never above BASE_CAP; equals the shader's old clamp(0.30 + 0.40 * sheet_n, 0, 0.75) exactly.
    inline F32 base(F32 coverage)
    {
        return llclamp(BASE_MIN + BASE_SPAN * coverage, 0.f, BASE_CAP);
    }

    // thinFracAt(horizDist): the LOD keep fraction at this distance - SSDeckLod::keepFrac, no more. It is a parameter of
    // veilAlpha rather than an internal call for two reasons: the twin test must feed both sides the same value, and
    // naming it in the signature makes it impossible for a call site to "helpfully" multiply the thinning back IN
    // (the exact bug this core replaces). Invariants: identical to SSDeckLod::keepFrac for every input.
    inline F32 thinFracAt(F32 horizDist) { return SSDeckLod::keepFrac(horizDist); }

    // keepNorm(thinFrac): keepFrac renormalized so its own floor reads as zero - (k - THIN_KEEP_MIN) / (1 - THIN_KEEP_MIN),
    // clamped. IDENTITY (proved in unit_veil_law.cpp, and the reason the shader can spell this as one smoothstep):
    // because keepFrac(d) = lerp(1, THIN_KEEP_MIN, s) with s = smoothstep(THIN_START_M, FIELD_DRAW_M, d), we get
    // keepNorm(keepFrac(d)) == 1 - smoothstep(THIN_START_M, FIELD_DRAW_M, d) EXACTLY, for every d.
    // Invariants: 1 at thinFrac 1; 0 at thinFrac THIN_KEEP_MIN and below; in [0,1]; monotone non-decreasing in thinFrac.
    inline F32 keepNorm(F32 thinFrac)
    {
        return llclamp((thinFrac - SSDeckLod::THIN_KEEP_MIN) / (1.f - SSDeckLod::THIN_KEEP_MIN), 0.f, 1.f);
    }

    // puffRimShare(horizDist, thinFrac): the share of the deck's own rim presence still being DRAWN at this distance -
    // the normalized keep fraction times the puffs' edge fade. This is the quantity the veil is the complement of, and
    // it is exactly what the puffs give up going outward: the LOD's thinning removes bodies from THIN_START_M (5000)
    // outward, and edgeFade takes what is left to nothing between FIELD_FADE_START_M (8000) and DECK_EDGE_M (9800).
    // Invariants: 1 at and inside THIN_START_M; 0 at and beyond DECK_EDGE_M; in [0,1]; monotone non-increasing in
    // horizDist (a product of two non-negative non-increasing factors).
    inline F32 puffRimShare(F32 horizDist, F32 thinFrac)
    {
        return keepNorm(thinFrac) * SSDeckLod::edgeFade(horizDist);
    }

    // rimIn(horizDist, thinFrac): THE FADE-IN WEIGHT. Stated as a formula, per the contract:
    //
    //     rimIn(d) = 1 - puffRimShare(d)
    //              = 1 - keepNorm(keepFrac(d)) * edgeFade(d)
    //              = 1 - (1 - smoothstep(THIN_START_M, FIELD_DRAW_M, d)) * (1 - smoothstep(FIELD_FADE_START_M, DECK_EDGE_M, d))
    //
    // - the COMPLEMENT of the deck's surviving rim presence, so "the veil rises by exactly what the puffs give up" is
    // an identity (rimIn + puffRimShare == 1 everywhere), not a tuning. It follows keepFrac's own curve where keepFrac
    // is the term that moves (5000-8000 m), and edgeFade's where that one does (8000-9800 m), and it is 1 precisely at
    // DECK_EDGE_M, where the last puff has gone.
    // Invariants: 0 at and inside THIN_START_M; 1 at and beyond DECK_EDGE_M; in [0,1]; monotone non-decreasing in horizDist.
    inline F32 rimIn(F32 horizDist, F32 thinFrac)
    {
        return 1.f - puffRimShare(horizDist, thinFrac);
    }

    // <SS:Nexii> WHERE rimIn ENTERS THE LAW, and the design finding that decided it (unit_veil_law.cpp caught this as a
    // failing assertion before it shipped): rimIn is NOT a factor of the base amplitude. The obvious spelling,
    // base(coverage) * rimIn, is wrong in both directions and there is no third option in a pure product - a rimIn that
    // starts at 0 DELETES the veil inside 5 km, which is the "continue to support the existing volumetric cloud field
    // at the bottom of the deck" half of the user's own sentence and the job the veil has been doing since it was
    // built; a rimIn that starts at 1 has nowhere left to rise. So rimIn multiplies the DEFICIT instead: the veil's
    // amplitude is its ordinary base near the camera and climbs toward RIM_FULL at the rim.
    //
    //     amp(coverage, rimW) = base(coverage) + (RIM_FULL - base(coverage)) * rimW
    //
    // RIM_FULL sits ABOVE BASE_CAP deliberately. BASE_CAP 0.75 is a NEAR-FIELD limit and its reason is near-field: a
    // dense flat plane a few tens of metres over your head shows its whole underside at once and reads as a black slab
    // under a gloom-crushed deck (the sheet branch's own comment). At 8-14 km the veil is seen at a grazing angle,
    // through the haze and into the dome handoff, and it is the ONLY cloud left out there - the slab failure cannot
    // happen and the thing to avoid instead is a transparent rim. 0.90 rather than 1.0 keeps the shared sun-through
    // fringe alive at the horizon.
    // Invariants: equals base(coverage) at rimW 0; equals RIM_FULL at rimW 1; monotone non-decreasing in rimW for every
    // coverage (RIM_FULL > BASE_CAP >= base); in [0, RIM_FULL].
    constexpr F32 RIM_FULL = 0.90f;

    inline F32 amp(F32 coverage, F32 rimW)
    {
        const F32 b = base(coverage);
        return b + (RIM_FULL - b) * rimW;
    }

    // reachOut(horizDist): the veil's own outward dissolve, on ITS rails - 1 through REACH_START_M, smoothstep to 0 at
    // REACH_END_M. This is the term that puts sheet where there is no puff at all, and the one 8c's annulus replaces
    // (see the header note). Invariants: 1 at and inside REACH_START_M; 0 at and beyond REACH_END_M; monotone
    // non-increasing; continuous.
    inline F32 reachOut(F32 horizDist)
    {
        return 1.f - smooth01(REACH_START_M, REACH_END_M, horizDist);
    }

    // THE VEIL LAW. horizDist is HORIZONTAL distance from the camera (the same `horiz` the sheet branch already
    // computes and the same quantity edgeFade takes); coverage is the veil's own mottle read; holesSoft is the
    // bilinearly softened coverage gate (ss_field_occupancy) times the sheet's own noise-map hole cut - the sky's
    // genuine holes, kept whole; thinFrac is the LOD keep fraction, which enters ONLY through rimIn's complement and
    // is never multiplied in.
    //
    //     veilAlpha(d, coverage, holesSoft, thinFrac) = amp(coverage, rimIn(d, thinFrac)) * holesSoft * reachOut(d)
    //
    // Two claims this law makes that the ladder proves rather than asserts (unit_veil_law.cpp):
    //   * BIT-IDENTICAL to the law it replaces inside THIN_START_M - rimIn is 0 and reachOut is 1 there, so
    //     veilAlpha reduces to base(coverage) * holesSoft exactly, which is what the deck's floor has always been.
    //     The bottom-of-deck support is not merely "kept", it is unchanged to the last bit.
    //   * >= the old law at EVERY distance, strictly greater everywhere past THIN_START_M. Algebraically:
    //     new - old = base * (1 - edgeFade) + (RIM_FULL - base) * rimIn, both terms non-negative.
    //
    // The near fade (SS_SHEET_NEAR_M, true eye distance, a parallax problem and not a reach problem) stays in the
    // shader and is deliberately NOT part of this law: it is a function of eye_dist, not of horizDist, and folding it
    // in here would put a second distance metric into a one-metric formula.
    // Invariants: in [0, RIM_FULL]; 0 wherever holesSoft is 0 (a genuine hole stays a hole); 0 at and beyond
    // REACH_END_M; monotone non-decreasing in horizDist on [0, REACH_START_M] for fixed coverage/holesSoft.
    inline F32 veilAlpha(F32 horizDist, F32 coverage, F32 holesSoft, F32 thinFrac)
    {
        return amp(coverage, rimIn(horizDist, thinFrac)) * holesSoft * reachOut(horizDist);
    }

    // The law this replaces, kept as a named FAILING CONTROL for the ladder (PLAN.md lesson: a scenario must vary the
    // failing axis with a failing control). This is ssVolCloudF.glsl's pre-8g sheet density with its distance terms
    // only - base * holes * edgeFade - and its defect is visible in one line of numbers: it is monotone non-INCREASING
    // over the rim and exactly 0 from DECK_EDGE_M outward, so at the outer rim where the puffs are sparse the veil is
    // sparser, and past 9800 m there is nothing at all. Never called by shipping code.
    inline F32 veilAlphaOld(F32 horizDist, F32 coverage, F32 holesSoft)
    {
        return base(coverage) * holesSoft * SSDeckLod::edgeFade(horizDist);
    }

    // The sheet draw's ONE loop radius, in COARSE_TILE_M blocks, so ssvolcloud.cpp and the test agree on the counts:
    // ceil(REACH_END_M / COARSE_TILE_M) = 27 -> a 55x55 = 3025 candidate square, culled on block centre against
    // REACH_END_M alone (2284 survive with the camera at a tile corner; 1160 of those subdivide). There is
    // deliberately no fineTileRadius() beside it any more - a second radius is a second loop, and a second loop over
    // a second tile size is exactly the F1 defect. Invariants: covers REACH_END_M in every direction from a block
    // origin (27 * 520 = 14040 >= 14000).
    inline S32 coarseTileRadius() { return (S32)std::ceil(REACH_END_M / COARSE_TILE_M); }
}

#endif
