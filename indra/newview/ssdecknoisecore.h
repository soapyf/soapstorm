/**
 * @file ssdecknoisecore.h
 * @brief Atmo Magic: the deck noise map's de-tiled presence read - the second incommensurate octave shared by builder, shader and shadow bake. Header-only core. CONTRACT.
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

#ifndef SS_DECKNOISECORE_H
#define SS_DECKNOISECORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_far_clouds.md section 2 step 4 (phase 6b). The deck's noise map tiles every SS_NOISE_TILE_M (2080 m after cell quantisation), so at the 10 km horizon the tower geography repeats ~5 times - puffs hide it (triplanar + skew), the veil hides it (the Penrose 5-way read), but the PRESENCE read that gates cells and the n_map column read do not. The fix is a second read of the SAME map at an incommensurate scale and rotation, mixed into the first: detileCoord(air) = rotate(air, DETILE_ROT) * DETILE_SCALE, presence n = mixDetile(n1, n2). This touches the three-way GATE replication (CPU builder / ss_cell_occupied + the sheet presence read / shadow bake) and the n_map reads, so every function here is GLSL-portable and copied verbatim into ssVolCloudF.glsl; the twin test transliterates the shader back. GATE CONSTANTS RETUNED, v2 (phase 6b calibration, first pass 2026-09-05, corrected same day per review F1): mixing two reads narrows the value distribution (mean preserved, variance 0.545x for w = 0.65 - varianceRatio's own invariant). The first pass matched TAIL SHARE at each hard edge (hole 0.30/0.66, tower 0.37/0.73) and was wrong: every consumer reads the continuous smoothstep through gate = gate_raw + (1-gate_raw)*(1-presence), not a hard cut, so matching only the tail deleted 18-50% of cells that used to be at least partly occupied. The corrected windows (HOLE_LO/HI = 0.264/0.530, TOWER_LO/HI = 0.456/0.722, below) scale the OLD edges about the map's measured mean by sqrt(varianceRatio(DETILE_WEIGHT)) - see retunedEdge() - which preserves OCCUPANCY rather than tail population; ssvolcloud.cpp's SS_HOLE_LO/HI and SS_TOWER_LO/HI carry the measured occupancy table in their own comment, pinned by V:\Scratch\atmo\tests\scenario_detile_occupancy.cpp. Bodies below are the implementer's, not stubs; the invariants are the test author's spec.
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSDeckNoise
{
    constexpr F32 DETILE_SCALE = 0.6180339887f; // 1/phi: the second octave's tile is phi x the first - the most incommensurate ratio, no common period below ~F32 range
    constexpr F32 DETILE_ROT_RAD = 0.6457718f;  // 37 degrees: breaks the axis-aligned repeat too
    constexpr F32 DETILE_WEIGHT = 0.65f;        // share of the FIRST read in the mix; 1.0 reproduces today's single read bit-exactly
    constexpr F32 CELL_M = 260.f;               // LOCKSTEP ssvolcloud.cpp CELL_M

    struct Vec2
    {
        F32 x = 0.f;
        F32 y = 0.f;
    };

    // The second octave's sample coordinate: rotate by DETILE_ROT_RAD then scale by DETILE_SCALE, in AIR metres (the caller
    // divides by the tile as for the first read). GLSL twin: mat2 rotation then multiply. Invariants: length scales by
    // exactly DETILE_SCALE (within 1e-5 relative); the zero vector maps to zero; bit-identical for equal inputs; rotating
    // (1,0) by 37 degrees gives (cos 37, sin 37) * DETILE_SCALE within 1e-6.
    inline Vec2 detileCoord(const Vec2& air)
    {
        static const F32 c = std::cos(DETILE_ROT_RAD);
        static const F32 s = std::sin(DETILE_ROT_RAD);
        Vec2 out;
        out.x = (air.x * c - air.y * s) * DETILE_SCALE;
        out.y = (air.x * s + air.y * c) * DETILE_SCALE;
        return out;
    }

    // The mix: n = DETILE_WEIGHT * n1 + (1 - DETILE_WEIGHT) * n2. Invariants: n1 == n2 returns n1 within one ulp; weight 1
    // would return n1 bit-exactly (documented, not a runtime path); in [0,1] for inputs in [0,1]; monotone in both.
    inline F32 mixDetile(F32 n1, F32 n2)
    {
        return DETILE_WEIGHT * n1 + (1.f - DETILE_WEIGHT) * n2;
    }

    // The variance ratio of the mixed read relative to a single read for two independent equal-variance inputs:
    // w^2 + (1-w)^2. Exposed so the calibration pass can predict the window retune. Invariants: 1 at w = 1; 0.5 at w = 0.5;
    // for DETILE_WEIGHT returns 0.545 within 1e-3.
    inline F32 varianceRatio(F32 w)
    {
        return w * w + (1.f - w) * (1.f - w);
    }

    // <SS:Nexii> Phase 6b retune v2 (2026-09-05, review F1): the first retune pinned TAIL SHARES (the population
    // with n <= lo or n >= hi) to reproduce the old single-read population at each hard edge - but nothing downstream
    // reads a hard edge. The builder (and every replicated site) consumes the CONTINUOUS smoothstep through
    // gate = gate_raw + (1 - gate_raw) * (1 - presence), so a cell can be partially cut without crossing either
    // edge at all. Matching only the tail-share moved the whole ramp toward the mean and deleted 18-50% of cells
    // that used to be at least partly occupied. The correction that preserves OCCUPANCY rather than tail share:
    // scale the OLD edge about the map's MEAN by sqrt(varianceRatio(DETILE_WEIGHT)) - the mix preserves the mean
    // and scales the value's spread by exactly that factor (varianceRatio's own definition), so scaling the
    // threshold the same way about the same mean keeps the fraction of the ramp any given gate value statistically
    // crosses unchanged, not just the count beyond a hard cutoff. See retunedEdge() below.
    constexpr F32 OLD_HOLE_LO = 0.16f;
    constexpr F32 OLD_HOLE_HI = 0.52f;
    constexpr F32 OLD_TOWER_LO = 0.42f;
    constexpr F32 OLD_TOWER_HI = 0.78f;

    // The procedural map's mean value, measured once over the full SS_NOISE_PROC_SIZE x SS_NOISE_PROC_SIZE grid at
    // seed 0x5EED1337 (ssvolcloud.cpp's calibration seed) after the SS_NOISE_GRID=64 box-average cache - not a
    // closed form; tileFbm's spread step (0.5 + (v - 0.5) * 1.9, clamped) has no simple one.
    constexpr F32 MAP_MEAN = 0.5579f;

    // The relationship itself: an old edge scaled about the map mean by sqrt(varianceRatio(w)) reproduces, through
    // the mixed field's narrower spread, the same statistical occupancy the old edge produced through the single
    // read's wider one. GLSL twin: none needed - this runs once, at the four constexpr call sites below; the
    // shader only ever sees the pinned results. Invariants: retunedEdge(mapMean, mapMean) == mapMean; monotone in
    // oldEdge; reduces to oldEdge unchanged when varianceRatio(DETILE_WEIGHT) == 1.
    inline F32 retunedEdge(F32 oldEdge, F32 mapMean)
    {
        return mapMean + (oldEdge - mapMean) * std::sqrt(varianceRatio(DETILE_WEIGHT));
    }

    // Pinned results of retunedEdge(OLD_*, MAP_MEAN). std::sqrt is not constexpr on this project's toolchain, so
    // these are computed literals rather than constexpr calls; V:\Scratch\atmo\tests\decknoisecore.cpp's
    // retuned_edge_literals_match_formula pins each against a live retunedEdge() call within 1e-3, so a change to
    // DETILE_WEIGHT, MAP_MEAN or the OLD_* constants that is not followed by re-deriving these four literals fails
    // that test rather than silently diverging. ssvolcloud.cpp's SS_HOLE_LO/HI and SS_TOWER_LO/HI read these
    // directly; ssVolCloudF.glsl's two hole-window smoothstep literals carry HOLE_LO/HOLE_HI by hand, LOCKSTEP.
    constexpr F32 HOLE_LO = 0.264f;
    constexpr F32 HOLE_HI = 0.530f;
    constexpr F32 TOWER_LO = 0.456f;
    constexpr F32 TOWER_HI = 0.722f;
}

#endif
