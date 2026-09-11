/**
 * @file ssdeckmacrocore.h
 * @brief Atmo Magic: the far deck's merged macro-puff tier (B) - macro-cell occupancy from the fine gate, coverage-conserving bodies, tier crossfade. Header-only core. CONTRACT.
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

#ifndef SS_DECKMACROCORE_H
#define SS_DECKMACROCORE_H

// <SS:Nexii> A CORE header (lldefs.h, ssdecklodcore.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_far_clouds.md section 2 step 2, Tier B (phase 6d). Beyond TIER_B_M the deck stops placing per-fine-cell puffs and places ONE large body per occupied MACRO cell (a 2x2 block of fine cells, MACRO_CELLS = 2): the macro cell's occupancy is the SHARE of its fine cells that pass the ordinary gate - the SAME gate the fine tier, the shader's ss_cell_occupied and the shadow bake evaluate, so the tiers agree about where cloud exists without a fourth gate implementation; the body's radius covers the block and its alpha is scaled by that share so aggregate coverage is conserved. Tier A and Tier B crossfade over TIER_BLEND_M (A's alpha down, B's up) so body count changes but coverage does not pop - and BOTH halves read ONE scalar, the block's horizontal, drift-only centre distance computed once in the fine loop and carried to the body (the 6d review found the body, at deck altitude, reading its own 3-D post-shift distance and so wB ~ 1 while its fine cells still read wA ~ 1: double coverage at the band's inner edge; a crossfade whose two sides read different distances is not a crossfade). The macro body is placed at the block centre + the block's mean displacement (drift + hero shift + O(z) of its altitude - the producer rule holds per block). The review's earlier rejection of a stride-2 decimation near the squash knee is answered by DISTANCE: at >= 5 km the residual disagreement between a 260 m veil carve and a 520 m body is below what the eye resolves through haze and squash; the fine gate is still what decides occupancy. LOD only: camera distance selects the tier and alphas, never the field. Bodies marked STUB are the implementer's; the invariants are the test author's spec.
// <SS:Nexii> 8g rim de-lattice (2026-09-06, doc/atmo_magic_phase8_show.md section 5 "Mid-field patchiness"): the user's top-down build shot showed the far deck as a REGULAR LATTICE of identical round blobs, one per 520 m tile, behind a stair-stepped tier boundary. Three causes, all here: the body sat at the exact block centre with a fixed radius (the fine tier has had per-sub jitter and a 0.7-1.3 size roll since the start, so only Tier B read as a grid); TIER_BLEND_M was 600 m, barely ONE macro tile, and the weight is per BLOCK, so blocks straddling the rail took near-binary weights on a square lattice - the stair; and THIN_START_M (5000) coincides with TIER_B_M so thinning and merging start together. Fixed here by bodyJitterM + bodyRadiusScale/bodyShape (hashed per block, deterministic, world-state-free - LOD is still camera-only) and TIER_BLEND_M = 3 * MACRO_M so at least three macro tiles sit inside the band and no single tile's rail crossing can carry the whole handoff. The ONE-body-per-macro-cell contract is untouched: jitter moves the one body, it never adds a second.
#include "lldefs.h"
#include "ssatmonoisecore.h"
#include "ssdecklodcore.h"

#include <cmath>
#include <cstdint>

namespace SSDeckMacro
{
    constexpr F32 CELL_M        = 260.f;   // LOCKSTEP ssvolcloud.cpp CELL_M
    constexpr S32 MACRO_CELLS   = 2;       // fine cells per macro cell side (2x2 blocks)
    constexpr F32 MACRO_M       = CELL_M * (F32)MACRO_CELLS;
    constexpr F32 TIER_B_M      = 5000.f;  // the tier RAIL - camera distance the A->B crossfade is centred on (LOCKSTEP: equals SSDeckLod::THIN_START_M). The crossfade BAND straddles this rail by TIER_BLEND_M/2 each side and DOES overlap the fine tier's thinning by design - see the TIER_BLEND_M note below for why and how much.
    // <SS:Nexii> 8g rim de-lattice: 600 -> 3 * MACRO_M (1560 m). The crossfade weight is a per-BLOCK quantity (both halves
    // read the block's one carried distance, by the 6d review's fix), so the band's WIDTH is measured in macro tiles, not
    // metres: at 600 m the band was 1.15 tiles wide, so a block was almost always fully A or fully B and the handoff fell on
    // the square lattice's own tile edges - the stair-step the user photographed. At 3 * MACRO_M three rings of tiles are
    // mid-fade at any time, so the transition is carried by many blocks at many partial weights and has no tile edge to
    // follow. DOCUMENTED OVERLAP (deliberate, THIN_START_M left alone): the band's inner rail is now TIER_B_M -
    // TIER_BLEND_M/2 = 4220 m, INSIDE SSDeckLod::THIN_START_M (5000), so between 5000 and 5780 m the fine tier is thinning
    // (keepPuff/thinAlphaComp) while Tier B is fading in. That is safe because the two conserve coverage in the same
    // currency and independently - thinning compensates the fine alpha it removes, the crossfade weights whatever fine
    // alpha survives by wA - but it means the fine tier's own alpha compensation is live inside the band, which is why the
    // body reads its fine-side alpha through SSDeckLod::thinAlphaComp at the SAME block distance rather than a constant.
    constexpr F32 TIER_BLEND_M  = 3.f * MACRO_M; // A -> B crossfade band straddling TIER_B_M - at least three macro tiles wide
    constexpr F32 BODY_RADIUS_FRAC = 0.85f * 0.5f; // LOCKSTEP the fine puff's base_radius = CELL_M * 0.85 * 0.5; the macro body uses MACRO_M * this
    constexpr F32 BODY_ALPHA_MAX = 1.f;

    // <SS:Nexii> 8g rim de-lattice: the per-block body jitter and size roll, mirroring what the fine tier has always done
    // per sub-puff (jx/jy = (hashUnit - 0.5) * CELL_M * 0.8 and a 0.7 + 0.6 * hashUnit radius roll in ssvolcloud.cpp).
    // MACRO_JITTER_FRAC 0.30 bounds the body centre to a DISC of radius 156 m about its block centre (see the F2 fix
    // paragraph below for why a disc, not two independent axis bounds). Why that bound, in three facts a test pins:
    // (1) 156 < MACRO_M/2 (260), so the body centre never leaves its OWN block - two neighbours can never swap places
    // and "one body per occupied macro cell" stays readable on the ground; (2) 156 < the SMALLEST body radius
    // (BODY_RADIUS_VAR_MIN * bodyRadiusM() = 176.8 m), EUCLIDEAN - the disc's radius IS the offset's Euclidean
    // magnitude by construction, so a jittered body still covers its block's centre - the lattice breaks up without
    // opening a hole where every block used to be covered; (3) the farthest a body can now reach from its block
    // centre is 156 + 1.25 * 221 = 432 m, past the block's own square, which is exactly the point - overlapping
    // neighbours are what stops the eye from reading a grid. That overhang costs the walk NOTHING (lesson 36: check the
    // consumer's alpha chain before paying for a membership pad) - the body's own alpha carries SSDeckLod::edgeFade of
    // its PLACED horizontal distance, zero at DECK_EDGE_M (9800), while eligibility is tested at the block centre inside
    // FIELD_DRAW_M + WALK_PAD_M (>= 10000 + shear + hero + jitter), so a body pushed outward by the jitter draws nothing
    // it would not have drawn and no pad term is owed.
    // <SS:Nexii> F2 fix (review, 2026-09-06): the first cut of this jitter bounded jx and jy INDEPENDENTLY, each on
    // [-cap, cap] - a SQUARE, not a disc. A block could then roll a near-corner jitter (|jx| ~ |jy| ~ cap) whose
    // Euclidean offset reached cap * sqrt(2) = 220.6 m, past the 176.8 m minimum body radius fact (2) exists to
    // guarantee: a block rolling that corner jitter AND a near-minimum radius left its own centre uncovered, the exact
    // hole the bound was chosen to prevent. Fixed by sampling the offset RADIALLY on a DISC of radius cap instead,
    // uniform over AREA (angle = 2*pi*u1, radius = cap*sqrt(u2), u1/u2 ~ U[0,1) independent hash lanes - sqrt(u2), not
    // u2, so equal-area annuli get equal weight rather than crowding samples toward the centre). sqrt(jx^2 + jy^2) <=
    // cap now holds BY CONSTRUCTION, so fact (2) is a true Euclidean statement instead of a per-axis one that quietly
    // assumed the diagonal never happens. Consequence for the mean: a square's mean |offset| is cap * (2/3) *
    // (sqrt(2) + ln(1 + sqrt(2))) = 0.765 * cap (~119 m); the disc's is the exact 2/3 * cap = 104 m. Still zero-mean
    // and symmetric either way, so no cloud mass moves. Pinned by deckmacrocore.cpp's jitter_bound_keeps_the_body_inside
    // and bodyJitter_bounded_pure_and_zero_mean_over_a_population tests, the old per-axis formula run as a failing
    // control against the same centre-coverage bound.
    constexpr F32 MACRO_JITTER_FRAC    = 0.30f;
    constexpr F32 JITTER_TWO_PI        = 6.283185307179586f; // full-turn constant for bodyJitterM's angle lane
    constexpr F32 BODY_RADIUS_VAR_MIN  = 0.80f;  // body radius = bodyRadiusM() * (MIN + SPAN * hashUnit)
    constexpr F32 BODY_RADIUS_VAR_SPAN = 0.45f;  // ... so the scale sweeps 0.80 .. 1.25
    // Area conservation, as a formula: a body's covered area is pi * r^2 * alpha with r = R * s, s = MIN + SPAN * u and
    // u ~ U[0,1] hashed per block. E[s^2] = MIN^2 + MIN*SPAN + SPAN^2/3 = 1.0675, so scaling alpha by BODY_AREA_NORM =
    // 1/E[s^2] = 0.936768 makes E[pi * r^2 * alpha] = pi * R^2 * alpha0 EXACTLY - the unjittered constant. Per-block area
    // is deliberately NOT conserved (that is the variance); the MEAN over a population is, which is what the deck's
    // aggregate coverage - and the eye at 5 km - actually reads. Pinned by deckmacrocore.cpp's 4096-block integration.
    constexpr F32 BODY_AREA_E_S2  = BODY_RADIUS_VAR_MIN * BODY_RADIUS_VAR_MIN
                                  + BODY_RADIUS_VAR_MIN * BODY_RADIUS_VAR_SPAN
                                  + BODY_RADIUS_VAR_SPAN * BODY_RADIUS_VAR_SPAN / 3.f;
    constexpr F32 BODY_AREA_NORM  = 1.f / BODY_AREA_E_S2;
    // Hash lanes for the per-block rolls - distinct constants so the jitter, the size roll and (through SSAtmoNoise's own
    // chain) SSDeckLod::keepPuff's fine-cell thinning decision cannot alias one another into a diagonal pattern.
    // LANE_JITTER_ANGLE / LANE_JITTER_RADIUS (F2 fix, 2026-09-06: renamed from LANE_JITTER_X / LANE_JITTER_Y, which
    // lied once the jitter became a polar disc sample rather than two independent per-axis rolls) drive bodyJitterM's
    // angle and radius; LANE_RADIUS is the unrelated body SIZE roll (bodyRadiusScale) and keeps its name.
    constexpr U32 LANE_JITTER_ANGLE  = 0x4Au;
    constexpr U32 LANE_JITTER_RADIUS = 0x4Bu;
    constexpr U32 LANE_RADIUS        = 0x52u;

    // The macro cell containing fine cell (cx, cy): floor division that is exact for negatives. Invariants: cells 0..1 map
    // to 0, -1..-2 map to -1; pure.
    inline S32 macroIndex(S32 fine)
    {
        return (fine >= 0) ? (fine / MACRO_CELLS) : -(((-fine) + MACRO_CELLS - 1) / MACRO_CELLS);
    }

    // Occupancy share of a macro cell from its MACRO_CELLS x MACRO_CELLS fine-gate verdicts (0/1 each): the count / total.
    // Invariants: in [0,1]; 0 when no fine cell passes; 1 when all do; equals count/4 for 2x2.
    inline F32 occupancy(const U8* fineVerdicts, S32 count)
    {
        if (count <= 0) return 0.f;
        S32 hit = 0;
        for (S32 i = 0; i < count; ++i)
        {
            if (fineVerdicts[i]) ++hit;
        }
        return (F32)hit / (F32)count;
    }

    // Body radius for a macro cell: MACRO_M * BODY_RADIUS_FRAC. Body alpha: fineAlpha * occupancy, capped at BODY_ALPHA_MAX.
    // Coverage conservation: radius^2 * alpha of one body equals the sum over the occupied fine cells of their radius^2 *
    // alpha within 5% for occupancy in {0.25, 0.5, 0.75, 1} (documented approximation - the areas overlap differently).
    inline F32 bodyRadiusM()
    {
        return MACRO_M * BODY_RADIUS_FRAC;
    }
    inline F32 bodyAlpha(F32 fineAlpha, F32 occupancy)
    {
        return llmin(BODY_ALPHA_MAX, fineAlpha * occupancy);
    }

    // <SS:Nexii> 8g rim de-lattice. The per-block hash: pure in (mcx, mcy, salt, lane), independent of the camera (LOD
    // selects the tier, never the field - and this is not even LOD, it is the body's own shape). Invariants: in [0,1);
    // two different lanes at the same block are independent; the same block/salt/lane repeats bit-for-bit forever.
    inline F32 blockHashUnit(S32 mcx, S32 mcy, U32 salt, U32 lane)
    {
        return SSAtmoNoise::hash01(SSAtmoNoise::combine(SSAtmoNoise::combine((U32)mcx, (U32)mcy), lane ^ salt));
    }

    // The body's position offset from its block centre, in metres, sampled uniformly over a DISC of radius cap =
    // MACRO_JITTER_FRAC * MACRO_M (F2 fix, 2026-09-06 - see the comment above MACRO_JITTER_FRAC: two independent
    // per-axis bounds let a corner roll combine past the Euclidean bound fact (2) there needs). Invariants:
    // sqrt(jx^2 + jy^2) <= cap (156 m), BY CONSTRUCTION rather than by luck; zero-mean symmetric about the block
    // centre (E[jx] = E[jy] = 0 - the angle lane is uniform over the full turn) so the block's EXPECTED body position
    // is still its centre - the jitter breaks the lattice without moving cloud mass; mean |offset| over a population
    // = (2/3) * cap = 104 m (E[r] for area-uniform disc sampling); pure in (mcx, mcy, salt).
    inline void bodyJitterM(S32 mcx, S32 mcy, U32 salt, F32& jx, F32& jy)
    {
        const F32 cap    = MACRO_JITTER_FRAC * MACRO_M;
        const F32 angle  = blockHashUnit(mcx, mcy, salt, LANE_JITTER_ANGLE) * JITTER_TWO_PI;
        const F32 radius = cap * std::sqrt(blockHashUnit(mcx, mcy, salt, LANE_JITTER_RADIUS)); // sqrt(u): area-uniform, not radius-uniform
        jx = radius * std::cos(angle);
        jy = radius * std::sin(angle);
    }

    // The body's radius scale for this block. Invariants: in [BODY_RADIUS_VAR_MIN, MIN + SPAN] = [0.80, 1.25]; pure;
    // mean 1.025 (NOT 1 - that is why the alpha carries BODY_AREA_NORM rather than the radius carrying a 1/mean).
    inline F32 bodyRadiusScale(S32 mcx, S32 mcy, U32 salt)
    {
        return BODY_RADIUS_VAR_MIN + BODY_RADIUS_VAR_SPAN * blockHashUnit(mcx, mcy, salt, LANE_RADIUS);
    }

    // Radius and alpha TOGETHER - the one function an emitter calls, so no call site can take the varied radius without
    // the alpha whose normalisation assumes it (or bodyAlpha's unvaried alpha with a varied radius, which would inflate
    // the deck's coverage by E[s^2] = 6.75%). radiusM = bodyRadiusM() * bodyRadiusScale; alpha = bodyAlpha * BODY_AREA_NORM.
    // Invariants: radiusM in [0.80, 1.25] * bodyRadiusM(); alpha <= BODY_ALPHA_MAX; E[pi * radiusM^2 * alpha] over a hashed
    // population equals pi * bodyRadiusM()^2 * bodyAlpha(fineAlpha, occupancy) within 2%; pure.
    inline void bodyShape(S32 mcx, S32 mcy, U32 salt, F32 fineAlpha, F32 occupancy, F32& radiusM, F32& alpha)
    {
        radiusM = bodyRadiusM() * bodyRadiusScale(mcx, mcy, salt);
        // The norm rides on the FINE alpha, inside bodyAlpha's own single cap - not on bodyAlpha's result, which would
        // cap first and then scale, so a saturated body could never reach BODY_ALPHA_MAX again. One formula site.
        alpha   = bodyAlpha(fineAlpha * BODY_AREA_NORM, occupancy);
    }

    // The tier weights at camera distance dist: wA = 1 - smoothstep(TIER_B_M - TIER_BLEND_M/2, TIER_B_M + TIER_BLEND_M/2,
    // dist), wB = 1 - wA. Invariants: wA + wB == 1 within 1e-6; wA 1 well inside; wB 1 well beyond; continuous; monotone.
    inline void tierWeights(F32 dist, F32& wA, F32& wB)
    {
        const F32 lo = TIER_B_M - TIER_BLEND_M * 0.5f;
        const F32 hi = TIER_B_M + TIER_BLEND_M * 0.5f;
        const F32 t = llclamp((dist - lo) / (hi - lo), 0.f, 1.f);
        wB = t * t * (3.f - 2.f * t); // smoothstep
        wA = 1.f - wB;
    }

    // The macro body's altitude fraction: the mean of the occupied fine cells' up_cell hashes (the caller supplies them)
    // so the body sits where its block's mass sits. Invariants: in [0,1]; equals the single value when one cell is occupied;
    // pure.
    inline F32 bodyUp(const F32* ups, const U8* verdicts, S32 count)
    {
        F32 sum = 0.f;
        S32 hit = 0;
        for (S32 i = 0; i < count; ++i)
        {
            if (verdicts[i])
            {
                sum += ups[i];
                ++hit;
            }
        }
        return (hit > 0) ? (sum / (F32)hit) : 0.5f;
    }

    // Whether the macro tier is even eligible for a macro cell: its block centre must be within the walk (SSDeckLod::cellInWalk
    // semantics on the block centre) and at least one fine cell occupied. Pure.
    inline bool macroEligible(F32 blockCentreX, F32 blockCentreY, F32 camX, F32 camY, F32 occupancy)
    {
        return SSDeckLod::cellInWalk(blockCentreX, blockCentreY, camX, camY) && occupancy > 0.f;
    }
}

#endif
