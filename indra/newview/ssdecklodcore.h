/**
 * @file ssdecklodcore.h
 * @brief Atmo Magic: the deck's distance LOD - circular walk, puffs-per-cell ramp, deterministic far thinning, veil rails, deck-order hysteresis. Header-only core. CONTRACT.
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

#ifndef SS_DECKLODCORE_H
#define SS_DECKLODCORE_H

// <SS:Nexii> A CORE header (lldefs.h, ssatmonoisecore.h, ssdeckframecore.h, sswindprofilecore.h, <cmath> only - cores may include cores). Design: doc/atmo_magic_far_clouds.md section 2 steps 1-3 (phase 6a: no shader changes). LOD here is a function of CAMERA DISTANCE ONLY and touches RENDER decisions only - which sub-puffs are placed, their alpha, the walk's extent - never the field: the cell gate, presence, tower and every world-state read stay exactly where they were, so two clients at different distances see the same clouds at different body counts (the determinism rule: camera affects LOD, never the world). Every gate-occupied cell keeps at least sub-puff 0 (constraint 5: the shader's ss_cell_occupied and the shadow bake still report it occupied); thinning removes only higher subs and compensates alpha, so aggregate coverage is conserved rather than a moving cliff at ~6.9km. The veil's hardcoded 6800/9800 rails become uniforms derived here so deck and veil reach SS_DECK_EDGE_M together. 8g CORRECTION (2026-09-06, ssveilcore.h): that last clause is no longer the veil's whole story and must not be read as one - the base veil now fades IN across this file's own rim (SSVeil::rimIn is exactly 1 - keepNorm(keepFrac(d)) * edgeFade(d), the complement of the share of rim presence the puffs still draw) and reaches PAST DECK_EDGE_M on rails of its own; the veil no longer multiplies itself by edgeFade at all. THIN_START_M, FIELD_DRAW_M, THIN_KEEP_MIN and edgeFade are therefore read by a second consumer whose sense is inverted - retuning any of them moves the veil's fade-in as well as the puffs' thinning, in opposite directions. Review correction (2026-09-05, doc/atmo_magic_far_clouds.md phase 6a outcome): a puff is drawn up to WALK_PAD_M from its cell's centre (the wind profile's shear lean, the hero's frame shift, this sub-puff's own jitter, all three stacked), so a pre-gate keyed on the bare FIELD_DRAW_M circle (cellInField) silently starved cells whose centre sat just outside it but whose puffs still leaned in; cellInWalk pads the membership test by that same displacement and is now the builder's actual pre-gate. 8e-b REVIEW N2 (2026-09-06, decided): a virga curtain's own wind-fall skew (SSVirga::SKEW_CAP_M) used to be a fourth term here (phase 8e, F3) but is DELETED - see WALK_PAD_M's own comment for why a skewed card from an invisible candidate never draws, so the pad owes it nothing. Bodies marked STUB are the implementer's; the invariants are the test author's spec.
#include "lldefs.h"
#include "ssatmonoisecore.h"
#include "ssdeckframecore.h"
#include "sswindprofilecore.h"

#include <cmath>
#include <cstdint>

namespace SSDeckLod
{
    constexpr F32 CELL_M          = 260.f;    // LOCKSTEP ssvolcloud.cpp CELL_M
    constexpr F32 FIELD_DRAW_M    = 10000.f;  // LOCKSTEP ssvolcloud.cpp FIELD_DRAW_M
    constexpr F32 FIELD_FADE_START_M = 8000.f;// LOCKSTEP ssvolcloud.cpp FIELD_FADE_START_M
    constexpr F32 DECK_EDGE_M     = 9800.f;   // LOCKSTEP lldrawpoolwlsky.cpp SS_DECK_EDGE_M (the dome's horizon melt) - the veil rails end here
    constexpr F32 SUBS_FULL_M     = 2000.f;   // all puffs_per_cell subs below this camera distance
    constexpr F32 SUBS_TWO_M      = 3500.f;   // two subs until here, one beyond (plus the crossfade bands)
    constexpr F32 SUBS_BLEND_M    = 400.f;    // the alpha crossfade band straddling each sub boundary
    constexpr F32 THIN_START_M    = 5000.f;   // far thinning begins
    constexpr F32 THIN_KEEP_MIN   = 0.25f;    // share of sub>0 puffs kept at FIELD_DRAW_M
    constexpr F32 ALPHA_COMP_MAX  = 2.5f;     // alpha compensation cap for surviving thinned puffs
    constexpr F32 ORDER_HYST_M2   = 250.f * 250.f; // under_on_top flips only when the mean-distance-squared gap exceeds this

    // The farthest a placed puff's world position can land from the lattice cell centre the builder hashed it from:
    // the wind profile's O(z) shear lean (SSWindProfile::SHEAR_CAP_M, bounded), the hero storm's local frame shift
    // (SSDeckFrame::HERO_SHIFT_CAP_M, bounded) and the jitter placeWorld's caller adds (up to CELL_M * 0.4 on each
    // axis). A sum of the LOCKSTEP constants those cores already own, not a hand-tuned literal.
    //
    // <SS:Nexii> 8e-b REVIEW N2 (decided, 2026-09-06): SSVirga::SKEW_CAP_M is DELIBERATELY NOT a term here any
    // more (it was, from phase 8e until this review). A skewed virga card's alpha still multiplies edgeFade of
    // its CANDIDATE's own un-skewed distance (ssvolcloud.cpp's shaft alpha line, F6) - the wind-fall skew is a
    // display-only lean applied AFTER the visibility decision, never something that can pull an invisible
    // candidate (beyond DECK_EDGE_M's edgeFade == 0) into a visible card. A card from a candidate this walk never
    // even collects (see ssvolcloud.cpp's own pre-filter, N2) draws nothing, so the pad owes the skew term
    // nothing: 2500 + 390 + 104 = 2994 m -> 50 cells, not the larger radius the skew term used to force.
    constexpr F32 WALK_PAD_M      = SSWindProfile::SHEAR_CAP_M + SSDeckFrame::HERO_SHIFT_CAP_M + 0.4f * CELL_M;

    // Cell centre inside the bare FIELD_DRAW_M circle? A general circle-membership test on FIELD_DRAW_M alone -
    // NOT the builder's pre-gate (that is cellInWalk below, padded by WALK_PAD_M for the displacement a puff can
    // carry away from this centre). Invariants: true when the cell centre is within FIELD_DRAW_M of the camera's
    // XY, false beyond; exact at the boundary by <=.
    inline bool cellInField(F32 cellCentreX, F32 cellCentreY, F32 camX, F32 camY)
    {
        const F32 dx = cellCentreX - camX;
        const F32 dy = cellCentreY - camY;
        return (dx * dx + dy * dy) <= (FIELD_DRAW_M * FIELD_DRAW_M);
    }

    // THE builder's pre-gate (ssvolcloud.cpp buildDeck, ahead of any hash/presence/hero work for the cell): cell
    // centre inside FIELD_DRAW_M + WALK_PAD_M, the walk padded by the farthest a puff can be placed from that
    // centre. A cell in the pad is gated and may place puffs at full or partial alpha - it is edgeFade (zero at
    // DECK_EDGE_M < FIELD_DRAW_M, comfortably inside this padded radius) that owns visibility, not this test; this
    // test only decides whether the cell is walked at all. Invariants: true when the cell centre is within
    // FIELD_DRAW_M + WALK_PAD_M of the camera's XY, false beyond; exact at the boundary by <=; strictly more
    // permissive than cellInField for the same inputs (WALK_PAD_M > 0).
    inline bool cellInWalk(F32 cellCentreX, F32 cellCentreY, F32 camX, F32 camY)
    {
        const F32 dx = cellCentreX - camX;
        const F32 dy = cellCentreY - camY;
        const F32 r = FIELD_DRAW_M + WALK_PAD_M;
        return (dx * dx + dy * dy) <= (r * r);
    }

    // How many sub-puffs a cell at camera distance dist places: puffsPerCell (the dial) below SUBS_FULL_M, min(2, dial)
    // between SUBS_FULL_M and SUBS_TWO_M, 1 beyond - but the count itself never pops: the HIGHEST sub index in a band fades
    // via subAlpha over SUBS_BLEND_M straddling the boundary. Invariants: >= 1 always; non-increasing in dist; equals the
    // dial below SUBS_FULL_M - SUBS_BLEND_M.
    inline S32 subsAt(F32 dist, S32 puffsPerCell)
    {
        const S32 dial = llmax(1, puffsPerCell);
        if (dist < SUBS_FULL_M) return dial;
        if (dist < SUBS_TWO_M) return llmin(2, dial);
        return 1;
    }

    // The alpha multiplier for sub index `sub` at distance dist: 1 for sub 0 always; for the sub that is about to be dropped
    // at the next boundary, 1 -> 0 across [boundary - SUBS_BLEND_M/2, boundary + SUBS_BLEND_M/2]; 0 for subs beyond the
    // band's count. Invariants: in [0,1]; sub 0 == 1 for every dist; continuous in dist; monotone non-increasing in dist
    // for a fixed sub; consistent with subsAt (0 exactly where subsAt says the sub is gone, past the blend band).
    inline F32 subAlpha(F32 dist, S32 sub, S32 puffsPerCell)
    {
        if (sub <= 0) return 1.f;
        const S32 dial = llmax(1, puffsPerCell);
        if (dial <= sub) return 0.f; // this sub is never placed at any distance for this dial
        const F32 boundary = (sub == 1) ? SUBS_TWO_M : SUBS_FULL_M; // sub 1 drops at the 2-sub->1-sub edge, sub>=2 drops at the full->2 edge
        const F32 half = SUBS_BLEND_M * 0.5f;
        const F32 t = llclamp((dist - (boundary - half)) / SUBS_BLEND_M, 0.f, 1.f);
        const F32 s = t * t * (3.f - 2.f * t); // smoothstep
        return 1.f - s;
    }

    // The far thinning keep fraction: 1 until THIN_START_M, then smoothstep down to THIN_KEEP_MIN at FIELD_DRAW_M.
    // Invariants: 1 below THIN_START_M; THIN_KEEP_MIN at and beyond FIELD_DRAW_M; monotone non-increasing; continuous.
    inline F32 keepFrac(F32 dist)
    {
        if (dist <= THIN_START_M) return 1.f;
        if (dist >= FIELD_DRAW_M) return THIN_KEEP_MIN;
        const F32 t = (dist - THIN_START_M) / (FIELD_DRAW_M - THIN_START_M);
        const F32 s = t * t * (3.f - 2.f * t); // smoothstep
        return std::lerp(1.f, THIN_KEEP_MIN, s);
    }

    // Deterministic thinning decision for (cell, sub): sub 0 is always kept; otherwise kept iff
    // SSAtmoNoise::hash01(combine(combine(cx, cy), sub ^ salt)) < keepFrac(dist). Invariants: sub 0 always true; pure in
    // (cx, cy, sub, salt, dist); the kept share over a large sample at fixed dist is within 0.03 of keepFrac(dist).
    inline bool keepPuff(S32 cx, S32 cy, S32 sub, U32 salt, F32 dist)
    {
        if (sub <= 0) return true;
        const U32 h = SSAtmoNoise::combine(SSAtmoNoise::combine((U32)cx, (U32)cy), (U32)sub ^ salt);
        return SSAtmoNoise::hash01(h) < keepFrac(dist);
    }

    // Alpha compensation for a surviving sub > 0 puff so the thinned field conserves aggregate coverage: min(ALPHA_COMP_MAX,
    // 1 / keepFrac(dist)). Invariants: 1 below THIN_START_M; never above ALPHA_COMP_MAX; monotone non-decreasing in dist.
    inline F32 thinAlphaComp(F32 dist)
    {
        return llmin(ALPHA_COMP_MAX, 1.f / keepFrac(dist));
    }

    // The deck's edge fade for puffs and the veil, ONE rail pair: 1 until FIELD_FADE_START_M, cubic down to 0 at
    // DECK_EDGE_M (not 9800 hardcoded in the shader, not FIELD_DRAW_M: the dome's melt line is where the deck must end).
    // Uploaded as ss_edge_rails (fadeStart, edge) and mirrored trivially in GLSL. Invariants: 1 below FIELD_FADE_START_M;
    // 0 at and beyond DECK_EDGE_M; monotone; continuous.
    inline F32 edgeFade(F32 horizDist)
    {
        if (horizDist <= FIELD_FADE_START_M) return 1.f;
        if (horizDist >= DECK_EDGE_M) return 0.f;
        const F32 t = (horizDist - FIELD_FADE_START_M) / (DECK_EDGE_M - FIELD_FADE_START_M);
        return 1.f - t * t * (3.f - 2.f * t); // cubic (smoothstep) falloff
    }

    // The shadow bake's optical-depth scale against far thinning: the bake integrates the column as if every sub existed,
    // so at distance dist from the bake's camera cell its tau is scaled by the share of bodies actually drawn:
    // (1 + (subsAt(dist, dial) - 1) * keepFrac(dist)) / dial. Invariants: 1 near; in (0, 1]; monotone non-increasing.
    inline F32 shadowTauScale(F32 dist, S32 puffsPerCell)
    {
        const S32 dial = llmax(1, puffsPerCell);
        return (1.f + (F32)(subsAt(dist, dial) - 1) * keepFrac(dist)) / (F32)dial;
    }

    // Deck draw order with hysteresis: the under deck draws on top only when its mean distance-squared is smaller than the
    // primary's by more than ORDER_HYST_M2 (and flips back only when larger by more than it); inside the band the previous
    // answer holds. Invariants: with prev false and underMean < primaryMean - HYST -> true; with prev true and underMean >
    // primaryMean + HYST -> false; inside the band returns prev; never flips twice for a monotone sweep across the band.
    inline bool underOnTop(bool prev, F32 underMeanDistSq, F32 primaryMeanDistSq)
    {
        if (!prev && underMeanDistSq < primaryMeanDistSq - ORDER_HYST_M2) return true;
        if (prev && underMeanDistSq > primaryMeanDistSq + ORDER_HYST_M2) return false;
        return prev;
    }
}

#endif
