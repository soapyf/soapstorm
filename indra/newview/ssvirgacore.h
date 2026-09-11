/**
 * @file ssvirgacore.h
 * @brief Atmo Magic: distant rain shafts (virga curtains) - qualification, card-stack geometry, alpha profile, particle-rain handoff, budget trim. Header-only core. CONTRACT.
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

#ifndef SS_VIRGACORE_H
#define SS_VIRGACORE_H

// <SS:Nexii> A CORE header (lldefs.h, ssatmonoisecore.h, <cmath>, <cstdint> only; keepHash below calls SSAtmoNoise::hash01/combine directly). The emitter's edge fade and placement come from ssdecklodcore.h / ssdeckframecore.h, which the SHELL includes - this core references neither. Design: doc/atmo_magic_far_clouds.md section 3 (phase 6c). Rain falling from distant cells is drawn as fog-like curtains: per qualifying cell a vertical stack of Z-axis-billboarded cards no taller than CELL_M from the deck base down to the ground reference, emitted INTO the deck's puff vector (flagged on the spare b vertex channel) so the existing farthest-first painter's sort interleaves them, shaded by the puff shade/form formulas with buried pinned near 1, REAL alpha in the sky forward pass only. Driver: precipitation intensity x cell presence (the storm coupling's per-cell tower feeds intensity when active). Handoff: shafts skip inside r2 x HANDOFF_SKIP of the particle rain's TIER_SHEETS radius and ramp in over HANDOFF_BAND_M so one shared radius owns the boundary. Budget: a STABLE HASH TRIM (keepHash) - each qualifying cell keeps iff hash01(cell) < p, p = MAX_SHAFTS / quantised(n) - so when the candidate count n moves (camera walk crossing, precipitation easing) only the cells whose hash sits between the old and new p change, never the whole kept set (the phase-6c review found an index-based trim re-picked all 64 curtains on every count change). The walk that bounds n is camera-centred (LOD), but each cell's qualify AND keep decision is a function of (cell, weather, the quantised n) only - with ONE stated exception: the hard-ceiling backstop (hardCap), a rank cut on a fixed per-cell hash key that fires only when the hash trim's random excursion keeps more than HARD_CAP_FRAC x MAX_SHAFTS (about a 4-sigma event at n >> cap, since E[kept] <= cap); when it fires, the dropped cells are those with the LARGEST keys, so growing or shrinking the kept set by one moves at most one cell across the cut - an order statistic, not a reshuffle. Card count is NOT distance LOD - it is the span / CARD_MAX_M (stated plainly; only alpha/handoff/edge and the far ground lift depend on distance). Cards in a stack OVERLAP by OVERLAP_FRAC of a card so the shader's two-sided soft ends crossfade instead of punching a transparent band at each seam. Beyond the squash knee the curtain's bottom LIFTS off the ground (groundLiftZ) - physically virga, precipitation evaporating before it lands - which also keeps ground-level geometry out of the far-squash depth fold that would otherwise draw it over terrain standing in front of it; stated residual: the fold still affects a curtain's mid-air cards against tall terrain, as it does far puffs. Phase 8e retunes the knee to bite only well beyond it (LIFT_START_FRAC 1.5, LIFT_FULL_FRAC 3.0, LIFT_MAX_FRAC 0.30) so near and mid curtains keep reaching the water/ground reference and only the far ones lift. The shaft is base-anchored in the O(z) sense (reads gate_air, like the sheet) and is placed by SSDeckFrame::placeWorld with the hero shift, never a respelled sum. Phase 8e (doc/atmo_magic_phase8_show.md section 3b) turns the shaft from a flat pillar into a wind-skewed sheet embedded into the deck's underside: skewOffsetM(windBase, dzBelowBase, fallSpeed) is the same wind x fall-time rule the particle rain uses for its landing shift, slope-capped at SKEW_RATE_MAX (F4/F5, 2026-09-06 review: never steeper than 45 degrees off vertical) and then magnitude-capped at SKEW_CAP_M (an ADDITIONAL displacement the shell's walk pad must include, alongside the existing hero/shear/jitter terms), evaluated at each card's actual zTop/zBot/zMid so every card derives its skew from the SAME pure function of height. With SKEW_RATE_MAX 1, a card's own shear (its top skew minus its bottom skew) is at most its own height, so the parallelogram's slanted edge is at most sqrt(2) times the card's height (F4/F5) - the chain across overlapping cards is continuity of that one function, not a copied value, EXCEPT for the one card whose span straddles the magnitude cap's own knee: within that single card's height skewOffsetM bends from the linear slope onto the clamp, so its top and bottom edges are a chord of a piecewise path rather than two points on one straight line, and its overlap partner (evaluated at ITS OWN zTop/zBot) generally disagrees with it across their shared band - a STATED RESIDUAL at that one boundary, not a second claim of a perfectly continuous slanted sheet. The stack's TOP is no longer the deck base but embedTopZ(baseZ, thickness) - a fixed fraction (DECK_EMBED_FRAC, capped DECK_EMBED_MAX_M) of the puff thickness above the base, so the curtain starts inside the cloud; embedAlpha (F2, 2026-09-06 review: now the FRAGMENT's own per-pixel read, via ssVolCloudF.glsl's ss_virga_embedAlpha twin - CardGeom itself carries no baked embed-fade field any more) ramps 1 -> 0 across that embedded span so it emerges rather than hanging off a hard line. cardCountOverlapped/cardSpan/cardSpanLifted take this embedTopZ as their "top" argument (the stack spans [groundZ, embedTopZ]), but the h01 fed to alphaFor/halfWidthM is measured against the DECK BASE (not the embedded top), clamped to [0,1], so cards sitting in the embedded portion still read the base's alpha/width rather than extrapolating past 1. CardGeom packages one card's placed, skewed slab for the emitter - no embed-fade field (F2): that fade is read by the fragment shader itself, per pixel.
#include "lldefs.h"
#include "ssatmonoisecore.h"
#include "sswindprofilecore.h"

#include <cmath>
#include <cstdint>

namespace SSVirga
{
    constexpr F32 CELL_M          = 260.f;   // LOCKSTEP ssvolcloud.cpp CELL_M
    constexpr F32 THRESHOLD       = 0.18f;   // precip * presence must clear this (scaled inversely by the Weather Influence "Distant rain" strength)
    constexpr F32 CARD_MAX_M      = 260.f;   // max card height (<= CELL_M so the per-vertex squash stays accurate, see the veil's own comment)
    // <SS:Nexii> WIDTH FOLLOWS INTENSITY (user, 2026-09-06): wispy filaments under light rain, walls hundreds of
    // metres to kilometres wide under dark heavy rain, lines under squalls. WIDTH_FRAC (a single fixed share) is
    // DELETED - widthFracAt(drive) below replaces it everywhere. WIDTH_WISPY's 0.22 is 57 m half-width at CELL_M;
    // WIDTH_HEAVY's 0.75 is 195 m, so two heavy neighbours (260 m apart) overlap by 1.5x their own half-width and
    // read as one wall, while a wispy curtain (57 m half-width, well under the 130 m half-cell) stands alone.
    constexpr F32 WIDTH_WISPY     = 0.22f;
    constexpr F32 WIDTH_HEAVY     = 0.75f;
    constexpr F32 TAPER_GROUND    = 0.6f;    // half-width at the ground as a share of the base half-width (virga narrows down)
    // <SS:Nexii> ONE PHENOMENON, EVAPORATION FROM BELOW (user, 2026-09-06): rain shafts and virga shade the same;
    // the OLD fixed vertical alpha profile (ALPHA_TOP at the base, ALPHA_GROUND at the ground) is DELETED -
    // alphaFor(drive) below replaces it (alpha is intensity x density across the whole volume, not a function of
    // height). ALPHA_MAX is the old ALPHA_TOP value, now the ceiling at drive 1 rather than a per-height target.
    constexpr F32 ALPHA_MAX       = 0.35f;
    constexpr F32 HANDOFF_SKIP    = 1.1f;    // no shaft cards inside r2 * HANDOFF_SKIP of the camera
    constexpr F32 HANDOFF_BAND_M  = 400.f;   // ramp from 0 at r2 * HANDOFF_SKIP to full over this band
    constexpr S32 MAX_SHAFTS      = 64;
    constexpr F32 HARD_CAP_FRAC   = 1.3f;    // hardCap: the hash trim's kept count may not exceed this share of MAX_SHAFTS (rank-cut backstop, see header)
    constexpr F32 BURIED          = 1.f;     // <SS:Nexii> D2: NO LONGER LOCKSTEP with ssvolcloud.h Deck::SHEET_BURIED, and deliberately - a curtain hangs BELOW the deck with the whole column over it (1.0 is honest), while the veil sits INSIDE the deck's own floor band and now grades the gloom at SSDeckShade::VEIL_DEPTH, the same representative depth its form term uses. The old shared value read the veil as more buried than any puff, which is what made it darker than the puffs beside it; see Deck::SHEET_BURIED's own note.
    constexpr F32 OVERLAP_FRAC    = 0.22f;   // LOCKSTEP ssVolCloudF.glsl SS_SHAFT_V_SOFT: consecutive cards overlap by this share of a card so the soft ends crossfade
    constexpr F32 LIFT_START_FRAC = 1.5f;    // ground lift begins at this multiple of the squash knee (8e retune: was 1.0 - bite further out so near/mid curtains keep the ground/water reference)
    constexpr F32 LIFT_FULL_FRAC  = 3.0f;    // ... and reaches LIFT_MAX_FRAC of the span at this multiple (8e retune: was 2.0)
    constexpr F32 LIFT_MAX_FRAC   = 0.30f;   // the far curtain ends this share of the way up from the ground (virga) (8e retune: was 0.55)
    constexpr F32 N_QUANT_STEP    = 1.25f;   // keepHash quantises n to the next power of this so p moves in coarse steps
    constexpr F32 SKEW_CAP_M      = 900.f;   // skewOffsetM's magnitude cap (F3, 2026-09-06 review: was 1500) - an ADDITIONAL bounded displacement on top of the frame's hero/shear/jitter terms; the shell's walk pad must include it
    constexpr F32 SKEW_RATE_MAX   = 1.0f;    // F4/F5 (2026-09-06 review): skewOffsetM's slope cap, horizontal metres per metre of dz - 1.0 is 45 degrees off vertical, so a card's shear across its own height is at most its own height
    constexpr F32 DECK_EMBED_FRAC = 0.30f;   // embedTopZ: the stack's top sits this fraction of the puff thickness above the deck base
    constexpr F32 DECK_EMBED_MAX_M = 150.f;  // ... capped at this many metres
    constexpr F32 SKEW_STEP_M     = CARD_MAX_M * 0.25f; // profileSkewM's integration step (a quarter card, 65 m)
    // <SS:Nexii> Evaporation mask (see alphaFor/evapHeight01 below): light rain's curtain ends two thirds of the
    // way up from the ground (EVAP_H_WISPY), heavy rain reaches all the way to it (EVAP_H_HEAVY 0); EVAP_SOFT is
    // cardEmittedFor's margin below the mask height a card's TOP may still poke into before it is skipped.
    constexpr F32 EVAP_H_WISPY    = 0.65f;
    constexpr F32 EVAP_H_HEAVY    = 0.f;
    constexpr F32 EVAP_SOFT       = 0.25f;

    struct Vec2
    {
        F32 x = 0.f;
        F32 y = 0.f;
    };

    // The wind-skew horizontal offset at a height dzBelowBase metres below the deck base. F4/F5 (2026-09-06 review):
    // the rate of lean is min(|windBase| / max(fallSpeed, 0.1), SKEW_RATE_MAX) - a SLOPE cap (horizontal metres per
    // metre of dz), not just a magnitude cap, so a card's shear across its own height is never steeper than 45
    // degrees (SKEW_RATE_MAX == 1.0) regardless of how strong the wind or how slow the fall - before this fix a
    // strong wind over a slow fall could lean a single CELL_M-tall card sideways by many multiples of its own
    // height. F10 (2026-09-06 review): fallSpeed is floored at 0.1, matching the particle rain's own floor
    // (ssprecipitation.cpp) and SSAtmoInfoViewCore::fallTiltOffsetM's comparison line - not the 0.5 this used
    // before, which the other two never shared. The rate-limited magnitude (rate * dzBelowBase, signed) is then
    // applied along windBase's own direction and, only after that, clamped a second time to SKEW_CAP_M with
    // direction preserved - the magnitude cap is a residual backstop once the slope cap already does the real
    // work. This is the same wind x fall-time rule the particle rain uses for its landing shift, with one stated
    // divergence (F10): the particle path's wind is the flowmap, read per client; the curtain's windBase is the
    // CURVE-RESOLVED wind at the deck base (SSAtmoEnvApplier::windProfileAt / SSWindProfile::windAt), never the
    // eased per-client wind and never the flowmap - geometry and the particle sim are deliberately allowed to
    // disagree about which wind they read. dzBelowBase may be negative (a height ABOVE the base, e.g. inside the
    // embedded portion) - the formula stays linear in dz there too, it is simply evaluated on the other side of
    // zero. Invariants: zero at dz == 0; linear in dz while the raw (unclamped) magnitude is <= SKEW_CAP_M; the
    // pre-magnitude-cap slope |result|/|dz| never exceeds SKEW_RATE_MAX; |result| <= SKEW_CAP_M always; pure (a
    // function of its three arguments only, called identically regardless of which card or caller asks for a given
    // z, which is what keeps consecutive cards' shear chains continuous - see cardGeom).
    inline Vec2 skewOffsetM(Vec2 windBase, F32 dzBelowBase, F32 fallSpeed)
    {
        Vec2 v;
        const F32 wind_mag = std::sqrt(windBase.x * windBase.x + windBase.y * windBase.y);
        if (wind_mag > 1e-6f)
        {
            const F32 rate = llmin(wind_mag / llmax(fallSpeed, 0.1f), SKEW_RATE_MAX);
            const F32 scale = rate * dzBelowBase / wind_mag; // signed; direction preserved via windBase itself
            v.x = windBase.x * scale;
            v.y = windBase.y * scale;
        }
        else
        {
            v.x = 0.f;
            v.y = 0.f;
        }
        const F32 mag = std::sqrt(v.x * v.x + v.y * v.y);
        if (mag > SKEW_CAP_M && mag > 1e-6f)
        {
            const F32 cap_scale = SKEW_CAP_M / mag;
            v.x *= cap_scale;
            v.y *= cap_scale;
        }
        return v;
    }

    // PROFILE SKEW (8e-b, 2026-09-06 review: "segment the virga in parts and follow the wind profile"). Where
    // skewOffsetM above leans a card by the SINGLE deck-base wind, this is the real drift of a drop released at
    // the deck base (AGL baseAglM) by the time it has fallen to zAglM: the INTEGRAL of the wind profile over the
    // fall, not one base sample - faster aloft, slower and veered near the ground. Implementation: N =
    // ceil((baseAglM - zAglM) / SKEW_STEP_M) equal steps spanning [zAglM, baseAglM], each contributing
    // SSWindProfile::windAt(step midpoint AGL, p) * stepM / max(fallSpeed, 0.1f), summed. The SAME two 8e-b caps
    // as skewOffsetM then bound the total: the slope (|sum| / dz) is capped at SKEW_RATE_MAX (scaling the whole
    // vector down, direction preserved) and the magnitude is then capped at SKEW_CAP_M. zAglM may exceed baseAglM
    // (a height above the base, e.g. inside the embedded portion) - dz is then <= 0 and the result is the zero
    // vector, matching skewOffsetM's own dz == 0 case rather than integrating backwards.
    //
    // Invariants: zero at zAglM >= baseAglM; equals skewOffsetM(windAt(baseAglM, p), baseAglM - zAglM, fallSpeed)
    // within 1e-3 when the profile is UNIFORM (speedScale 1 and no veer at every altitude - windAt(z, p) ==
    // windAt(baseAglM, p) for every z in [zAglM, baseAglM]), since the sum then telescopes to windAt(baseAglM, p)
    // * dz / fallSpeed exactly, before either cap, which is skewOffsetM's own uncapped formula; both caps then
    // apply identically to the two, so they agree. |result| is monotone non-decreasing in dz = baseAglM - zAglM
    // for a profile with no veer (a constant wind direction only ever adds more of the same-signed component as
    // dz grows, and both caps are order-preserving up to saturation). Both caps hold: pre-slope-cap slope
    // |sum|/dz <= SKEW_RATE_MAX is enforced by construction when dz > 0, and |result| <= SKEW_CAP_M always.
    // Deterministic: N is a function of dz (== baseAglM - zAglM) ONLY, never of any camera or frame quantity, so
    // two clients evaluating the same (p, baseAglM, zAglM, fallSpeed) integrate the identical steps. Pure.
    inline Vec2 profileSkewM(const SSWindProfile::Params& p, F32 baseAglM, F32 zAglM, F32 fallSpeed)
    {
        Vec2 sum;
        const F32 dz = baseAglM - zAglM;
        if (dz <= 0.f)
        {
            return sum;
        }
        const S32 n = llmax(1, (S32)std::ceil(dz / SKEW_STEP_M));
        const F32 stepM = dz / (F32)n;
        const F32 fall = llmax(fallSpeed, 0.1f);
        for (S32 i = 0; i < n; ++i)
        {
            const F32 midAgl = baseAglM - ((F32)i + 0.5f) * stepM;
            const SSWindProfile::Vec2 w = SSWindProfile::windAt(midAgl, p);
            sum.x += w.x * stepM / fall;
            sum.y += w.y * stepM / fall;
        }
        const F32 rate_limit = SKEW_RATE_MAX * dz;
        const F32 mag = std::sqrt(sum.x * sum.x + sum.y * sum.y);
        if (mag > rate_limit && mag > 1e-6f)
        {
            const F32 k = rate_limit / mag;
            sum.x *= k;
            sum.y *= k;
        }
        const F32 mag2 = std::sqrt(sum.x * sum.x + sum.y * sum.y);
        if (mag2 > SKEW_CAP_M && mag2 > 1e-6f)
        {
            const F32 k = SKEW_CAP_M / mag2;
            sum.x *= k;
            sum.y *= k;
        }
        return sum;
    }

    // Drive strength of a cell: precipIntensity * presence * (0.85 + 0.30 * tower), clamped to [0,1] - the same shape
    // precipitation's own spawn-rate gate uses, so a curtain hangs exactly where rain would fall. Invariants: 0 when either
    // precipIntensity or presence is 0; monotone in all three; <= 1.
    inline F32 drive(F32 precipIntensity, F32 presence, F32 tower)
    {
        return llclampf(precipIntensity * presence * (0.85f + 0.30f * tower));
    }

    // Whether a cell qualifies: drive >= THRESHOLD / max(strength, 0.05) where strength is the influence row's 0..1 dial
    // (higher strength -> lower effective threshold). Invariants: strength 0 never qualifies; drive 1 always qualifies for
    // strength >= 0.18; monotone in drive and in strength; pure (no distance term - qualification is world state).
    inline bool qualifies(F32 drive, F32 strength)
    {
        return drive >= (THRESHOLD / llmax(strength, 0.05f));
    }

    // The embedded stack top: baseZ + min(DECK_EMBED_FRAC * thicknessM, DECK_EMBED_MAX_M) - the curtain starts this
    // far above the deck base, inside the puffs, rather than hanging off the base as a hard line. Negative
    // thicknessM is treated as 0 (no embed) rather than pulling the top below the base. Invariants: >= baseZ always;
    // capped (never more than DECK_EMBED_MAX_M above baseZ).
    inline F32 embedTopZ(F32 baseZ, F32 thicknessM)
    {
        return baseZ + llmin(DECK_EMBED_FRAC * llmax(thicknessM, 0.f), DECK_EMBED_MAX_M);
    }

    // The fragment's own fade, twinned (F2, 2026-09-06 review): 1 at or below baseZ (fully outside the cloud, full
    // curtain alpha), falling linearly to 0 at topZ (the top of the embedded stack, see embedTopZ) so the curtain
    // emerges from inside the puffs instead of appearing at a hard line. This core no longer has a CPU caller - the
    // CPU used to bake one alphaMul per card from this, at the card's mid-height, which never let the fade ramp
    // WITHIN a card; the fade now runs once per FRAGMENT, in ssVolCloudF.glsl's ss_virga_embedAlpha (a LITERAL
    // twin, on ss_shaft_embed and the fragment's own world_true.z), so this function's job here is to BE that
    // twin's contract - the invariants below are what the GLSL copy is pinned against, not a formula this file
    // still evaluates itself. Degenerate topZ <= baseZ (zero embed) steps from 1 to 0 exactly at baseZ rather than
    // dividing by zero. Invariants: result in [0,1]; monotone non-increasing in z; continuous (the degenerate case
    // is a single point, matching the general formula's own limit there).
    inline F32 embedAlpha(F32 z, F32 baseZ, F32 topZ)
    {
        if (z <= baseZ) return 1.f;
        const F32 span = topZ - baseZ;
        if (span <= 1e-6f) return 0.f;
        return llclamp(1.f - (z - baseZ) / span, 0.f, 1.f);
    }

    // Card count for a shaft spanning [groundZ, baseZ]: ceil(span / CARD_MAX_M), at least 1; 0 when baseZ <= groundZ.
    inline S32 cardCount(F32 baseZ, F32 groundZ)
    {
        const F32 span = baseZ - groundZ;
        if (span <= 0.f) return 0;
        return llmax(1, (S32)std::ceil(span / CARD_MAX_M));
    }

    // WIDTH FOLLOWS INTENSITY: the base half-width's share of CELL_M, ramping from WIDTH_WISPY (light rain) to
    // WIDTH_HEAVY (heavy rain) over drive's [0.15, 0.85] window. Invariants: monotone non-decreasing in drive;
    // == WIDTH_WISPY at drive <= 0.15; == WIDTH_HEAVY at drive >= 0.85.
    inline F32 widthFracAt(F32 drive)
    {
        const F32 t = llclamp((drive - 0.15f) / (0.85f - 0.15f), 0.f, 1.f);
        const F32 s = t * t * (3.f - 2.f * t); // smoothstep
        return WIDTH_WISPY + (WIDTH_HEAVY - WIDTH_WISPY) * s;
    }

    // Half-width at height fraction h01 (0 ground, 1 deck base) for a cell of the given drive:
    // CELL_M * widthFracAt(drive) * lerp(TAPER_GROUND, 1, h01). Invariants: monotone non-decreasing in h01 and in
    // drive; at h01 1 equals CELL_M * widthFracAt(drive).
    inline F32 halfWidthM(F32 h01, F32 drive)
    {
        return CELL_M * widthFracAt(drive) * (TAPER_GROUND + (1.f - TAPER_GROUND) * llclampf(h01));
    }

    // ONE PHENOMENON, EVAPORATION FROM BELOW: alpha is intensity x density across the whole volume, not a function
    // of height any more - alphaFor(drive) = ALPHA_MAX * drive. The old height-dependent profile is gone; the
    // evaporated-from-below look now comes entirely from cardEmittedFor skipping cards below evapHeight01, not
    // from a vertical alpha ramp. Invariants: monotone non-decreasing in drive; 0 at drive 0; == ALPHA_MAX at
    // drive 1.
    inline F32 alphaFor(F32 drive)
    {
        return ALPHA_MAX * llclampf(drive);
    }

    // The curtain height fraction (0 ground, 1 deck base) below which precipitation has evaporated before
    // landing: lerp(EVAP_H_WISPY, EVAP_H_HEAVY, smoothstep(0.15, 0.85, drive)) - light rain (low drive) ends two
    // thirds of the way up, heavy rain (high drive) reaches the ground. This is GEOMETRY's evaporation mask
    // (physics: how far down the precipitation survives), separate from the far ground lift (groundLiftZ), which
    // is a squash-fold workaround and must not be confused with this in comments. Invariants: monotone
    // non-increasing in drive; == EVAP_H_WISPY at drive <= 0.15; == EVAP_H_HEAVY at drive >= 0.85.
    inline F32 evapHeight01(F32 drive)
    {
        const F32 t = llclamp((drive - 0.15f) / (0.85f - 0.15f), 0.f, 1.f);
        const F32 s = t * t * (3.f - 2.f * t); // smoothstep
        return EVAP_H_WISPY + (EVAP_H_HEAVY - EVAP_H_WISPY) * s;
    }

    // Whether a card whose TOP sits at height fraction cardTopH01 should be emitted at all, given drive: false
    // (skip - geometry saved, the fragment's own ragged mask does the eroded edge) only when the card's top is at
    // or below evapHeight01(drive) - EVAP_SOFT, i.e. the WHOLE card sits inside the evaporated zone plus a soft
    // margin. Invariants: true for every card when drive >= 0.85 (evapHeight01 is 0 there, so cardTopH01 > -0.25
    // always holds for a card whose top is at or above the ground, h01 >= 0).
    inline bool cardEmittedFor(F32 cardTopH01, F32 drive)
    {
        return cardTopH01 > evapHeight01(drive) - EVAP_SOFT;
    }

    // The handoff weight against the particle rain's sheets radius r2 (metres): 0 for dist <= r2 * HANDOFF_SKIP, rising
    // with smoothstep to 1 at r2 * HANDOFF_SKIP + HANDOFF_BAND_M. Camera-distance LOD only (never changes which cells
    // qualify). Invariants: 0 inside; 1 beyond the band; monotone; continuous.
    inline F32 handoff(F32 dist, F32 r2)
    {
        const F32 skipR = r2 * HANDOFF_SKIP;
        if (dist <= skipR) return 0.f;
        const F32 bandEnd = skipR + HANDOFF_BAND_M;
        if (dist >= bandEnd) return 1.f;
        const F32 t = (dist - skipR) / HANDOFF_BAND_M;
        return t * t * (3.f - 2.f * t); // smoothstep
    }

    // Quantise the candidate count upward to the next power of N_QUANT_STEP (>= 1). Invariants: >= n; >= 1; monotone
    // non-decreasing in n; a change of n within one quantum returns the same value.
    inline F32 quantiseCount(S32 n)
    {
        const F32 nf = (F32)llmax(0, n);
        if (nf <= 1.f) return 1.f;
        F32 q = 1.f;
        while (q < nf) q *= N_QUANT_STEP;
        return q;
    }

    // STABLE budget trim: keep iff SSAtmoNoise::hash01(combine(cellId, salt)) < min(1, cap / quantiseCount(n)). Invariants:
    // pure in (cellId, salt, n, cap); keeps everything when n <= cap; over 20000 hashed cells the kept share is within 0.03 of
    // min(1, cap / quantiseCount(n)); for two counts n1 < n2 inside one quantum the kept sets are IDENTICAL; across a
    // quantum boundary only cells whose hash lies between the two thresholds change (measured share within 0.03 of the
    // threshold difference).
    //
    // <SS:Nexii> The 64-bit cellId is folded into the 32-bit hash the same way the shell folds any 64-bit lattice id
    // elsewhere: low word and (high word XOR salt) through SSAtmoNoise::combine, so salt perturbs the fold without a
    // second combine call. STATED RESIDUAL: the "keeps everything when n <= cap" invariant is exact only when
    // quantiseCount(n) itself does not overshoot cap - N_QUANT_STEP's 25% ceiling means a narrow band of n just below
    // cap (n in (cap/N_QUANT_STEP, cap]) quantises past cap and trims slightly (p as low as ~0.8, never lower); the
    // min(1, ...) clamp still makes it exact everywhere else. No test asserts the literal "everything" claim for that
    // band; the "kept share within 0.03 of p" test covers the true behaviour there instead.
    inline bool keepHash(U64 cellId, U32 salt, S32 n, S32 cap)
    {
        const U32 lo = (U32)cellId;
        const U32 hi = (U32)(cellId >> 32) ^ salt;
        const F32 h = SSAtmoNoise::hash01(SSAtmoNoise::combine(lo, hi));
        const F32 p = llmin(1.f, (F32)cap / quantiseCount(n));
        return h < p;
    }

    // The hard ceiling the shell's rank-cut backstop enforces on the hash-kept set: floor(cap * HARD_CAP_FRAC).
    // Invariants: >= cap for cap >= 0; monotone in cap; hardCap(MAX_SHAFTS) == 83.
    inline S32 hardCap(S32 cap)
    {
        return (S32)((F32)llmax(0, cap) * HARD_CAP_FRAC);
    }

    // The k-th card's slab (k = 0 at the stack's top) for a stack spanning [groundZ, baseZ]: cards are CARD_MAX_M tall and
    // step down by CARD_MAX_M * (1 - OVERLAP_FRAC), the last card clamped to end at groundZ; h01Mid is the card centre's
    // height fraction against THIS function's own [groundZ, baseZ] span (0 ground, 1 at the "baseZ" argument) - used for
    // this struct's own layout math (cardSpanLifted's truncation), never fed straight to alphaFor/halfWidthM. Since phase
    // 8e (embedded top), the caller passes embedTopZ(realBaseZ, thickness) as this "baseZ" argument, so the stack spans
    // [groundZ, embedTopZ]; the profile h01 that DOES go to alphaFor/halfWidthM is measured against the real deck base
    // instead, clamped to [0,1] (see CardGeom.h01Mid) - a card embedded above the real base still reads the top-of-
    // profile alpha/width rather than extrapolating past 1. Invariants: card 0's top == baseZ (the argument, i.e. the
    // stack's actual top); consecutive cards overlap by >= OVERLAP_FRAC * CARD_MAX_M (except the last, which may be
    // shorter); the last card's bottom == groundZ; zTop > zBot for every card; cardCountOverlapped(baseZ, groundZ) cards
    // cover the span with no gap.
    struct CardSpan
    {
        F32 zTop = 0.f;
        F32 zBot = 0.f;
        F32 h01Mid = 0.f;
    };
    inline S32 cardCountOverlapped(F32 baseZ, F32 groundZ)
    {
        const F32 span = baseZ - groundZ;
        if (span <= 0.f) return 0;
        if (span <= CARD_MAX_M) return 1;
        const F32 stride = CARD_MAX_M * (1.f - OVERLAP_FRAC);
        const F32 remain = span - CARD_MAX_M;
        return 1 + (S32)std::ceil(remain / stride);
    }
    inline CardSpan cardSpan(S32 k, F32 baseZ, F32 groundZ)
    {
        const F32 stride = CARD_MAX_M * (1.f - OVERLAP_FRAC);
        const S32 n = cardCountOverlapped(baseZ, groundZ);
        const F32 top = baseZ - (F32)k * stride;
        F32 bot = top - CARD_MAX_M;
        if (k >= n - 1 || bot <= groundZ)
        {
            bot = groundZ; // last card (or one that would undershoot) clamps to the ground reference
        }
        CardSpan cs;
        cs.zTop = top;
        cs.zBot = bot;
        const F32 span = llmax(1e-6f, baseZ - groundZ);
        cs.h01Mid = llclamp(((top + bot) * 0.5f - groundZ) / span, 0.f, 1.f);
        return cs;
    }

    // The far ground lift: the curtain's effective ground altitude at camera distance dist, given the squash knee: groundZ +
    // (baseZ - groundZ) * LIFT_MAX_FRAC * smoothstep(knee * LIFT_START_FRAC, knee * LIFT_FULL_FRAC, dist). Cards whose whole
    // slab lies below the lifted ground are not emitted; the lowest emitted card's bottom is the lifted ground. Invariants:
    // == groundZ at or inside the knee; monotone in dist; never above groundZ + LIFT_MAX_FRAC * span; continuous.
    inline F32 groundLiftZ(F32 groundZ, F32 baseZ, F32 dist, F32 kneeM)
    {
        const F32 span = baseZ - groundZ;
        const F32 t0 = kneeM * LIFT_START_FRAC;
        const F32 t1 = kneeM * LIFT_FULL_FRAC;
        F32 s;
        if (t1 <= t0)
        {
            s = (dist >= t0) ? 1.f : 0.f; // degenerate knee (<= 0): step instead of divide by zero
        }
        else
        {
            const F32 t = llclamp((dist - t0) / (t1 - t0), 0.f, 1.f);
            s = t * t * (3.f - 2.f * t); // smoothstep
        }
        return groundZ + span * LIFT_MAX_FRAC * s;
    }

    // The k-th card's slab after the far ground lift: false (not emitted) when the whole slab lies at or below liftZ;
    // otherwise out = cardSpan(k, ...) with zBot raised to liftZ. h01Mid is left at the card's TRUE (unlifted) height
    // fraction - the lift truncates what is drawn, it does not change the physical curtain's vertical profile.
    // Invariants: returns false iff cardSpan(k).zTop <= liftZ; when true, zBot >= liftZ, zTop > zBot, zTop and h01Mid
    // equal cardSpan(k)'s; with liftZ <= groundZ it is cardSpan(k) unchanged.
    inline bool cardSpanLifted(S32 k, F32 baseZ, F32 groundZ, F32 liftZ, CardSpan& out)
    {
        const CardSpan cs = cardSpan(k, baseZ, groundZ);
        if (cs.zTop <= liftZ) return false;
        out = cs;
        out.zBot = llmax(cs.zBot, liftZ);
        return out.zTop > out.zBot;
    }

    // One card's placed, wind-skewed slab (phase 8e, 8e-b PROFILE SKEW). centreXY is the horizontal skew at the
    // card's mid-height; shearXY is the top-relative-to-bottom skew across the card's own height (skew(zTop) -
    // skew(zBot)), turning the card into a parallelogram; both are profileSkewM(params, baseAglM, zAgl, fallSpeed)
    // evaluated at that card's own z values (converted to AGL via baseAglM - (baseZ - z)) - the SAME pure function
    // every other card also calls, which is what makes consecutive cards' shear chain continuous (no per-card
    // copy, no interpolation) and what makes each card a CHORD of the curved, wind-profile-integrated trajectory
    // rather than a point on one straight line (skewOffsetM's old uniform-wind shape). zTop/zBot are the
    // far-ground-lift-truncated slab (cardSpanLifted) for a stack spanning [groundZ, topZ] where topZ is the
    // caller's embedTopZ(realBaseZ, thickness). h01Mid is the DECK BASE profile fraction (clamped to [0,1]) for
    // alphaFor/halfWidthM, NOT CardSpan's own h01Mid (which is measured against topZ for this function's internal
    // layout). F2 (2026-09-06 review): this struct carries NO embed-fade field any more (the old alphaMul, one
    // value baked per whole card at its mid-height, never let the fade ramp WITHIN a card - deleted rather than
    // kept as an always-something field nobody reads). The embed fade is now the fragment's own, per pixel, off
    // world_true.z and ss_shaft_embed - see embedAlpha's own comment for the twin.
    struct CardGeom
    {
        Vec2 centreXY;
        Vec2 shearXY;
        F32 zTop = 0.f;
        F32 zBot = 0.f;
        F32 h01Mid = 0.f;
    };

    // The DECK BASE profile fraction (0 ground, 1 base) for height z, clamped to [0,1] - the ONE normalization
    // cardGeom's own h01Mid uses, exposed so a caller needing the SAME fraction for a different height on the same
    // card (e.g. the emitter's evaporation gate, cardEmittedFor, which needs the card's TOP fraction rather than
    // its mid) never respells the clamp/divide by hand. Invariants: 0 at z <= groundZ; 1 at z >= baseZ; monotone
    // non-decreasing in z; pure.
    inline F32 baseProfileH01(F32 z, F32 baseZ, F32 groundZ)
    {
        const F32 span = llmax(1e-6f, baseZ - groundZ);
        return llclamp((z - groundZ) / span, 0.f, 1.f);
    }

    // Build card k's placed geometry for a shaft whose real deck base is baseZ (AGL baseAglM, so windAt reads the
    // correct height above ground), embedded stack top topZ (== embedTopZ(baseZ, thickness)), ground reference
    // groundZ, far-ground-lift altitude liftZ (groundLiftZ), the curve-resolved wind PROFILE params and
    // precipitation fall speed fallSpeed. emitted is set false iff cardSpanLifted(k, topZ, groundZ, liftZ, ...) is
    // false, in which case the returned CardGeom is default-constructed and must not be drawn.
    // Invariants: emitted false iff cardSpanLifted(k, topZ, groundZ, liftZ) is false; when emitted, zTop/zBot equal
    // that call's zTop/zBot exactly; shearXY == profileSkewM(params, baseAglM, baseAglM-(baseZ-zTop), fallSpeed)
    // minus the same at zBot (top relative to bottom); centreXY == profileSkewM(params, baseAglM,
    // baseAglM-(baseZ-zMid), fallSpeed) at the card's true (unlifted-truncation-independent) mid-height zMid ==
    // (zTop + zBot) * 0.5; h01Mid == clamp((zMid - groundZ) / (baseZ - groundZ), 0, 1) - the DECK BASE profile
    // fraction, not cardSpan's own topZ-relative one; consecutive cards' shear chains continuously because every
    // card derives its skew from the one profileSkewM function evaluated at that card's own z, never a value
    // carried over from a neighbour.
    inline CardGeom cardGeom(S32 k, F32 baseZ, F32 topZ, F32 groundZ, F32 liftZ, const SSWindProfile::Params& params,
                              F32 baseAglM, F32 fallSpeed, bool& emitted)
    {
        CardGeom out;
        CardSpan cs;
        emitted = cardSpanLifted(k, topZ, groundZ, liftZ, cs);
        if (!emitted) return out;

        const F32 zMid = (cs.zTop + cs.zBot) * 0.5f;
        const F32 aglTop = baseAglM - (baseZ - cs.zTop);
        const F32 aglBot = baseAglM - (baseZ - cs.zBot);
        const F32 aglMid = baseAglM - (baseZ - zMid);
        const Vec2 skewTop = profileSkewM(params, baseAglM, aglTop, fallSpeed);
        const Vec2 skewBot = profileSkewM(params, baseAglM, aglBot, fallSpeed);

        out.zTop = cs.zTop;
        out.zBot = cs.zBot;
        out.centreXY = profileSkewM(params, baseAglM, aglMid, fallSpeed);
        out.shearXY.x = skewTop.x - skewBot.x;
        out.shearXY.y = skewTop.y - skewBot.y;
        out.h01Mid = baseProfileH01(zMid, baseZ, groundZ);
        return out;
    }

    // Edge fade: the emitter calls SSDeckLod::edgeFade directly (one formula site); this core deliberately owns no copy.
}

#endif
