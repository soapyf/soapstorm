/**
 * @file sssurfacedropcore.h
 * @brief Atmo Magic surface drops: the lattice a drop sits on, and the frame a
 *        drip slides down. The one formula site for both - ssSurfaceNormalF.glsl
 *        transliterates this and nothing else.
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

// <SS:Nexii> Atmo Magic surface drop lattice and drip frame

#ifndef SS_SURFACE_DROP_CORE_H
#define SS_SURFACE_DROP_CORE_H

#include "lldefs.h"
#include <cmath>
#include <cstdint>

// THE ANCHORING RULE for every world-locked lattice a screen-space pass draws.
//
// The pass reconstructs its agent-space position from the gbuffer, so the position it
// hands a lattice is only as good as an F32 round trip through a depth buffer. Two
// separate things go wrong if a lattice is spelled naively as floor(p / pitch):
//
//   1. MAGNITUDE, which is REAL BUT SMALL here, and the number is the point. The
//      reconstruction's absolute error is a few ulp of the largest quantity in the
//      chain: measured 0.11 mm at 3 m and 5.3 mm at 120 m near the agent origin,
//      rising to 4.1 mm at 3 m and 190 mm at 120 m for a point 4 km out
//      (unit_surfaceanchor surfaceanchor_A_roundtrip). Against a 24 mm drop cell that
//      is under a fiftieth of a cell wherever a drop is still more than a pixel wide
//      (~16 m), and the lattice's OWN arithmetic contributes another 0.0009 cells.
//      A camera-anchored spelling was built and measured against it and is NOT an
//      improvement - see anchoredCell. So the static drops are anchored as spelled,
//      which is what the user reports.
//
//   2. DIRECTION, which is the DEFECT. Any lattice whose coordinate is a projection
//      dot(p, axis) multiplies the axis's angular error by |p|. A screen-space
//      derivative normal - cross(dFdx(pos_view), dFdy(pos_view)) over a one-pixel
//      baseline of depth-reconstructed positions - carries 0.03 to 0.24 rad of it
//      (unit_surfaceanchor surfaceanchor_C_control_derivative_axis), so at agent-space
//      distances the projected coordinate moves by METRES: measured 4.06 m (68 drip
//      cells) for a wall 141 m from the agent origin and 384 m (6408 cells) at 1 km,
//      re-rolling the whole drip pattern every time the camera moved. Cured by making
//      the axis a bit-stable function of the SURFACE: an azimuth quantised to a fixed
//      set of directions, taken from the gbuffer normal (whose own error is of order
//      1e-4 rad, three orders finer than a quantisation step) rather than from a
//      derivative. Measured after: 0.0078 m, 0.13 of a cell, zero cell flips.
//
// The rule the two share is the deck's air-frame rule one level down: keep the
// high-frequency coordinate small, and never let a per-pixel estimate scale with a
// world-sized number. Which of the two bites is a measurement, not a guess.

namespace SSSurfaceDrop
{
    // GLSL fract().
    inline F32 fract(F32 x) { return x - std::floor(x); }

    // The drop lattice pitch in metres. sz is ssDropLaw's size term (0.6 at the first
    // spits, 1.6 in a downpour); scale is the user's SSAtmoSurfaceDropScale.
    inline F32 pitchM(F32 sz, F32 scale)
    {
        const F32 p = 0.04f * sz * scale;
        return p > 1.0e-4f ? p : 1.0e-4f;
    }

    // The lattice hash. LOCKSTEP with ssFieldHash in ssSurfaceFieldF.glsl - an integer bit
    // mix, because every caller feeds it an integer CELL INDEX and those reach 10^3 to
    // 10^4, which put the old fract(sin(dot(p, k)) * 43758.5453) spelling's sin() at
    // arguments of 10^5 to 10^6 radians: past where a GPU sin() is specified to hold any
    // precision, and where the F32 argument is already quantised to a thirty-second of a
    // radian. Same constants as ssPuddleLatHash/ssStateHash, the fork's existing hash.
    inline F32 hash(F32 x, F32 y)
    {
        const S32 cx = (S32)std::floor(x);
        const S32 cy = (S32)std::floor(y);
        U32 h = (U32)cx * 374761393u + (U32)cy * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (F32)((h ^ (h >> 16)) & 0xffffffu) / 16777216.f;
    }

    // ------------------------------------------------------------------
    // Rule 1: the lattice cell (and the anchored spelling that did not help).
    // ------------------------------------------------------------------

    // A lattice cell: the WORLD cell index the hash is taken at, and the sub-cell
    // coordinate within it.
    struct Cell { F32 ix, iy, lx, ly; };

    // The shipping spelling: the world cell index and the sub-cell coordinate straight
    // from p / pitch. LOCKSTEP with ssSurfaceNormalF.glsl's drop_cell_uv.
    inline Cell cellAt(F32 px, F32 py, F32 pitch_m)
    {
        const F32 ux = px / pitch_m;
        const F32 uy = py / pitch_m;
        const F32 fx = std::floor(ux);
        const F32 fy = std::floor(uy);
        Cell c;
        c.ix = fx; c.iy = fy; c.lx = ux - fx; c.ly = uy - fy;
        return c;
    }

    // The anchor's own cell index, from the camera's agent-space position.
    inline F32 anchorIndex(F32 cam_component, F32 pitch_m)
    {
        return std::floor(cam_component / pitch_m);
    }

    // The camera-anchored alternative, KEPT AND NOT SHIPPED, so the next reader does not
    // re-derive it. The idea is sound and is the right cure elsewhere (the deck's air
    // frame): measure the sub-cell coordinate from a lattice-aligned point near the
    // camera and recover the world index as an integer sum, so the high-frequency term
    // never carries the agent-space magnitude. Measured on THIS lattice it is not an
    // improvement - the anchor's own aix * pitch_m multiply reintroduces the same
    // rounding, at 0.0052 cells of sub-cell error at 4 km against 0.00089 for the plain
    // spelling (unit_surfaceanchor surfaceanchor_B_control_unanchored_quantum) - and
    // both are two orders below the depth buffer's own contribution. See rule 1 above:
    // the magnitude term is real but small here; the DIRECTION term is the defect.
    inline Cell anchoredCell(F32 px, F32 py, F32 aix, F32 aiy, F32 pitch_m)
    {
        const F32 ax = aix * pitch_m;
        const F32 ay = aiy * pitch_m;
        const F32 ux = (px - ax) / pitch_m;
        const F32 uy = (py - ay) / pitch_m;
        const F32 fx = std::floor(ux);
        const F32 fy = std::floor(uy);
        Cell c;
        c.ix = aix + fx;
        c.iy = aiy + fy;
        c.lx = ux - fx;
        c.ly = uy - fy;
        return c;
    }

    struct Sample
    {
        F32 cx, cy;      // the cell whose drop this fragment landed in, if any
        F32 lx, ly;      // the sub-cell coordinate within the fragment's own cell, 0..1
        F32 presence;    // the fragment's own cell's occupancy roll, 0..1
        F32 height;      // the spherical cap's z at this point, 0 outside every cap
        F32 tiltx, tilty;// the cap's tangential tilt, 0 outside every cap
    };

    // ------------------------------------------------------------------
    // Rule 4: a drop is not a stamp on a grid.
    //
    // Spelled one drop per cell, radius 0.25..0.45 of a cell and offset clamped inside
    // it, the field reads as a stamped lattice: identical domes on an obvious pitch,
    // one per cell, none of them ever touching or overlapping. The cloud deck's Tier B
    // macro bodies had the same complaint and the fix there is the one followed here
    // (ssdeckmacrocore.h): jitter, a hashed size, and an AREA-CONSERVING occupancy so
    // the surface does not get wetter or drier - only less regular.
    //
    //   1. The drop may leave its cell. The offset is uniform over the WHOLE cell and
    //      the fragment tests its own cell and the eight around it, so caps overlap cell
    //      boundaries, sit at any spacing from touching to far apart, and are never cut.
    //      That also retires the straight edge across each dome's base: it was fract()
    //      clipping a cap that reached past the cell line (offset to 0.85 plus radius to
    //      0.45 = 1.30), and with the neighbour tests there is nothing to clip.
    //   2. The radius spread widens from 1.8x to 4.8x, so no two drops are the same size.
    //   3. AREA_NORM keeps the mean covered area EXACTLY what it was. The covered
    //      fraction is occupancy * pi * E[r^2]; widening the radius raises E[r^2], so the
    //      occupancy is divided by the same ratio. Without it a wider spread would wet
    //      the surface more at the same rain rate, which is a look change disguised as a
    //      variance fix.
    // ------------------------------------------------------------------

    // The old radius band, kept because AREA_NORM is defined against it.
    const F32 DROP_R_OLD_LO = 0.25f;
    const F32 DROP_R_OLD_HI = 0.45f;
    // The new one. LOCKSTEP with ssSurfaceNormalF.glsl's item-1 block.
    const F32 DROP_R_LO = 0.12f;
    const F32 DROP_R_HI = 0.58f;

    // E[r^2] for a radius uniform on [lo, hi]: (lo^2 + lo*hi + hi^2) / 3.
    inline F32 meanRadiusSq(F32 lo, F32 hi) { return (lo * lo + lo * hi + hi * hi) / 3.f; }

    // The occupancy scale that holds the mean covered area fixed across the widening.
    inline F32 areaNorm()
    {
        return meanRadiusSq(DROP_R_OLD_LO, DROP_R_OLD_HI) / meanRadiusSq(DROP_R_LO, DROP_R_HI);
    }

    // One cell's drop, as the 3x3 walk reads it.
    struct CellDrop { F32 ox, oy, radius, presence; };

    inline CellDrop cellDrop(F32 cx, F32 cy)
    {
        CellDrop d;
        d.presence = hash(cx, cy);
        d.ox = hash(cx + 11.7f, cy + 3.1f);
        d.oy = hash(cx + 3.7f, cy + 17.9f);
        d.radius = DROP_R_LO + (DROP_R_HI - DROP_R_LO) * hash(cx + 29.3f, cy + 7.7f);
        return d;
    }

    // The drop covering an agent-space XY, searched over the fragment's own cell and the
    // eight around it. gate is the occupancy the pass has resolved for this fragment;
    // the area normalisation is applied here so no call site can forget it.
    // Transliterated by ssSurfaceNormalF.glsl's item-1 block.
    inline Sample sampleAt(F32 x, F32 y, F32 pitch_m, F32 gate)
    {
        const Cell c = cellAt(x, y, pitch_m);
        Sample s;
        s.cx = c.ix; s.cy = c.iy; s.lx = c.lx; s.ly = c.ly;
        s.presence = hash(c.ix, c.iy);
        s.height = 0.f; s.tiltx = 0.f; s.tilty = 0.f;
        const F32 occ = llclamp(gate * areaNorm(), 0.f, 1.f);
        for (S32 oy = -1; oy <= 1; ++oy)
            for (S32 ox = -1; ox <= 1; ++ox)
            {
                const F32 nx = c.ix + (F32)ox;
                const F32 ny = c.iy + (F32)oy;
                const CellDrop d = cellDrop(nx, ny);
                if (d.presence >= occ) continue;
                const F32 dx = (c.lx - (F32)ox - d.ox) / d.radius;
                const F32 dy = (c.ly - (F32)oy - d.oy) / d.radius;
                const F32 d2 = dx * dx + dy * dy;
                if (d2 >= 1.f) continue;
                const F32 h = std::sqrt(1.f - d2 * 0.81f);
                if (h > s.height)
                {
                    s.height = h;
                    s.tiltx = -dx * 0.9f;
                    s.tilty = -dy * 0.9f;
                    s.cx = nx;
                    s.cy = ny;
                }
            }
        return s;
    }

    // ------------------------------------------------------------------
    // Rule 2: the drip frame.
    // ------------------------------------------------------------------

    // Cell size, metres. LOCKSTEP with ssSurfaceNormalF.glsl's drip_cell_uv.
    const F32 DRIP_CELL_U_M = 0.06f;
    const F32 DRIP_CELL_V_M = 0.30f;

    // How many horizontal directions the drip axis may take. 16 is fine enough that a
    // wall's lattice is stretched by at most 1/cos(11.25 deg) = 1.02 against its true
    // tangent, and coarse enough that the gbuffer normal's own encoding error (of order
    // 1e-4 rad) can never reach a step boundary. It is NOT coarse enough to absorb a
    // screen-space derivative normal, which is the point: the axis comes from the
    // gbuffer normal, and the quantisation is what makes it BIT-stable rather than
    // merely accurate.
    const S32 DRIP_AZIMUTHS = 16;

    // The horizontal axis a drip lattice runs along, from the surface normal. The
    // tangent is cross(up, n) = (-n.y, n.x, 0); its azimuth is quantised, and the axis
    // is then rebuilt from the quantised angle alone - so two fragments on one wall get
    // a bit-identical vector however the camera is placed.
    inline void dripAxis(F32 nx, F32 ny, F32 out[3])
    {
        const F32 TWO_PI = 6.28318530718f;
        const F32 step = TWO_PI / (F32)DRIP_AZIMUTHS;
        // cross(up, n) = (-n.y, n.x, 0). It vanishes on a horizontal surface, where
        // atan2(0, 0) is undefined in GLSL - fall back to world X on both sides.
        F32 tx = -ny, ty = nx;
        if (tx * tx + ty * ty < 1.0e-12f) { tx = 1.f; ty = 0.f; }
        const F32 az = std::atan2(ty, tx);
        const F32 q = std::floor(az / step + 0.5f) * step;
        out[0] = std::cos(q);
        out[1] = std::sin(q);
        out[2] = 0.f;
    }

    // The along-wall coordinate in metres, measured in the CAMERA-RELATIVE frame: the
    // per-fragment term is a difference of nearby numbers, and the whole-lattice offset
    // is one per-frame constant, so a rounding error in it shifts the entire lattice
    // coherently by microns instead of shearing it per fragment.
    inline F32 dripU(F32 px, F32 py, F32 pz, F32 cx, F32 cy, F32 cz, const F32 t[3])
    {
        const F32 rel = (px - cx) * t[0] + (py - cy) * t[1] + (pz - cz) * t[2];
        const F32 base = cx * t[0] + cy * t[1] + cz * t[2];
        return rel + base;
    }

    // ------------------------------------------------------------------
    // Rule 3: a runnel is COHERENT, which is a statement about frequency.
    //
    // The drip's own features - a cap of radius 0.008 * scale metres and a trail of
    // sigma 0.004 m - are seven to fifteen times finer than the 0.06 x 0.30 m cell they
    // live in. An LOD fade measured on CELLS therefore leaves the features drawn at full
    // strength while a pixel already covers several of them, and a hard cap sampled that
    // far below Nyquist is a scatter of isolated dots rather than a streak: the speckle.
    // Both fixes are the same rule - every length the pattern is drawn at is compared
    // against the pixel's own world footprint, and any kernel narrower than the footprint
    // is widened to it with its amplitude divided by the widening so the integral holds.
    // ------------------------------------------------------------------

    const F32 DRIP_TRAIL_SIGMA_M = 0.004f;
    const F32 DRIP_CAP_R_PER_SCALE = 0.008f;

    inline F32 capRadiusM(F32 scale) { return DRIP_CAP_R_PER_SCALE * scale; }

    // GLSL smoothstep.
    inline F32 smoothstep(F32 e0, F32 e1, F32 x)
    {
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // How much of the drip lattice survives at a pixel whose world footprint is px_m.
    // LOCKSTEP with ssSurfaceNormalF.glsl's drip_reach.
    inline F32 dripReach(F32 px_m, F32 scale)
    {
        const F32 feature = llmax(capRadiusM(scale), 1.0e-4f);
        return 1.f - smoothstep(0.5f, 3.f, px_m / feature);
    }

    // The sliding cap's mask: a footprint-wide band at its rim instead of a hard cut.
    inline F32 capMask(F32 caplen, F32 cap_r, F32 px_m)
    {
        const F32 aa = llmax(px_m, 1.0e-5f);
        const F32 edge = llmax(aa / cap_r, 0.02f);
        return 1.f - smoothstep(1.f - edge, 1.f + edge, caplen);
    }

    // The trailing ridge's mask. du_m is the across-flow offset in metres, dv_above the
    // along-flow distance behind the drop as a fraction of the cell.
    inline F32 trailMask(F32 du_m, F32 dv_above, F32 px_m)
    {
        const F32 sigma = llmax(DRIP_TRAIL_SIGMA_M, llmax(px_m, 0.f));
        const F32 g = std::exp(-(du_m * du_m) / (2.f * sigma * sigma))
                      * (DRIP_TRAIL_SIGMA_M / sigma);
        const F32 fade = 1.f - smoothstep(0.f, 0.6f, dv_above);
        return g * fade;
    }

    // The whole drip weight at a point inside a cell, as the shader assembles it: the cap
    // where it covers, the trail behind it otherwise. (lx, ly) are cell fractions,
    // (ox, oy) the drop's position in the cell.
    inline F32 dripWeightAt(F32 lx, F32 ly, F32 ox, F32 oy, F32 scale, F32 px_m)
    {
        const F32 cap_r = capRadiusM(scale);
        const F32 ux_m = (lx - ox) * DRIP_CELL_U_M;
        const F32 vy_m = (ly - oy) * DRIP_CELL_V_M;
        const F32 caplen = std::sqrt(ux_m * ux_m + vy_m * vy_m) / cap_r;
        const F32 cm = capMask(caplen, cap_r, px_m);
        const F32 dv_above = fract(ly - oy);
        const F32 tm = trailMask(ux_m, dv_above, px_m);
        return tm > cm ? tm : cm;
    }
}

#endif
