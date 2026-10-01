/**
 * @file ssworldfieldcore.h
 * @brief Atmo Magic world field: the cell grid's pure core - span insertion, the
 *        sheet-to-grid materialisation, the gap lookup, and the air classification
 *        (reachability, the geometric reach budget, the labels).
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

// <SS:Nexii> A CORE header (lldefs.h + <cmath>/<cstdint> + POD containers only; NO
// llmath.h), the same split every other Atmo core follows: everything that decides
// what the world field SAYS lives here as pure functions, and ssworldfield.cpp owns
// the navmesh snapshot, the scheduling and the storage. Nothing here reads a setting,
// the camera or a system, so the scratch harness at V:\Scratch\atmo (worldfieldcore,
// worldfieldcore_adv, worldfieldcore_cost) pins the exact code the viewer runs -
// including the sheet-to-grid coordinate mapping, where a half-cell shift would
// silently poison every downstream answer.
// Coordinates: cells are region-anchored and indexed col = y * res + x; spans are
// [k * res * res + col] so a slot is one contiguous plane (the layout
// SSAcoustic::Snap walks); z is the absolute altitude, metres.

#ifndef SS_WORLDFIELD_CORE_H
#define SS_WORLDFIELD_CORE_H

#include "lldefs.h"

#include <cmath>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

namespace SSWorldFieldCore
{
    // An empty span slot. Matches ssworldfield.cpp's NO_SURFACE; a slot is
    // occupied when its top is above NO_SURFACE * 0.5.
    constexpr F32 NO_SURFACE = -3.402823466e+38F;

    // The air labels, value-for-value SSWorldField::EAirLabel. Plain constants so the
    // core never includes the shell's header; the shell static_asserts them.
    constexpr U8 AIR_SOLID     = 0;
    constexpr U8 AIR_OUTDOORS  = 1;
    constexpr U8 AIR_SHELTERED = 2;
    constexpr U8 AIR_INTERIOR  = 3;
    constexpr U8 AIR_UNKNOWN   = 4;

    // "Never reached", and the exclusive upper bound on a stored distance.
    constexpr U16 DEPTH_UNREACHED = 0xFFFFu;

    // <SS:Nexii> The assumed minimum slab: two bodies closer together than this merge
    // into one span rather than leaving an air gap nothing fits through, so a wall
    // standing on a floor never reads as a hollow shell and a room is one air interval
    // whatever band its floor and its ceiling were rasterised in.
    constexpr F32 SPAN_SLAB_M = 0.25f;

    // <SS:Nexii> How much further than the outdoors reach an opening's budget still
    // carries as SHELTER. One cot(theta) of gap height is the distance the sky is
    // still overhead at theta; this multiple of it is the distance a covered space
    // still feels like it has a way out. A multiple rather than a second angle,
    // because the two are the same geometry and should not be tunable apart.
    // Owner verdict 2026-09-11: 2, not 4 - at 4 a 20 m-ceilinged warehouse read about
    // 80 m sheltered, which is most of a region.
    constexpr F32 SHELTER_MULT = 2.f;

    // Two gaps are adjacent only when their z-intervals STRICTLY overlap by this much.
    // A corner touch where one gap ends exactly where the neighbour's begins is a wall
    // junction, not a door.
    constexpr F32 TOUCH_EPS = 0.05f;

    // ---------------------------------------------------------------- the span list

    // How many spans a cell holds. The list is dense from slot 0 and terminated by
    // the first empty top.
    inline S32 spanCount(const F32* span_top, S32 max_spans, size_t layer, size_t col)
    {
        S32 n = 0;
        while (n < max_spans && span_top[(size_t)n * layer + col] > NO_SURFACE * 0.5f) ++n;
        return n;
    }

    namespace detail
    {
        // <SS:Nexii> Absorb every following span that widening span `i` has brought
        // within the slab of it. Without this, a body inserted just above a floor slab
        // merges DOWN into the floor and the merged top can reach clean past the
        // ceiling span above it, leaving the list overlapping and out of order - a
        // pillar standing on a floor and passing through a ceiling, which is ordinary
        // content. Downstream that misreports the landing surface, double-counts solid
        // metres in traceSolid, and inflates the gap height the whole reach rule is
        // computed from. Returns the new span count.
        // [interaction: V:\Scratch\atmo\tests\worldfieldcore_adv.cpp]
        inline S32 cascade(F32* span_bottom, F32* span_top, U8* span_flags,
                           size_t layer, size_t col, S32 i, S32 n)
        {
            while (i + 1 < n)
            {
                const size_t here = (size_t)i * layer + col;
                const size_t next = (size_t)(i + 1) * layer + col;
                if (span_bottom[next] - span_top[here] >= SPAN_SLAB_M) break;

                if (span_top[next] > span_top[here])
                {
                    span_top[here] = span_top[next];
                    span_flags[here] = span_flags[next];
                }
                for (S32 j = i + 1; j + 1 < n; ++j)
                {
                    span_bottom[(size_t)j * layer + col] = span_bottom[(size_t)(j + 1) * layer + col];
                    span_top[(size_t)j * layer + col] = span_top[(size_t)(j + 1) * layer + col];
                    span_flags[(size_t)j * layer + col] = span_flags[(size_t)(j + 1) * layer + col];
                }
                const size_t last = (size_t)(n - 1) * layer + col;
                span_bottom[last] = NO_SURFACE;
                span_top[last] = NO_SURFACE;
                span_flags[last] = 0;
                --n;
            }
            return n;
        }

        // Merge the thinnest air gap away: the two spans around it become one. The
        // list stays sorted and every gap in it at least the slab tall, and no body is
        // ever dropped - the gap becomes solid instead.
        inline void foldThinnest(F32* span_bottom, F32* span_top, U8* span_flags,
                                 size_t layer, size_t col, S32 n)
        {
            // With fewer than two spans there is no gap between any pair to fold, and
            // the search below would leave `thinnest` at 0 and then read span 1 - off
            // the end of a buffer sized for one. Unreachable from the viewer (the
            // budget is 6, static_asserted against the sheet's) but the core is
            // documented as generic and the harness calls it directly.
            if (n < 2) return;
            S32 thinnest = 0;
            F32 best = 3.4e38f;
            for (S32 j = 0; j + 1 < n; ++j)
            {
                const F32 gap = span_bottom[(size_t)(j + 1) * layer + col] - span_top[(size_t)j * layer + col];
                if (gap < best) { best = gap; thinnest = j; }
            }
            span_top[(size_t)thinnest * layer + col] = span_top[(size_t)(thinnest + 1) * layer + col];
            span_flags[(size_t)thinnest * layer + col] = span_flags[(size_t)(thinnest + 1) * layer + col];
            for (S32 j = thinnest + 1; j + 1 < n; ++j)
            {
                span_bottom[(size_t)j * layer + col] = span_bottom[(size_t)(j + 1) * layer + col];
                span_top[(size_t)j * layer + col] = span_top[(size_t)(j + 1) * layer + col];
                span_flags[(size_t)j * layer + col] = span_flags[(size_t)(j + 1) * layer + col];
            }
            const size_t last = (size_t)(n - 1) * layer + col;
            span_bottom[last] = NO_SURFACE;
            span_top[last] = NO_SURFACE;
            span_flags[last] = 0;
        }
    }

    // <SS:Nexii> Inserts one solid body into a cell's span list, preserving the
    // invariant the whole field rests on: SORTED and DISJOINT, with every air gap at
    // least the slab threshold tall. Three outcomes - merge into the span below, merge
    // into the span above, or a fresh slot - and each merge cascades forward, because
    // widening a span can bring it within the slab of the next one. Over the span
    // budget the thinnest air gap folds away first and the insert is then retried
    // against the folded list, which is why this is a two-pass loop rather than a
    // straight line: folding changes both the insert position and which merges apply.
    inline void spanInsert(F32* span_bottom, F32* span_top, U8* span_flags,
                           S32 max_spans, size_t layer, size_t col, F32 bot, F32 tp, U8 fl)
    {
        if (max_spans < 1 || tp < bot) return;

        for (S32 pass = 0; pass < 2; ++pass)
        {
            S32 n = spanCount(span_top, max_spans, layer, col);

            S32 at = n;
            while (at > 0 && span_bottom[(size_t)(at - 1) * layer + col] > bot) --at;

            if (at > 0 && bot - span_top[(size_t)(at - 1) * layer + col] < SPAN_SLAB_M)
            {
                const size_t pi = (size_t)(at - 1) * layer + col;
                const bool higher = tp > span_top[pi];
                span_bottom[pi] = llmin(span_bottom[pi], bot);
                span_top[pi] = llmax(span_top[pi], tp);
                if (higher) span_flags[pi] = fl;
                detail::cascade(span_bottom, span_top, span_flags, layer, col, at - 1, n);
                return;
            }
            if (at < n && tp > span_bottom[(size_t)at * layer + col] - SPAN_SLAB_M)
            {
                const size_t ni = (size_t)at * layer + col;
                const bool higher = bot < span_bottom[ni];
                span_bottom[ni] = llmin(span_bottom[ni], bot);
                span_top[ni] = llmax(span_top[ni], tp);
                if (!higher) span_flags[ni] = fl;
                detail::cascade(span_bottom, span_top, span_flags, layer, col, at, n);
                return;
            }
            if (n < max_spans)
            {
                // Neither merge applied, so the new span touches nothing: no cascade.
                for (S32 j = n; j > at; --j)
                {
                    span_bottom[(size_t)j * layer + col] = span_bottom[(size_t)(j - 1) * layer + col];
                    span_top[(size_t)j * layer + col] = span_top[(size_t)(j - 1) * layer + col];
                    span_flags[(size_t)j * layer + col] = span_flags[(size_t)(j - 1) * layer + col];
                }
                span_bottom[(size_t)at * layer + col] = bot;
                span_top[(size_t)at * layer + col] = tp;
                span_flags[(size_t)at * layer + col] = fl;
                return;
            }

            // Full and nothing merged: fold the thinnest gap and try once more. The
            // second pass always finds room, so the loop terminates.
            detail::foldThinnest(span_bottom, span_top, span_flags, layer, col, n);
        }
    }

    // ---------------------------------------------------------------- the gap lookup

    // The air gaps of a cell are the intervals between, beneath and above its solid
    // spans. Gap k of a cell with n spans runs from the span below (or the world
    // floor) to the span above (or the grid ceiling).
    inline F32 gapLo(const F32* span_top, size_t layer, size_t col, S32 k)
    {
        return (k == 0) ? 0.f : span_top[(size_t)(k - 1) * layer + col];
    }
    inline F32 gapHi(const F32* span_bottom, size_t layer, size_t col, S32 k, S32 n, F32 ceiling)
    {
        return (k == n) ? ceiling : span_bottom[(size_t)k * layer + col];
    }

    // <SS:Nexii> Which air gap of a cell contains z: the gap index (0 below the lowest
    // span, n above the highest), or -1 when z falls inside a body - the grid has no
    // verdict for the inside of solid things. The top gap is reported as reaching at
    // least z itself, so a point above the grid's ceiling (a flying avatar, rain
    // aloft) still resolves rather than falling off the end of the data.
    // THE one place gap indexing is defined; classify derives its bounds from the same
    // gapLo/gapHi, so a query and the classification can never disagree about which
    // gap a point is in. [interaction: SSWorldField::gapAt]
    inline S32 gapIndexAt(const F32* span_top, const F32* span_bottom, S32 max_spans,
                          size_t layer, size_t col, F32 z, F32 ceiling, F32& g0, F32& g1)
    {
        F32 prev = 0.f;
        for (S32 k = 0; k < max_spans; ++k)
        {
            const size_t si = (size_t)k * layer + col;
            const F32 stop = span_top[si];
            if (stop <= NO_SURFACE * 0.5f)
            {
                g0 = prev; g1 = llmax(ceiling, z + 1.f);
                return k;                                   // open above the last span
            }
            const F32 bottom = span_bottom[si];
            if (z < bottom - 0.01f)
            {
                g0 = prev; g1 = bottom;
                return k;                                   // the gap beneath this span
            }
            if (z <= stop + 0.01f) return -1;               // inside the body
            prev = stop;
        }
        g0 = prev; g1 = llmax(ceiling, z + 1.f);
        return max_spans;                                   // above the last span
    }

    // ------------------------------------------------------------ sheets into a grid

    // <SS:Nexii> One published navmesh band sheet, as the core sees it: a square of
    // mRes x mRes cells over mExtent metres whose south-west corner is at (mX0, mY0)
    // in REGION-LOCAL metres, each cell holding up to mSpans solid spans at absolute
    // z. Deliberately raw pointers and floats rather than the viewer's BandSheet, so
    // the mapping below is a pure function the harness can pin.
    struct SheetRef
    {
        F32 mX0 = 0.f, mY0 = 0.f;
        F32 mExtent = 16.f;
        S32 mRes = 64;
        S32 mSpans = 6;
        const U8* mCount = nullptr;
        const F32* mBottom = nullptr;
        const F32* mTop = nullptr;
        const U8* mFlags = nullptr;
    };

    // <SS:Nexii> The region's cell grid, materialised from a set of band sheets. One
    // pass per sheet: every grid cell the sheet covers takes the spans of every sheet
    // cell IT covers (a grid coarser than the sheet unions them; at the default they
    // are the same 0.25 m cell and the mapping is one-to-one), through spanInsert so
    // the sorted-and-disjoint invariant holds however the bands interleave. Bands are
    // separated by real air gaps by construction, so two bands of one column never
    // contest a z range and insertion order does not matter.
    //
    // Then one pass per cell: a span the census marked terrain (terrain_flag) reaches
    // the world floor, swallowing whatever the raster put beneath the land - what
    // keeps a wall standing on unmeasured ground from reading as a hollow shell.
    //
    // out_sheeted, when given, is set to 1 for every grid cell at least one sheet
    // actually covered. A cell left 0 is not empty sky - it is NOT SURVEYED, and the
    // caller must label it AIR_UNKNOWN rather than let its 0..ceiling gap read as
    // outdoors. [interaction: SSWorldField::scheduleGrid, classify's unsurveyed mask]
    inline void buildGrid(S32 res, F32 cell, S32 max_spans,
                          const SheetRef* sheets, S32 sheet_count, U8 terrain_flag,
                          F32* span_bottom, F32* span_top, U8* span_flags,
                          U8* out_sheeted)
    {
        if (res < 1 || cell <= 0.f || max_spans < 1) return;
        const size_t layer = (size_t)res * (size_t)res;

        for (size_t i = 0; i < (size_t)max_spans * layer; ++i)
        {
            span_bottom[i] = NO_SURFACE;
            span_top[i] = NO_SURFACE;
            span_flags[i] = 0;
        }
        if (out_sheeted) for (size_t i = 0; i < layer; ++i) out_sheeted[i] = 0;

        for (S32 s = 0; s < sheet_count; ++s)
        {
            const SheetRef& sh = sheets[s];
            if (!sh.mCount || !sh.mBottom || !sh.mTop || !sh.mFlags) continue;
            if (sh.mRes < 1 || sh.mSpans < 1 || sh.mExtent <= 0.f) continue;

            const S32 R = sh.mRes, K = sh.mSpans;
            const F32 sheet_cell = sh.mExtent / (F32)R;

            // <SS:Nexii> The grid cells this sheet covers. The navmesh's 16 m lattice
            // is pinned at init and region origins are multiples of 256, so the corner
            // lands exactly on a grid line at any cell size that divides 16 - the
            // +0.5f is a rounding guard against float drift in that division, not a
            // half-cell offset. A consumer materialising its own grid must use the
            // same convention or its cells will sit half a cell off this one's.
            const S32 cx0 = llclamp((S32)floorf(sh.mX0 / cell + 0.5f), 0, res);
            const S32 cx1 = llclamp((S32)floorf((sh.mX0 + sh.mExtent) / cell + 0.5f), 0, res);
            const S32 cy0 = llclamp((S32)floorf(sh.mY0 / cell + 0.5f), 0, res);
            const S32 cy1 = llclamp((S32)floorf((sh.mY0 + sh.mExtent) / cell + 0.5f), 0, res);

            for (S32 y = cy0; y < cy1; ++y)
            {
                for (S32 x = cx0; x < cx1; ++x)
                {
                    const size_t col = (size_t)y * (size_t)res + (size_t)x;
                    if (out_sheeted) out_sheeted[col] = 1;

                    const S32 sx0 = llclamp((S32)floorf(((F32)x * cell - sh.mX0) / sheet_cell), 0, R - 1);
                    const S32 sx1 = llclamp((S32)ceilf(((F32)(x + 1) * cell - sh.mX0) / sheet_cell), sx0 + 1, R);
                    const S32 sy0 = llclamp((S32)floorf(((F32)y * cell - sh.mY0) / sheet_cell), 0, R - 1);
                    const S32 sy1 = llclamp((S32)ceilf(((F32)(y + 1) * cell - sh.mY0) / sheet_cell), sy0 + 1, R);
                    for (S32 sy = sy0; sy < sy1; ++sy) for (S32 sx = sx0; sx < sx1; ++sx)
                    {
                        const size_t sc = (size_t)sy * (size_t)R + (size_t)sx;
                        const S32 have = llmin((S32)sh.mCount[sc], K);
                        for (S32 k = 0; k < have; ++k)
                        {
                            spanInsert(span_bottom, span_top, span_flags, max_spans, layer, col,
                                       sh.mBottom[sc * (size_t)K + (size_t)k],
                                       sh.mTop[sc * (size_t)K + (size_t)k],
                                       sh.mFlags[sc * (size_t)K + (size_t)k]);
                        }
                    }
                }
            }
        }

        // The land reaches the world floor: everything below the lowest terrain span
        // folds into it.
        for (size_t col = 0; col < layer; ++col)
        {
            const S32 n = spanCount(span_top, max_spans, layer, col);
            S32 land = -1;
            for (S32 k = 0; k < n; ++k)
            {
                if (span_flags[(size_t)k * layer + col] & terrain_flag) { land = k; break; }
            }
            if (land < 0) continue;

            span_bottom[(size_t)land * layer + col] = 0.f;
            if (land == 0) continue;
            const S32 kept = n - land;
            for (S32 k = 0; k < kept; ++k)
            {
                span_bottom[(size_t)k * layer + col] = span_bottom[(size_t)(k + land) * layer + col];
                span_top[(size_t)k * layer + col] = span_top[(size_t)(k + land) * layer + col];
                span_flags[(size_t)k * layer + col] = span_flags[(size_t)(k + land) * layer + col];
            }
            for (S32 k = kept; k < max_spans; ++k)
            {
                span_bottom[(size_t)k * layer + col] = NO_SURFACE;
                span_top[(size_t)k * layer + col] = NO_SURFACE;
                span_flags[(size_t)k * layer + col] = 0;
            }
        }
    }

    // ------------------------------------------------------------- the classification

    // <SS:Nexii> The air classification over the cell grid - the pass the whole field
    // exists to produce. Two gaps of neighbouring cells are adjacent when their
    // z-intervals strictly overlap. Four walks:
    //
    // 1. REACHABILITY. Every cell's top gap is open sky and every gap of a border cell
    //    can walk out sideways; a flood from those marks the air connected to outside
    //    at all. What the flood never touched is sealed, and is INTERIOR by
    //    construction.
    //
    // 2. THE REACH BUDGET (the owner's outdoors rule). A gap with structure over it is
    //    COVERED. An opening - a reachable uncovered gap touching covered air - hands
    //    its covered neighbours a budget in METRES, and every cell step spends one cell
    //    width. The budget is capped at every step by the LOCAL gap's own capacity,
    //    gap_height * open_k * SHELTER_MULT, so a gap that pinches under a low beam
    //    cuts whatever it was carrying while a gap that stays tall keeps it. Widest
    //    path first, because the strongest opening must decide how deep shelter
    //    reaches; first pop is final, since every later entry carries a smaller budget.
    //
    // 3. THE COVERED DISTANCE, as a SEPARATE shortest-path walk over the air step 2
    //    reached. It must not be the widest path's own length: a cell one step inside a
    //    narrow door that is also reachable from a wide door 30 m away would record
    //    30 m, and that figure feeds the enclosure ramp, airDepthAt and the acoustic
    //    bake's travel-to-outdoors. Every step costs the same cell width, so plain BFS
    //    order is already shortest and this costs one queue pass. The walk carries the
    //    LEVEL - the step count - and step 4 turns it into metres once, because
    //    accumulating a rounded per-step distance is what made 0.25 m cells record 20%
    //    long.
    //
    // 4. THE LABEL. Covered air within ONE unmultiplied capacity of its nearest opening
    //    still has the sky overhead at theta or better, so it reads OUTDOORS. Past that
    //    but still in budget, SHELTERED. Out of budget, INTERIOR - and never reached is
    //    INTERIOR only when REAL GEOMETRY sealed it; a pocket sealed by the SURVEY
    //    BOUNDARY is UNKNOWN, because the field has not earned a verdict there.
    //
    // surveyed, when given, is buildGrid's out_sheeted: a cell no sheet covered has no
    // geometry, NOT empty sky. Every gap of it is labelled AIR_UNKNOWN, it never seeds,
    // and it BLOCKS - walking through it would be walking through geometry nobody has
    // looked at. Because it blocks, whatever it seals off is unproven rather than proven
    // interior, and step 4's doubt flood labels that UNKNOWN too. A partially surveyed
    // region therefore answers honestly where it has looked and says so, in BOTH
    // directions, where it has not. open_k is cot(theta); cell_m is the grid's cell size
    // in metres; ceiling is where a cell's top gap ends.
    inline void classify(S32 res, S32 max_spans, F32 cell_m, F32 ceiling, F32 open_k,
                         const F32* span_top, const F32* span_bottom,
                         std::vector<U8>& gap_label, std::vector<U16>& gap_depth,
                         const U8* surveyed = nullptr)
    {
        const size_t layer = (size_t)res * (size_t)res;
        const size_t per_col = (size_t)max_spans + 1;
        const size_t nodes = layer * per_col;
        gap_label.assign(nodes, AIR_SOLID);
        gap_depth.assign(nodes, DEPTH_UNREACHED);
        if (res < 1 || max_spans < 1) return;

        static const S32 DX[4] = { 1, -1, 0, 0 };
        static const S32 DY[4] = { 0, 0, 1, -1 };
        const F32 step_m = llmax(cell_m, 0.01f);

        // <SS:Nexii> Span counts are the only per-cell scratch kept. The gap bounds
        // used to be two F32 planes over every node - 58.7 MB at the default cell -
        // and they are two array reads away from the spans, so they are recomputed
        // instead. The remaining scratch is a U16 budget plane and two bitsets.
        // [interaction: doc/atmo_magic_worldfield.md Costs]
        std::vector<U8> span_n(layer, 0);
        for (size_t col = 0; col < layer; ++col)
        {
            span_n[col] = (U8)spanCount(span_top, max_spans, layer, col);
        }

        auto lo = [&](size_t col, S32 k) { return gapLo(span_top, layer, col, k); };
        auto hi = [&](size_t col, S32 k) { return gapHi(span_bottom, layer, col, k, (S32)span_n[col], ceiling); };
        auto exists = [&](size_t col, S32 k)
        {
            if (k > (S32)span_n[col]) return false;
            return hi(col, k) > lo(col, k) + TOUCH_EPS;
        };
        auto known = [&](size_t col) { return !surveyed || surveyed[col] != 0; };

        // Bitsets rather than byte planes: 918 KB each instead of 7.34 MB.
        std::vector<U64> reached((nodes + 63) / 64, 0);
        std::vector<U64> covered((nodes + 63) / 64, 0);
        auto getBit = [](const std::vector<U64>& b, size_t i) { return (b[i >> 6] >> (i & 63)) & 1ull; };
        auto setBit = [](std::vector<U64>& b, size_t i) { b[i >> 6] |= 1ull << (i & 63); };

        auto touches = [&](size_t node, auto&& fn)
        {
            const size_t col = node / per_col;
            const S32 k = (S32)(node % per_col);
            const S32 x = (S32)(col % (size_t)res);
            const S32 y = (S32)(col / (size_t)res);
            const F32 a0 = lo(col, k), a1 = hi(col, k);
            for (S32 d = 0; d < 4; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;

                const size_t ncol = (size_t)ny * (size_t)res + (size_t)nx;
                // <SS:Nexii> An unsurveyed column BLOCKS the walk - it has no spans, so
                // walking through it would be walking through geometry nobody has
                // looked at. What that blocking seals off is therefore not proven
                // interior, only unproven, and step 4 labels it AIR_UNKNOWN rather than
                // AIR_INTERIOR. Getting that verdict wrong is the same bug the survey
                // mask exists to fix, with the sign flipped: a porch whose only way out
                // crosses the survey boundary would read sealed, and every consumer
                // would take it. [interaction: the doubt flood in step 4]
                if (!known(ncol)) continue;
                const S32 nn = (S32)span_n[ncol];
                for (S32 kj = 0; kj <= nn; ++kj)
                {
                    const F32 b0 = lo(ncol, kj), b1 = hi(ncol, kj);
                    if (b1 <= b0 + TOUCH_EPS) continue;
                    if (!(a0 < b1 - TOUCH_EPS && b0 < a1 - TOUCH_EPS)) continue;
                    fn(ncol * per_col + (size_t)kj);
                }
            }
        };

        // ---- 1. reachability ----
        std::vector<S32> queue;
        queue.reserve(nodes / 8 + 1);
        for (size_t col = 0; col < layer; ++col)
        {
            if (!known(col)) continue;
            const S32 x = (S32)(col % (size_t)res);
            const S32 y = (S32)(col / (size_t)res);
            const bool border = x == 0 || y == 0 || x == res - 1 || y == res - 1;
            const S32 n = (S32)span_n[col];

            for (S32 k = 0; k <= n; ++k)
            {
                if (!exists(col, k)) continue;
                if (k != n && !border) continue;
                const size_t node = col * per_col + (size_t)k;
                setBit(reached, node);
                queue.push_back((S32)node);
            }
        }

        for (size_t head = 0; head < queue.size(); ++head)
        {
            touches((size_t)queue[head], [&](size_t nnode)
            {
                if (getBit(reached, nnode)) return;
                setBit(reached, nnode);
                queue.push_back((S32)nnode);
            });
        }

        // Covered: reachable air with structure standing over it. A gap below a cell's
        // top span always has that structure; the top gap never does.
        for (size_t col = 0; col < layer; ++col)
        {
            if (!known(col)) continue;
            const S32 n = (S32)span_n[col];
            for (S32 k = 0; k < n; ++k)
            {
                const size_t node = col * per_col + (size_t)k;
                if (getBit(reached, node) && exists(col, k)) setBit(covered, node);
            }
        }

        // ---- 2. the reach budget ----
        // <SS:Nexii> Counted in whole CELL STEPS, not in decimetres. The decimetre form
        // rounded the step itself - toDm(0.25) is 3, so the shipping 0.25 m cell spent
        // 0.3 m of budget per 0.25 m of travel and recorded distances 20% long. A step
        // is exactly one step, so the arithmetic is integral and exact; sub-cell
        // precision would be false precision anyway, since one cell is the grid's whole
        // resolution. remaining[] holds 1 + the steps still affordable, so 0 still means
        // "never reached". [interaction: V:\Scratch\atmo\tests\worldfield_review2.cpp]
        constexpr U32 STEPS_MAX = 65533u;
        std::vector<U16> remaining(nodes, 0);
        std::priority_queue<std::pair<U16, S32> > heap;
        auto capacity = [&](size_t node)
        {
            const size_t col = node / per_col;
            const S32 k = (S32)(node % per_col);
            return (hi(col, k) - lo(col, k)) * open_k;
        };
        // How many cell steps this gap's own local capacity would allow, floored.
        auto capSteps = [&](size_t node) -> U32
        {
            const F32 steps = capacity(node) * SHELTER_MULT / step_m;
            if (steps <= 0.f) return 0u;
            return (steps >= (F32)STEPS_MAX) ? STEPS_MAX : (U32)steps;
        };

        for (size_t node = 0; node < nodes; ++node)
        {
            if (!getBit(covered, node)) continue;

            // Seeded by any uncovered reachable neighbour: that is an opening.
            bool porch = false;
            touches(node, [&](size_t nnode)
            {
                if (!getBit(covered, nnode) && getBit(reached, nnode)) porch = true;
            });
            if (!porch) continue;

            const U32 c = capSteps(node);
            if (c < 1u) continue;                       // cannot even afford the step in
            const U16 r = (U16)c;                       // 1 + (c - 1) steps left
            if (r <= remaining[node]) continue;
            remaining[node] = r;
            heap.emplace(r, (S32)node);
        }

        while (!heap.empty())
        {
            const U16 b = heap.top().first;
            const size_t node = (size_t)heap.top().second;
            heap.pop();
            if (b != remaining[node]) continue;     // a stronger seed already passed
            if (b <= 1u) continue;                  // arrived with nothing left to hand on

            const U32 left = (U32)b - 1u;           // steps still affordable from here
            touches(node, [&](size_t nnode)
            {
                if (!getBit(covered, nnode)) return;
                const U32 carry = llmin(left, capSteps(nnode));
                if (carry < 1u) return;
                const U16 r = (U16)carry;
                if (r <= remaining[nnode]) return;
                remaining[nnode] = r;
                heap.emplace(r, (S32)nnode);
            });
        }

        // ---- 3. the covered distance: shortest path, not the widest one ----
        // <SS:Nexii> gap_depth carries the BFS LEVEL while the walk runs - the number of
        // cell steps from the opening - and step 4 converts it to decimetres once, as
        // level * cell_m. Accumulating decimetres per step is what rounded 0.25 m to
        // 0.30; one multiply at the end cannot. This also retires the separate float
        // travel plane the walk used to keep.
        queue.clear();
        for (size_t node = 0; node < nodes; ++node)
        {
            if (!remaining[node]) continue;
            bool porch = false;
            touches(node, [&](size_t nnode)
            {
                if (!getBit(covered, nnode) && getBit(reached, nnode)) porch = true;
            });
            if (!porch) continue;
            gap_depth[node] = 1;                    // one step in from the opening
            queue.push_back((S32)node);
        }
        for (size_t head = 0; head < queue.size(); ++head)
        {
            const size_t cur = (size_t)queue[head];
            const U32 next_level = (U32)gap_depth[cur] + 1u;
            if (next_level >= (U32)DEPTH_UNREACHED) continue;
            touches(cur, [&](size_t nnode)
            {
                if (!remaining[nnode]) return;
                if (gap_depth[nnode] != DEPTH_UNREACHED) return;    // BFS: first visit is shortest
                gap_depth[nnode] = (U16)next_level;
                queue.push_back((S32)nnode);
            });
        }

        // ---- 4a. unsurveyed cells, and what their blocking sealed off ----
        // Every gap of an unsurveyed cell is UNKNOWN, including the one that would
        // otherwise run 0..ceiling and read as open sky.
        for (size_t col = 0; col < layer; ++col)
        {
            if (known(col)) continue;
            for (S32 k = 0; k <= max_spans; ++k)
            {
                const size_t node = col * per_col + (size_t)k;
                gap_label[node] = AIR_UNKNOWN;
                gap_depth[node] = DEPTH_UNREACHED;
            }
        }

        // <SS:Nexii> The doubt flood. An unsurveyed column blocks the reachability walk,
        // so a covered space whose only route to open air crosses the survey boundary is
        // never reached - and calling that INTERIOR is a confident answer the field has
        // not earned. It is not proven sealed, only unproven, so the whole unreached
        // pocket that touches the boundary reads UNKNOWN and every consumer keeps its
        // fallback. A pocket that touches no unsurveyed column IS proven sealed by real
        // geometry and stays INTERIOR. gap_label doubles as the visited mark, so this
        // costs no extra plane. [interaction: worldfield_review2.cpp]
        if (surveyed)
        {
            auto columnBordersUnsurveyed = [&](size_t col)
            {
                const S32 x = (S32)(col % (size_t)res);
                const S32 y = (S32)(col / (size_t)res);
                for (S32 d = 0; d < 4; ++d)
                {
                    const S32 nx = x + DX[d], ny = y + DY[d];
                    if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;
                    if (!surveyed[(size_t)ny * (size_t)res + (size_t)nx]) return true;
                }
                return false;
            };

            queue.clear();
            for (size_t col = 0; col < layer; ++col)
            {
                if (!known(col) || !columnBordersUnsurveyed(col)) continue;
                const S32 n = (S32)span_n[col];
                for (S32 k = 0; k <= n; ++k)
                {
                    const size_t node = col * per_col + (size_t)k;
                    if (getBit(reached, node) || !exists(col, k)) continue;
                    if (gap_label[node] == AIR_UNKNOWN) continue;
                    gap_label[node] = AIR_UNKNOWN;
                    queue.push_back((S32)node);
                }
            }
            for (size_t head = 0; head < queue.size(); ++head)
            {
                touches((size_t)queue[head], [&](size_t nnode)
                {
                    if (getBit(reached, nnode)) return;              // the walk got there honestly
                    if (gap_label[nnode] == AIR_UNKNOWN) return;     // already doubted
                    gap_label[nnode] = AIR_UNKNOWN;
                    queue.push_back((S32)nnode);
                });
            }
        }

        // ---- 4b. the labels ----
        for (size_t col = 0; col < layer; ++col)
        {
            if (!known(col)) continue;
            const S32 n = (S32)span_n[col];
            for (S32 k = 0; k <= max_spans; ++k)
            {
                const size_t node = col * per_col + (size_t)k;
                if (k > n || !exists(col, k)) continue;             // no such gap
                if (gap_label[node] == AIR_UNKNOWN) continue;       // the doubt flood got here first
                if (!getBit(reached, node))
                {
                    gap_label[node] = AIR_INTERIOR;                 // sealed by real geometry
                    continue;
                }
                if (!getBit(covered, node))
                {
                    gap_label[node] = AIR_OUTDOORS;
                    gap_depth[node] = 0;
                    continue;
                }
                if (!remaining[node] || gap_depth[node] == DEPTH_UNREACHED)
                {
                    gap_label[node] = AIR_INTERIOR;
                    gap_depth[node] = DEPTH_UNREACHED;
                    continue;
                }

                // The BFS level becomes a real distance here, once: level * cell_m.
                const F32 d = (F32)gap_depth[node] * step_m;
                const U32 dm = (U32)(d * 10.f + 0.5f);
                gap_depth[node] = (U16)llmin(dm, (U32)DEPTH_UNREACHED - 1u);
                // Within one unmultiplied open_k of the LOCAL gap height the sky is
                // still up there: that is outdoors, however much roof stands over it.
                gap_label[node] = (d <= capacity(node)) ? AIR_OUTDOORS : AIR_SHELTERED;
            }
        }
    }
}

#endif
