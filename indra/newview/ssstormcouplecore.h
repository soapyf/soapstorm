/**
 * @file ssstormcouplecore.h
 * @brief Atmo Magic: storm cells -> volumetric deck coupling, header-only core shared by CPU builder, GLSL twin and precipitation. CONTRACT.
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

#ifndef SS_STORMCOUPLECORE_H
#define SS_STORMCOUPLECORE_H

// <SS:Nexii> A CORE header (lldefs.h, sswindprofilecore.h, <cmath> only). Design: doc/atmo_magic_storm_dynamics.md section 3, phase 1 of the coupling: storm cells modulate the deck's TOWER shaping and ANVIL only - never the cell gate, never presence - so the three-way gate replication (builder / ss_cell_occupied / shadow bake) stays untouched and the shadow bake, which reads presence and gate only, needs no storm term of ITS OWN yet. NEW-7 correction (2026-09-05): that claim covers only what THIS header's own functions touch (tower/anvil shaping) - it is no longer true of the coupling as a whole. Phase 4's hero frame shift S (ssdeckframecore.h, doc/atmo_magic_storm_dynamics.md section 3 "Storm motion vs cloud drift") DOES move presence and the shadow bake: bakeGroundShadow's TEXEL loop and the fragment stage's cell gate both read presence at gateAir = air - S, so a hero cell's influence field (this header's own `influence`, sampled through SSDeckFrame::heroShift) reaches gate/presence/bake after all - through SSDeckFrame, not through anything added here. What IS replicated is smaller and new: every function here is written in a GLSL-portable subset (smoothstep, mix/lerp, max, length, clamp) so that phase 3 can copy it verbatim into ssVolCloudF.glsl; the phase-3 twin test (V:\Scratch\atmo\tests\twin_stormcouple.cpp) is what will transliterate the shader text back and assert equality over a grid - until that test exists and passes, no equivalence with the shader is claimed. Inputs are WORLD-frame (agent) coordinates: the builder converts its air-frame cell centre with + drift, the fragment stage uses world_true.xy, precipNoiseAt uses pos_agent - three call sites, one frame. Cell parameters arrive as small uniform-shaped structs the shell fills from SSStormCells once per frame - at most MAX_CELLS slots, filled HERO FIRST and then by distance to the weather-domain ANCHOR (shared world state, so every client under the sky fills the same slots), never by camera distance. The 2b calibration showed severe skies carry 20-40 alive cells for 4 slots; the anchor-nearest rule makes the cells that matter around the region the ones that shape the deck, and the deck-wide consolidation ramp already covers the rest. selectSlots is that anchor-nearest ranking, callable from the shell (SSStormCells::fillUniforms) without either side re-deriving the rule by hand. motionDirection/rotSignOf/overshootBonusM/mammatusScale are CPU-ONLY shape helpers - unlike the functions above, phase 1 does not mirror them into ssVolCloudF.glsl (overshoot/mammatus are builder-side height and flatten adjustments, never a fragment read), so no twin test is owed for them.
#include "lldefs.h"
#include "sswindprofilecore.h"

#include <cmath>
#include <cstdint>

namespace SSStormCouple
{
    constexpr S32 MAX_CELLS       = 4;       // LOCKSTEP ssVolCloudF.glsl uniform array length ss_storm_a/ss_storm_b/ss_storm_c[MAX_CELLS]
    constexpr F32 STORM_TOWER_LO  = 0.12f;   // LOCKSTEP ssvolcloud.cpp consolidation target window (lerp(SS_TOWER_LO, 0.12, storm))
    constexpr F32 STORM_TOWER_HI  = 0.60f;   // LOCKSTEP ssvolcloud.cpp consolidation target window
    constexpr F32 INFLUENCE_CORE  = 0.4f;    // LOCKSTEP SSStormCell::influence - full influence inside this share of the radius
    constexpr F32 PRECIP_SHIFT_M  = 900.f;   // how far downwind (along storm motion) the precipitation column is displaced at full meso
    // <SS:Nexii> NEW-7 (2026-09-05): KEY_POS_QUANT_M/KEY_VAL_QUANT and the foldKey they fed are DELETED - dead code.
    // They were "provided for phase 4" against a cache key phase 4 never used: the bake's actual hero-frame key
    // term is SSDeckFrame::foldFrameKey (ssdeckframecore.h), a different quantisation scheme entirely (CELL_M and
    // CELL_M/4, not these constants), folded straight into the shadow bake's own key in bakeGroundShadow.
    constexpr F32 DELEGATION      = 0.5f;    // share of the deck-wide consolidation handed to the cells while any cell is active (review S3: at spawning weathers the deck-wide ramp already saturates the window, leaving the per-cell boost nothing to add; delegating half of it back makes cells stand out of a less-consolidated sky)
    constexpr F32 RADIUS_MIN_M    = 900.f;   // LOCKSTEP SSStormCell::RADIUS_MIN_M - cellsActivity's fade-in radius
    constexpr F32 PRECIP_SHIFT_MAX_FRAC = 0.6f; // the precip displacement never exceeds this share of the owner cell's radius (review S11: PRECIP_SHIFT_M equals RADIUS_MIN_M, so an uncapped shift could fold two landing points onto one column)
    constexpr F32 OVERSHOOT_HEIGHT_FRAC = 0.35f; // LOCKSTEP ssvolcloud.cpp buildDeck - overshootBonusM's share of the deck's thickness at full overshoot
    constexpr F32 MAMMATUS_TOP_FRAC = 2.f / 3.f; // LOCKSTEP ssvolcloud.cpp buildDeck - mammatusScale applies only to sub-puffs at or above this up_cell fraction (top third of the column)
    constexpr F32 MAMMATUS_SCALE_GAIN = 0.6f;    // LOCKSTEP ssvolcloud.cpp buildDeck - mammatusScale's gain at full mammatus
    constexpr F32 POCKET_HEIGHT_FRAC = 0.55f;    // cellShapeAt: how low the pockets between towers are held at full convection, as a fraction of the height they would otherwise reach - moved from ssvolcloud.cpp's SS_POCKET_H (softened from 0.45, where pockets read as stubs hanging under the deck and the map's carving dominated the silhouette from below; at 0.55 the field keeps a body under its own structure)
    constexpr F32 CLUSTER_EDGE_HEIGHT_FRAC = 0.3f; // cellShapeAt: the cluster lattice's own edge-of-mass height floor, before the tower ramp's say over it

    // One active cell as the shader sees it (two vec4s + one vec4 of direction/sign). All floats so it maps 1:1 onto
    // uniform arrays; the shell fills it from SSStormCells (world/agent frame centre, metres).
    struct CellUniform
    {
        F32 x = 0.f, y = 0.f;     // ss_storm_a.xy  centre, world frame
        F32 radius = 0.f;         // ss_storm_a.z   current influence radius (SSStormCell::radiusAt); <= 0 disables the cell
        F32 boost = 0.f;          // ss_storm_a.w   lifecycle tower boost * intensity, [0,1]
        F32 anvil = 0.f;          // ss_storm_b.x   lifecycle anvil * intensity
        F32 meso = 0.f;           // ss_storm_b.y   lifecycle meso (0 for non-supercells)
        F32 overshoot = 0.f;      // ss_storm_b.z
        F32 mammatus = 0.f;       // ss_storm_b.w
        F32 dirX = 0.f, dirY = 1.f; // ss_storm_c.xy unit storm-motion direction (precip displacement axis)
        F32 rotSign = 1.f;        // ss_storm_c.z   +1 cyclonic, -1 anticyclonic
    };

    // What the field reads at one world point.
    struct Sample
    {
        F32 boost = 0.f;      // max over cells of influence * boost
        F32 anvil = 0.f;      // max over cells of influence * anvil
        F32 overshoot = 0.f;  // max over cells of influence * overshoot
        F32 mammatus = 0.f;   // max over cells of influence * mammatus
        F32 meso = 0.f;       // OWNER cell's influence * meso  (owner = the cell with the largest influence here; rotation never blends)
        F32 dirX = 0.f, dirY = 0.f; // owner's motion direction (zero vector when no owner)
        F32 rotSign = 0.f;    // owner's sign, 0 when no owner
        F32 owner = 0.f;      // owner's influence itself
        F32 lineWall = 0.f;   // 8a: the squall line-band's wall term here (applyLineBand), [0,1]; 0 with no line
        F32 lineShelf = 0.f;  // 8a: the gust-front shelf term ahead of the line, [0,1]; 0 with no line
        F32 ownerRadius = 0.f; // owner's radius, metres (0 when no owner) - CPU-only consumer (precipShift's cap); the GLSL twin's struct may omit it and the twin test compares the other fields explicitly
    };

    // GLSL semantics smoothstep, so the twin is a literal copy. Invariants: 0 for x<=e0, 1 for x>=e1, monotone; e1<=e0 -> step.
    inline F32 smoothstep(F32 e0, F32 e1, F32 x)
    {
        if (e1 <= e0)
        {
            return (x <= e0) ? 0.f : 1.f;
        }
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // Radial influence of a cell at (px,py): 1 - smoothstep(radius * INFLUENCE_CORE, radius, dist) - edges spelled e0 < e1
    // so the GLSL-semantics smoothstep above never hits its degenerate step - 1 inside the core, 0 at and beyond the
    // radius, 0 everywhere when radius <= 0. LOCKSTEP with SSStormCell::influence (same formula and spelling; a
    // cross-section test pins them bit-equal). GLSL: 1.0 - smoothstep(r * 0.4, r, length(p - c)) guarded by r > 0.
    inline F32 influence(F32 cx, F32 cy, F32 radius, F32 px, F32 py)
    {
        if (radius <= 0.f)
        {
            return 0.f;
        }
        const F32 dx = px - cx;
        const F32 dy = py - cy;
        const F32 dist = std::sqrt(dx * dx + dy * dy);
        return 1.f - smoothstep(radius * INFLUENCE_CORE, radius, dist);
    }

    // The storm field at a world point over up to n cells (n clamped to [0, MAX_CELLS]; cells with radius <= 0 skipped).
    // boost/anvil/overshoot/mammatus combine by MAX of influence*value; meso/dir/rotSign come from the single owner cell
    // (largest influence; ties broken by lower index so the result is order-stable). Invariants: all fields 0 when n==0
    // or every cell has radius <= 0; every field in [0,1] except dir (unit or zero) and rotSign (+-1 or 0); a point
    // outside every radius reads all zero; a point at a cell centre with boost 1 reads boost 1; adding a cell can never
    // lower boost/anvil/overshoot/mammatus; result independent of the order of cells except for exact-tie ownership.
    inline Sample sampleAt(const CellUniform* cells, S32 n, F32 px, F32 py)
    {
        Sample s;
        const S32 count = llclamp(n, (S32)0, MAX_CELLS);
        F32 ownerInf = 0.f;
        S32 ownerIdx = -1;
        for (S32 i = 0; i < count; ++i)
        {
            const CellUniform& c = cells[i];
            if (c.radius <= 0.f)
            {
                continue;
            }
            const F32 inf = influence(c.x, c.y, c.radius, px, py);
            s.boost     = llmax(s.boost,     inf * c.boost);
            s.anvil     = llmax(s.anvil,     inf * c.anvil);
            s.overshoot = llmax(s.overshoot, inf * c.overshoot);
            s.mammatus  = llmax(s.mammatus,  inf * c.mammatus);
            if (inf > ownerInf)
            {
                ownerInf = inf;
                ownerIdx = i;
            }
        }
        if (ownerIdx >= 0)
        {
            const CellUniform& o = cells[ownerIdx];
            s.meso = ownerInf * o.meso;
            s.dirX = o.dirX;
            s.dirY = o.dirY;
            s.rotSign = o.rotSign;
            s.owner = ownerInf;
            s.ownerRadius = o.radius; // <SS:Nexii> owner's radius, metres; 0 when no owner (Sample's default) - precipShift's cap reads this
        }
        return s;
    }

    // The deck's baked tower window, blended toward the storm window by boost: lo' = mix(lo, STORM_TOWER_LO, boost),
    // hi' = mix(hi, STORM_TOWER_HI, boost). Invariants: boost 0 returns the inputs bit-exactly; boost 1 returns the storm
    // window bit-exactly; lo' <= hi' whenever lo <= hi and STORM_TOWER_LO <= STORM_TOWER_HI.
    inline void towerWindow(F32 lo, F32 hi, F32 boost, F32& loOut, F32& hiOut)
    {
        const F32 b = llclamp(boost, 0.f, 1.f);
        loOut = lo + (STORM_TOWER_LO - lo) * b;
        hiOut = hi + (STORM_TOWER_HI - hi) * b;
    }

    // The tower weight of a raw map sample n under the (possibly storm-widened) window: smoothstep(lo', hi', n). This is
    // exactly what the builder's noiseFieldAt and the shader's `tower` compute today with boost 0. Invariants: boost 0
    // equals smoothstep(lo, hi, n); monotone non-decreasing in n and in boost (widening the window can only raise tower
    // for fixed n in the region between the windows); in [0,1].
    inline F32 towerFromMap(F32 n, F32 lo, F32 hi, F32 boost)
    {
        F32 loP, hiP;
        towerWindow(lo, hi, boost, loP, hiP);
        return smoothstep(loP, hiP, n);
    }

    // The cell/fragment anvil weight: max(deckAnvil, smoothstep(0.40, 0.70, convection) * tower, stormAnvil) - the two
    // existing terms (ssvolcloud.cpp cell_anvil, ssVolCloudF.glsl anvil_w) plus the storm's own. Invariants: stormAnvil 0
    // reproduces the existing expression bit-exactly; monotone in every argument; in [0,1] for inputs in [0,1].
    // CAVEAT (phase-3c F3): the fragment's LITERAL twin (ssVolCloudF.glsl ss_storm_anvilWeight) is called with
    // ss_churn in this slot, not a fragment-stage convection value - the two are the same figure in this fork
    // (ss_churn is uploaded as clamp(convection)) but that is a wiring fact the caller owns, not something this
    // function checks; a future caller that decouples churn from convection must re-derive this invariant.
    inline F32 anvilWeight(F32 deckAnvil, F32 convection, F32 tower, F32 stormAnvil)
    {
        return llmax(deckAnvil, smoothstep(0.40f, 0.70f, convection) * tower, stormAnvil);
    }

    // The per-cell height/anvil shaping the CPU builder's placement loop and the V2 profile-outline debug view both
    // derive from one cell's (convection, storm_eff, tower, coreness): conv_gain (how much say the map's tower ramp
    // gets over height at all), cell_height (the column's own ceiling as a fraction of the deck's thickness) and
    // cell_anvil (the deck/cell-wide anvil floor before any per-puff ramp term). Moved here from ssvolcloud.cpp
    // (numeric shape maths belongs in a core, not the shell - PLAN.md lesson 2) so the two call sites cannot respell
    // it out of step with each other; ssvolcloud.cpp keeps only the LL-type adaptation around the call. Invariants:
    // conv_gain in [0,1], 0 when storm_eff is 1 regardless of convection; cell_height in
    // [CLUSTER_EDGE_HEIGHT_FRAC * POCKET_HEIGHT_FRAC, 1] for coreness/tower/conv_gain in [0,1] (the low end is
    // coreness 0, conv_gain 1, tower 0 - a full-convection pocket at the map's own floor); cell_anvil ==
    // anvilWeight(deckAnvil, convection, tower, stormAnvil) bit-exactly; pure function of its arguments.
    struct CellShape
    {
        F32 conv_gain = 0.f;
        F32 cell_height = 0.f;
        F32 cell_anvil = 0.f;
    };

    inline CellShape cellShapeAt(F32 convection, F32 storm_eff, F32 tower, F32 coreness, F32 deckAnvil, F32 stormAnvil)
    {
        CellShape out;
        out.conv_gain = smoothstep(0.12f, 0.55f, convection) * (1.f - storm_eff);
        const F32 height_shape = std::lerp(1.f, POCKET_HEIGHT_FRAC + (1.f - POCKET_HEIGHT_FRAC) * tower, out.conv_gain);
        out.cell_height = (CLUSTER_EDGE_HEIGHT_FRAC + (1.f - CLUSTER_EDGE_HEIGHT_FRAC) * coreness) * height_shape;
        out.cell_anvil = anvilWeight(deckAnvil, convection, tower, stormAnvil);
        return out;
    }

    // CPU-ONLY shape modifiers - never mirrored in ssVolCloudF.glsl, per the phase-1 brief ("no shader change"):
    // overshoot lifts the tallest sub-puff of a coupled cell, mammatus flares the flattening on puffs already in
    // the anvil's top third. Both are formulas, so they live here rather than in ssvolcloud.cpp's builder loop.

    // The 3-D squash-knee cull the builder applies at a puff's own TRUE distance to camera (dx,dy,dz, world/agent
    // frame): true once that distance exceeds kneeM. CPU-ONLY (never mirrored in ssVolCloudF.glsl) - the ONE
    // predicate buildDeck's per-cell "will the overshoot-eligible sub survive" guard and its per-sub cull
    // (`squashed && sub > 0`) both read, so the guard can never call a sub eligible for the overshoot bonus that
    // the cull is about to drop (phase-3c review F1: the guard used to test only dx/dy, and with the deck 800-2000m
    // above a ground camera the omitted dz term made the guard and the cull disagree). Invariants: false whenever
    // dx==dy==dz==0 and kneeM>0; true iff dx*dx+dy*dy+dz*dz > kneeM*kneeM; matches dist_sq > kneeM*kneeM bit-exactly
    // when dist_sq is computed the same way; symmetric in the sign of dx/dy/dz.
    inline bool squashedAt(F32 dx, F32 dy, F32 dz, F32 kneeM)
    {
        return (dx * dx + dy * dy + dz * dz) > kneeM * kneeM;
    }

    // The overshooting-top height bonus added to a cell's tallest sub-puff only: OVERSHOOT_HEIGHT_FRAC of the
    // deck's own thickness at overshoot 1, scaling linearly with it. Invariants: 0 at overshoot 0; ==
    // OVERSHOOT_HEIGHT_FRAC * thicknessM at overshoot 1; monotone in overshoot; never negative for a non-negative
    // thicknessM.
    inline F32 overshootBonusM(F32 thicknessM, F32 overshoot)
    {
        return OVERSHOOT_HEIGHT_FRAC * llmax(thicknessM, 0.f) * llclamp(overshoot, 0.f, 1.f);
    }

    // The mammatus waist/flare multiplier for a sub-puff already in the anvil's top third (up_cell >=
    // MAMMATUS_TOP_FRAC): scales the puff's (waist + flare - 1) flattening term by this factor. Invariants: 1 at
    // mammatus 0 (no change); 1 + MAMMATUS_SCALE_GAIN at mammatus 1; monotone in mammatus; never below 1.
    inline F32 mammatusScale(F32 mammatus)
    {
        return 1.f + MAMMATUS_SCALE_GAIN * llclamp(mammatus, 0.f, 1.f);
    }

    // The storm-motion unit direction the shader/precip axis reads, guarding the degenerate zero-motion case the
    // same way every other drift/wind normalisation in this fork does: falls back to (0,1) - due north, the
    // CellUniform default - rather than propagate a NaN. Invariants: |result| == 1 whenever mx or my is non-zero;
    // exactly (0,1) when both are 0; parallel to (mx, my) otherwise.
    inline void motionDirection(F32 mx, F32 my, F32& dirX, F32& dirY)
    {
        const F32 mag = std::sqrt(mx * mx + my * my);
        if (mag < 1e-5f)
        {
            dirX = 0.f;
            dirY = 1.f;
            return;
        }
        dirX = mx / mag;
        dirY = my / mag;
    }

    // The rotation sign the shader reads: +1 cyclonic, -1 anticyclonic, and (matching SSStormCell::candidate's own
    // hashed sign draw) +1 on the exact-zero edge rather than a third state - CellUniform::rotSign has no zero
    // case. Invariants: result is exactly +1 or -1; matches the sign of a non-zero rotation.
    inline F32 rotSignOf(F32 rotation)
    {
        return (rotation < 0.f) ? -1.f : 1.f;
    }

    // The storm consolidation product the builder has always computed - smoothstep(0.55,0.85,moisture) *
    // smoothstep(0.45,0.75,convection) - now in ONE place (review 3b NEW-3: it was respelled in the builder and the V2
    // overlay; phase-3c F4: and again in sswindprofilecore.h's autoShear, which is the actual sole definition now -
    // SSWindProfile::consolidation - since a storm core may include the wind core but not the reverse). Invariants: 0
    // when either input is at or below its window floor; 1 at (0.85, 0.75) and above; monotone in both; in [0,1].
    inline F32 consolidation(F32 moisture, F32 convection)
    {
        return SSWindProfile::consolidation(moisture, convection);
    }

    // How active the fetched cells are for the delegation, as a SMOOTH figure (review 3b NEW-8: a 0/1 step on 'any
    // radius > 0' snapped the whole deck's window on a cell's birth frame): max over cells of min(1, radius / RADIUS_MIN_M).
    // Because radiusAt rises from 0 with the lifecycle and falls back to 0 in decay, this fades the delegation in and out
    // with the storm itself. Invariants: 0 when n == 0 or every radius <= 0; in [0,1]; monotone non-decreasing in every
    // radius; equals 1 once any radius >= RADIUS_MIN_M; independent of cell order.
    inline F32 cellsActivity(const CellUniform* cells, S32 n)
    {
        const S32 count = llclamp(n, (S32)0, MAX_CELLS);
        F32 activity = 0.f;
        for (S32 i = 0; i < count; ++i)
        {
            const CellUniform& c = cells[i];
            if (c.radius <= 0.f)
            {
                continue;
            }
            activity = llmax(activity, llmin(1.f, c.radius / RADIUS_MIN_M));
        }
        return activity;
    }

    // The effective lid altitude for the fragment stage's lid cut and cap band (review 3b NEW-4: the CPU lifts the tallest
    // sub-puff by overshootBonusM ABOVE top_z, then the shader's lid cut erased it exactly when anvil_w ~ 1, the state that
    // produces overshoot). lidTopM(topZ, thicknessM, overshoot) = topZ + overshootBonusM(thicknessM, overshoot). MIRRORED in
    // GLSL (unlike the other CPU-only shape helpers) and sampled at the same quantized cell as the storm field, so the lid
    // rises exactly where the CPU lifted the puff. Invariants: overshoot 0 returns topZ bit-exactly; monotone in overshoot;
    // never below topZ; equals topZ + OVERSHOOT_HEIGHT_FRAC * thicknessM at overshoot 1.
    inline F32 lidTopM(F32 topZ, F32 thicknessM, F32 overshoot)
    {
        return topZ + overshootBonusM(thicknessM, overshoot);
    }

    // The deck-wide consolidation figure once cells exist: storm * (1 - DELEGATION * cellsActive), where storm is the
    // builder's smoothstep(0.55,0.85,moisture) * smoothstep(0.45,0.75,convection) and cellsActive is 1 when at least one
    // uniform slot has radius > 0 (0 otherwise; the shell may pass a fraction to fade). The builder derives mNoiseTowerLo/Hi
    // AND conv_gain from this figure instead of raw storm, so the baked window uniform carries the delegation to the
    // fragment stage with no new replication. Invariants: cellsActive 0 returns storm bit-exactly; monotone
    // non-decreasing in storm and non-increasing in cellsActive; in [0,1] for inputs in [0,1].
    inline F32 deckConsolidation(F32 storm, F32 cellsActive)
    {
        const F32 active = llclamp(cellsActive, 0.f, 1.f); // <SS:Nexii> phase 3b: storm * (1 - DELEGATION * cellsActive) per doc/atmo_magic_storm_dynamics.md S3 Delegation
        return storm * (1.f - DELEGATION * active);
    }

    // The precipitation column displacement at a point: precipitation that lands at p was fed by the column
    // PRECIP_SHIFT_M * s.meso UPWIND along the owner's motion direction, so the gate samples the field at p - shift and
    // the rain therefore appears DOWNWIND of the updraft while the updraft itself stays rain-free. Returns the shift to
    // SUBTRACT from the sample point. The magnitude is min(PRECIP_SHIFT_M * meso, PRECIP_SHIFT_MAX_FRAC * ownerRadius) so
    // the mapping from landing point to feeding column stays inside the owner cell and monotone (review S11).
    // Invariants: zero when meso is 0 or there is no owner; |shift| == min(PRECIP_SHIFT_M * meso, PRECIP_SHIFT_MAX_FRAC *
    // ownerRadius) within 1e-3; parallel to (dirX, dirY). NOTE (review S1): the caller displaces only the RAW MAP sample
    // (tower) by this shift - presence is read at the undisplaced point, or rain falls out of holes.
    inline void precipShift(const Sample& s, F32& dx, F32& dy)
    {
        const F32 mag = llmin(PRECIP_SHIFT_M * s.meso, PRECIP_SHIFT_MAX_FRAC * s.ownerRadius); // <SS:Nexii> review S11 cap: never exceed this share of the owner's radius
        dx = mag * s.dirX;
        dy = mag * s.dirY;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // 8a LINE BAND (doc/atmo_magic_phase8_show.md section 3 item 2): a squall line couples into the deck as ONE continuous
    // wall along its segment, not as seven discs fighting for MAX_CELLS slots. The shell fills one LineBand per frame from
    // the active line (SSStormCells: the line's origin advected to now, its direction and unit motion, half-length,
    // SSSquall::LINE_BAND_M / LINE_SHELF_M, strength = the line's live intensity), strength <= 0 disables it. LOCKSTEP
    // ssVolCloudF.glsl uniforms ss_line_a (ox, oy, dirX, dirY), ss_line_b (motX, motY, halfLen, bandM), ss_line_c (shelfM,
    // strength, 0, 0) and the GLSL twins ss_line_field / ss_line_apply (twin test transliterates them).
    struct LineBand
    {
        F32 ox = 0.f, oy = 0.f;       // segment centre, world frame, at now
        F32 dirX = 0.f, dirY = 1.f;   // unit along the segment
        F32 motX = 0.f, motY = 0.f;   // unit motion (the line's advance); (0,0) when the line is still
        F32 halfLen = 0.f;            // metres
        F32 bandM = 0.f;              // wall half-thickness (SSSquall::LINE_BAND_M)
        F32 shelfM = 0.f;             // shelf reach ahead (SSSquall::LINE_SHELF_M)
        F32 strength = 0.f;           // [0,1]; <= 0 disables
    };
    struct LineField
    {
        F32 wall = 0.f;   // [0,1]
        F32 shelf = 0.f;  // [0,1]
    };

    // The wall and shelf terms at a world point. perp = |component of (p - o) perpendicular to dir|, along = dot(p - o,
    // dir), ahead = dot(p - o, mot). endcap = 1 - smoothstep(halfLen, halfLen + bandM, |along|). wall = strength * endcap *
    // (1 - smoothstep(0.5 * bandM, bandM, perp)). shelf = strength * endcap * smoothstep(0, 0.3 * shelfM, ahead) *
    // (1 - smoothstep(0.6 * shelfM, shelfM, ahead)) - zero behind the line and beyond the shelf's reach. Invariants: both
    // 0 when strength <= 0 or bandM <= 0; both in [0,1]; wall == strength on the segment (perp <= 0.5 bandM, |along| <=
    // halfLen); wall == 0 at perp >= bandM; shelf == 0 for ahead <= 0 and for ahead >= shelfM; symmetric under
    // dir -> -dir; pure; GLSL-portable (smoothstep/dot/abs/max only) so the twin is literal.
    inline LineField lineField(const LineBand& b, F32 px, F32 py)
    {
        LineField f;
        if (b.strength <= 0.f || b.bandM <= 0.f)
        {
            return f;
        }
        const F32 dx = px - b.ox;
        const F32 dy = py - b.oy;
        const F32 along = dx * b.dirX + dy * b.dirY;
        const F32 perp = std::sqrt(llmax(dx * dx + dy * dy - along * along, 0.f));
        const F32 ahead = dx * b.motX + dy * b.motY;
        const F32 endcap = 1.f - smoothstep(b.halfLen, b.halfLen + b.bandM, std::abs(along));
        f.wall = b.strength * endcap * (1.f - smoothstep(0.5f * b.bandM, b.bandM, perp));
        f.shelf = b.strength * endcap * smoothstep(0.f, 0.3f * b.shelfM, ahead) * (1.f - smoothstep(0.6f * b.shelfM, b.shelfM, ahead));
        return f;
    }

    // Folds a line field into a Sample: boost = max(boost, wall); anvil = max(anvil, wall * anvilFrac); lineWall = wall;
    // lineShelf = shelf; every other field untouched (rotation/motion ownership never blends - a line has no meso here).
    // Invariants: idempotent; never lowers boost/anvil; a zero field leaves the Sample bit-identical.
    inline void applyLineBand(Sample& s, const LineField& f, F32 anvilFrac)
    {
        s.boost = llmax(s.boost, f.wall);
        s.anvil = llmax(s.anvil, f.wall * anvilFrac);
        s.lineWall = f.wall;
        s.lineShelf = f.shelf;
    }

    // review 3b NEW-7 (doc S3 "Sample point lockstep"): the QUANTIZED cell centre a world/air point's storm sample
    // must be taken at, so the builder's per-cell sample, precipNoiseAt's landing-point sample and the shader's own
    // literal twin (ss_storm_samplePoint in ssVolCloudF.glsl) all read the same point instead of each rounding an
    // arbitrary sub-cell position to its own answer. airX/airY are AIR-frame (already drift-subtracted); the result
    // is WORLD-frame (drift added back), matching sampleAt's own frame contract. Invariants: result is independent
    // of the exact airX/airY within one cellM x cellM cell (only the cell floor(air/cellM) matters); equals
    // (floor(airX/cellM)+0.5)*cellM + driftX (and the y twin) exactly; cellM > 0 assumed (the caller's CELL_M).
    inline void samplePointM(F32 airX, F32 airY, F32 cellM, F32 driftX, F32 driftY, F32& outX, F32& outY)
    {
        outX = (std::floor(airX / cellM) + 0.5f) * cellM + driftX;
        outY = (std::floor(airY / cellM) + 0.5f) * cellM + driftY;
    }

    // The uniform-slot ranking key SSStormCells::fillUniforms sorts by: just an id, a centre and whether the cell
    // is slot-preferred, stripped of every other field so this stays a shell-independent core type. The shell
    // fills one per active cell (world/agent frame - either works, since only relative distance matters). 7c
    // NEW-6: `hero` is true for the resolved hero AND for an authored forced pin (mIsHero || mIsForced) - a pin
    // is guaranteed-active by the same authoring intent a hero flyby is, so it must not lose its slot to an
    // ordinary cell merely because it is not the one resolveActive happened to name hero.
    struct SelectKey
    {
        U64 id = 0;
        F32 x = 0.f, y = 0.f;
        bool hero = false;
    };

    // Chooses up to `cap` of `cells[0..n)` for the fixed-size uniform arrays: the FIRST entry with hero == true
    // (in input order - the resolved hero if it is one, else whichever forced pin comes first) always takes slot
    // 0 when present, and every remaining slot - including any OTHER hero-flagged entry, such as a second forced
    // pin the shell did not name hero - is filled by ascending distance of the cell's centre to (anchorX,
    // anchorY) - the shared weather-domain anchor, NEVER the camera - ties broken by ascending id so the choice
    // is independent of the input order and stable when two cells sit at the exact same distance. Writes the
    // chosen indices (into `cells`) to outIdx. Invariants: returns min(n, cap); when n > 0 and cap > 0 and some
    // cell has hero == true, outIdx[0] is that (first) cell's index; the distances of the cells at outIdx[1..)
    // are non-decreasing; every kept cell is at least as close to the anchor as every dropped cell not chosen for
    // slot 0 (a true partial selection sort, not merely "some" ordering); result independent of the order of
    // `cells` except for exact ties, which resolve by id, and except for which hero-flagged entry lands in slot 0
    // when more than one is present.
    // 7f F3: whether an entry earns SelectKey::hero. A slot-preferred cell must be able to INFLUENCE the deck: the
    // 7f hero is cull-exempt and may spawn up to HERO_SPAWN_MAX_M (30 km) out, where every deck sample reads influence
    // 0, yet it would hold one of MAX_CELLS slots for half its life. So preference needs the flag AND the centre
    // within reachM of the anchor (the shell passes its field radius + SSStormCell::RADIUS_MAX_M); reachM <= 0 means
    // no distance condition. Invariants: false when !flag; true when flag and reachM <= 0; pure.
    inline bool slotPreferred(bool flag, F32 dx, F32 dy, F32 reachM)
    {
        if (!flag) return false;
        if (reachM <= 0.f) return true;
        return dx * dx + dy * dy <= reachM * reachM;
    }

    inline S32 selectSlots(const SelectKey* cells, S32 n, F32 anchorX, F32 anchorY, S32* outIdx, S32 cap)
    {
        const S32 count = llmax(n, (S32)0);
        const S32 want = llmin(cap, count);
        if (!cells || !outIdx || want <= 0)
        {
            return 0;
        }

        S32 heroIdx = -1;
        for (S32 i = 0; i < count; ++i)
        {
            if (cells[i].hero)
            {
                heroIdx = i;
                break;
            }
        }

        S32 filled = 0;
        if (heroIdx >= 0)
        {
            outIdx[filled++] = heroIdx;
        }

        // Selection-sort the rest by distance-to-anchor (ties by id): cap is small (MAX_CELLS), so an O(want *
        // count) scan beats pulling <algorithm>'s std::sort in for a handful of picks.
        for (S32 slot = filled; slot < want; ++slot)
        {
            S32 bestIdx = -1;
            F32 bestDistSq = 0.f;
            U64 bestId = 0;
            for (S32 i = 0; i < count; ++i)
            {
                if (i == heroIdx)
                {
                    continue;
                }
                bool already = false;
                for (S32 k = 0; k < slot; ++k)
                {
                    if (outIdx[k] == i)
                    {
                        already = true;
                        break;
                    }
                }
                if (already)
                {
                    continue;
                }
                const F32 dx = cells[i].x - anchorX;
                const F32 dy = cells[i].y - anchorY;
                const F32 distSq = dx * dx + dy * dy;
                if (bestIdx < 0 || distSq < bestDistSq
                    || (distSq == bestDistSq && cells[i].id < bestId))
                {
                    bestIdx = i;
                    bestDistSq = distSq;
                    bestId = cells[i].id;
                }
            }
            if (bestIdx < 0)
            {
                return slot; // fewer distinct cells than `want` (should not happen: bestIdx always finds one while slot < count)
            }
            outIdx[slot] = bestIdx;
        }
        return want;
    }
}

#endif
