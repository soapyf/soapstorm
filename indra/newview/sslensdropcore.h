/**
 * @file sslensdropcore.h
 * @brief Atmo Magic: water on the camera lens. ONE population of drops that arrive small, GROW BY EATING EACH OTHER, break loose when
 *        they are too heavy for surface tension to hold, and shed what they cannot carry as they run. Placement, motion, merge, shed
 *        and evaporation laws, plus the cap normal / Fresnel / refraction optics the post pass shades them with. Header-only core.
 *        CONTRACT.
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

#ifndef SS_LENSDROPCORE_H
#define SS_LENSDROPCORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_surface_weather.md section 12.
//
// R16 (2026-09-07): A REWRITE, NOT A RETUNE, and the reason is worth writing down because the previous model was not badly built - it
// was built against a different reference. The R1-R15 build had THREE populations: a stateless procedural LATTICE of fine drops (one
// hashed life cycle per cell, no state, so two of them could never interact), a bounded list of simulated RUNNERS whose path the shader
// reconstructed in closed form, and BEADS derived procedurally from a runner's path. Against that model the user asked for four things
// - drops that appear suddenly and merge with surface tension, trail drops that pop into place, streak heads that grow the way the
// reference's do, and paths that are not straight - and every one of them is a property the model cannot have rather than a number that
// needs tuning. A lattice cell cannot merge with its neighbour. A closed-form path cannot be jagged (it is a smooth function of s).
// Growth that is an integral of an absorption rate cannot happen in jumps.
//
// All four are the SAME property of the reference (the codrops rain demo), which is a particle simulation with COLLISION MERGING:
//   - drops arrive small and immediately, at full size, because nothing grows them in;
//   - a drop grows ONLY by absorbing another drop, so growth is a jump, and the jump is where the tension wobble comes from;
//   - a drop that has grown past what surface tension can pin breaks loose and runs, sweeping up everything it touches, so it
//     accelerates in steps down the glass rather than gliding;
//   - a running drop sheds what it cannot carry, and a shed drop is a NEW DROP AT FULL SIZE - it pops;
//   - horizontal momentum takes a small random kick every step, so the path is jagged rather than a smooth meander.
// One mechanism, five behaviours. So this file now describes one population and the laws that act on it, and the three-population
// machinery (cellAnchor/fineRadius/cellSupply/tailFill, Runner/stepRunner/runSpeed/captureRatio/absorbRate, beadRadiusAt/headRadiusAtS/
// pathPerp/trackOpacity/tailFalloff and their constants) is GONE rather than left to rot beside its replacement.
//
// WHAT SURVIVED, deliberately: the OPTICS section at the bottom, verbatim - the cap normal, the contact angle, Fresnel, the refraction
// gain and the silhouette are about what a drop looks like once it is somewhere, they were right, and the user's own verdict on the
// R1-R15 build singled out the shading (the condensation especially) as the part worth keeping. So did the clear-centre coating law
// (clearRadial), which is a statement about the glass, not about the model of the water on it.
//
// COORDINATES, unchanged: STABLE SCREEN UNITS. x in [-aspect/2, aspect/2], y in [-0.5, 0.5], one unit = one screen HEIGHT, so a radius
// means the same thing however wide the window is and drops are round rather than stretched. Placement is a function of that frame and
// nothing else - no camera, no wind, no slide direction (defect R1, still binding: water already on the glass does not move because the
// camera rolled).
//
// WHERE THE DROPS ARE DRAWN, which is what makes hundreds of them affordable. The R1-R15 build evaluated every population per fragment,
// which is why the runner list had to fit in 24 uniform slots and why the fine drops had to be a lattice a fragment could look up in
// O(1). This build does what the reference does: the shell draws each drop ONCE as a sprite into an offscreen DROP MAP (rgb = the cap
// normal, a = coverage) and the post pass takes one texture fetch. Per-fragment cost stops scaling with the drop count entirely, and
// the population is free to be a few hundred drops with real state. It also gets the merge look for nothing: two overlapping sprites
// blend their normals across the neck between them, which is the tension the user asked for.
//
// STATE AND DETERMINISM. SSScreenFXPost owns the Drop list and steps it here. There is still NO RUNTIME RNG anywhere: every "random"
// quantity is hash21 of quantities the simulation already has (the drop's own seed, a monotone spawn counter, the step index), so two
// clients with the same inputs see the same glass. That house rule is why the laws below take their randomness as an explicit hash
// argument rather than drawing it.

#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSLensDrop
{
    constexpr F32 TAU_F = 6.28318530717958647692f;

    // <SS:Nexii> GLSL-semantics smoothstep, local so this core needs nothing beyond lldefs.h. Every fade below is one of these because
    // its derivative vanishes at both ends. Guarded: e1 <= e0 is a degenerate edge, not a divide-by-zero - the ramp becomes a step.
    inline F32 smoothstep(F32 e0, F32 e1, F32 x)
    {
        if (e1 <= e0) return (x <= e0) ? 0.f : 1.f;
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    inline F32 fract(F32 x) { return x - std::floor(x); }
    inline F32 mix(F32 a, F32 b, F32 t) { return a + (b - a) * t; }

    // The hash. Presentation and per-step jitter only, never state, so fract(sin(dot(...))) is fine - but it is REPLICATED in the
    // shader, so the constants live here and the shader spells the same three. Returns [0,1).
    inline F32 hash21(F32 px, F32 py)
    {
        return fract(std::sin(px * 12.9898f + py * 78.233f) * 43758.5453f);
    }

    // A drop's normalised volume. The 4/3 pi is dropped on both sides of every budget below - nothing here is ever compared against a
    // real volume, only against another drop's, so the constant would cancel everywhere it appeared.
    inline F32 volumeOf(F32 r) { return r * r * r; }
    inline F32 radiusOf(F32 v) { return std::pow(llmax(v, 0.f), 1.f / 3.f); }

    // ================================================================== the size dial

    // <SS:Nexii> R14's dial, kept: every length below is its authored figure times SIZE, so the whole family can be scaled without
    // disturbing a single ratio. TO RETUNE: change this and re-evaluate every constant marked "* SIZE" HERE AND IN THE SHADER. They are
    // spelled as literals on both sides rather than as products, because the twin holds the two files BIT-IDENTICAL and 0.12f * 0.60f
    // does not round to the same float as the literal 0.072 - the product form fails by one ulp. The twin is therefore also the check
    // on the hand-evaluation: get the arithmetic wrong on either side and it says so.
    //
    // The figures below are also, now, close to the reference's own. codrops works in pixels on a canvas about 1000 px tall: droplets
    // 2-4 px and drops 10-40 px, i.e. 0.2-0.4% and 1-4% of the height. At SIZE 0.60 a fresh droplet here is 0.36-1.0% of the height and
    // a running drop 3.4-5.5%. The same two clusters, slightly coarser.
    constexpr F32 SIZE             = 0.60f;

    // ================================================================== the population

    constexpr S32 MAX_DROPS        = 512;     // hard ceiling on the list. The cost of a drop is now one sprite in the drop map, not a
                                              // per-fragment test, so this is a memory and CPU-merge bound rather than a shading one.
    constexpr F32 SPAWN_HZ_FULL    = 45.f;    // droplets arriving per second at wet 1, before the budget dial. High on purpose: arrival
                                              // is the ONLY source of water in this model, and everything bigger than a fresh droplet
                                              // got that way by eating some of these.
    constexpr F32 R_SPAWN_MIN      = 0.0018f; // 0.0030 * SIZE - a fresh droplet's radius, stable screen units...
    constexpr F32 R_SPAWN_MAX      = 0.00522f;// 0.0087 * SIZE - ... hashed between the two, biased small (see spawnRadius)
    constexpr F32 R_SPAWN_BIAS     = 2.5f;    // the hash is raised to this before the lerp, so most arrivals are near the minimum and
                                              // the big ones are rare - the drop-size distribution on wetted glass is heavily skewed,
                                              // and a uniform draw reads as a field of identical dots
    constexpr F32 R_MOVE           = 0.0168f; // 0.028 * SIZE - THE RELEASE LAW, unchanged in substance from R6: surface tension's
                                              // pinning force grows with the contact line (~r) and weight with volume (~r^3), so below
                                              // this a drop is pinned and above it the weight wins and never stops winning. What IS new
                                              // is how a drop gets here: only by MERGING. Nothing accretes continuously any more, so a
                                              // release is always the consequence of a visible event.
    constexpr F32 R_CAP            = 0.0276f; // 0.046 * SIZE - a drop cannot hold more than this; past it the excess is shed (shedNow)
    constexpr F32 R_DIE            = 0.0012f; // 0.0020 * SIZE - smaller than this and it has evaporated; the shell drops it
    constexpr F32 R_SPREAD_MAX     = 0.35f;   // the largest fractional stretch a merge can impose (see mergeSpread)

    // MOTION. A running drop's terminal speed goes as r^2 (weight ~r^3 drives, contact-line pinning ~r retards), and the real velocity
    // relaxes toward that target rather than being it - so a merge, which changes r in a step, shows up as a lurch that decays. That is
    // the R9 finding from the previous build and it survives the rewrite intact; what it no longer needs is R9's synthetic boost
    // machinery (BOOST_VOL_STEP/BOOST_KICK_FRAC), because in this model the radius really does jump and the lurch is the physics.
    constexpr F32 SPEED_REF_PER_S  = 0.50f;   // screen heights per second at exactly R_MOVE
    constexpr F32 SPEED_POW        = 2.0f;    // ... times (r / R_MOVE)^2
    constexpr F32 VELOCITY_TAU_S   = 0.10f;   // how fast the real velocity chases the target
    constexpr F32 WOBBLE_PER_S     = 0.22f;   // THE PATH IS NOT STRAIGHT. Sideways velocity gets a hashed kick of up to this many screen
                                              // heights per second, per second, every step - the reference's random horizontal momentum.
                                              // A running drop crosses ground that is wetted unevenly and its contact line keeps
                                              // catching and releasing on one side or the other; the visible result is a path that jinks
                                              // rather than curving. NOT a smooth noise field: that was the previous build's meander,
                                              // and a smooth function of distance produces a gentle S, never a jink.
    constexpr F32 WOBBLE_DAMP_PER_S = 6.0f;   // ... and sideways velocity is damped this hard, so a kick is a jink and not a drift
    constexpr F32 R_WOBBLE_REF     = 0.020f;  // wobble is scaled by R_WOBBLE_REF / r: a big drop has more momentum and is deflected less
                                              // by the same catch, which is why the small ones skitter and the heavy ones fall nearly
                                              // straight - visible in the reference and the thing that makes a field of them read as
                                              // varied rather than as one animation played at several speeds

    // MERGING is the whole engine. Any two drops whose centres are closer than MERGE_REACH times the sum of their radii combine: the
    // larger keeps its identity, its volume becomes the sum of both, and its momentum is the mass-weighted average (a static drop eaten
    // by a runner slows the runner slightly; a runner eaten by a bigger runner speeds the bigger one).
    constexpr F32 MERGE_REACH      = 0.85f;   // as a share of (r_a + r_b) - under 1 so the two have to visibly overlap before they snap
                                              // together, rather than merging across a gap
    constexpr F32 MERGE_SPREAD_GAIN = 0.55f;  // how much of the offset direction becomes stretch at the moment of a merge (see
                                              // mergeSpread) - the tension wobble, and the reason a merge reads as two drops pulling
                                              // into one rather than as one drop silently getting bigger
    constexpr F32 SPREAD_TAU_S     = 0.16f;   // ... which relaxes back to round over this. Surface tension's restoring time on a drop
                                              // this size is milliseconds; this is slowed to where an eye can see it, deliberately.

    // SHEDDING. A running drop leaves a trail, and in this model the trail is REAL DROPS, deposited whole, not a procedural derivation
    // of the path. Each is a new entry in the same list, static, at full size the instant it appears - the "popping into place" the user
    // asked for is simply what happens when nothing fades it in.
    constexpr F32 SHED_EVERY_R     = 2.2f;    // a running drop sheds once per this many of its OWN radii travelled, so a big drop leaves
                                              // a coarser trail than a small one and neither leaves a dotted line
    constexpr F32 SHED_FRAC        = 0.42f;   // the shed drop's radius as a share of the parent's, before the parent loses that volume.
                                              // Under the "strictly smaller than the head that laid it" ordering the previous build
                                              // proved, and now it holds by construction: the volume comes OUT of the parent.
    constexpr F32 SHED_MIN_R       = 0.0016f; // a parent this small has nothing left to give and stops shedding

    // EVAPORATION. Everything on the glass is drying all the time; a drop's VOLUME goes down linearly, so its radius draws in slowly at
    // first and quickly at the end - which is what makes a drop look like it is being absorbed rather than fading out.
    constexpr F32 EVAP_VOL_PER_S   = 3.4e-7f; // normalised volume per second at the edge of the coating (see clearRadial), so a fresh
                                              // droplet lives ~10 s and a big one much longer

    // One drop. Everything positional is stable screen units.
    struct Drop
    {
        F32 mX = 0.f, mY = 0.f;        // centre
        F32 mR = 0.f;                  // radius
        F32 mVX = 0.f, mVY = 0.f;      // velocity, screen heights per second (mVY is negative: down the glass)
        F32 mSpreadX = 0.f, mSpreadY = 0.f; // the tension stretch left over from its last merge, decaying to 0
        F32 mSinceShed = 0.f;          // distance run since it last shed
        F32 mAgeS = 0.f;               // seconds since it arrived
        F32 mSeed = 0.f;               // per-drop hash in [0,1)
        bool mRunning = false;         // has it broken loose
    };

    // A fresh droplet's radius from one hash, biased toward the minimum. Invariants: in [R_SPAWN_MIN, R_SPAWN_MAX]; monotone in h;
    // == R_SPAWN_MIN at h 0 and R_SPAWN_MAX at h 1; pure.
    inline F32 spawnRadius(F32 h)
    {
        const F32 t = std::pow(llclamp(h, 0.f, 1.f), R_SPAWN_BIAS);
        return mix(R_SPAWN_MIN, R_SPAWN_MAX, t);
    }

    // Release: one-way, and driven by size alone. Invariants: false below R_MOVE, true at or above it; pure.
    inline bool canRun(F32 r) { return r >= R_MOVE; }

    // The terminal speed a drop of this radius falls at, as a POSITIVE magnitude. Invariants: > 0 for r > 0; == SPEED_REF_PER_S at
    // r == R_MOVE; strictly increasing in r; pure.
    inline F32 runSpeed(F32 r)
    {
        return SPEED_REF_PER_S * std::pow(llmax(r, 0.f) / R_MOVE, SPEED_POW);
    }

    // Do these two touch? Invariants: symmetric; false at any separation above MERGE_REACH * (ra + rb); true at zero separation.
    inline bool touches(F32 ax, F32 ay, F32 ar, F32 bx, F32 by, F32 br)
    {
        const F32 dx = ax - bx, dy = ay - by;
        const F32 reach = MERGE_REACH * (ar + br);
        return (dx * dx + dy * dy) < (reach * reach);
    }

    // The radius of the drop the two of them become: volume adds, so radius is the cube root of the sum of cubes. Invariants: >= both
    // inputs; symmetric; == r for merging r with 0; strictly increasing in both.
    inline F32 mergeRadius(F32 ra, F32 rb)
    {
        return radiusOf(volumeOf(llmax(ra, 0.f)) + volumeOf(llmax(rb, 0.f)));
    }

    // The stretch a merge imposes, as a fraction of the survivor's radius, along the direction the eaten drop lay in. This is the
    // TENSION: the combined drop is briefly an ellipse pulled toward where its meal was, and relaxes to round over SPREAD_TAU_S. Scaled
    // by the eaten drop's share of the total volume, so swallowing something tiny barely registers and swallowing an equal barely-
    // resolvable twin is the biggest wobble there is. Invariants: 0 when the eaten volume is 0; <= R_SPREAD_MAX; pure.
    inline F32 mergeSpread(F32 rSurvivor, F32 rEaten)
    {
        const F32 vs = volumeOf(llmax(rSurvivor, 0.f)), ve = volumeOf(llmax(rEaten, 0.f));
        const F32 share = (vs + ve > 0.f) ? (ve / (vs + ve)) : 0.f;
        return llmin(MERGE_SPREAD_GAIN * share, R_SPREAD_MAX);
    }

    // Absorb b into a. a keeps its position and identity; volume adds; momentum is mass-weighted (so the result conserves it); and the
    // tension stretch is set along the offset. Invariants: a.mR strictly grows unless b.mR is 0; momentum conserved to float precision;
    // b is left for the caller to remove.
    inline void mergeInto(Drop& a, const Drop& b)
    {
        const F32 va = volumeOf(a.mR), vb = volumeOf(b.mR);
        const F32 vt = va + vb;
        if (vt <= 0.f) return;

        const F32 dx = b.mX - a.mX, dy = b.mY - a.mY;
        const F32 d = std::sqrt(llmax(dx * dx + dy * dy, 1.0e-12f));
        const F32 spread = mergeSpread(a.mR, b.mR);

        a.mVX = (a.mVX * va + b.mVX * vb) / vt;
        a.mVY = (a.mVY * va + b.mVY * vb) / vt;
        a.mR  = radiusOf(vt);
        a.mSpreadX = llclamp(a.mSpreadX + spread * std::fabs(dx) / d, 0.f, R_SPREAD_MAX);
        a.mSpreadY = llclamp(a.mSpreadY + spread * std::fabs(dy) / d, 0.f, R_SPREAD_MAX);
        a.mRunning = a.mRunning || canRun(a.mR);
    }

    // The radius of the drop a running parent pinches off, and what the parent is left with. The volume moves rather than appearing:
    // shedRadius is what leaves, shedParentRadius is the parent afterwards. Invariants: shed < parent always; the two volumes sum to the
    // parent's original; both 0 for a parent at or below SHED_MIN_R.
    inline F32 shedRadius(F32 parentR)
    {
        return (parentR > SHED_MIN_R) ? (parentR * SHED_FRAC) : 0.f;
    }
    inline F32 shedParentRadius(F32 parentR)
    {
        const F32 shed = shedRadius(parentR);
        if (shed <= 0.f) return parentR;
        return radiusOf(llmax(volumeOf(parentR) - volumeOf(shed), 0.f));
    }

    // How far a drop runs between sheds. Invariants: > 0; proportional to its own radius.
    inline F32 shedInterval(F32 r) { return SHED_EVERY_R * llmax(r, 1.0e-5f); }

    // Evaporation over dt, in VOLUME, scaled by the coating (tailMul == clearRadial at the drop's own position: the hydrophobic centre
    // sheds water faster, which is the whole of what that surface does). Invariants: non-increasing; never negative; 0 at dt 0.
    inline F32 evaporate(F32 r, F32 dt, F32 tailMul)
    {
        const F32 loss = EVAP_VOL_PER_S * llmax(dt, 0.f) / llmax(llclamp(tailMul, 0.05f, 1.f), 0.05f);
        return radiusOf(llmax(volumeOf(llmax(r, 0.f)) - loss, 0.f));
    }

    // One drop, one step. Pure in (drop, dt, tailMul, kick): the caller supplies the sideways kick as a hash so this stays free of any
    // runtime RNG. Returns false when the drop has evaporated and should be removed.
    //
    // The order matters and is the reference's: evaporate, then decide whether it can run, then chase the terminal speed, then take the
    // sideways kick and damp it, then move, then relax the tension stretch. A drop that cannot run still evaporates and still relaxes -
    // it is only the motion that is gated.
    // Invariants: a pinned drop's position never changes; a running drop's mVY is <= 0 and its speed approaches runSpeed(r) from below
    // when nothing perturbs it; mSpreadX/Y decay monotonically toward 0 in the absence of merges; false only when mR <= R_DIE.
    inline bool stepDrop(Drop& d, F32 dt, F32 tailMul, F32 kick01)
    {
        if (dt <= 0.f) return d.mR > R_DIE;

        d.mAgeS += dt;
        d.mR = evaporate(d.mR, dt, tailMul);
        if (d.mR <= R_DIE) return false;

        d.mRunning = d.mRunning || canRun(d.mR);

        if (d.mRunning)
        {
            const F32 target = -runSpeed(d.mR);
            const F32 relax = llclamp(dt / VELOCITY_TAU_S, 0.f, 1.f);
            d.mVY += (target - d.mVY) * relax;

            // The jink. Scaled by R_WOBBLE_REF / r so heavy drops are deflected less, and damped hard so each kick is a step sideways
            // rather than a drift that accumulates into a diagonal.
            const F32 gain = R_WOBBLE_REF / llmax(d.mR, 1.0e-5f);
            d.mVX += (kick01 - 0.5f) * 2.f * WOBBLE_PER_S * gain * dt;
            d.mVX -= d.mVX * llclamp(WOBBLE_DAMP_PER_S * dt, 0.f, 1.f);

            const F32 dx = d.mVX * dt, dy = d.mVY * dt;
            d.mX += dx;
            d.mY += dy;
            d.mSinceShed += std::sqrt(dx * dx + dy * dy);
        }

        const F32 decay = 1.f - llclamp(dt / SPREAD_TAU_S, 0.f, 1.f);
        d.mSpreadX *= decay;
        d.mSpreadY *= decay;

        return true;
    }

    // Has this drop run far enough to pinch one off? Invariants: false for a pinned drop; false for a parent at or below SHED_MIN_R.
    inline bool shedNow(const Drop& d)
    {
        return d.mRunning && d.mR > SHED_MIN_R && d.mSinceShed >= shedInterval(d.mR);
    }

    // How many drops the field should be carrying at this wet, before the user's budget. Quadratic in wet, like the previous build's
    // targetRunners: the first drops of a shower are sparse and the last stretch to a soaked lens adds most of them.
    // Invariants: 0 at wet 0; <= budget; monotone in wet.
    inline S32 targetDrops(F32 wet01, S32 budget)
    {
        const F32 w = llclamp(wet01, 0.f, 1.f);
        const S32 want = (S32)std::floor(w * w * (F32)llmin(budget, MAX_DROPS) + 0.5f);
        return llclamp(want, 0, llmin(budget, MAX_DROPS));
    }

    // ================================================================== the coating (kept from R8)

    // <SS:Nexii> R8: THE GEOMETRY IS THE USER'S OWN MODEL - a car windscreen with a hydrophobic coating over the middle of the glass. A
    // hydrophobic coating does not stop water ARRIVING, it stops it CLINGING: drops still land in the centre, they just shed fast rather
    // than never landing. In the R1-R15 build that was expressed as a lifetime multiplier on a lattice cell's cycle; here it is simpler
    // and more literal, because there are real drops to apply it to - it divides into the evaporation rate (evaporate, above), so a drop
    // in the centre of the glass really does dry faster, and it is still not an arrival mask. Sized in the user's own words:
    // "hydrophobic surface in the center ~80% of height... soft gradient of 20% that size". CLEAR_R_IN is HALF of 80% of height (a
    // radius) and the gradient adds 20% of that span. Measured in the ASPECT-CORRECTED frame, so the coating is a CIRCLE and a wide
    // monitor does not distort it. NOT WIRED TO FOV, deliberately: a property of the glass, not of where the camera looks.
    // Invariants: == 1 for any r >= CLEAR_R_OUT; == 1 everywhere at strength 0; monotone NON-DECREASING in r (no ring); in
    // [CLEAR_COVER_MIN, 1]; continuous with continuous first derivative everywhere (it is a smoothstep).
    constexpr F32 CLEAR_R_IN       = 0.40f;
    constexpr F32 CLEAR_R_OUT      = 0.56f;
    constexpr F32 CLEAR_COVER_MIN  = 0.12f;

    inline F32 clearRadial(F32 rStable, F32 strength01)
    {
        const F32 outward = smoothstep(CLEAR_R_IN, CLEAR_R_OUT, rStable);
        return 1.f - llclamp(strength01, 0.f, 1.f) * (1.f - CLEAR_COVER_MIN) * (1.f - outward);
    }

    // The same figure as an amplitude cap, for the condensation layer, which has no per-drop lifetime to shorten - it is a continuous
    // haze, so a hydrophobic surface simply holds proportionately less of it.
    // Invariants: identity at 1; == CLEAR_COVER_MIN at CLEAR_COVER_MIN; in [0,1]; pure.
    inline F32 stippleAmplitude(F32 tailMul) { return llclamp(tailMul, 0.f, 1.f); }

    // ================================================================== the swept channel

    // <SS:Nexii> R17: WHAT A RUNNER LEAVES IN THE CONDENSATION. A drop running down misted glass does not merely sit on the haze, it
    // TAKES it - and the bare channel stays bare well after the drop that cut it has gone off the bottom of the frame. That is the one
    // thing the R16 rewrite lost: the previous build could ask a runner's stored analytic path "were you here, and how long ago", and a
    // texture cannot be asked that. So the answer is a channel MAP - a persistent single-channel buffer that running drops mark and that
    // fades on its own clock, which is a truer model of the thing anyway: the state belongs to the GLASS, not to whichever drop happened
    // to cut it, and once it is the glass's the map keeps working long after that drop is gone.
    //
    // Deliberately NOT the same as the water's own life. Water drains in a couple of seconds; a wiped channel in condensation re-hazes
    // over something much longer, which is why a windscreen keeps showing where the last few drops ran. CLEAR_DECAY_S is that second
    // number, and it is close to the R1-R15 build's TRACK_DECAY_S (26 s) halved - the old figure was chosen to keep a whole screen's
    // worth of headless channel visible at once, which was compensating for a model where nothing else marked a runner's path.
    constexpr F32 CLEAR_DECAY_S    = 12.f;

    // How much of the channel map to take away over dt - an exponential relaxation spelled as the fraction removed, because that is what
    // a blend factor wants. Invariants: 0 at dt 0; in [0,1); monotone in dt; independent of frame rate in the sense that composing two
    // half-steps gives the same survival as one whole one.
    inline F32 clearDecay(F32 dt)
    {
        return 1.f - std::exp(-llmax(dt, 0.f) / CLEAR_DECAY_S);
    }

    // ================================================================== what water looks like (defect R4, kept verbatim)

    constexpr F32 CONTACT_ANGLE_DEG = 84.f;   // the drop's contact angle with the glass. A coated or greasy lens is hydrophobic, 80-100
                                              // degrees; a clean hydrophilic sheet would be 20-40 and would give an almost flat lens with
                                              // no rim at all. 84 makes the rim graze - see rimCos. (82 was the first choice; at 82 the
                                              // rim's Fresnel is 0.483, so reflection was the minority share of the edge and the rim
                                              // still under-read. Measured in tests/lensdropcore.cpp, which is why this is 84.)
    constexpr F32 IOR_WATER         = 1.333f; // so the refraction gain is derived rather than a magic 0.03
    constexpr F32 REFRACT_DEPTH     = 0.072f; // 0.12 * SIZE. Screen-space distance a drop "sees" past itself, before the IOR factor. A
                                              // length, so it scales with SIZE: a smaller lens gathers its image from proportionately
                                              // nearer by, and holding it fixed while the drops shrank would turn them into stronger
                                              // lenses than they were - a look change smuggled in under a size change.
    constexpr F32 DROP_F0           = 0.02f;  // water's normal-incidence reflectance, ((1.333-1)/(1.333+1))^2 = 0.0203
    constexpr F32 EDGE_SOFT         = 0.20f;  // the silhouette fades over this share of r2 - antialiasing, not a shape

    inline F32 sinContact() { return std::sin(CONTACT_ANGLE_DEG * (TAU_F / 360.f)); }

    // THE CAP NORMAL. A sessile drop is a spherical cap meeting the glass at CONTACT_ANGLE_DEG. Parameterise it by q = r / R in [0,1]:
    // the surface normal's tilt from vertical is asin(q sin(theta_c)), so its horizontal component has magnitude q sin(theta_c) and its
    // vertical component sqrt(1 - q^2 sin^2(theta_c)). That collapses to something exact and one line:
    //     n_xy = delta * sin(theta_c),   n_z = sqrt(1 - r2 * sin^2(theta_c))
    // with delta = (p - centre) / R and r2 = dot(delta, delta). No approximation and already unit length.
    // Invariants: n_z in (0,1]; |n_xy| == q * sin(theta_c); n_xy^2 + n_z^2 == 1 within 1e-6 for r2 <= 1; n_z is MINIMAL at the rim;
    // |n_xy| is monotone in q, with no interior peak.
    inline void capNormal(F32 dx, F32 dy, F32& nx, F32& ny, F32& nz)
    {
        const F32 s = sinContact();
        const F32 r2 = llclamp(dx * dx + dy * dy, 0.f, 1.f);
        nx = dx * s;
        ny = dy * s;
        nz = std::sqrt(llmax(1.f - r2 * s * s, 1.0e-6f));
    }

    // The cosine the drop presents AT ITS RIM - the smallest it presents anywhere, and therefore where Fresnel is strongest. Exists so
    // the contact angle can be chosen against a number instead of by eye: at 84 degrees the rim cos is 0.1045 and fresnel() there is
    // 0.584, so a clear majority of the light off a drop's edge is reflection. Invariant: == cos(theta_c).
    inline F32 rimCos() { return std::sqrt(llmax(1.f - sinContact() * sinContact(), 0.f)); }

    // Schlick's Fresnel on the drop's own normal. Invariants: == DROP_F0 at cos 1; == 1 at cos 0; monotone non-increasing; in [F0, 1].
    inline F32 fresnel(F32 cosTheta)
    {
        const F32 c = llclamp(cosTheta, 0.f, 1.f);
        const F32 m = 1.f - c;
        const F32 m2 = m * m;
        return DROP_F0 + (1.f - DROP_F0) * (m2 * m2 * m);
    }

    // The screen-space offset the scene is re-sampled at. Snell at a thin water lens deflects by (1 - 1/n) of the surface tilt to first
    // order, so the gain is a derived 0.25 and REFRACT_DEPTH is the only free number. Invariants: linear in n_xy; the factor is exactly
    // 1 - 1/IOR_WATER.
    inline F32 refractGain() { return REFRACT_DEPTH * (1.f - 1.f / IOR_WATER); }

    // The drop's silhouette, in [0,1].
    //
    // SPELLED ASCENDING ON PURPOSE, and this is a bug the twin caught. The natural spelling is smoothstep(1, 1 - EDGE_SOFT, r2) - a
    // DESCENDING edge - and that is what the shader carried before 2026-09-07. Two things are wrong with it. This core's smoothstep
    // guards e1 <= e0 by returning a hard step, so the descending call silently became `r2 <= 1 ? 0 : 1` - the silhouette INVERTED,
    // every drop drawn as a hole. And in GLSL a descending smoothstep is not defined by the spec at all; it happens to work on the
    // drivers we tried, which is not the same as being correct. `1 - smoothstep(lo, hi, x)` is identical arithmetic, defined
    // everywhere, and identical on both sides.
    // Invariants: 1 at r2 0; 0 at r2 >= 1; monotone non-increasing; C1 at r2 == 1 and at r2 == 1 - EDGE_SOFT.
    inline F32 silhouette(F32 r2) { return 1.f - smoothstep(1.f - EDGE_SOFT, 1.f, r2); }
}

#endif
