/**
 * @file ssacousticcore.h
 * @brief Atmo Magic acoustics: the ACOUSTIC channel's pure core - probe placement,
 *        the occlusion DDA, the tier A statistic bake, the probe graph and its
 *        Dijkstra solve, and the tier B stochastic ray bundle.
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

// <SS:Nexii> A CORE header (lldefs.h + <cmath>/<cstdint>/<cstring> + POD containers
// only; NO llmath.h), the split doc/atmo_magic_acoustics.md fixes for the channel:
// placement, linking, the graph solve and the ray bundle live here as pure functions
// of the span snapshot; ssworldfield.cpp owns snapshots, scheduling and storage, the
// same split every other Atmo core follows. Everything is a pure function of its
// arguments so the scratch harness can pin it; nothing here reads a setting, the
// camera or a system. Coordinates: x/y are REGION-LOCAL metres, z is the absolute
// altitude the span store already speaks (the store's columns are region-anchored, so
// a query converts at the shell boundary and the core never sees a region handle).

#ifndef SS_ACOUSTIC_CORE_H
#define SS_ACOUSTIC_CORE_H

#include "lldefs.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace SSAcoustic
{
    // ---- constants (LOCKSTEP notes at each) ----

    // Ear probe height above a gap's floor span: small-avatar ear height, where reverb
    // is heard and sources sit.
    constexpr F32 EAR_HEIGHT_M = 1.2f;
    // Ceiling probe clearance under the gap's roof span.
    constexpr F32 CEILING_DROP_M = 0.4f;
    // Below this gap height the ear and ceiling probes are one probe at the midpoint.
    constexpr F32 MIN_SPLIT_GAP_M = 2.4f;
    // Intermediate probe spacing for tall gaps (atria, canyons, stairwells).
    constexpr F32 INTERMEDIATE_STEP_M = 4.0f;
    // The minimum vertical overlap two neighbouring probes' gaps must share for a
    // horizontal link: the crouch-clearance figure. Below it, air technically connects
    // but sound carrying through it should pay the aperture penalty, not travel freely.
    constexpr F32 LINK_MIN_OVERLAP_M = 0.7f;
    // An aperture of this many metres passes full energy; the overlap clamped against
    // the midpoint clearance is scaled against it. A door-sized opening reads 1, a
    // mail-slot overlap reads the floor.
    constexpr F32 APERTURE_DOOR_M = 2.0f;
    constexpr F32 APERTURE_MIN = 0.05f;
    // Fixed per-portal loss, expressed in metres of equivalent path so it folds into
    // the Dijkstra cost directly.
    constexpr F32 PORTAL_COST_M = 8.0f;
    // How far a wall-profile walk runs before a direction is called open. Above the
    // soundscape's own 50 m side-ray length, so a probe's "open" can never count as a
    // wall hit in its consumers. (The shell's old SS_WF_ACOUSTIC_REACH_M constant
    // retired with the ring lattice; this is the one reach now.)
    constexpr F32 WALL_REACH_M = 64.0f;
    // The room-volume flood's radius cap, in metres of graph distance at lattice
    // resolution.
    constexpr F32 ROOM_RADIUS_M = 32.0f;
    // Absorption per span-flag class - invented, permanent, and stated so (the doc's
    // own adversarial review: nothing in SL content says "curtain" vs "marble"). Water
    // is hard, heightmap terrain soft, built surfaces mid.
    constexpr F32 ABSORB_WATER_M   = 0.06f;
    constexpr F32 ABSORB_TERRAIN_M = 0.35f;
    constexpr F32 ABSORB_SOLID_M   = 0.20f;
    // The bundle's absorption is the solid-class default: a statistic bake over mixed
    // boundaries carries one number, and the per-class split only matters when a room
    // is homogeneous enough for the difference to be audible.
    constexpr F32 BUNDLE_ABSORB = ABSORB_SOLID_M;
    // Speed of sound for the bundle's time figures. The runtime thunder delay uses the
    // weather's own temperature-corrected speed (sssoundscape.cpp speed_of_sound_ms);
    // the bake's RT60 is a room constant, not a weather reading.
    constexpr F32 SPEED_OF_SOUND_MS = 343.0f;
    // A candidate link validated with this much solid in its way is blocked. Above the
    // DDA's own epsilon so a grazing graze through a corner does not cut a legal link.
    constexpr F32 LINK_BLOCK_SOLID_M = 0.05f;
    // The anchor-column spiral's radius, in lattice cells: how far from the cell centre
    // a representative column may sit before the cell is declared probe-less.
    constexpr S32 ANCHOR_SPIRAL_CELLS = 3;
    // Tier B defaults (the dials the doc names): rays per probe, bounce cap, and the
    // span-mip coarsening factor (0.25 m columns trace reverb statistics at 1 m).
    constexpr S32 BUNDLE_RAYS = 128;
    constexpr S32 BUNDLE_BOUNCES = 8;
    constexpr S32 BUNDLE_MIP = 4;
    constexpr F32 BUNDLE_MAX_DIST_M = 64.0f;
    // First-arrival clustering: first hits within this window of the earliest count as
    // the dominant early reflection (the slap of a facing wall).
    constexpr F32 FIRST_ARRIVAL_WINDOW_S = 0.005f;
    // Echo-density threshold: the share of rays whose FIRST arrival lands later than
    // this. A canyon's slapback reads high, a room's wash low.
    constexpr F32 ECHO_LATE_S = 0.080f;
    // The tier-A wall distances the space/size classification reads: the four
    // cardinals count as walls under the soundscape's own side-ray threshold, so a
    // probe's verdict lands on the same rungs the raycast verdict did.
    // LOCKSTEP sssoundscape.cpp SIDE_RAY_LENGTH (50 m) and its -0.5 fudge.
    constexpr F32 WALL_CLASSIFY_M = 49.5f;

    // ---- the snapshot view ----

    // A read-only view of one tile's span store plus the flood's gap outputs - the
    // everything every core function asks. Arrays are the shell's own (col-major,
    // [k * res * res + col] for spans, [col * (max_spans + 1) + k] for gaps); the core
    // copies nothing.
    struct Snap
    {
        const F32*  mTop = nullptr;        // span tops, NO_SURFACE (-FLT_MAX) where empty
        const F32*  mBottom = nullptr;     // span bottoms
        const U8*   mFlags = nullptr;      // SSRainShadowMap::SURF_* per span
        const U8*   mGapLabel = nullptr;   // SSWorldField::EAirLabel per gap node
        const U16*  mGapDepth = nullptr;   // decimetres of covered travel from the opening, per gap node
        // <SS:Nexii> Optional survey mask, one byte per COLUMN: 0 means no band sheet ever covered that cell, so its empty span list is "not looked at", not "empty sky". Null means the whole grid is surveyed. [interaction: SSWorldField::traceSolid]
        const U8*   mSurveyed = nullptr;
        S32   mRes = 0;
        F32   mCell = 1.f;                 // capture cell size, metres
        F32   mCeiling = 0.f;              // capture ceiling, metres
        S32   mMaxSpans = 6;               // SS_WF_MAX_SPANS
    };

    constexpr F32 NO_SURFACE_F = -3.402823466e+38F;

    // The column's stored span count: slots past it are empty.
    inline S32 spanCount(const Snap& s, size_t col)
    {
        const size_t layer = (size_t)s.mRes * (size_t)s.mRes;
        S32 n = 0;
        while (n < s.mMaxSpans && s.mTop[(size_t)n * layer + col] > NO_SURFACE_F * 0.5f) ++n;
        return n;
    }

    // Whether the point (col, z) is air: the same verdict the shipped lattice's walk
    // used, so a probe's wall walk and the graph's validation ray agree with what the
    // flood labelled.
    inline bool airAt(const Snap& s, size_t col, F32 z)
    {
        const size_t layer = (size_t)s.mRes * (size_t)s.mRes;
        F32 prev = 0.f;
        for (S32 k = 0; k < s.mMaxSpans; ++k)
        {
            const F32 stop = s.mTop[(size_t)k * layer + col];
            if (stop <= NO_SURFACE_F * 0.5f) return true;      // open above the last span
            const F32 bottom = s.mBottom[(size_t)k * layer + col];
            if (z < bottom - 0.01f) return true;               // the gap beneath this span
            if (z <= stop + 0.01f) return false;               // inside the body
            prev = stop;
        }
        return prev < s.mCeiling - 0.01f;                      // above the last span
    }

    // Which air gap of a column contains z (mirrors SSWorldField::gapAt): 0 below the
    // lowest span, n above the highest, -1 inside a body. Bounds of the gap come back.
    inline S32 gapIndexAt(const Snap& s, size_t col, F32 z, F32& g0, F32& g1)
    {
        const size_t layer = (size_t)s.mRes * (size_t)s.mRes;
        F32 prev = 0.f;
        for (S32 k = 0; k < s.mMaxSpans; ++k)
        {
            const size_t si = (size_t)k * layer + col;
            const F32 stop = s.mTop[si];
            if (stop <= NO_SURFACE_F * 0.5f)
            {
                g0 = prev; g1 = s.mCeiling;
                return k;
            }
            const F32 bottom = s.mBottom[si];
            if (z < bottom - 0.01f)
            {
                g0 = prev; g1 = bottom;
                return k;
            }
            if (z <= stop + 0.01f) return -1;
            prev = stop;
        }
        g0 = prev; g1 = s.mCeiling;
        return s.mMaxSpans;
    }

    // ---- the occlusion DDA ----

    struct Trace
    {
        F32 mSolidM = 0.f;    // metres of the segment's 3D length inside solid spans
        S32 mCrossings = 0;   // bodies entered
        bool mUnsurveyed = false;   // the walk crossed a column no sheet covered: the count is a floor, not an answer
    };

    // 2D DDA over the columns the segment a->b crosses; per column, the segment's
    // z-interval within that column is intersected against the column's span
    // intervals; solid metres and body crossings accumulate. Exact against the store,
    // no scene raycast, main thread, ~4 columns per metre at the 0.25 m cell. Returns
    // false when the segment leaves the tile's columns - the store has no verdict past
    // its border, and a partial count would read as confidently open air, the
    // optimistic direction for audio; the caller falls back instead.
    inline bool traceSolid(const Snap& s, const F32 a[3], const F32 b[3], Trace& out)
    {
        out.mSolidM = 0.f;
        out.mCrossings = 0;
        out.mUnsurveyed = false;
        if (s.mRes < 1 || s.mCell <= 0.f) return false;

        const F32 width = (F32)s.mRes * s.mCell;
        // Both endpoints inside the tile's columns (z unbounded - the store spans
        // 0..ceiling and a z outside it simply intersects nothing).
        if (a[0] < 0.f || a[0] >= width || a[1] < 0.f || a[1] >= width) return false;
        if (b[0] < 0.f || b[0] >= width || b[1] < 0.f || b[1] >= width) return false;

        const F32 dx = b[0] - a[0];
        const F32 dy = b[1] - a[1];
        const F32 dz = b[2] - a[2];
        const F32 seg_len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (seg_len < 1.0e-5f) return true;
        const F32 len_xy = sqrtf(dx * dx + dy * dy);

        const S32 cx0 = llclamp((S32)(a[0] / s.mCell), 0, s.mRes - 1);
        const S32 cy0 = llclamp((S32)(a[1] / s.mCell), 0, s.mRes - 1);

        const size_t layer = (size_t)s.mRes * (size_t)s.mRes;

        // Zero horizontal extent: one column, the z-interval is the whole segment.
        if (len_xy < 1.0e-5f)
        {
            const size_t col = (size_t)cy0 * (size_t)s.mRes + (size_t)cx0;
            if (s.mSurveyed && !s.mSurveyed[col]) out.mUnsurveyed = true;
            const F32 zlo = llmin(a[2], b[2]);
            const F32 zhi = llmax(a[2], b[2]);
            const S32 n = spanCount(s, col);
            for (S32 k = 0; k < n; ++k)
            {
                const size_t si = (size_t)k * layer + col;
                const F32 lo = llmax(zlo, s.mBottom[si]);
                const F32 hi = llmin(zhi, s.mTop[si]);
                if (hi - lo > 0.01f)
                {
                    out.mSolidM += hi - lo;
                    ++out.mCrossings;
                }
            }
            return true;
        }

        // Amanatides & Woo over the column grid. t is the segment parameter in [0,1].
        S32 cx = cx0, cy = cy0;
        const S32 step_x = (dx > 0.f) ? 1 : (dx < 0.f ? -1 : 0);
        const S32 step_y = (dy > 0.f) ? 1 : (dy < 0.f ? -1 : 0);
        const F32 t_delta_x = (step_x != 0) ? s.mCell / fabsf(dx) : FLT_MAX;
        const F32 t_delta_y = (step_y != 0) ? s.mCell / fabsf(dy) : FLT_MAX;
        F32 t_max_x = (step_x != 0)
            ? ((step_x > 0 ? ((F32)(cx + 1) * s.mCell - a[0]) : (a[0] - (F32)cx * s.mCell)) / fabsf(dx))
            : FLT_MAX;
        F32 t_max_y = (step_y != 0)
            ? ((step_y > 0 ? ((F32)(cy + 1) * s.mCell - a[1]) : (a[1] - (F32)cy * s.mCell)) / fabsf(dy))
            : FLT_MAX;

        const S32 max_steps = s.mRes * 2 + 4;
        F32 t_enter = 0.f;

        for (S32 step = 0; step <= max_steps; ++step)
        {
            F32 t_exit = llmin(t_max_x, t_max_y, 1.f);

            const size_t col = (size_t)cy * (size_t)s.mRes + (size_t)cx;
            if (s.mSurveyed && !s.mSurveyed[col]) out.mUnsurveyed = true;
            // The segment's z-interval while inside this column. A horizontal
            // segment's interval is a POINT - and a point inside a body means the
            // body holds the whole horizontal run, which a naive z-overlap test
            // (point intervals overlap a span by zero metres) would miss entirely.
            const F32 z0 = a[2] + dz * t_enter;
            const F32 z1 = a[2] + dz * t_exit;
            const F32 zlo = llmin(z0, z1);
            const F32 zhi = llmax(z0, z1);
            const F32 z_range = zhi - zlo;

            const S32 n = spanCount(s, col);
            for (S32 k = 0; k < n; ++k)
            {
                const size_t si = (size_t)k * layer + col;
                const F32 sb = s.mBottom[si];
                const F32 st = s.mTop[si];
                bool in_body = false;
                F32 frac = 0.f;
                if (z_range < 0.01f)
                {
                    // Horizontal (or flat): inside iff the body spans the altitude.
                    in_body = (sb + 0.01f < zlo && zlo < st - 0.01f);
                    frac = in_body ? 1.f : 0.f;
                }
                else
                {
                    const F32 lo = llmax(zlo, sb);
                    const F32 hi = llmin(zhi, st);
                    if (hi - lo > 0.01f)
                    {
                        in_body = true;
                        frac = (hi - lo) / z_range;
                    }
                }
                if (in_body)
                {
                    // The segment's in-column 3D run, scaled by the share of its
                    // z-interval the body covers - the true metres inside solid.
                    out.mSolidM += (t_exit - t_enter) * seg_len * frac;
                    ++out.mCrossings;
                }
            }

            if (t_exit >= 1.f) break;
            t_enter = t_exit;
            if (t_max_x < t_max_y)
            {
                cx += step_x;
                t_max_x += t_delta_x;
                if (cx < 0 || cx >= s.mRes) break;
            }
            else
            {
                cy += step_y;
                t_max_y += t_delta_y;
                if (cy < 0 || cy >= s.mRes) break;
            }
        }

        return true;
    }

    // Wall distance from (x, y) at height z walking direction (dx, dy), unit length:
    // capture-column steps until solid, saturated at max_m. Off-tile reads as OPEN at
    // the cap - the shipped lattice's own convention, "not a nearby wall" - so a tile
    // boundary is never mistaken for a wall and a probe's open can never count as a
    // wall hit in its consumers. The one walk everything directional (profiles, link
    // clearances) composes.
    inline F32 wallDistDir(const Snap& s, F32 x, F32 y, F32 z, F32 dx, F32 dy, F32 max_m)
    {
        const S32 reach = llclamp((S32)(llmin(max_m, WALL_REACH_M) / s.mCell), 1, s.mRes);
        F32 px = x, py = y;
        for (S32 i = 0; i < reach; ++i)
        {
            px += dx * s.mCell;
            py += dy * s.mCell;
            if (px < 0.f || py < 0.f || px >= (F32)s.mRes * s.mCell || py >= (F32)s.mRes * s.mCell)
            {
                return max_m;   // off-tile: open, not a nearby wall
            }
            const size_t col = (size_t)(py / s.mCell) * (size_t)s.mRes + (size_t)(px / s.mCell);
            // <SS:Nexii> A column nobody surveyed has no spans, so airAt calls it open
            // and the ray would run to the reach cap over geometry the field has never
            // seen - which is what inflates a probe's room volume and its Sabine RT60.
            // Stop there instead: "we do not know" is far closer to a wall than to
            // 64 m of clear air. [interaction: SSWorldField's survey mask]
            if (s.mSurveyed && !s.mSurveyed[col]) return (F32)i * s.mCell;
            if (!airAt(s, col, z)) return (F32)i * s.mCell;
        }
        return max_m;
    }

    // ---- probe placement ----

    struct ProbeDef
    {
        F32 mZ = 0.f;
        F32 mGapBottom = 0.f;
        F32 mGapTop = 0.f;
        bool mRoofed = false;
        bool mSkyOpen = false;
    };

    // The probes one air gap carries: an ear probe at floor + 1.2 m, a ceiling probe
    // at roof - 0.4 m when the gap is roofed and tall enough that the two are
    // distinct, intermediates every ~4 m between, or a single midpoint probe for short
    // gaps. The sky-open top gap gets the ear probe only (outdoors air needs no
    // vertical stack - its reverb is the outdoor ambience - but the ear probe must exist so
    // outdoor sources and listeners have a graph entry point). Returns the count
    // written, 0 when the gap is too short to hold even a midpoint probe.
    inline S32 placeGapProbes(F32 g0, F32 g1, bool sky_open, ProbeDef* out, S32 max_out)
    {
        const F32 h = g1 - g0;
        if (h < EAR_HEIGHT_M * 0.75f || max_out < 1) return 0;

        if (!sky_open && h > MIN_SPLIT_GAP_M)
        {
            const F32 ear = g0 + EAR_HEIGHT_M;
            const F32 ceil_p = g1 - CEILING_DROP_M;
            if (ear < ceil_p - 0.1f)
            {
                S32 n = 0;
                out[n].mZ = ear; out[n].mGapBottom = g0; out[n].mGapTop = g1;
                out[n].mRoofed = true; out[n].mSkyOpen = false;
                ++n;
                // Intermediates every ~4 m between ear and ceiling, so a vertical
                // path is never a single 20 m hop through a wall the graph cannot
                // see. Capped at max_out - 1 so the ceiling probe below always
                // fits: a gap tall enough to fill the budget (~67 m) must not
                // write past it.
                for (F32 z = ear + INTERMEDIATE_STEP_M; z < ceil_p - 1.0f && n < max_out - 1; z += INTERMEDIATE_STEP_M)
                {
                    out[n].mZ = z; out[n].mGapBottom = g0; out[n].mGapTop = g1;
                    out[n].mRoofed = true; out[n].mSkyOpen = false;
                    ++n;
                }
                out[n].mZ = ceil_p; out[n].mGapBottom = g0; out[n].mGapTop = g1;
                out[n].mRoofed = true; out[n].mSkyOpen = false;
                ++n;
                return n;
            }
        }

        // Short gap, or the sky-open top: one probe. A short roofed gap puts it at the
        // midpoint (serves ear and ceiling both); the sky-open gap anchors at ear
        // height off the gap's floor, clamped into the gap.
        F32 z = sky_open ? (g0 + EAR_HEIGHT_M) : (g0 + g1) * 0.5f;
        z = llclamp(z, g0 + 0.1f, g1 - 0.1f);
        out[0].mZ = z; out[0].mGapBottom = g0; out[0].mGapTop = g1;
        out[0].mRoofed = !sky_open;
        out[0].mSkyOpen = sky_open;
        return 1;
    }

    // Whether the column carries at least one gap tall enough to anchor a probe stack
    // (air at ear height): the anchor-column spiral's acceptance test. A centre column
    // landing inside a pillar poisons the whole cell's probe stack; this is what lets
    // the spiral step to a neighbour instead, and a cell with no representative column
    // stays probe-less so its neighbours' interpolation covers it rather than storing
    // a pillar's answer.
    inline bool columnHasEarAir(const Snap& s, size_t col)
    {
        // An unsurveyed column has an empty span list, which would otherwise read as
        // one clear gap from the floor to the ceiling and look like the best anchor in
        // the neighbourhood.
        if (s.mSurveyed && !s.mSurveyed[col]) return false;
        const size_t layer = (size_t)s.mRes * (size_t)s.mRes;
        F32 prev = 0.f;
        for (S32 k = 0; k < s.mMaxSpans; ++k)
        {
            const size_t si = (size_t)k * layer + col;
            const F32 stop = s.mTop[si];
            if (stop <= NO_SURFACE_F * 0.5f)
            {
                return (s.mCeiling - prev) >= EAR_HEIGHT_M * 0.75f;
            }
            if (stop - prev >= EAR_HEIGHT_M * 0.75f) return true;
            prev = stop;
        }
        return (s.mCeiling - prev) >= EAR_HEIGHT_M * 0.75f;
    }

    // The anchor spiral: candidate column offsets within ANCHOR_SPIRAL_CELLS of the
    // cell centre, ordered by ring then by angle, so the first accepted candidate is
    // the nearest representative. Writes at most (2R+1)^2 pairs, returns the count.
    inline S32 anchorSpiral(S32* out_dx, S32* out_dy)
    {
        const S32 R = ANCHOR_SPIRAL_CELLS;
        S32 n = 0;
        for (S32 ring = 0; ring <= R; ++ring)
        {
            if (ring == 0)
            {
                out_dx[n] = 0; out_dy[n] = 0; ++n;
                continue;
            }
            // Walk the ring's perimeter clockwise from its top-left corner.
            static const S32 DX[4] = { 1, 0, -1, 0 };
            static const S32 DY[4] = { 0, 1, 0, -1 };
            S32 x = -ring, y = -ring;
            for (S32 side = 0; side < 4; ++side)
            {
                const S32 len = 2 * ring;
                for (S32 i = 0; i < len; ++i)
                {
                    out_dx[n] = x; out_dy[n] = y; ++n;
                    x += DX[side]; y += DY[side];
                }
            }
        }
        return n;
    }

    // ---- tier A: the statistic bake ----

    struct Stats
    {
        F32 mWall[8];          // 8 horizontal directions, metres, saturated at WALL_REACH_M
        F32 mSkyOpen = 0.f;    // 1 when nothing stands over the gap in this column
        F32 mVolume = 0.f;     // m^3, the bounded room flood
        F32 mArea = 0.f;       // m^2, the flood's boundary
        F32 mRT60 = 0.f;       // Sabine over the flood's V and S
        S32 mSpaceClass = 0;   // 0 outdoors / 1 sheltered / 2 small / 3 medium / 4 big
        S32 mSizeClass = 0;    // 0 tight / 1 medium / 2 open
        F32 mTravelOutdoorsM = -1.f; // the flood's gap depth x cell size; -1 unreached
    };

    // The 8-direction wall profile at the probe's real z, walking the capture columns
    // from (x, y) until solid stops each direction - the 4 side-raycasts' answer, per
    // probe, at bake. Open directions read the reach cap.
    inline void wallProfile(const Snap& s, F32 x, F32 y, F32 z, F32 out[8])
    {
        static const F32 DX[8] = { 1.f, 0.70710678f, 0.f, -0.70710678f, -1.f, -0.70710678f, 0.f, 0.70710678f };
        static const F32 DY[8] = { 0.f, 0.70710678f, 1.f, 0.70710678f, 0.f, -0.70710678f, -1.f, 0.70710678f };
        for (S32 d = 0; d < 8; ++d)
        {
            out[d] = wallDistDir(s, x, y, z, DX[d], DY[d], WALL_REACH_M);
        }
    }

    // Sky openness from the column stack above the probe's gap: 1 when nothing stands
    // over the gap in this column, 0 under a roof. The exact-for-the-column half of
    // the 3 up-rays' answer; the horizontal half is the wall profile's business.
    inline F32 skyOpenness(const Snap& s, size_t col, S32 gap)
    {
        return (gap >= spanCount(s, col)) ? 1.f : 0.f;
    }

    // Sabine RT60 from the flood's V and S with the solid-class absorption - one
    // material until span flags grow absorption classes (the doc's stated guess).
    inline F32 sabineRT60(F32 volume, F32 area)
    {
        return 0.161f * volume / (llmax(area, 1.f) * ABSORB_SOLID_M);
    }

    // The space/size classes a probe's bake stores, from its wall profile and air
    // state - the same rungs the soundscape's raycast classification uses, so a
    // probe's verdict lands on the enum the ambient loops already read.
    // LOCKSTEP sssoundscape.cpp updateProbes (SMALL_SPACE_AVG 10, MEDIUM_SPACE_AVG 30,
    // SIDE_RAY_LENGTH 50) and SSSoundscape::ESpace/ESize's own order.
    // label mirrors SSWorldField::EAirLabel (0 solid, 1 outdoors, 2 sheltered,
    // 3 interior, 4 unknown) - a mirrored int, the idiom the other cores use.
    inline void classify(bool roofed, U8 label, const F32 wall[8],
                         S32& space_class, S32& size_class)
    {
        const F32 avg = (wall[0] + wall[2] + wall[4] + wall[6]) * 0.25f;
        S32 walls = 0;
        for (S32 i = 0; i < 8; i += 2)
        {
            if (wall[i] < WALL_CLASSIFY_M) ++walls;
        }
        size_class = (avg < 10.f) ? 0 : (avg < 30.f) ? 1 : 2;

        if (label == 1 && !roofed)          // outdoors air, open above
        {
            space_class = 0;                // SPACE_OUTDOOR
        }
        else if (walls <= 1)
        {
            space_class = 1;                // SPACE_SHELTERED
        }
        else if (walls >= 3 && avg < 10.f)
        {
            space_class = 2;                // SPACE_SMALL
        }
        else if (avg < 30.f)
        {
            space_class = 3;                // SPACE_MEDIUM
        }
        else
        {
            space_class = 4;                // SPACE_BIG
        }
    }

    // ---- the probe ----

    struct Probe
    {
        // Placement.
        F32 mX = 0.f, mY = 0.f, mZ = 0.f;    // region-local x/y, absolute z
        F32 mGapBottom = 0.f, mGapTop = 0.f;
        S32 mCell = 0;                        // lattice cell
        S32 mGap = 0;
        U8  mLabel = 4;                       // AIR_UNKNOWN
        U8  mRoofed = 0;
        U16 mGapDepth = 0xFFFF;               // the classification's covered distance from the opening, DECIMETRES (0xFFFF = never reached)

        // Tier A.
        F32 mWall[8];
        F32 mSkyOpen = 0.f;
        F32 mVolume = 0.f;
        F32 mArea = 0.f;
        F32 mRT60 = 0.f;
        S32 mSpaceClass = 0;
        S32 mSizeClass = 0;
        F32 mTravelM = -1.f;

        // Tier B (written later, serial-gated store-back per batch).
        F32 mMFP = 0.f;
        F32 mEcho = 0.f;
        F32 mFirstDelay = 0.f;
        F32 mFirstDir[3];
        F32 mOpenness[8];
        U8  mHaveBundle = 0;

        Probe()
        {
            for (S32 i = 0; i < 8; ++i) { mWall[i] = WALL_REACH_M; mOpenness[i] = 0.f; }
            mFirstDir[0] = mFirstDir[1] = 0.f; mFirstDir[2] = 1.f;
        }
    };

    // ---- the lattice-scale gap graph (the room flood's node space) ----

    // One anchor column's gaps at lattice resolution: per lattice cell, up to
    // max_spans+1 gaps of that cell's anchor column (or none where the cell is
    // probe-less). The room flood walks these - a room volume at 8 m resolution is the
    // right scale for reverb, and a capture-column flood at 0.25 m cells would cost
    // ~50k nodes per probe per flood.
    struct LatGaps
    {
        S32 mLatRes = 0;
        S32 mMaxSpans = 6;
        std::vector<F32> mB0;      // [cell * (max_spans+1) + k]
        std::vector<F32> mB1;
        std::vector<S32> mSpanN;   // the anchor column's span count, for the roof test
        std::vector<size_t> mCol;  // the anchor column per cell (or SIZE_MAX when none)

        void build(S32 lat_res, S32 max_spans)
        {
            mLatRes = lat_res;
            mMaxSpans = max_spans;
            const size_t layer = (size_t)lat_res * (size_t)lat_res;
            const size_t per = (size_t)max_spans + 1;
            mB0.assign(layer * per, 0.f);
            mB1.assign(layer * per, 0.f);
            mSpanN.assign(layer, 0);
            mCol.assign(layer, ~(size_t)0);
        }
        inline bool exists(S32 cell, S32 k) const
        {
            const size_t i = (size_t)cell * (size_t)(mMaxSpans + 1) + (size_t)k;
            return mB1[i] > mB0[i] + 0.05f;
        }
        inline bool existsNode(S32 node) const
        {
            const S32 per = mMaxSpans + 1;
            return exists(node / per, node % per);
        }
    };

    // Walks one anchor column's gaps into the lattice gap table. g0 of gap 0 is the
    // world floor; the top gap runs to the capture ceiling.
    inline void fillLatGaps(const Snap& s, size_t col, S32 cell, LatGaps& out)
    {
        const size_t per = (size_t)s.mMaxSpans + 1;
        const size_t base = (size_t)cell * per;
        const S32 n = spanCount(s, col);
        out.mSpanN[cell] = n;
        out.mCol[cell] = col;
        F32 prev = 0.f;
        for (S32 k = 0; k <= s.mMaxSpans; ++k)
        {
            if (k > n) break;
            out.mB0[base + (size_t)k] = prev;
            if (k < n)
            {
                const size_t layer = (size_t)s.mRes * (size_t)s.mRes;
                out.mB1[base + (size_t)k] = s.mBottom[(size_t)k * layer + col];
                prev = s.mTop[(size_t)k * layer + col];
            }
            else
            {
                out.mB1[base + (size_t)k] = s.mCeiling;
            }
        }
    }

    // The bounded room flood from one lattice gap node: air-cell volume and boundary
    // area over the connected lattice gaps within ROOM_RADIUS_M of graph distance,
    // giving Sabine's V and S at lattice resolution. Floors count always (the world
    // floor is a surface - terrain, soft - even under the open sky's top gap it is the
    // ground the room stands on); roofs count where a span stands over the gap; walls
    // count where a neighbour column has no overlapping gap in the flood, over the
    // probe gap's own height. Faces shared with a connected neighbour inside the
    // radius count from neither side.
    inline void roomEstimate(const Snap& s, const LatGaps& lg, S32 start_cell, S32 start_gap,
                             F32& volume, F32& area)
    {
        volume = 0.f;
        area = 0.f;
        const S32 per = s.mMaxSpans + 1;
        const S32 res = lg.mLatRes;
        if (res < 1 || !lg.exists(start_cell, start_gap)) return;
        const S32 start = start_cell * per + start_gap;

        const F32 step_m = (s.mRes > 0 && res > 0)
            ? s.mCell * (F32)s.mRes / (F32)res : 8.f;
        const S32 max_steps = (S32)(ROOM_RADIUS_M / llmax(step_m, 1.f)) + 1;

        const size_t nodes = (size_t)res * (size_t)res * (size_t)per;
        std::vector<U8> seen(nodes, 0);
        std::vector<S32> queue;
        std::vector<S32> depth;
        queue.reserve(64);
        depth.reserve(64);
        queue.push_back(start);
        depth.push_back(0);
        seen[start] = 1;

        static const S32 DX[4] = { 1, -1, 0, 0 };
        static const S32 DY[4] = { 0, 0, 1, -1 };

        F32 v = 0.f, a = 0.f;
        for (size_t head = 0; head < queue.size(); ++head)
        {
            const S32 node = queue[head];
            const S32 d = depth[head];
            const S32 cell = node / per;
            const S32 k = node % per;
            const S32 lx = cell % res;
            const S32 ly = cell / res;

            const F32 g0 = lg.mB0[(size_t)node];
            const F32 g1 = lg.mB1[(size_t)node];
            const F32 h = g1 - g0;
            v += step_m * step_m * h;

            a += step_m * step_m;                       // floor
            if (k < lg.mSpanN[cell]) a += step_m * step_m;   // roof

            for (S32 dir = 0; dir < 4; ++dir)
            {
                const S32 nx = lx + DX[dir];
                const S32 ny = ly + DY[dir];
                const bool inside = (nx >= 0 && ny >= 0 && nx < res && ny < res);

                // Best-overlap connected gap of that neighbour, if any.
                S32 best_k = -1;
                F32 best_ov = 0.f;
                if (inside)
                {
                    const S32 ncell = ny * res + nx;
                    for (S32 kj = 0; kj <= lg.mMaxSpans; ++kj)
                    {
                        if (!lg.exists(ncell, kj)) continue;
                        const size_t ni = (size_t)ncell * (size_t)per + (size_t)kj;
                        const F32 ov = llmin(g1, lg.mB1[ni]) - llmax(g0, lg.mB0[ni]);
                        if (ov > 0.05f && ov > best_ov) { best_ov = ov; best_k = kj; }
                    }
                }

                if (best_k < 0)
                {
                    a += h * step_m;                    // wall over the gap's height
                }
                else
                {
                    const S32 ni = (ny * res + nx) * per + best_k;
                    if (seen[ni]) continue;             // open face into visited air
                    if (d + 1 > max_steps) { a += h * step_m; continue; }  // radius ran out
                    seen[ni] = 1;
                    queue.push_back(ni);
                    depth.push_back(d + 1);
                }
            }
        }

        volume = v;
        area = llmax(a, 1.f);
    }

    // ---- the graph ----

    struct Link
    {
        S32 mA = -1;
        S32 mB = -1;
        F32 mLen = 0.f;       // 3D metres, probe to probe
        F32 mAperture = 1.f;  // the overlap clamped against the midpoint clearance
        U8  mPortal = 0;      // crosses the OUTDOORS boundary
    };

    // Vertical overlap height of two probes' gaps (the crouch figure decides).
    inline F32 overlapHeight(const Probe& a, const Probe& b)
    {
        return llmin(a.mGapTop, b.mGapTop) - llmax(a.mGapBottom, b.mGapBottom);
    }
    inline bool gapsOverlap(const Probe& a, const Probe& b)
    {
        return overlapHeight(a, b) >= LINK_MIN_OVERLAP_M;
    }

    // Aperture: the vertical overlap clamped against a horizontal clearance sample at
    // the link's midpoint (nearest wall distance perpendicular to the link, capped at
    // the lattice cell - a wider clearance than the cell cannot widen the aperture
    // past the overlap that defines it). A door-sized aperture passes nearly full
    // energy; a mail-slot overlap is a heavy penalty.
    inline F32 apertureOf(const Probe& a, const Probe& b, F32 clearance_perp)
    {
        const F32 overlap = overlapHeight(a, b);
        const F32 eff = llmin(overlap, llmax(clearance_perp, 0.f));
        return llclamp(eff / APERTURE_DOOR_M, APERTURE_MIN, 1.f);
    }

    // The directed edge cost a Dijkstra walk pays: length divided by aperture, plus
    // the fixed portal loss.
    inline F32 linkCost(const Link& l)
    {
        return l.mLen / llmax(l.mAperture, APERTURE_MIN) + (l.mPortal ? PORTAL_COST_M : 0.f);
    }

    // ---- the solve ----

    // Dijkstra over the CSR adjacency (adj_start[n+1], adj_node[2E], adj_cost[2E]),
    // early exit at dst. Fills parent (the source's parent is itself) and dist;
    // returns whether dst was reached. Binary heap on a local vector - 4k nodes solve
    // in well under a millisecond, and the scratch never outlives the call.
    inline bool dijkstra(S32 n, const S32* adj_start, const S32* adj_node, const F32* adj_cost,
                         S32 src, S32 dst, S32* parent, F32* dist)
    {
        const F32 INF = 3.4e38f;
        for (S32 i = 0; i < n; ++i) { dist[i] = INF; parent[i] = -1; }
        if (n < 1 || src < 0 || src >= n || dst < 0 || dst >= n) return false;

        std::vector<std::pair<F32, S32> > heap;
        heap.reserve(256);
        auto push = [&heap](F32 d, S32 v)
        {
            heap.emplace_back(d, v);
            size_t i = heap.size() - 1;
            while (i > 0)
            {
                const size_t p = (i - 1) / 2;
                if (heap[p].first <= heap[i].first) break;
                std::swap(heap[p], heap[i]);
                i = p;
            }
        };
        auto pop = [&heap](F32& d, S32& v)
        {
            d = heap.front().first;
            v = heap.front().second;
            heap.front() = heap.back();
            heap.pop_back();
            const size_t n_h = heap.size();
            size_t i = 0;
            while (true)
            {
                const size_t l = i * 2 + 1;
                const size_t r = l + 1;
                size_t m = i;
                if (l < n_h && heap[l].first < heap[m].first) m = l;
                if (r < n_h && heap[r].first < heap[m].first) m = r;
                if (m == i) break;
                std::swap(heap[m], heap[i]);
                i = m;
            }
        };

        dist[src] = 0.f;
        parent[src] = src;
        push(0.f, src);

        while (!heap.empty())
        {
            F32 d; S32 v;
            pop(d, v);
            if (d > dist[v]) continue;      // stale entry
            if (v == dst) return true;

            for (S32 e = adj_start[v]; e < adj_start[v + 1]; ++e)
            {
                const S32 u = adj_node[e];
                const F32 nd = d + adj_cost[e];
                if (nd < dist[u])
                {
                    dist[u] = nd;
                    parent[u] = v;
                    push(nd, u);
                }
            }
        }
        return false;
    }

    // ---- tier B: the stochastic ray bundle ----

    struct Bundle
    {
        F32 mMFP = 0.f;          // mean free path: the honest 4V/S without the shoebox
        F32 mRT60 = 0.f;         // decay-curve RT60 from per-bounce absorption
        F32 mEcho = 0.f;         // share of rays whose first arrival is later than ECHO_LATE_S
        F32 mFirstDelay = 0.f;   // the dominant early reflection's delay
        F32 mFirstDir[3];
        F32 mOpenness[8];        // share of a ray's first-heading octant that escaped to sky
        Bundle()
        {
            mFirstDir[0] = mFirstDir[1] = 0.f; mFirstDir[2] = 1.f;
            for (S32 i = 0; i < 8; ++i) mOpenness[i] = 0.f;
        }
    };

    // Deterministic per-probe RNG (xorshift32), seeded from the probe's position bits
    // so a re-bake of an unchanged probe reproduces its bundle.
    inline U32 bundleSeed(F32 x, F32 y, F32 z)
    {
        U32 h = 0x9e3779b9u;
        const F32 c[3] = { x, y, z };
        for (S32 i = 0; i < 3; ++i)
        {
            U32 v = 0;
            std::memcpy(&v, &c[i], sizeof(U32));   // bit-mix the float, not its truncation
            h ^= v;
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        }
        return h ? h : 0x1234567u;
    }

    inline U32 bundleNext(U32& rng)
    {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    }
    inline F32 bundleRand01(U32& rng)
    {
        return (F32)(bundleNext(rng) & 0x00FFFFFFu) / (F32)0x01000000u;
    }

    // K rays, up to `bounces` specular-ish bounces off the store's axis-aligned span
    // faces (jittered - the doc's own fix for raster bias in domed halls). Collected:
    // mean free path, the decay-curve RT60, echo density, the first-arrival profile
    // and per-octant sky openness. A probe of a domed or faceted hall inherits raster
    // bias; a parameter bake accepts it (Project Acoustics itself bakes on voxels).
    inline void traceBundle(const Snap& s, const Probe& p, S32 rays, S32 bounces, Bundle& out)
    {
        U32 rng = bundleSeed(p.mX, p.mY, p.mZ);
        const F32 step = llmax(s.mCell * 0.5f, 0.125f);
        const F32 width = (F32)s.mRes * s.mCell;

        F32 seg_total = 0.f;
        S32 seg_count = 0;
        S32 late_first = 0;
        S32 escaped_total = 0;
        S32 octant_rays[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        S32 octant_open[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

        // Per-ray first arrivals, for the dominant-early-reflection cluster.
        std::vector<F32> fa_t((size_t)llmax(rays, 1), -1.f);
        std::vector<F32> fa_x((size_t)llmax(rays, 1), 0.f);
        std::vector<F32> fa_y((size_t)llmax(rays, 1), 0.f);
        std::vector<F32> fa_z((size_t)llmax(rays, 1), 0.f);

        // n60 for the decay curve: reflections until (1-a)^n = 1e-6.
        const F32 n60 = -6.f / log10f(1.f - BUNDLE_ABSORB);

        for (S32 r = 0; r < rays; ++r)
        {
            // Uniform sphere direction.
            const F32 uz = bundleRand01(rng) * 2.f - 1.f;
            const F32 phi = bundleRand01(rng) * 6.2831853f;
            const F32 rr = sqrtf(llmax(0.f, 1.f - uz * uz));
            F32 dir[3] = { rr * cosf(phi), rr * sinf(phi), uz };

            // The ray's first-heading octant: which direction's openness it votes on.
            F32 h0 = atan2f(dir[1], dir[0]);
            if (h0 < 0.f) h0 += 6.2831853f;
            const S32 o0 = llclamp((S32)(h0 / 0.78539816f), 0, 7);
            ++octant_rays[o0];

            F32 pos[3] = { p.mX, p.mY, p.mZ };
            F32 path = 0.f;
            bool escaped = false;

            for (S32 b = 0; b <= bounces; ++b)
            {
                // March until a hit, an escape, or the distance budget.
                F32 travelled = 0.f;
                bool hit = false;
                S32 hit_axis = 2;
                while (travelled < BUNDLE_MAX_DIST_M)
                {
                    const F32 px = pos[0], py = pos[1], pz = pos[2];
                    pos[0] += dir[0] * step;
                    pos[1] += dir[1] * step;
                    pos[2] += dir[2] * step;
                    travelled += step;

                    if (pos[0] < 0.f || pos[1] < 0.f ||
                        pos[0] >= width || pos[1] >= width ||
                        pos[2] < 0.f || pos[2] > s.mCeiling)
                    {
                        // Escaped the store's envelope - open energy.
                        escaped = true;
                        break;
                    }

                    const size_t col = (size_t)(pos[1] / s.mCell) * (size_t)s.mRes
                                     + (size_t)(pos[0] / s.mCell);
                    if (!airAt(s, col, pos[2]))
                    {
                        hit = true;
                        // Which face: the axis whose last step crossed a boundary -
                        // x or y when that column changed alone, z otherwise, the
                        // larger step axis when both columns changed.
                        const bool chx = (S32)(px / s.mCell) != (S32)(pos[0] / s.mCell);
                        const bool chy = (S32)(py / s.mCell) != (S32)(pos[1] / s.mCell);
                        if (chx && !chy) hit_axis = 0;
                        else if (chy && !chx) hit_axis = 1;
                        else if (fabsf(dir[0]) >= fabsf(dir[1]) && fabsf(dir[0]) >= fabsf(dir[2])) hit_axis = 0;
                        else if (fabsf(dir[1]) >= fabsf(dir[2])) hit_axis = 1;
                        else hit_axis = 2;

                        dir[hit_axis] = -dir[hit_axis];
                        // Jitter the bounce (diffuse-ish) so domed and faceted halls
                        // do not fold every ray onto one axis-aligned orbit.
                        const F32 j1 = (bundleRand01(rng) - 0.5f) * 0.3f;
                        const F32 j2 = (bundleRand01(rng) - 0.5f) * 0.3f;
                        const S32 o1 = (hit_axis == 0) ? 1 : 0;
                        const S32 o2 = (hit_axis == 2) ? 1 : 2;
                        dir[o1] += j1;
                        dir[o2] += j2;
                        const F32 nl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                        if (nl > 1.0e-5f) { dir[0] /= nl; dir[1] /= nl; dir[2] /= nl; }
                        // Step off the face along the reflected direction.
                        pos[0] = px + dir[0] * step;
                        pos[1] = py + dir[1] * step;
                        pos[2] = pz + dir[2] * step;
                        break;
                    }
                }

                path += travelled;
                seg_total += travelled;
                ++seg_count;

                if (fa_t[r] < 0.f)
                {
                    if (hit)
                    {
                        // First arrival: the first wall the ray met, arriving FROM it.
                        fa_t[r] = path / SPEED_OF_SOUND_MS;
                        fa_x[r] = -dir[0]; fa_y[r] = -dir[1]; fa_z[r] = -dir[2];
                    }
                    else if (escaped || b == bounces)
                    {
                        // No wall met at all: no first arrival to cluster.
                        fa_t[r] = 3.4e38f;
                    }
                }

                if (escaped) break;
            }

            if (escaped)
            {
                ++escaped_total;
                ++octant_open[o0];
            }
            if (fa_t[r] > ECHO_LATE_S && fa_t[r] < 3.4e38f) ++late_first;
        }

        out.mMFP = (seg_count > 0) ? seg_total / (F32)seg_count : 0.f;
        // Decay RT60: the mean free path is the distance between reflections, so the
        // -60 dB point sits n60 reflections out.
        out.mRT60 = (out.mMFP > 0.f) ? n60 * out.mMFP / SPEED_OF_SOUND_MS : 0.f;
        out.mEcho = (rays > 0) ? (F32)late_first / (F32)rays : 0.f;

        // Dominant early reflection: the earliest first arrival, and the mean direction
        // of every first arrival within its window - the slap of a facing wall.
        F32 earliest = 3.4e38f;
        for (S32 r = 0; r < rays; ++r)
        {
            if (fa_t[r] >= 0.f && fa_t[r] < earliest) earliest = fa_t[r];
        }
        if (earliest < 3.4e38f)
        {
            F32 fx = 0.f, fy = 0.f, fz = 0.f;
            S32 cluster = 0;
            for (S32 r = 0; r < rays; ++r)
            {
                if (fa_t[r] >= 0.f && fa_t[r] <= earliest + FIRST_ARRIVAL_WINDOW_S)
                {
                    fx += fa_x[r]; fy += fa_y[r]; fz += fa_z[r];
                    ++cluster;
                }
            }
            out.mFirstDelay = earliest;
            if (cluster > 0)
            {
                const F32 il = 1.f / sqrtf(fx * fx + fy * fy + fz * fz + 1.0e-6f);
                out.mFirstDir[0] = fx * il; out.mFirstDir[1] = fy * il; out.mFirstDir[2] = fz * il;
            }
        }
        for (S32 i = 0; i < 8; ++i)
        {
            out.mOpenness[i] = (octant_rays[i] > 0) ? (F32)octant_open[i] / (F32)octant_rays[i] : 0.f;
        }
        (void)escaped_total;
    }

    // ---- the span mip (tier B's trace target) ----

    // Coarsens the snapshot BUNDLE_MIP-x: each mip column merges the solid intervals
    // of its 4x4 block of capture columns. Reverb statistics do not need 0.25 m walls;
    // the mip is what turns ~1e9 column tests per region into ~1e8. The buffers are
    // the caller's (transient, per flood); the returned Snap views them and must not
    // outlive them.
    inline Snap buildMip(const Snap& s, std::vector<F32>& top, std::vector<F32>& bottom,
                         std::vector<U8>& flags)
    {
        Snap m;
        if (s.mRes < BUNDLE_MIP || s.mCell <= 0.f) return m;

        const S32 res = s.mRes / BUNDLE_MIP;
        const S32 max_spans = s.mMaxSpans + 2;   // merging can briefly exceed the cap
        const size_t layer = (size_t)res * (size_t)res;
        const size_t src_layer = (size_t)s.mRes * (size_t)s.mRes;

        top.assign((size_t)max_spans * layer, NO_SURFACE_F);
        bottom.assign((size_t)max_spans * layer, NO_SURFACE_F);
        flags.assign((size_t)max_spans * layer, 0);

        std::vector<F32> iv_lo;
        std::vector<F32> iv_hi;
        std::vector<U8> iv_fl;

        for (S32 my = 0; my < res; ++my)
        {
            for (S32 mx = 0; mx < res; ++mx)
            {
                const size_t mcol = (size_t)my * (size_t)res + (size_t)mx;
                iv_lo.clear(); iv_hi.clear(); iv_fl.clear();

                for (S32 oy = 0; oy < BUNDLE_MIP; ++oy)
                {
                    for (S32 ox = 0; ox < BUNDLE_MIP; ++ox)
                    {
                        const S32 sx = mx * BUNDLE_MIP + ox;
                        const S32 sy = my * BUNDLE_MIP + oy;
                        if (sx >= s.mRes || sy >= s.mRes) continue;
                        const size_t col = (size_t)sy * (size_t)s.mRes + (size_t)sx;
                        const S32 n = spanCount(s, col);
                        for (S32 k = 0; k < n; ++k)
                        {
                            const size_t si = (size_t)k * src_layer + col;
                            iv_lo.push_back(s.mBottom[si]);
                            iv_hi.push_back(s.mTop[si]);
                            iv_fl.push_back(s.mFlags ? s.mFlags[si] : (U8)0);   // <SS:Nexii> A snapshot without a flags plane is a valid input (the flood's acoustic path builds one), so the mip carries plain untyped solids rather than dereferencing null.
                        }
                    }
                }

                // Insertion sort the block's intervals bottom-up (<= 96 entries).
                const S32 count = (S32)iv_lo.size();
                for (S32 i = 1; i < count; ++i)
                {
                    const F32 klo = iv_lo[i];
                    const F32 khi = iv_hi[i];
                    const U8 kf = iv_fl[i];
                    S32 j = i - 1;
                    while (j >= 0 && iv_lo[j] > klo)
                    {
                        iv_lo[j + 1] = iv_lo[j];
                        iv_hi[j + 1] = iv_hi[j];
                        iv_fl[j + 1] = iv_fl[j];
                        --j;
                    }
                    iv_lo[j + 1] = klo;
                    iv_hi[j + 1] = khi;
                    iv_fl[j + 1] = kf;
                }

                // Merge overlapping/touching intervals.
                S32 n = 0;
                for (S32 i = 0; i < count; ++i)
                {
                    if (n > 0 && iv_lo[i] <= top[(size_t)(n - 1) * layer + mcol] + 0.01f)
                    {
                        const size_t pi = (size_t)(n - 1) * layer + mcol;
                        if (iv_hi[i] > top[pi])
                        {
                            top[pi] = iv_hi[i];
                            flags[pi] = (U8)(flags[pi] | iv_fl[i]);
                        }
                        continue;
                    }
                    // <SS:Nexii> Out of slots. A mip column gathers up to BUNDLE_MIP*BUNDLE_MIP * mMaxSpans intervals and every one of them can be disjoint, but the buffers only hold max_spans per column - unbounded n walked off the end of the layer and into the next column's rows (heap corruption). The intervals are sorted bottom-up, so folding the overflow into the top slot stretches that span over everything above it: solid where the mip should have had air, which is the conservative direction for a trace target - dropping the interval instead would open a hole to shoot through.
                    if (n >= max_spans)
                    {
                        const size_t pi = (size_t)(max_spans - 1) * layer + mcol;
                        if (iv_hi[i] > top[pi]) top[pi] = iv_hi[i];
                        flags[pi] = (U8)(flags[pi] | iv_fl[i]);
                        continue;
                    }
                    const size_t ni = (size_t)n * layer + mcol;
                    bottom[ni] = iv_lo[i];
                    top[ni] = iv_hi[i];
                    flags[ni] = iv_fl[i];
                    ++n;
                }
            }
        }

        m.mTop = top.data();
        m.mBottom = bottom.data();
        m.mFlags = flags.data();
        m.mGapLabel = nullptr;
        m.mGapDepth = nullptr;
        m.mSurveyed = nullptr;      // the mip is a different resolution; the caller's mask does not index it
        m.mRes = res;
        m.mCell = s.mCell * (F32)BUNDLE_MIP;
        m.mCeiling = s.mCeiling;
        m.mMaxSpans = max_spans;
        return m;
    }
}

#endif
