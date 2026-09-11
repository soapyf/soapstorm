/**
 * @file ssdeckframecore.h
 * @brief Atmo Magic: the deck's frame transforms - the wind profile's O(z) shear table and the hero storm's bounded local frame shift. Header-only core.
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

#ifndef SS_DECKFRAMECORE_H
#define SS_DECKFRAMECORE_H

// <SS:Nexii> A CORE header (lldefs.h, sswindprofilecore.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_wind_profile.md section 4 and doc/atmo_magic_storm_dynamics.md section 3 ("Storm motion vs cloud drift"). Revised 2026-09-05 after the phase-4 opus review found the first contract unsound; the rules below are the corrected ones and every call site must follow them.
//
// TWO TRANSFORMS ON THE AIR-FRAME SAMPLE COORDINATE:
//  O(z) - the wind profile's bounded shear offset at altitude z. It is a PLACEMENT transform: the CPU draws a puff of column c at altitude z at c + O(z) (placeWorld). Every OBSERVER of a fragment at altitude z therefore UNDOES it - frameAir = air - S - O(z) - for ALL of its pattern reads: the n_map column read (tower window, anvil, floor, fill), the detail octaves, the triplanar skew, the cap band. (The 4b draft that read n_map at gateAir was inverted: it classified a leaned puff by the column it landed over, up to 2.5 km from the one that built it - the exact failure the wind-profile doc names.) The veil SHEET is base-anchored BY DEFINITION - it is the deck floor's veil even though it is drawn 46-106 m above z0 - so every sheet read (presence, gate, mottle) uses gateAir = air - S and never O(z); the shadow bake and precipNoiseAt are base-anchored too and never evaluate O(z) at all. (Stating it as "O(z0) == 0 so it reduces" was wrong for the sheet: at +46..106 m a storm profile already carries 90-210 m of O.) The GPU never evaluates the wind profile (pow/sin/cos differ from libm): the CPU bakes O(z) into an 8-entry altitude table per build - inputs from the PURE profile path (SSAtmoEnvApplier::windProfileAt(track, phase) and the AUTHORED dome height; never cirrusAltitudeMetres(), which reads a per-client setting and the live deck) - and both sides read it with lerpExact, the one explicit lerp formula spelled identically on both sides (GLSL mix() would round differently; twin_deckframe pins the table read bit-exact).
//  S - the hero storm's local frame shift, bounded in DISPLACEMENT (min(HERO_SHIFT_RADIUS_FRAC * radius, HERO_SHIFT_CAP_M), about one cell), not merely in rate. A rate clamp alone let the shift grow to 12 km over a 70-minute life against a 2 km radius, at which point the cells selected before the shift and the content landing after it were disjoint. With the bound, the falloff ring's shear is at most |grad infl| * |S|^2 <= (1.5 / (0.6 r)) * (0.15 r)^2 = 0.056 r (112 m at r = 2000 m, under half a cell) - THAT is the stated per-cell bound.
//
// PRODUCER / OBSERVER RULE (which cell, on which side):
//  The CPU builder is the PRODUCER: for lattice cell c it evaluates S at the UNSHIFTED centre (world c + drift), reads the pattern (gate hash, presence, n_map) at the plain centre c, and places the puff at placeWorld = c + drift + S + O(z_puff). The fragment stage, the shadow bake and precipNoiseAt are OBSERVERS of a world point: air = world - drift; they evaluate S at their own quantized cell (samplePointM) and read EVERYTHING at frameAir(air, z) = air - S - O(z); for the base-anchored observers (sheet, bake, precip) z is the floor and frameAir is gateAir. The shadow bake's CELL loop is a producer mirror (plain-centre read for the plain hashed index); its TEXEL loop is an observer for EVERY read - the cell-occupancy lookup, presence, tower and mottle all take the texel's gateAir (texel -> cell via floor((air - S) / CELL_M)), with the occupancy grid padded by the cap's worth of cells. OBSERVER QUANTIZATION: the observer's storm sample / hero influence is quantized from air - O(z) (O does not depend on influence, so it is removed FIRST), which is the un-leaned column the producer evaluated influence at; quantizing from the leaned air would put a high-altitude puff's sample up to 2.5 km from its producer cell and outside the hero entirely. For a puff placed by the producer the observer's frameAir lands back on cell c (the round trip closes for BOTH S and O(z)), so both sides hash the same cell and read the map at the same centre. Residuals, stated: S differs between producer and observer only through the influence gradient across |S| (the bound above); the fragment's z is its own altitude, not the puff centre's, so within one puff the pattern shears by |dO/dz| x puff radius (up to ~250 m near the anvil at SHEAR_LEAN_S = 600; see the wind-profile doc's tuning note).
//
// HONEST CONSEQUENCE: the visible slide is about one cell over the hero's life; the storm's kilometre-scale motion is carried by the coupling field (tower/anvil/meso modulation moving with the cell centre). A rigid kilometre-scale translation is incompatible with a lattice-hashed field without repopping it. HERO_SHIFT_CAP_M = 0 removes the slide entirely.
#include "lldefs.h"
#include "sswindprofilecore.h"

#include <cmath>
#include <cstdint>

namespace SSDeckFrame
{
    constexpr S32 SHEAR_TABLE_N          = 8;      // LOCKSTEP ssVolCloudF.glsl uniform vec2 ss_shear_table[8]
    constexpr F32 HERO_SLIDE_MAX_MS      = 3.f;    // rate clamp on |v_storm - v_drift| (doc: "a few m/s")
    constexpr F32 HERO_SHIFT_RADIUS_FRAC = 0.15f;  // displacement cap as a share of the hero's influence radius
    constexpr F32 HERO_SHIFT_CAP_M       = 390.f;  // absolute displacement cap (1.5 cells); with RADIUS_MAX_M 2200 the radius term (330 m) binds first, so this only guards a future radius change - and 0 disables the slide.
    constexpr F32 CELL_M                 = 260.f;  // LOCKSTEP ssvolcloud.cpp CELL_M [interaction: SSVolCloud]

    struct Vec2
    {
        F32 x = 0.f;
        F32 y = 0.f;
    };

    // The baked O(z) table: z0 is the deck base (O == 0 there by construction), z1 the highest altitude anything reads the
    // table at (the AUTHORED dome height above the floor - pure), N samples inclusive. Uploaded as ss_shear_z and ss_shear_table.
    struct ShearTable
    {
        F32 z0 = 0.f;
        F32 z1 = 1.f;
        Vec2 o[SHEAR_TABLE_N];
    };

    // The hero's frame as uniforms: storm motion (m/s), the deck's curve-resolved drift velocity at base (m/s), age (s),
    // centre (agent frame) and influence radius. All zero / age 0 when there is no hero.
    struct HeroFrame
    {
        Vec2 motion;
        Vec2 driftVel;
        F32 ageS = 0.f;
        Vec2 centre;
        F32 radius = 0.f;
    };

    // THE one lerp formula both sides use: a + (b - a) * t. GLSL mix() is x * (1 - a) + y * a and rounds differently; the
    // shader spells this form out instead of calling mix(). Invariants: t = 0 -> a bit-exactly; t = 1 -> a + (b - a) (within
    // one ulp of b, NOT bit-exactly b in general - stated, not hidden).
    inline F32 lerpExact(F32 a, F32 b, F32 t) { return a + (b - a) * t; }

    // Bake the table: o[i] = SSWindProfile::shearOffset(z_i - groundZ, baseAgl, p) for z_i = z0 + (z1 - z0) * i / (N - 1),
    // baseAgl = z0 - groundZ. Invariants: o[0] is the zero vector; |o[i]| <= SSWindProfile::SHEAR_CAP_M; z1 < z0 + 1 is clamped
    // to z0 + 1; bit-identical for equal inputs.
    inline ShearTable bakeShearTable(F32 z0, F32 z1, F32 groundZ, const SSWindProfile::Params& p)
    {
        ShearTable tbl;
        tbl.z0 = z0;
        tbl.z1 = (z1 < z0 + 1.f) ? (z0 + 1.f) : z1;
        const F32 baseAgl = z0 - groundZ;
        for (S32 i = 0; i < SHEAR_TABLE_N; ++i)
        {
            const F32 zi = tbl.z0 + (tbl.z1 - tbl.z0) * (F32)i / (F32)(SHEAR_TABLE_N - 1);
            const SSWindProfile::Vec2 o = SSWindProfile::shearOffset(zi - groundZ, baseAgl, p);
            tbl.o[i].x = o.x;
            tbl.o[i].y = o.y;
        }
        return tbl;
    }

    // Read the table at altitude z: t = clamp((z - z0) / (z1 - z0), 0, 1) * (N - 1); i = floor(t) clamped to N - 2;
    // lerpExact(o[i], o[i+1], t - i). This is the ONLY formula the shader mirrors for O(z) (ss_frame_shearTableAt, same
    // clamp/floor order, same lerpExact spelling). Invariants: z <= z0 returns o[0] bit-exactly; z >= z1 returns o[N-1]
    // within one ulp; continuous and piecewise linear in z.
    inline Vec2 shearTableAt(const ShearTable& tbl, F32 z)
    {
        const F32 span = tbl.z1 - tbl.z0;
        F32 t = (span > 0.f) ? (llclamp((z - tbl.z0) / span, 0.f, 1.f) * (F32)(SHEAR_TABLE_N - 1)) : 0.f;
        S32 i = (S32)std::floor(t);
        i = llclamp(i, 0, SHEAR_TABLE_N - 2);
        const F32 frac = t - (F32)i;
        Vec2 v;
        v.x = lerpExact(tbl.o[i].x, tbl.o[i + 1].x, frac);
        v.y = lerpExact(tbl.o[i].y, tbl.o[i + 1].y, frac);
        return v;
    }

    // The displacement cap for a hero of this radius: min(HERO_SHIFT_RADIUS_FRAC * radius, HERO_SHIFT_CAP_M), never negative.
    inline F32 heroShiftCapM(F32 radius)
    {
        return llmax(0.f, llmin(HERO_SHIFT_RADIUS_FRAC * llmax(0.f, radius), HERO_SHIFT_CAP_M));
    }

    // The hero's local frame shift at a point of influence `influence` (the storm field's radial influence of the hero at
    // the quantized cell the caller is working on): d = motion - driftVel clamped to HERO_SLIDE_MAX_MS in magnitude
    // (direction kept); raw = d * ageS * influence; then |raw| is capped at heroShiftCapM(radius) * influence (direction kept)
    // so the shift saturates instead of growing with age. GLSL twin: ss_frame_heroShift, same order. Invariants: zero when
    // ageS is 0, influence is 0, motion == driftVel, or HERO_SHIFT_CAP_M is 0; |shift| <= min(HERO_SLIDE_MAX_MS * ageS,
    // heroShiftCapM(radius)) * influence within 1e-3; parallel to d; monotone non-decreasing in ageS; bit-identical for equal
    // inputs.
    inline Vec2 heroShift(const HeroFrame& h, F32 influence)
    {
        Vec2 d;
        d.x = h.motion.x - h.driftVel.x;
        d.y = h.motion.y - h.driftVel.y;
        const F32 len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len > HERO_SLIDE_MAX_MS)
        {
            const F32 k = HERO_SLIDE_MAX_MS / len;
            d.x *= k;
            d.y *= k;
        }
        Vec2 shift;
        shift.x = d.x * h.ageS;
        shift.y = d.y * h.ageS;
        const F32 mag = std::sqrt(shift.x * shift.x + shift.y * shift.y);
        const F32 cap = heroShiftCapM(h.radius);
        if (mag > cap)
        {
            const F32 k = (mag > 0.f) ? (cap / mag) : 0.f;
            shift.x *= k;
            shift.y *= k;
        }
        shift.x *= influence;
        shift.y *= influence;
        return shift;
    }

    // OBSERVER: the gate / presence / n_map coordinate for a world point whose air-frame position is `air`: air - S.
    // Invariants: S zero returns air bit-exactly.
    inline Vec2 gateAir(const Vec2& air, const Vec2& heroShiftM)
    {
        Vec2 a;
        a.x = air.x - heroShiftM.x;
        a.y = air.y - heroShiftM.y;
        return a;
    }

    // OBSERVER: the pattern coordinate for a fragment at altitude z - EVERY read (n_map column, detail, skew, cap band):
    // gateAir - O(z). The CPU has no per-fragment read and never calls this; it exists so the GLSL twin (ss_frame_frameAir)
    // has a named CPU counterpart for the round-trip twin: placeWorld then frameAir at the puff's altitude must land back on
    // the producer's cell. Invariants: zero table and zero S return air bit-exactly; equals gateAir at z <= z0 when o[0] is
    // zero; frameAir(placeWorld(c, drift, S, z, tbl) - drift, z, tbl, S) == c within one ulp per component.
    inline Vec2 frameAir(const Vec2& air, F32 z, const ShearTable& tbl, const Vec2& heroShiftM)
    {
        const Vec2 g = gateAir(air, heroShiftM);
        const Vec2 o = shearTableAt(tbl, z);
        Vec2 a;
        a.x = g.x - o.x;
        a.y = g.y - o.y;
        return a;
    }

    // PRODUCER: where the CPU draws a puff of lattice cell c (air-frame centre + jitter) at altitude z: c + drift + S + O(z).
    // The O(z) lean lives HERE so no call site adds it by hand. Invariants: zero S and zero table return c + drift bit-exactly;
    // linear in S; the O term equals shearTableAt(tbl, z).
    inline Vec2 placeWorld(const Vec2& cellPointAir, const Vec2& drift, const Vec2& heroShiftM, F32 z, const ShearTable& tbl)
    {
        const Vec2 o = shearTableAt(tbl, z);
        Vec2 w;
        w.x = cellPointAir.x + drift.x + heroShiftM.x + o.x;
        w.y = cellPointAir.y + drift.y + heroShiftM.y + o.y;
        return w;
    }

    // The shadow bake's frame key term. The bake applies no O(z), so the table is NOT folded; what moves its presence reads
    // is the hero's footprint and shift: fold the centre quantised to CELL_M, the radius quantised to CELL_M and the plateau
    // shift quantised to CELL_M / 4. The air/world drift offset is NOT folded here: it reaches the bake key through the
    // existing camera air-cell term (cam_cx/cam_cy), so a camera translating with the wind pins that term and the hero
    // field's freshness then rides on the centre crossing its own CELL_M quantum - bounded and self-correcting, stated.
    // Every observer derives S in ONE step from its own quantized cell (samplePointM of air - O(z), or of air for the
    // base-anchored ones); no observer iterates a fixed point, so the bake, the fragment and precip agree on S exactly
    // wherever their quantized cells agree. Invariants: same inputs -> same key; a centre/shift change below half a quantum from a
    // quantum boundary does not change the key; no hero (ageS <= 0 or radius <= 0) returns the input key unchanged.
    inline U64 foldFrameKey(U64 key, const HeroFrame& h)
    {
        if (h.ageS <= 0.f || h.radius <= 0.f)
        {
            return key;
        }
        const auto fold = [&key](U64 v) { key ^= v + 0x9e3779b97f4a7c15ULL; key *= 1099511628211ULL; };
        const auto quant = [](F32 v, F32 q) -> S32 { return (S32)std::floor(v / q + 0.5f); };
        const Vec2 shift = heroShift(h, 1.f);
        fold((U64)(U32)quant(h.centre.x, CELL_M));
        fold((U64)(U32)quant(h.centre.y, CELL_M));
        fold((U64)(U32)quant(h.radius, CELL_M));
        fold((U64)(U32)quant(shift.x, CELL_M * 0.25f));
        fold((U64)(U32)quant(shift.y, CELL_M * 0.25f));
        return key;
    }
}

#endif
