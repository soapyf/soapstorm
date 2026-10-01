/**
 * @file sssurfacefield.cpp
 * @brief See sssurfacefield.h.
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

#include "llviewerprecompiledheaders.h"

#include "sssurfacefield.h"

#include "ssatmomagic.h"
#include "ssavatarwet.h"
#include "ssgranular.h"
#include "ssvolcloud.h"
#include "sswindflow.h"
#include "ssprecippreset.h"
#include "ssprecipitation.h"
#include "ssrainshadow.h"
#include "ssworldfield.h"

#include "llappviewer.h"
#include "llenvironment.h"
#include "llfasttimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llsettingswater.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include <glm/gtc/type_ptr.hpp>
#include <glm/mat4x4.hpp>

// Smooth value noise over metres - the static mask that decides where puddles are allowed to stand.
static F32 ssPuddleMaskNoise(F32 mx, F32 my, F32 scale_m)
{
    auto latticeHash = [](S32 cx, S32 cy)
    {
        U32 h = (U32)cx * 374761393u + (U32)cy * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (F32)((h ^ (h >> 16)) & 0xffffffu) / (F32)0x1000000;
    };
    const F32 fx = mx / llmax(scale_m, 1.f);
    const F32 fy = my / llmax(scale_m, 1.f);
    const S32 ix = (S32)floorf(fx);
    const S32 iy = (S32)floorf(fy);
    const F32 tx = fx - (F32)ix;
    const F32 ty = fy - (F32)iy;
    const F32 sx = tx * tx * (3.f - 2.f * tx);
    const F32 sy = ty * ty * (3.f - 2.f * ty);
    return lerp(lerp(latticeHash(ix, iy),     latticeHash(ix + 1, iy),     sx),
                lerp(latticeHash(ix, iy + 1), latticeHash(ix + 1, iy + 1), sx), sy);
}

// <SS:Nexii> Look equality within tolerance - a preset whose look already matches the mix's mA must start no crossfade (doc sec 2). Every field, not just the tint, so a preset that only bumped depthFull still counts as "different".
static bool looksEqual(const SSSurfaceState::LiquidLook& a, const SSSurfaceState::LiquidLook& b)
{
    const F32 eps = 1.0e-4f;
    return fabsf(a.mTint.r - b.mTint.r) < eps && fabsf(a.mTint.g - b.mTint.g) < eps
        && fabsf(a.mTint.b - b.mTint.b) < eps && fabsf(a.mOpacity - b.mOpacity) < eps
        && fabsf(a.mStain - b.mStain) < eps && fabsf(a.mMetal - b.mMetal) < eps;
}

static bool looksEqual(const SSSurfaceState::DepositLook& a, const SSSurfaceState::DepositLook& b)
{
    const F32 eps = 1.0e-4f;
    return fabsf(a.mTint.r - b.mTint.r) < eps && fabsf(a.mTint.g - b.mTint.g) < eps
        && fabsf(a.mTint.b - b.mTint.b) < eps && fabsf(a.mSparkle - b.mSparkle) < eps
        && fabsf(a.mTranslucency - b.mTranslucency) < eps && fabsf(a.mDepthFull - b.mDepthFull) < eps
        && fabsf(a.mWash - b.mWash) < eps && fabsf(a.mMelts - b.mMelts) < eps;
}

extern bool gCubeSnapshot;

static const F32 TICK_INTERVAL   = 0.25f;
static const F32 MAX_TICK_DT     = 8.f;
static const F64 FIELD_KEEP      = 20.0;
static const size_t MAX_FIELDS   = 4;

static const F32 REBUILD_DZ      = 0.35f;

static const F32 SLOPE_STEP_MAX  = 3.f;

static const S32 WINDOW_RES = 256;
static const F32 WINDOW_NO_SURFACE = -1.0e6f;

static const F32 FALLBACK_DRY   = 0.002f;
static const F32 FALLBACK_MELT  = 0.0000045f;
static const F32 FALLBACK_DRAIN = 0.0001f;

// <SS:Nexii> The melt's live-warmth scale: whether it SNOWS is the preset's call (made upstream from temperature+convection), but how fast the pack goes answers the day's actual temperature - the authored melt rate means "a mild +10C day", real warmth runs up to double it, and sub-zero air holds the pack with only a whisper of sublimation so an abandoned drift still leaves eventually. Wetting, drying and drainage stay temperature-blind: a cold puddle is still a puddle.
static const F32 MELT_FULL_C      = 10.f;
static const F32 MELT_WARM_MAX    = 2.f;
static const F32 MELT_SUBLIMATION = 0.02f;

// <SS:Nexii> The tick lands every quarter second, so the whole cost of one step shows up on one frame. Split per stage: geometry rebuild, ground-flow sample, the transport steps, the shed cursor and the window rebuild all have completely different fixes, and a single summed handle cannot tell them apart. The upload is timed apart from the fill it uploads because only the fill can ever move off this thread.
static LLTrace::BlockTimerStatHandle FTM_SS_SURFACE("Atmo Magic Surface Field");
static LLTrace::BlockTimerStatHandle FTM_SS_SURFACE_GEOM("Surface Geometry");
static LLTrace::BlockTimerStatHandle FTM_SS_SURFACE_FLOW("Surface Ground Flow");
static LLTrace::BlockTimerStatHandle FTM_SS_SURFACE_TICK("Surface Tick");
static LLTrace::BlockTimerStatHandle FTM_SS_SURFACE_SHED("Surface Shed");
static LLTrace::BlockTimerStatHandle FTM_SS_SURFACE_WINDOW("Surface Window");
static LLTrace::BlockTimerStatHandle FTM_SS_SURFACE_UPLOAD("Surface Upload");

// Drops all fields, geometry and GL - full rebuild on demand.
void SSSurfaceField::clear()
{
    mFields.clear();
    mWindowValid = false;
    mLastStep = -1.0;
    mPeakWet = mPeakSnow = mPeakPuddle = 0.f;
    mPeakIce = mPeakFrost = mPeakStain = 0.f;
    mPeakWetGain = mPeakDepositGain = 0.f;
    mPeakWetPresent = mPeakDepositPresent = 0.f;
    // <SS:Nexii> A full rebuild resets the looks and rings too - nothing left on the ground for them to describe.
    mLiquidMix = SSSurfaceState::Mix<SSSurfaceState::LiquidLook>();
    mDepositMix = SSSurfaceState::Mix<SSSurfaceState::DepositLook>();
    mRings = SSSurfaceState::RingBuffer();
}

static const F32 SLOPE_RUN_FULL = 0.85f;

static const F32 GEOM_EDGE_DROP = 1.5f;

static const F32 SHED_MERGE = 12.f;

static const F32 SHED_STREAM_MIN  = 8.f;
static const F32 SHED_STREAM_FULL = 24.f;

static const F32 SHED_MAX_RATE = 40.f;
static const S32 SHED_MAX_BURST = 4;
static const S32 SHED_VISIT_PER_FRAME = 96;

static const S32 DRIP_BUDGET = 220;

static const F32 STREAM_SPAN_MAX = 24.f;

static const S32 GEOM_RES = 128;

static const F32 GEOM_WALL_SLOPE = 1.2f;

static const F32 GEOM_POOL_SLOPE = 0.06f;

static const F32 GEOM_FLAT_NOISE = 0.02f;

// Turns a shadow-map surface grid into drainage geometry: cells, slopes, flow targets and shelter edge cells.
void SSSurfaceField::buildGeometry(const SSRainShadowMap::SurfaceGrid& grid, Geometry& out)
{
    const S32 n = grid.mN;
    const size_t count = (size_t)n * n;

    out.mN = n;
    out.mCell = grid.mCell;
    out.mGeomSerial = grid.mGeomSerial;
    out.mZ = grid.mZ;
    out.mFlags = grid.mFlags;
    out.mAbove = grid.mAbove;
    out.mSlopeX.assign(count, 0.f);
    out.mSlopeY.assign(count, 0.f);
    out.mSlope.assign(count, 0.f);
    out.mPool.assign(count, 0);
    out.mEdge.assign(count, 0);
    out.mEdgeX.assign(count, 0.f);
    out.mEdgeY.assign(count, 0.f);
    out.mEdgeCells.clear();

    const F32 cell = grid.mCell;
    if (n < 3 || cell <= 0.f) return;

    const F32 step_max = cell * 3.f;

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;
            if (!out.solid(i) || out.water(i)) continue;

            const F32 z0 = out.mZ[i];

            auto gradAlong = [&](S32 lo, S32 hi, bool have_lo, bool have_hi) -> F32
            {
                const bool ok_lo = have_lo && out.solid((size_t)lo)
                    && fabsf(out.mZ[(size_t)lo] - z0) < step_max;
                const bool ok_hi = have_hi && out.solid((size_t)hi)
                    && fabsf(out.mZ[(size_t)hi] - z0) < step_max;

                if (ok_lo && ok_hi) return (out.mZ[(size_t)hi] - out.mZ[(size_t)lo]) * 0.5f / cell;
                if (ok_hi) return (out.mZ[(size_t)hi] - z0) / cell;
                if (ok_lo) return (z0 - out.mZ[(size_t)lo]) / cell;
                return 0.f;
            };

            const F32 gx = gradAlong((S32)i - 1, (S32)i + 1, x > 0, x < n - 1);
            const F32 gy = gradAlong((S32)i - n, (S32)i + n, y > 0, y < n - 1);

            const F32 mag = sqrtf(gx * gx + gy * gy);
            out.mSlope[i] = mag;
            if (mag > 0.0001f)
            {
                out.mSlopeX[i] = -gx / mag;
                out.mSlopeY[i] = -gy / mag;
            }

            if (mag > GEOM_POOL_SLOPE) continue;

            bool dips = true;
            for (S32 dy = -1; dy <= 1 && dips; ++dy)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    if (dx == 0 && dy == 0) continue;
                    const S32 nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

                    const size_t ni = (size_t)ny * n + nx;
                    if (!out.solid(ni)) continue;
                    if (fabsf(out.mZ[ni] - z0) > step_max) continue;

                    if (out.mZ[ni] < z0 - GEOM_FLAT_NOISE)
                    {
                        dips = false;
                        break;
                    }
                }
            }

            out.mPool[i] = dips ? 1 : 0;
        }
    }

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;
            if (!out.solid(i) || out.water(i)) continue;

            const F32 z0 = out.mZ[i];
            F32 ox = 0.f, oy = 0.f;

            static const S32 DX[4] = { 1, -1, 0, 0 };
            static const S32 DY[4] = { 0, 0, 1, -1 };
            for (S32 d = 0; d < 4; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

                const size_t ni = (size_t)ny * n + nx;
                const bool open = !out.solid(ni);
                const bool below = out.solid(ni) && (z0 - out.mZ[ni]) > GEOM_EDGE_DROP;
                if (!open && !below) continue;

                ox += (F32)DX[d];
                oy += (F32)DY[d];
            }

            const F32 len = sqrtf(ox * ox + oy * oy);
            if (len < 0.0001f) continue;

            // A drip line is an architectural thing: water gathering along a
            // roof lip and coming off it as drops. A terrain ledge drops just
            // as sharply and is not that - rain runs down a hillside as a
            // sheet, and hanging drip curtains off every terraced cliff is
            // what the capture's height-above-terrain channel exists to stop.
            // A lip has to be standing structure, not ground that happens to
            // step down.
            static LLCachedControl<F32> edge_min_above(gSavedSettings, "SSAtmoRunoffEdgeMinAbove", 1.f);
            if (out.above(i) < llmax((F32)edge_min_above, 0.f)) continue;

            out.mEdge[i] = 1;
            out.mEdgeX[i] = ox / len;
            out.mEdgeY[i] = oy / len;
            out.mEdgeCells.push_back((S32)i);
        }
    }
}

// <SS:Nexii> The liquid runoff network: the DRAINAGE_NETWORK channel at its
// own finer resolution, traced over the same source grid the wet field reads
// at GEOM_RES. buildDrainage fills the surface's depressions, directs D8 down
// the filled surface and accumulates catchment with the eave rule terminating
// flow at the capture's discontinuities; this pass picks the lip cells that
// termination leaves holding catchment, floods them into runs - the connected
// lines water actually leaves a roof along - and refines each lip point
// against the world field's full-resolution column tops so a stream or drip
// leaves the edge of the object rather than the centre of whichever cell
// happened to straddle it. Runs carry everything the shed needs: summed
// catchment, the mean way the water leaves, the span to cut curtain slots
// across, and a key that survives retraces which keep the run's biggest
// feeder, so a run's reservoir outlives a prim edit next door.
void SSSurfaceField::buildRunoff(U64 region_handle, Geometry& out, SSWorldField* field)
{
    static const F32 EAVE_DROP_M  = 0.75f;  // at least a storey-step of fall
    static const F32 EAVE_SLOPE   = 2.0f;   // steeper than any roof pitch (~63 deg)
    static const F32 RUN_Z_JOIN   = 2.f;    // members of a run sit within this of each other
    static const F32 RUN_DIR_COS  = 0.5f;   // ...and shed within 60 degrees of the same way
    static const S32 RUN_MAX        = 128;
    static const S32 RUN_MEMBER_MAX = 64;

    out.mRunoff = Geometry::Runoff();

    // The network's own resolution - finer than the wet field's lattice by
    // default, because an eave is a line and 2 m cells draw it as dots. Both
    // sources serve the same SurfaceGrid contract; the world field's 0.25 m
    // store additionally gives the lip refinement below full-resolution
    // column tops to walk against.
    static LLCachedControl<S32> res_setting(gSavedSettings, "SSAtmoRunoffRes", 256);
    const S32 rn = llclamp((S32)res_setting, 64, 512);

    SSRainShadowMap::SurfaceGrid rgrid;
    const bool have = field ? field->buildSurfaceGrid(region_handle, rn, rgrid)
                            : SSRainShadowMap::getInstance()->buildSurfaceGrid(region_handle, rn, rgrid);
    if (!have) return;

    const S32 n = rgrid.mN;
    const F32 cell = rgrid.mCell;
    if (n < 3 || cell <= 0.f) return;

    SSWorldField::Drainage drain;
    if (!SSWorldField::buildDrainage(rgrid, drain)) return;

    Geometry::Runoff& ro = out.mRunoff;
    ro.mGeomSerial = rgrid.mGeomSerial;
    ro.mN = n;
    ro.mCell = cell;
    ro.mCatch = std::move(drain.mCatch);

    static LLCachedControl<F32> edge_min_above(gSavedSettings, "SSAtmoRunoffEdgeMinAbove", 1.f);
    const F32 min_above = llmax((F32)edge_min_above, 0.f);

    // Lips, at the network's own resolution. The same two questions the wet
    // field's coarse edge pass asks - is a neighbour open air, or is it a
    // discontinuity the water leaves the surface by - with the eave pair
    // (steep enough AND tall enough) the finer cells can actually resolve.
    static const S32 DX[4] = { 1, -1, 0, 0 };
    static const S32 DY[4] = { 0, 0, 1, -1 };

    std::vector<LLVector3> out_dir;
    out_dir.assign((size_t)n * n, LLVector3::zero);

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;
            const U8 f = rgrid.mFlags[i];
            if (f == 0 || (f & SSRainShadowMap::SURF_WATER) != 0) continue;
            if (rgrid.mZ[i] <= -FLT_MAX * 0.5f) continue;
            if (rgrid.mAbove[i] < min_above) continue;

            const F32 z0 = rgrid.mZ[i];
            F32 ox = 0.f, oy = 0.f;

            for (S32 d = 0; d < 4; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

                const size_t ni = (size_t)ny * n + nx;
                const U8 nf = rgrid.mFlags[ni];
                const bool open = (nf == 0) || rgrid.mZ[ni] <= -FLT_MAX * 0.5f;
                const F32 drop = z0 - rgrid.mZ[ni];
                const bool below = !open && (nf & SSRainShadowMap::SURF_WATER) == 0
                    && drop >= EAVE_DROP_M && drop >= EAVE_SLOPE * cell;
                if (!open && !below) continue;

                ox += (F32)DX[d];
                oy += (F32)DY[d];
            }

            const F32 len = sqrtf(ox * ox + oy * oy);
            if (len < 0.0001f) continue;

            out_dir[i].setVec(ox / len, oy / len, 0.f);
            ro.mEdgeCells.push_back((S32)i);
        }
    }

    if (ro.mEdgeCells.empty()) return;

    // Runs: flood-fill the lips into the lines water leaves along. Members
    // join when they touch, sit at much the same height, and shed the same
    // way - a run is split where its direction turns, so one run is one
    // straightish stretch of eave and a slot cut across it is one curtain.
    const size_t cells = (size_t)n * n;
    std::vector<U8> visited(cells, 0);
    std::vector<S32> queue;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    const LLVector3 origin = regionp ? regionp->getOriginAgent() : LLVector3::zero;

    for (const S32 seed_cell : ro.mEdgeCells)
    {
        if (visited[(size_t)seed_cell]) continue;

        queue.clear();
        queue.push_back(seed_cell);
        visited[(size_t)seed_cell] = 1;

        std::vector<S32> members;
        while (!queue.empty())
        {
            const S32 c = queue.back();
            queue.pop_back();
            members.push_back(c);

            const size_t ui = (size_t)c;
            const F32 z0 = rgrid.mZ[ui];
            const LLVector3& d0 = out_dir[ui];

            const S32 cx = c % n;
            const S32 cy = c / n;
            for (S32 dy = -1; dy <= 1; ++dy)
            {
                for (S32 dx = -1; dx <= 1; ++dx)
                {
                    if (dx == 0 && dy == 0) continue;
                    const S32 nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

                    const size_t ni = (size_t)ny * n + nx;
                    if (visited[ni] || out_dir[ni].magVecSquared() < 0.0001f) continue;
                    if (fabsf(rgrid.mZ[ni] - z0) > RUN_Z_JOIN) continue;
                    if (out_dir[ni] * d0 < RUN_DIR_COS) continue;

                    visited[ni] = 1;
                    queue.push_back((S32)ni);
                }
            }
        }

        Geometry::Run run;
        F32 total_catch = 0.f;
        F32 sx = 0.f, sy = 0.f;
        F32 cx_sum = 0.f, cy_sum = 0.f, cz_sum = 0.f;

        for (const S32 c : members)
        {
            const size_t ui = (size_t)c;
            total_catch += ro.mCatch[ui];
            sx += out_dir[ui].mV[VX];
            sy += out_dir[ui].mV[VY];
            cx_sum += ((F32)(c % n) + 0.5f) * cell;
            cy_sum += ((F32)(c / n) + 0.5f) * cell;
            cz_sum += rgrid.mZ[ui];
        }

        if (total_catch <= 0.f) continue;

        const F32 out_len = sqrtf(sx * sx + sy * sy);
        if (out_len < 0.0001f) continue;
        run.mOut.setVec(sx / out_len, sy / out_len, 0.f);
        run.mCatch = total_catch;

        // Along the run: the axis the streams measure their pitch by is
        // z cross out - the convention the stream renderer reads the slope
        // back in. Members order themselves along it, endpoints to endpoints.
        const LLVector3 axis(-run.mOut.mV[VY], run.mOut.mV[VX], 0.f);

        std::sort(members.begin(), members.end(),
                  [&](S32 a, S32 b)
                  {
                      const F32 pa = ((F32)(a % n) + 0.5f) * cell * axis.mV[VX]
                                   + ((F32)(a / n) + 0.5f) * cell * axis.mV[VY];
                      const F32 pb = ((F32)(b % n) + 0.5f) * cell * axis.mV[VX]
                                   + ((F32)(b / n) + 0.5f) * cell * axis.mV[VY];
                      if (pa != pb) return pa < pb;
                      return a < b;
                  });

        run.mCentroid.setVec(cx_sum / (F32)members.size(),
                             cy_sum / (F32)members.size(),
                             cz_sum / (F32)members.size());

        // The key is the run's biggest feeder - the cell the most catchment
        // drains through. A retrace that keeps the roof keeps the feeder, so
        // the reservoir the shed holds under this key survives the edit.
        U32 key = (U32)members.front();
        F32 best_catch = -1.f;
        for (const S32 c : members)
        {
            const F32 catch_c = ro.mCatch[(size_t)c];
            if (catch_c > best_catch)
            {
                best_catch = catch_c;
                key = (U32)c;
            }
        }
        run.mKey = key;

        // Thinning keeps the whole line covered: stride, never truncate, and
        // always the two ends a slot's pitch is measured between.
        if ((S32)members.size() > RUN_MEMBER_MAX)
        {
            const S32 stride = (S32)((members.size() + RUN_MEMBER_MAX - 1) / RUN_MEMBER_MAX);
            std::vector<S32> thinned;
            thinned.reserve(members.size() / stride + 2);
            for (size_t k = 0; k < members.size(); k += (size_t)stride)
            {
                thinned.push_back(members[k]);
            }
            if (thinned.back() != members.back()) thinned.push_back(members.back());
            members.swap(thinned);
        }

        run.mCells = std::move(members);

        // Refine the lip points against the world field's own resolution -
        // walk outward from each cell centre at storey-step granularity until
        // the surface falls away, and leave the drip at the last point still
        // standing on it. Without the field (the rain shadow source), the
        // cell centres stand in, as they always have.
        run.mLips.reserve(run.mCells.size());
        const F32 refine_step = field ? 0.25f : cell;
        for (const S32 c : run.mCells)
        {
            const size_t ui = (size_t)c;
            const F32 lx = ((F32)(c % n) + 0.5f) * cell;
            const F32 ly = ((F32)(c / n) + 0.5f) * cell;
            F32 z = rgrid.mZ[ui];
            F32 bx = lx, by = ly;

            const LLVector3& dir = out_dir[ui];
            for (F32 t = refine_step; t <= cell + refine_step * 0.5f; t += refine_step)
            {
                const LLVector3 probe = origin + LLVector3(lx + dir.mV[VX] * t,
                                                           ly + dir.mV[VY] * t,
                                                           0.f);
                F32 probe_z = 0.f;
                U8 probe_flags = 0;
                if (!field || !field->surfaceTop(probe, probe_z, probe_flags)) break;
                if (probe_z < z - 0.05f) break;

                bx = lx + dir.mV[VX] * t;
                by = ly + dir.mV[VY] * t;
                z = probe_z;
            }

            run.mLips.emplace_back(bx, by, z);
        }

        ro.mRuns.push_back(std::move(run));
    }

    // The budget goes to the biggest gutters first.
    std::sort(ro.mRuns.begin(), ro.mRuns.end(),
              [](const Geometry::Run& a, const Geometry::Run& b)
              {
                  return a.mCatch > b.mCatch;
              });
    if ((S32)ro.mRuns.size() > RUN_MAX)
    {
        ro.mRuns.resize(RUN_MAX);
    }
}

// Rebuilds geometry for tiles whose captures changed - the retrace gate.
// The grid source is the rain shadow capture by default; SSWorldFieldSurfaceTop
// routes it through the shared world field's SURFACE_TOP channel instead. The
// grid shape and serial semantics are identical, so nothing downstream changes.
void SSSurfaceField::refreshGeometry()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_SURFACE_GEOM);

    static LLCachedControl<bool> use_field(gSavedSettings, "SSWorldFieldSurfaceTop", false);
    SSWorldField* field = use_field ? SSWorldField::getInstance() : nullptr;
    SSRainShadowMap* shadow = SSRainShadowMap::getInstance();

    std::vector<std::pair<U64, U32> > tiles;
    if (field)
    {
        field->validTiles(tiles);
    }
    else
    {
        shadow->validTiles(tiles);
    }

    for (const auto& entry : tiles)
    {
        Geometry& geom = mGeometry[entry.first];
        if (geom.valid() && geom.mGeomSerial == entry.second) continue;

        SSRainShadowMap::SurfaceGrid grid;
        const bool have = field ? field->buildSurfaceGrid(entry.first, GEOM_RES, grid)
                                : shadow->buildSurfaceGrid(entry.first, GEOM_RES, grid);
        if (!have) continue;

        buildGeometry(grid, geom);

        // <SS:Nexii> With the world field as the grid source, the pool mask comes from its DRAINAGE_NETWORK topology - the priority-flood depression fill - instead of buildGeometry's local dips check. What that changes: standing water now needs a genuine depression with a spill outlet, so a flat roof row stops holding puddles and a real basin starts to. The rain shadow source keeps the dips check exactly as it was; the gate is the same SSWorldFieldSurfaceTop switch by which the grid itself moved.
        if (field)
        {
            SSWorldField::Drainage drain;
            if (SSWorldField::buildDrainage(grid, drain) && drain.mPool.size() == geom.mPool.size())
            {
                geom.mPool = std::move(drain.mPool);
            }
        }

        // <SS:Nexii> The liquid runoff network, at its own finer resolution
        // (SSAtmoRunoffRes) off the same source. Both sources serve the same
        // SurfaceGrid contract, so the network follows the grid switch the
        // wet field already uses; the world field's 0.25 m store also gives
        // buildRunoff the full-resolution column tops it refines lip points
        // against, which is why the source pointer rides along.
        buildRunoff(entry.first, geom, field);

        // <SS:Nexii> Carry the liquid reservoirs across the retrace: a run
        // keeps its water when its biggest feeder cell survives the edit, and
        // whatever belonged to runs that no longer exist is shared out over
        // the new ones by catchment - the same hand-over the trace has always
        // needed so a prim edit next door reads as invisible rather than as
        // every eave on the region pausing for a drain cycle.
        auto fld_it = mFields.find(entry.first);
        if (fld_it != mFields.end() && fld_it->second.mN == geom.mN)
        {
            Field& fld = fld_it->second;

            std::set<U32> live;
            F32 total_catch = 0.f;
            for (const Geometry::Run& run : geom.mRunoff.mRuns)
            {
                live.insert(run.mKey);
                total_catch += run.mCatch;
            }

            F32 orphan = 0.f;
            for (auto it = fld.mRunStore.begin(); it != fld.mRunStore.end(); )
            {
                if (live.count(it->first))
                {
                    ++it;
                }
                else
                {
                    orphan += it->second;
                    it = fld.mRunStore.erase(it);
                }
            }
            for (auto it = fld.mRunAccum.begin(); it != fld.mRunAccum.end(); )
            {
                if (live.count(it->first)) ++it;
                else it = fld.mRunAccum.erase(it);
            }

            if (orphan > 0.f && total_catch > 0.f)
            {
                for (const Geometry::Run& run : geom.mRunoff.mRuns)
                {
                    fld.mRunStore[run.mKey] += orphan * run.mCatch / total_catch;
                }
            }
        }
    }

    for (auto it = mGeometry.begin(); it != mGeometry.end(); )
    {
        bool still_there = false;
        for (const auto& entry : tiles)
        {
            if (entry.first == it->first) { still_there = true; break; }
        }
        it = still_there ? std::next(it) : mGeometry.erase(it);
    }
}

static const F32 SHED_DRAIN_TAU = 6.f;

static const F32 SHED_STORE_CEILING = 8.f;

// Spends the frame's weather on every region's shed: granular drains the
// creep spill at the lips, liquid fills and drains the runoff network's runs.
void SSSurfaceField::shedEdges(F32 dt)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_SURFACE_SHED);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSPrecipSim* sim = atmo ? atmo->sim() : nullptr;
    if (!sim || dt <= 0.f) return;

    static LLCachedControl<bool> shed_enabled(gSavedSettings, "SSAtmoRunoff", true);
    if (!shed_enabled) return;

    static LLCachedControl<F32> scale_setting(gSavedSettings, "SSAtmoRunoffScale", 1.f);
    const F32 scale = llclamp((F32)scale_setting, 0.f, 4.f);
    if (scale <= 0.f) return;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    const F32 rate_m2 = SSPrecipSim::dropRateAt(cam) * scale;

    for (auto& entry : mFields)
    {
        auto geom_it = mGeometry.find(entry.first);
        if (geom_it == mGeometry.end()) continue;

        const Geometry& geom = geom_it->second;
        Field& fld = entry.second;
        if (!geom.valid() || geom.mN != fld.mN) continue;

        shedRegion(entry.first, geom, fld, dt, rate_m2, cam);
    }
}

// One region's edge shedding. Granular weather drains the transport's creep
// spill through the per-lip stores it was debited to - one ledger, one
// cursor, unchanged. Liquid sheds from the runoff network's runs instead: a
// run is a stretch of eave with a catchment, so it holds one reservoir, puts
// up curtain streams cut across its span when it sheds faster than drips can
// carry, and lets individual drips fall wherever its own flows converge -
// weighted by the catchment each member drains, so the valley corner a whole
// roof plane runs into gets the drips a straight rake never earns.
void SSSurfaceField::shedRegion(U64 region_handle, const Geometry& geom, Field& fld,
                                F32 dt, F32 rate_m2, const LLVector3& camera_agent)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSPrecipSim* sim = atmo->sim();

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return;

    const LLVector3 origin = regionp->getOriginAgent();

    static LLCachedControl<F32> radius_setting(gSavedSettings, "SSAtmoRunoffRadius", 48.f);
    const F32 radius = llclamp((F32)radius_setting, 8.f, 128.f);
    const F32 radius_sq = radius * radius;

    const S32 live = sim->dripCount();
    const F32 budget = (live >= DRIP_BUDGET) ? 0.f
                     : llmin(1.f, (F32)(DRIP_BUDGET - live) / (F32)(DRIP_BUDGET / 4));

    const F64 now = atmo->sharedTime();

    if (atmo->granularWeather())
    {
        if (geom.mEdgeCells.empty()) return;

        const F32 cell = geom.mCell;

        S32& cursor = mShedCursor[region_handle];
        const S32 lip_count = (S32)geom.mEdgeCells.size();
        if (cursor >= lip_count) cursor = 0;

        S32 visited = 0;

        for (S32 k = 0; k < lip_count; ++k)
        {
            const S32 i = geom.mEdgeCells[(size_t)k];
            const size_t ui = (size_t)i;

            // The store is debited where the creep pass delivers; this cursor
            // only drains it into cascades.
            const F32 outflow = fld.mStore[ui] / SHED_DRAIN_TAU;
            fld.mStore[ui] = llmax(0.f, fld.mStore[ui] - outflow * dt);

            if (outflow <= 0.01f)
            {
                fld.mAccum[ui] = 0.f;
                continue;
            }

            const S32 offset = (k - cursor + lip_count) % lip_count;
            if (offset >= SHED_VISIT_PER_FRAME) continue;
            ++visited;

            const S32 x = i % geom.mN;
            const S32 y = i / geom.mN;
            const LLVector3 lip = origin
                + LLVector3(((F32)x + 0.5f) * cell, ((F32)y + 0.5f) * cell, 0.f);
            const LLVector3 lip_agent(lip.mV[VX], lip.mV[VY], geom.mZ[ui]);

            const LLVector3 delta = lip_agent - camera_agent;
            if (delta.magVecSquared() > radius_sq)
            {
                fld.mAccum[ui] = 0.f;
                continue;
            }

            const LLVector3 out_dir(geom.mEdgeX[ui], geom.mEdgeY[ui], 0.f);

            LLVector3 land = lip_agent;
            bool on_water = false;
            SSRainShadowMap::getInstance()->resolveColumn(
                lip_agent + out_dir * (cell * 0.5f) - LLVector3(0.f, 0.f, 0.1f), land, on_water);

            const F32 raw_rate = llmin(outflow / SHED_MERGE, SHED_MAX_RATE);

            const F32 stream_drive = llclamp(
                (raw_rate - SHED_STREAM_MIN) / SHED_STREAM_FULL, 0.f, 1.f);

            const U32 key = SSAtmoNoise::combine(
                SSAtmoNoise::combine((U32)region_handle, (U32)(region_handle >> 32)),
                (U32)i);

            SSRandStream rng(SSAtmoNoise::combine(atmo->seed(),
                SSAtmoNoise::combine((U32)(S64)(now * 8.0), key)));
            rng.next();

            if (stream_drive > 0.f)
            {
                const SSPrecipPreset& preset = atmo->preset();
                const F32 width = (preset.mStreamSpan > 0.f)
                    ? llclamp(preset.mStreamSpan, 1.f, STREAM_SPAN_MAX) : cell;

                sim->refreshStream(key, lip_agent, out_dir, land,
                                   stream_drive, width, 0.f, rng);
            }

            const F32 drips_per_s = raw_rate * (1.f - 0.9f * stream_drive);
            if (budget <= 0.f) continue;

            fld.mAccum[ui] += drips_per_s * budget * dt;
            S32 shed_now = (S32)fld.mAccum[ui];
            if (shed_now <= 0) continue;

            shed_now = llmin(shed_now, SHED_MAX_BURST);
            fld.mAccum[ui] -= (F32)shed_now;

            for (S32 d = 0; d < shed_now; ++d)
            {
                const LLVector3 along(-out_dir.mV[VY], out_dir.mV[VX], 0.f);
                const LLVector3 jitter = along * (rng.frand(-0.5f, 0.5f) * cell);
                sim->spawnDrip(lip_agent + jitter, out_dir, land + jitter, SHED_MERGE, rng);
            }
        }

        cursor = (cursor + llmax(1, visited)) % lip_count;
        return;
    }

    const Geometry::Runoff& ro = geom.mRunoff;
    if (!ro.valid() || ro.mRuns.empty()) return;

    const F32 cell = ro.mCell;

    for (const Geometry::Run& run : ro.mRuns)
    {
        F32& store = fld.mRunStore[run.mKey];
        F32& accum = fld.mRunAccum[run.mKey];

        // The roof's reservoir: the run's whole catchment fills it, it drains
        // on its own time constant, and what drains is what the eave sheds. A
        // gust lull overhead empties the store, not the eave.
        const F32 inflow = run.mCatch * rate_m2;
        if (inflow > 0.f)
        {
            store = llmin(store + inflow * dt, inflow * SHED_STORE_CEILING + 1.f);
        }

        const F32 outflow = store / SHED_DRAIN_TAU;
        store = llmax(0.f, store - outflow * dt);

        if (outflow <= 0.01f)
        {
            accum = 0.f;
            continue;
        }

        const LLVector3 run_centroid(run.mCentroid.mV[VX] + origin.mV[VX],
                                     run.mCentroid.mV[VY] + origin.mV[VY],
                                     run.mCentroid.mV[VZ]);
        const LLVector3 delta = run_centroid - camera_agent;
        if (delta.magVecSquared() > radius_sq)
        {
            accum = 0.f;
            continue;
        }

        const F32 raw_rate = llmin(outflow / SHED_MERGE, SHED_MAX_RATE);

        const F32 stream_drive = llclamp(
            (raw_rate - SHED_STREAM_MIN) / SHED_STREAM_FULL, 0.f, 1.f);

        const LLVector3 out_dir = run.mOut;
        // Along the lip: z cross out, the axis the network ordered the run by
        // and the one the stream renderer reads its pitch back in.
        const LLVector3 along(-out_dir.mV[VY], out_dir.mV[VX], 0.f);

        const U32 key = SSAtmoNoise::combine(
            SSAtmoNoise::combine((U32)region_handle, (U32)(region_handle >> 32)),
            run.mKey);

        SSRandStream rng(SSAtmoNoise::combine(atmo->seed(),
            SSAtmoNoise::combine((U32)(S64)(now * 8.0), key)));
        rng.next();

        if (stream_drive > 0.f)
        {
            // The curtain spans what the art was drawn for: the preset's span,
            // or its sheet tier's quad width, cut into slots across the run so
            // together they are the length of the eave - a gutter, without
            // anything having to know that the run exists.
            const SSPrecipPreset& preset = atmo->preset();
            const F32 span = (preset.mStreamSpan > 0.f)
                ? llclamp(preset.mStreamSpan, 1.f, STREAM_SPAN_MAX)
                : llclamp(preset.mTiers[TIER_SHEETS].mSizeX * 2.f, 1.f, STREAM_SPAN_MAX);

            const size_t count = run.mCells.size();
            auto proj_at = [&](size_t m)
            {
                const LLVector3& lip = run.mLips[m];
                return lip.mV[VX] * along.mV[VX] + lip.mV[VY] * along.mV[VY];
            };

            // The run's own span, measured over the refined lip points.
            const F32 run_len = llmax(proj_at(count - 1) - proj_at(0), 0.f) + cell;
            const S32 slots = llclamp((S32)(run_len / span + 0.5f), 1, 64);
            const F32 slot_len = run_len / (F32)slots;

            size_t m = 0;
            for (S32 s = 0; s < slots; ++s)
            {
                const F32 lo = proj_at(0) + (F32)s * slot_len;
                const F32 hi = lo + slot_len;
                const F32 mid = lo + 0.5f * slot_len;

                size_t first = count, last = 0, mid_at = 0;
                F32 near_dp = FLT_MAX;
                while (m < count)
                {
                    const F32 p = proj_at(m);
                    if (p >= hi) break;
                    if (p >= lo)
                    {
                        if (first == count) first = m;
                        last = m;
                        const F32 dp = fabsf(p - mid);
                        if (dp < near_dp)
                        {
                            near_dp = dp;
                            mid_at = m;
                        }
                    }
                    ++m;
                }
                if (first == count)
                {
                    // A thinned long run can leave a slot between members:
                    // anchor it on the nearest member there is.
                    mid_at = last = first = llmin(m, count - 1);
                }

                const LLVector3& lip_local = run.mLips[mid_at];
                const LLVector3 lip_agent(lip_local.mV[VX] + origin.mV[VX],
                                          lip_local.mV[VY] + origin.mV[VY],
                                          lip_local.mV[VZ]);

                // The pitch of this stretch of lip, so a curtain off a rake
                // hangs along the rake instead of cutting through the roof.
                F32 pitch = 0.f;
                if (last > first)
                {
                    const F32 run_m = llmax(proj_at(last) - proj_at(first), 0.01f);
                    pitch = llclamp((run.mLips[last].mV[VZ] - run.mLips[first].mV[VZ]) / run_m,
                                    -4.f, 4.f);
                }

                LLVector3 land = lip_agent;
                bool on_water = false;
                SSRainShadowMap::getInstance()->resolveColumn(
                    lip_agent + out_dir * (cell * 0.5f) - LLVector3(0.f, 0.f, 0.1f), land, on_water);

                const U32 slot_key = SSAtmoNoise::combine(key, (U32)s);
                sim->refreshStream(slot_key, lip_agent, out_dir, land,
                                   stream_drive, slot_len, pitch, rng);
            }
        }

        const F32 drips_per_s = raw_rate * (1.f - 0.9f * stream_drive);
        if (budget <= 0.f) continue;

        accum += drips_per_s * budget * dt;
        S32 shed_now = (S32)accum;
        if (shed_now <= 0) continue;

        shed_now = llmin(shed_now, SHED_MAX_BURST);
        accum -= (F32)shed_now;

        for (S32 d = 0; d < shed_now; ++d)
        {
            // Members are picked weighted by the catchment each drains: a
            // drip lands where the roof's flows gathered, which is why a
            // valley corner runs while the plain rake beside it stays quiet.
            F32 pick_total = 0.f;
            for (const S32 c : run.mCells)
            {
                pick_total += ro.mCatch[(size_t)c];
            }

            F32 pick = rng.frand(0.f, pick_total);
            size_t mi = 0;
            for (; mi < run.mCells.size(); ++mi)
            {
                pick -= ro.mCatch[(size_t)run.mCells[mi]];
                if (pick <= 0.f) break;
            }
            if (mi >= run.mCells.size()) mi = run.mCells.size() - 1;

            const LLVector3& lip_local = run.mLips[mi];
            const LLVector3 lip_agent(lip_local.mV[VX] + origin.mV[VX],
                                      lip_local.mV[VY] + origin.mV[VY],
                                      lip_local.mV[VZ]);
            const LLVector3 jitter = along * (rng.frand(-0.5f, 0.5f) * cell);

            LLVector3 land = lip_agent;
            bool on_water = false;
            SSRainShadowMap::getInstance()->resolveColumn(
                lip_agent + jitter + out_dir * (cell * 0.5f) - LLVector3(0.f, 0.f, 0.1f),
                land, on_water);

            sim->spawnDrip(lip_agent + jitter, out_dir, land, SHED_MERGE, rng);
        }
    }
}

// The per-region wet/snow/puddle field, created to match its geometry.
SSSurfaceField::Field* SSSurfaceField::fieldFor(U64 region_handle, const Geometry& geom, F64 now)
{
    Field& fld = mFields[region_handle];
    fld.mRegionHandle = region_handle;
    fld.mLastTouched = now;

    if (fld.mN != geom.mN)
    {
        fld.mN = geom.mN;
        fld.mCell = geom.mCell;
        fld.mZ.assign(geom.mZ.size(), 0.f);
        fld.mWet.assign(geom.mZ.size(), 0.f);
        fld.mSnow.assign(geom.mZ.size(), 0.f);
        fld.mPuddle.assign(geom.mZ.size(), 0.f);
        fld.mIce.assign(geom.mZ.size(), 0.f);
        fld.mFrost.assign(geom.mZ.size(), 0.f);
        fld.mStain.assign(geom.mZ.size(), 0.f);
        fld.mAge.assign(geom.mZ.size(), 0.f);
        fld.mGroundSpeed01.assign(geom.mZ.size(), 0.f);
        fld.mScorch.assign(geom.mZ.size(), 0.f);
        fld.mLift.assign(geom.mZ.size(), 0.f);
        fld.mInflow.assign(geom.mZ.size(), 0.f);
        fld.mStore.assign(geom.mZ.size(), 0.f);
        fld.mAccum.assign(geom.mZ.size(), 0.f);

        fld.mZ = geom.mZ;
    }

    fld.mCell = geom.mCell;
    return &fld;
}

// Integrates one region's field for a step: wetting, drying, snow settle and melt, puddle fill and drainage flow,
// then what the wind does to all of it.
void SSSurfaceField::tick(Field& fld, const Geometry& geom, F32 dt,
                          const SSPrecipPreset& preset, F32 intensity, F32 melt_scale,
                          const SSGranularParams& granular, const LLVector4* flow)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_SURFACE_TICK);

    const S32 n = geom.mN;
    const F32 cell = geom.mCell;
    const F32 cell_area = cell * cell;
    const bool falling = intensity > 0.001f;

    const F32 repose = llclamp(preset.mSnowRepose, 5.f, 89.f) * DEG_TO_RAD;

    static LLCachedControl<bool> puddles_on(gSavedSettings, "SSAtmoWetPuddles", true);

    const bool wetting  = falling && preset.mWetRate > 0.f;
    const bool snowing  = falling && preset.mSnowRate > 0.f && preset.mSnowDepth > 0.f;
    const bool pooling  = falling && preset.mPuddleRate > 0.f && preset.mPuddleDepth > 0.f
                          && puddles_on;

    const F32 wet_rate    = wetting ? preset.mWetRate * intensity
                                    : (preset.mDryRate > 0.f ? preset.mDryRate : FALLBACK_DRY);
    const F32 wet_target  = wetting ? 1.f : 0.f;
    const F32 wet_blend   = 1.f - expf(-wet_rate * dt);

    const F32 snow_gain   = preset.mSnowRate * intensity * dt;
    const F32 snow_loss   = (preset.mSnowMelt > 0.f ? preset.mSnowMelt : FALLBACK_MELT)
                          * melt_scale * dt;
    const F32 puddle_gain = preset.mPuddleRate * intensity * dt;
    const F32 puddle_loss = (preset.mPuddleDrain > 0.f ? preset.mPuddleDrain : FALLBACK_DRAIN) * dt;

    static LLCachedControl<F32> pool_depth_max(gSavedSettings, "SSAtmoWetPoolDepthMax", 0.15f);
    const F32 puddle_depth_ceiling = llmin(preset.mPuddleDepth, llmax((F32)pool_depth_max, 0.f));

    static LLCachedControl<F32> mask_strength(gSavedSettings, "SSAtmoWetPuddleMask", 0.75f);
    static LLCachedControl<F32> mask_scale(gSavedSettings, "SSAtmoWetPuddleMaskScale", 7.f);
    const F32 mask_amt = llclamp((F32)mask_strength, 0.f, 1.f);
    const F32 mask_wave = llmax((F32)mask_scale, 1.f) / llmax(cell, 0.25f);

    auto puddleMask = [&](S32 cx, S32 cy)
    {
        const F32 v = ssPuddleMaskNoise((F32)cx * cell, (F32)cy * cell, mask_wave * cell);
        const F32 patch = llclamp((v - 0.36f) / 0.34f, 0.f, 1.f);
        return lerp(1.f, patch * patch * (3.f - 2.f * patch), mask_amt);
    };

    // How much of a standing structure a cell is, 0 at grade to 1 from
    // SSAtmoSurfaceStructAbove metres over the terrain - the capture's
    // height-above-terrain channel as a factor. Faded in over the top half of
    // that span rather than stepped, so a low porch roof is still mostly
    // ground-like and only genuinely tall decks are treated as towers.
    static LLCachedControl<F32> struct_above(gSavedSettings, "SSAtmoSurfaceStructAbove", 12.f);
    const F32 struct_h = llmax((F32)struct_above, 1.f);
    auto structFactor = [&](size_t i)
    {
        const F32 t = llclamp((geom.above(i) - struct_h * 0.5f) / (struct_h * 0.5f), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    };

    // Deep snow piles belong at grade. A tall roof still whitens - snowfall
    // lands there like anywhere - but it holds a fraction of the ground's
    // depth ceiling rather than growing the same drifts a street does.
    static LLCachedControl<F32> snow_struct(gSavedSettings, "SSAtmoSnowStructDepth", 0.4f);
    const F32 snow_struct_frac = llclamp((F32)snow_struct, 0.f, 1.f);

    // <SS:Nexii> Surface weather state (doc sec 2): the tick's shared scalars, assembled once and copied per cell below. mExposure has no sky-view figure to read yet - TODO(core): a WorldField COVERAGE channel would replace the flat 1.0. [interaction: SSAtmoMagic::temperatureC/humidity/sunUp, none else]
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool liquidFalling = falling && !preset.isGranular();
    const SSSurfaceState::DepositLook resolved_deposit = depositLook();
    SSSurfaceState::CellIn state_in_template;
    state_in_template.mExposure = 1.f;
    state_in_template.mTempC = atmo->temperatureC();
    state_in_template.mHumidity = atmo->humidity();
    state_in_template.mSunlit = atmo->sunUp();
    state_in_template.mLiquidStain = liquidFalling ? preset.mLiquid.mStain : 0.f;
    state_in_template.mLiquidIntensity = liquidFalling ? intensity : 0.f;
    state_in_template.mDepositWash = resolved_deposit.mWash;
    state_in_template.mDepositFull = llmax(resolved_deposit.mDepthFull, 1.0e-6f);
    const F32 deposit_melts = resolved_deposit.mMelts;

    F32 peak_wet = 0.f, peak_snow = 0.f, peak_puddle = 0.f;
    F32 peak_ice = 0.f, peak_frost = 0.f, peak_stain = 0.f;
    F32 peak_wet_gain = 0.f, peak_deposit_gain = 0.f;

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;

            if (!geom.solid(i))
            {
                fld.mWet[i] = fld.mSnow[i] = fld.mPuddle[i] = 0.f;
                fld.mIce[i] = fld.mFrost[i] = fld.mStain[i] = fld.mAge[i] = 0.f;
                fld.mZ[i] = geom.mZ[i];
                continue;
            }

            if (fabsf(geom.mZ[i] - fld.mZ[i]) > REBUILD_DZ)
            {
                fld.mWet[i] = fld.mSnow[i] = fld.mPuddle[i] = 0.f;
                fld.mIce[i] = fld.mFrost[i] = fld.mStain[i] = fld.mAge[i] = 0.f;
                fld.mZ[i] = geom.mZ[i];
            }

            if (geom.water(i))
            {
                fld.mWet[i] = fld.mSnow[i] = fld.mPuddle[i] = 0.f;
                fld.mIce[i] = fld.mFrost[i] = fld.mStain[i] = fld.mAge[i] = 0.f;
                continue;
            }

            auto lieHere = [&]()
            {
                const F32 z0 = geom.mZ[i];
                const F32 limit = cell * SLOPE_STEP_MAX;
                auto slopeAlong = [&](S32 lo_i, S32 hi_i, bool have_lo, bool have_hi)
                {
                    F32 d_lo = 0.f, d_hi = 0.f;
                    bool ok_lo = false, ok_hi = false;
                    if (have_lo && geom.solid(lo_i) && fabsf(geom.mZ[lo_i] - z0) < limit)
                    {
                        d_lo = z0 - geom.mZ[lo_i];
                        ok_lo = true;
                    }
                    if (have_hi && geom.solid(hi_i) && fabsf(geom.mZ[hi_i] - z0) < limit)
                    {
                        d_hi = geom.mZ[hi_i] - z0;
                        ok_hi = true;
                    }
                    if (ok_lo && ok_hi) return (d_lo + d_hi) * 0.5f / cell;
                    if (ok_lo) return d_lo / cell;
                    if (ok_hi) return d_hi / cell;
                    return 0.f;
                };

                const F32 gx = slopeAlong((S32)i - 1, (S32)i + 1, x > 0, x < n - 1);
                const F32 gy = slopeAlong((S32)i - n, (S32)i + n, y > 0, y < n - 1);
                const F32 angle = atanf(sqrtf(gx * gx + gy * gy));
                return llclamp(1.f - angle / repose, 0.f, 1.f);
            };

            // <SS:Nexii> A cell a strike flash-boiled holds off re-accumulation until its hold runs out: the drying and draining paths below still run, so the patch keeps behaving like scorched ground rather than freezing as it was. Without this the very next rain tick puts the puddle straight back and the vaporisation reads as nothing having happened. [interaction: SSLightning::vaporise]
            const bool scorched = !fld.mScorch.empty() && fld.mScorch[i] > 0.f;
            if (scorched) fld.mScorch[i] = llmax(0.f, fld.mScorch[i] - dt);

            const F32 wet_before = fld.mWet[i];
            fld.mWet[i] += ((scorched ? 0.f : wet_target) - fld.mWet[i])
                         * (scorched ? (1.f - expf(-(preset.mDryRate > 0.f ? preset.mDryRate : FALLBACK_DRY) * dt)) : wet_blend);

            F32 deposit_gain_applied = 0.f;
            if (snowing && !scorched)
            {
                const F32 depth_scale = lerp(1.f, snow_struct_frac, structFactor(i));
                const F32 room = preset.mSnowDepth * depth_scale * lieHere() - fld.mSnow[i];
                if (room > 0.f)
                {
                    deposit_gain_applied = llmin(room, snow_gain);
                    fld.mSnow[i] += deposit_gain_applied;
                }
            }
            else if (fld.mSnow[i] > 0.f)
            {
                // <SS:Nexii> Melt scaled by the resolved deposit look's mMelts - a deposit with melts 0 (sand, ash) never melts here; it only leaves through washRemoval below. doc sec 8.
                fld.mSnow[i] = llmax(0.f, fld.mSnow[i] - snow_loss * deposit_melts);
            }

            // Standing water is a grade phenomenon: a hollow in a street
            // fills, a hollow in a tower roof drains through whatever the
            // build actually is up there, and a puddle field on a skyline
            // deck reads as a bug even when the trace found a genuine dip.
            // Tall structure cells stop accumulating and let what they hold
            // drain out through the ordinary loss path.
            const F32 grade = 1.f - structFactor(i);
            if (pooling && !scorched && geom.mPool[i] && grade > 0.01f)
            {
                const F32 mask = puddleMask(x, y) * grade;
                fld.mPuddle[i] = llmin(puddle_depth_ceiling * mask,
                                       fld.mPuddle[i] + puddle_gain * mask);
            }
            else if (fld.mPuddle[i] > 0.f)
            {
                fld.mPuddle[i] = llmax(0.f, fld.mPuddle[i] - puddle_loss);
            }

            // <SS:Nexii> The state step and the wash removal, in that order, right after settle so both read this tick's post-settle wet/puddle/deposit - doc sec 2's tick order (settle -> stepCell -> washRemoval -> transport).
            {
                SSSurfaceState::CellIn cell_in = state_in_template;
                cell_in.mWet = fld.mWet[i];
                cell_in.mPuddle = fld.mPuddle[i];
                cell_in.mDeposit = fld.mSnow[i];
                cell_in.mDepositGain = deposit_gain_applied;
                cell_in.mWetGain = llmax(0.f, fld.mWet[i] - wet_before);

                SSSurfaceState::CellState cell_state;
                cell_state.mIce = fld.mIce[i];
                cell_state.mFrost = fld.mFrost[i];
                cell_state.mStain = fld.mStain[i];
                cell_state.mAge = fld.mAge[i];
                SSSurfaceState::stepCell(cell_state, cell_in, dt);
                fld.mIce[i] = cell_state.mIce;
                fld.mFrost[i] = cell_state.mFrost;
                fld.mStain[i] = cell_state.mStain;
                fld.mAge[i] = cell_state.mAge;

                fld.mSnow[i] = llmax(0.f, fld.mSnow[i] - SSSurfaceState::washRemoval(cell_in, dt));

                peak_wet_gain = llmax(peak_wet_gain, cell_in.mWetGain);
                peak_deposit_gain = llmax(peak_deposit_gain, cell_in.mDepositGain);
            }

            peak_wet = llmax(peak_wet, fld.mWet[i]);
            peak_snow = llmax(peak_snow, fld.mSnow[i]);
            peak_puddle = llmax(peak_puddle, fld.mPuddle[i]);
            peak_ice = llmax(peak_ice, fld.mIce[i]);
            peak_frost = llmax(peak_frost, fld.mFrost[i]);
            peak_stain = llmax(peak_stain, fld.mStain[i]);
        }
    }

    mPeakWet = llmax(mPeakWet, peak_wet);
    mPeakSnow = llmax(mPeakSnow, peak_snow);
    mPeakPuddle = llmax(mPeakPuddle, peak_puddle);
    mPeakIce = llmax(mPeakIce, peak_ice);
    mPeakFrost = llmax(mPeakFrost, peak_frost);
    mPeakStain = llmax(mPeakStain, peak_stain);
    mPeakWetGain = llmax(mPeakWetGain, peak_wet_gain);
    mPeakDepositGain = llmax(mPeakDepositGain, peak_deposit_gain);
    mPeakWetPresent = llmax(mPeakWetPresent, peak_wet);
    mPeakDepositPresent = llmax(mPeakDepositPresent, peak_snow);

    // <SS:Nexii> Granular transport: what the wind does to what settle just left. Runs after the settle pass so fresh snow can lift in the same step it landed; the peak scan is re-run afterwards because erosion and banking both move it.
    if (flow)
    {
        SSGranularParams p = granular;
        p.mFlow = flow;
        SSGranular::step(fld, geom, p, dt);

        F32 wind_peak = 0.f;
        for (const F32 depth : fld.mSnow)
        {
            wind_peak = llmax(wind_peak, depth);
        }
        peak_snow = llmax(peak_snow, wind_peak);
        mPeakSnow = llmax(mPeakSnow, peak_snow);
        mPeakDepositPresent = llmax(mPeakDepositPresent, peak_snow);
    }
}

// Drops fields and geometry for regions that left the world.
void SSSurfaceField::evict(F64 now)
{
    for (auto it = mFields.begin(); it != mFields.end();)
    {
        const bool gone = !LLWorld::getInstance()->getRegionFromHandle(it->first);
        it = (gone || now - it->second.mLastTouched > FIELD_KEEP)
                 ? mFields.erase(it) : std::next(it);
    }

    while (mFields.size() > MAX_FIELDS)
    {
        auto oldest = mFields.begin();
        for (auto it = mFields.begin(); it != mFields.end(); ++it)
        {
            if (it->second.mLastTouched < oldest->second.mLastTouched) oldest = it;
        }
        mFields.erase(oldest);
    }
}

// Per-frame drive: refresh geometry, step fields on the fixed shared-time clock, shed edges,
// refresh the GPU window.
void SSSurfaceField::idle(F32 dt)
{
    (void)dt; // the transport steps on shared time; presentation below uses the fixed quanta too

    // <SS:Nexii> Rings expire once per frame regardless of the early returns below - a ring's life is camera time, not the field's own tick cadence. doc sec 6. The rate is what turns the ring's own clock into seconds, so a fast ripple setting retires the buffer sooner as well as drawing it quicker.
    mRings.expire(gFrameTimeSeconds, ringRate());

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    if (!atmo->isEnabled())
    {
        if (!mFields.empty()) clear();
        return;
    }

    const SSPrecipPreset& preset = atmo->preset();

    const bool blows = atmo->granularWeather() && preset.mSnowLiftRate > 0.f;
    const bool marks = preset.marksSurface() || blows;
    if (!marks && mFields.empty()) return;

    // <SS:Nexii> The transport clock. Steps land on exact quanta of shared time rather than whatever the frame hands over, so creep, erosion and the regime evaluation are identical across viewers and frame rates - the discipline the architecture doc fixes for anything that changes ground state. Presentation (the shed cursor, drip spawns) still runs once per frame below, on the frame's own accumulated dt.
    const F64 now = atmo->sharedTime();
    if (mLastStep < 0.0) mLastStep = now;
    F64 elapsed = now - mLastStep;
    if (elapsed > (F64)MAX_TICK_DT)
    {
        // stalled or joined mid-session: resync instead of replaying an eight-second storm
        mLastStep = now;
        elapsed = 0.0;
    }
    U32 steps = (U32)(elapsed / (F64)TICK_INTERVAL);
    if (steps == 0) return;
    static const U32 MAX_STEPS_PER_FRAME = 4;
    const U32 ran = llmin(steps, MAX_STEPS_PER_FRAME);
    mLastStep += (F64)ran * (F64)TICK_INTERVAL;

    LL_RECORD_BLOCK_TIME(FTM_SS_SURFACE);
    LLTimer timer;

    const F32 intensity = atmo->hasWeather() ? llclamp(atmo->precipitation(), 0.f, 1.f) : 0.f;

    // Live warmth for the snow melt, one figure per step like every other tick input.
    const F32 melt_scale = llclamp(atmo->temperatureC() / MELT_FULL_C,
                                   MELT_SUBLIMATION, MELT_WARM_MAX);

    mPeakWet = mPeakSnow = mPeakPuddle = 0.f;
    mPeakIce = mPeakFrost = mPeakStain = 0.f;
    mPeakWetGain = mPeakDepositGain = 0.f;
    mPeakWetPresent = mPeakDepositPresent = 0.f;

    refreshGeometry();

    // <SS:Nexii> Granular transport inputs, assembled once: the parameter bundle and each region's ground-flow grid, sampled straight out of the solved flowmap without the gust layer (the envelope rides the bundle as one scalar, never per cell).
    SSGranularParams granular;
    atmo->fillTransportParams(granular);

    // Grade-vs-structure depth scaling, handed in as plain figures the way
    // every other input reaches the transport - it reads no settings itself.
    {
        static LLCachedControl<F32> struct_above(gSavedSettings, "SSAtmoSurfaceStructAbove", 12.f);
        static LLCachedControl<F32> snow_struct(gSavedSettings, "SSAtmoSnowStructDepth", 0.4f);
        granular.mStructAboveH = llmax((F32)struct_above, 1.f);
        granular.mStructDepth = llclamp((F32)snow_struct, 0.f, 1.f);
    }

    const bool blows_here = granular.mLiftRate > 0.f || granular.mDepositRate > 0.f
                         || granular.mCreepRate > 0.f;

    // <SS:Nexii> AUDIT (finding 16): per-region setup (fld/flow) stays a once-per-idle cost exactly as before - only the TICK loop below moved, so a stalled client catching up (ran > 1) still samples the flow grid once, not `ran` times. Each region now owns its own flow grid vector (mFlowGrid) rather than sharing one reused buffer: the old code used a region's flow pointer immediately, in the same loop iteration that filled it, but the tick calls are deferred to the s-loop below now, so an earlier region's pointer into a shared, since-refilled buffer would dangle.
    struct RegionTick { Field* mFld; const Geometry* mGeom; std::vector<LLVector4> mFlowGrid; const LLVector4* mFlow; };
    std::vector<RegionTick> region_ticks;
    region_ticks.reserve(mGeometry.size());

    for (const auto& entry : mGeometry)
    {
        const Geometry& geom = entry.second;
        if (!geom.valid()) continue;

        Field* fld = fieldFor(entry.first, geom, now);
        if (!fld) continue;

        region_ticks.push_back({ fld, &geom, {}, nullptr });
        RegionTick& rt = region_ticks.back();

        if (blows_here)
        {
            LL_RECORD_BLOCK_TIME(FTM_SS_SURFACE_FLOW);

            LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
            if (regionp && SSWindFlowMap::getInstance()->sampleGroundGrid(regionp, geom.mN, geom.mCell,
                                                                          geom.mZ.data(), rt.mFlowGrid))
            {
                rt.mFlow = rt.mFlowGrid.data();
            }
        }

        // <SS:Nexii> Wind carry (doc sec 8): a static figure per solve, taken straight from the flow grid already sampled above - never integrated, never decayed here. Left untouched (whatever the last solve wrote) when this idle() has no flow grid for the region.
        if (rt.mFlow && fld->mGroundSpeed01.size() == fld->mZ.size())
        {
            static LLCachedControl<F32> lift_hi_setting(gSavedSettings, "SSAtmoSnowLiftHi", 8.0f);
            const F32 lift_hi = llmax((F32)lift_hi_setting, 0.1f);
            for (size_t i = 0; i < fld->mGroundSpeed01.size(); ++i)
            {
                const F32 speed = sqrtf(rt.mFlow[i].mV[0] * rt.mFlow[i].mV[0] + rt.mFlow[i].mV[1] * rt.mFlow[i].mV[1]);
                fld->mGroundSpeed01[i] = llclamp(speed / lift_hi, 0.f, 1.f);
            }
        }
    }

    // <SS:Nexii> AUDIT (finding 16): the look crossfade now advances once per TICK (s), using that tick's own gain across every region, rather than once per idle() using a gain maxed over up to MAX_STEPS_PER_FRAME ticks - a client that stalled and is replaying several quanta this frame no longer crosses fades several times slower than one that didn't. mPeakWet/mPeakSnow (the render-gate other callers read via peakWet()/peakSnow()) are deliberately left accumulating for the WHOLE idle() as before; the *Gain accumulators AND the present normaliser they divide by are both tick-scoped, so two clients replaying the same shared-time quanta at different frame rates land on the same mT.
    for (U32 s = 0; s < ran; ++s)
    {
        mPeakWetGain = mPeakDepositGain = 0.f;
        mPeakWetPresent = mPeakDepositPresent = 0.f;
        for (const RegionTick& rt : region_ticks)
        {
            tick(*rt.mFld, *rt.mGeom, TICK_INTERVAL, preset, intensity, melt_scale, granular, rt.mFlow);
        }

        // Advance toward whatever the active preset currently prescribes; a preset already equal to mA starts nothing, so switching presets with nothing yet fallen changes the ground not at all. [interaction: none - reads only the preset already fetched above]
        if (!looksEqual(preset.mLiquid, mLiquidMix.mA))
        {
            mLiquidMix.mB = preset.mLiquid;
            mLiquidMix.mT = SSSurfaceState::advanceMix(mLiquidMix.mT, mPeakWetGain, mPeakWetPresent, 1.f);
            if (mLiquidMix.mT >= 1.f)
            {
                mLiquidMix.mA = mLiquidMix.mB;
                mLiquidMix.mT = 0.f;
            }
        }
        if (!looksEqual(preset.mDeposit, mDepositMix.mA))
        {
            mDepositMix.mB = preset.mDeposit;
            mDepositMix.mT = SSSurfaceState::advanceMix(mDepositMix.mT, mPeakDepositGain, mPeakDepositPresent, mDepositMix.mB.mDepthFull);
            if (mDepositMix.mT >= 1.f)
            {
                mDepositMix.mA = mDepositMix.mB;
                mDepositMix.mT = 0.f;
            }
        }
    }

    shedEdges((F32)ran * TICK_INTERVAL);

    evict(now);
    updateWindow();

    mLastTickMS = (F32)(timer.getElapsedTimeF64() * 1000.0);
}

// Point query of the field (surface height, wet, snow, puddle) for CPU consumers like avatar exposure.
// <SS:Nexii> Flash-boils the surface water under a strike. Weighted by distance across the disc so the burst has a soft edge instead of a stamped circle, and weighted per medium by what a return stroke would actually do: the standing puddle goes outright at the centre, the film of wet mostly goes, the snow loses its top but not the drift - a bolt does not clear a snowfield. The hold is written to every cell it touches, in proportion, so the rim recovers before the middle. Returns the water taken against a full puddle over the disc, which is the steam burst's own scale. doc/atmo_magic_lightning_strike.md
F32 SSSurfaceField::vaporise(const LLVector3& center_agent, F32 radius_m, F32 hold_s)
{
    // What counts as a full one of its kind when the three media are summed: the pool depth ceiling's own default, and a snow depth deep enough to walk in.
    constexpr F32 PUDDLE_FULL_M = 0.15f;
    constexpr F32 SNOW_FULL_M = 0.25f;

    if (radius_m <= 0.f) return 0.f;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(center_agent);
    if (!regionp) return 0.f;

    auto it = mFields.find(regionp->getHandle());
    if (it == mFields.end()) return 0.f;

    Field& fld = it->second;
    if (fld.mN < 1 || fld.mCell <= 0.f || fld.mScorch.size() != fld.mZ.size()) return 0.f;

    const LLVector3 local = center_agent - regionp->getOriginAgent();
    const F32 cx = local.mV[VX] / fld.mCell;
    const F32 cy = local.mV[VY] / fld.mCell;
    const F32 cr = radius_m / fld.mCell;

    const S32 x0 = llclamp((S32)floorf(cx - cr), 0, fld.mN - 1);
    const S32 x1 = llclamp((S32)ceilf(cx + cr), 0, fld.mN - 1);
    const S32 y0 = llclamp((S32)floorf(cy - cr), 0, fld.mN - 1);
    const S32 y1 = llclamp((S32)ceilf(cy + cr), 0, fld.mN - 1);

    F32 taken = 0.f;
    F32 area = 0.f;

    for (S32 y = y0; y <= y1; ++y)
    {
        for (S32 x = x0; x <= x1; ++x)
        {
            const F32 dx = ((F32)x + 0.5f) - cx;
            const F32 dy = ((F32)y + 0.5f) - cy;
            const F32 d = sqrtf(dx * dx + dy * dy) / llmax(cr, 0.001f);
            if (d >= 1.f) continue;

            const F32 w = 1.f - d * d;
            const size_t i = (size_t)y * fld.mN + x;
            area += w;

            // The three media carry different units - puddle and snow in metres, wet as a 0-1 film - so each is normalised against what counts as a full one of its kind before they are summed. A brimming puddle alone is worth a full burst; wet ground alone a third of one.
            taken += llmin(fld.mPuddle[i] / PUDDLE_FULL_M, 1.f) * w;
            fld.mPuddle[i] *= 1.f - w;

            taken += fld.mWet[i] * 0.35f * w;
            fld.mWet[i] *= 1.f - 0.85f * w;

            taken += llmin(fld.mSnow[i] / SNOW_FULL_M, 1.f) * 0.5f * w;
            fld.mSnow[i] *= 1.f - 0.5f * w;

            fld.mScorch[i] = llmax(fld.mScorch[i], hold_s * w);
        }
    }

    return (area > 0.f) ? taken / area : 0.f;
}

SSSurfaceField::Sample SSSurfaceField::sample(const LLVector3& pos_agent) const
{
    Sample out;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return out;

    auto it = mFields.find(regionp->getHandle());
    if (it == mFields.end()) return out;

    const Field& fld = it->second;
    if (fld.mN < 1 || fld.mCell <= 0.f) return out;

    const LLVector3 local = pos_agent - regionp->getOriginAgent();
    const S32 x = (S32)(local.mV[VX] / fld.mCell);
    const S32 y = (S32)(local.mV[VY] / fld.mCell);
    if (x < 0 || y < 0 || x >= fld.mN || y >= fld.mN) return out;

    const size_t i = (size_t)y * fld.mN + x;
    out.mWet = fld.mWet[i];
    out.mSnow = fld.mSnow[i];
    out.mPuddle = fld.mPuddle[i];
    out.mLift = fld.mLift.empty() ? 0.f : fld.mLift[i];
    out.mIce = fld.mIce.empty() ? 0.f : fld.mIce[i];
    out.mFrost = fld.mFrost.empty() ? 0.f : fld.mFrost[i];
    out.mStain = fld.mStain.empty() ? 0.f : fld.mStain[i];
    out.mAge = fld.mAge.empty() ? 0.f : fld.mAge[i];
    out.mValid = true;

    // <SS:Nexii> The HEIGHT is bilinear over the four surrounding cell centres, where wetness, snow and puddles stay nearest-cell. A stair-stepped surface is a fair answer for a material property and a bad one for a height: every consumer that walks it - the ground crawl above all, stepping 1.5-3m against a 2m continuity guard - reads a cell boundary as a cliff and stops there. Over the Linden heightmap neighbouring cells differ by centimetres and it never showed; over a sculpted or mesh sim surround they differ by the whole relief, so the crawl died on its first step every time. Sampling at cell CENTRES (the half-cell shift) is what keeps the interpolant from leaning half a cell off the data. doc/atmo_magic_lightning_strike.md
    {
        const F32 gx = local.mV[VX] / fld.mCell - 0.5f;
        const F32 gy = local.mV[VY] / fld.mCell - 0.5f;
        const S32 x0 = llclamp((S32)floorf(gx), 0, fld.mN - 1);
        const S32 y0 = llclamp((S32)floorf(gy), 0, fld.mN - 1);
        const S32 x1 = llmin(x0 + 1, fld.mN - 1);
        const S32 y1 = llmin(y0 + 1, fld.mN - 1);
        const F32 tx = llclamp(gx - (F32)x0, 0.f, 1.f);
        const F32 ty = llclamp(gy - (F32)y0, 0.f, 1.f);
        const F32 z00 = fld.mZ[(size_t)y0 * fld.mN + x0];
        const F32 z10 = fld.mZ[(size_t)y0 * fld.mN + x1];
        const F32 z01 = fld.mZ[(size_t)y1 * fld.mN + x0];
        const F32 z11 = fld.mZ[(size_t)y1 * fld.mN + x1];
        out.mSurfaceZ = lerp(lerp(z00, z10, tx), lerp(z01, z11, tx), ty);
    }

    if (out.mPuddle > 0.f)
    {
        static LLCachedControl<F32> mask_strength(gSavedSettings, "SSAtmoWetPuddleMask", 0.75f);
        static LLCachedControl<F32> mask_scale(gSavedSettings, "SSAtmoWetPuddleMaskScale", 7.f);
        const F32 amt = llclamp((F32)mask_strength, 0.f, 1.f);
        if (amt > 0.f)
        {
            const F32 v = ssPuddleMaskNoise(local.mV[VX], local.mV[VY], (F32)mask_scale);
            const F32 t = llclamp((v - 0.47f) / 0.09f, 0.f, 1.f);
            out.mPuddle *= lerp(1.f, t * t * (3.f - 2.f * t), amt);
        }
    }
    return out;
}

// <SS:Nexii> Granular access: the one write path into mSnow from outside, and the drift tier's spawn walk over the lift the transport computed.

// Credits a landing clump against the cell's repose room. The preset's ceiling and repose own the
// cap; the transport's depositAt does the clamping.
void SSSurfaceField::depositAt(const LLVector3& pos_agent, F32 depth)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    if (!atmo->granularWeather() || depth <= 0.f) return;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return;

    auto geom_it = mGeometry.find(regionp->getHandle());
    auto fld_it = mFields.find(regionp->getHandle());
    if (geom_it == mGeometry.end() || fld_it == mFields.end()) return;

    const Geometry& geom = geom_it->second;
    Field& fld = fld_it->second;
    if (!geom.valid() || geom.mN != fld.mN) return;

    const LLVector3 local = pos_agent - regionp->getOriginAgent();
    const S32 x = (S32)(local.mV[VX] / geom.mCell);
    const S32 y = (S32)(local.mV[VY] / geom.mCell);
    if (x < 0 || y < 0 || x >= geom.mN || y >= geom.mN) return;

    const S32 i = y * geom.mN + x;
    const SSPrecipPreset& preset = atmo->preset();
    const F32 repose = llclamp(preset.mSnowRepose, 5.f, 89.f) * DEG_TO_RAD;

    // The same grade-vs-structure depth scaling the settle and transport paths
    // apply, so a clump landing on a tower deck banks against the deck's own
    // reduced ceiling rather than the street's.
    static LLCachedControl<F32> struct_above(gSavedSettings, "SSAtmoSurfaceStructAbove", 12.f);
    static LLCachedControl<F32> snow_struct(gSavedSettings, "SSAtmoSnowStructDepth", 0.4f);
    const F32 h = llmax((F32)struct_above, 1.f);
    const F32 t = llclamp((geom.above(i) - h * 0.5f) / (h * 0.5f), 0.f, 1.f);
    const F32 scale = lerp(1.f, llclamp((F32)snow_struct, 0.f, 1.f), t * t * (3.f - 2.f * t));

    const F32 ceiling = llmax(preset.mSnowDepth, 0.f) * scale;
    SSGranular::depositAt(fld, geom, i, depth, ceiling, repose);
    mPeakSnow = llmax(mPeakSnow, fld.mSnow[i]);
}

// Walks every lifted, snow-holding cell in a circle - deterministic order, cheap rejection by
// bounding box first. The sim's drift spawn runs this at its own tick rate.
void SSSurfaceField::forEachLiftCell(const LLVector3& center_agent, F32 radius_m,
                                     const std::function<void(const LLVector3& pos_agent, F32 depth, F32 lift)>& fn) const
{
    if (radius_m <= 0.f) return;
    const F32 radius_sq = radius_m * radius_m;

    for (const auto& entry : mFields)
    {
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        auto geom_it = mGeometry.find(entry.first);
        if (!regionp || geom_it == mGeometry.end()) continue;

        const Geometry& geom = geom_it->second;
        const Field& fld = entry.second;
        if (!geom.valid() || geom.mN != fld.mN || fld.mLift.size() != fld.mSnow.size()) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const F32 span = geom.mCell * (F32)geom.mN;

        // bounding box of the circle against this region's grid
        const S32 x0 = llclamp((S32)floorf((center_agent.mV[VX] - radius_m - origin.mV[VX]) / geom.mCell), 0, geom.mN - 1);
        const S32 x1 = llclamp((S32)floorf((center_agent.mV[VX] + radius_m - origin.mV[VX]) / geom.mCell), 0, geom.mN - 1);
        const S32 y0 = llclamp((S32)floorf((center_agent.mV[VY] - radius_m - origin.mV[VY]) / geom.mCell), 0, geom.mN - 1);
        const S32 y1 = llclamp((S32)floorf((center_agent.mV[VY] + radius_m - origin.mV[VY]) / geom.mCell), 0, geom.mN - 1);
        if (origin.mV[VX] > center_agent.mV[VX] + radius_m || origin.mV[VX] + span < center_agent.mV[VX] - radius_m
            || origin.mV[VY] > center_agent.mV[VY] + radius_m || origin.mV[VY] + span < center_agent.mV[VY] - radius_m)
        {
            continue;
        }

        for (S32 y = y0; y <= y1; ++y)
        {
            for (S32 x = x0; x <= x1; ++x)
            {
                const size_t i = (size_t)y * geom.mN + x;
                const F32 lift = fld.mLift[i];
                if (lift <= 0.01f || fld.mSnow[i] <= 2.0e-4f) continue;

                const LLVector3 pos(origin.mV[VX] + ((F32)x + 0.5f) * geom.mCell,
                                    origin.mV[VY] + ((F32)y + 0.5f) * geom.mCell,
                                    fld.mZ[i]);
                const F32 dx = pos.mV[VX] - center_agent.mV[VX];
                const F32 dy = pos.mV[VY] - center_agent.mV[VY];
                if (dx * dx + dy * dy > radius_sq) continue;

                fn(pos, fld.mSnow[i], lift);
            }
        }
    }
}

// Re-bakes the camera-centred texture window the shaders read, snapped to the field grid.
void SSSurfaceField::updateWindow()
{
    if (mFields.empty())
    {
        mWindowValid = false;
        return;
    }

    LL_RECORD_BLOCK_TIME(FTM_SS_SURFACE_WINDOW);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    F32 cell = 0.f;
    {
        LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);
        auto it = cam_region ? mFields.find(cam_region->getHandle()) : mFields.end();
        if (it != mFields.end() && it->second.mCell > 0.f) cell = it->second.mCell;
        else cell = mFields.begin()->second.mCell;
    }
    if (cell <= 0.f)
    {
        mWindowValid = false;
        return;
    }

    const F32 span = cell * (F32)WINDOW_RES;
    LLVector3 origin(floorf((cam.mV[VX] - span * 0.5f) / cell) * cell,
                     floorf((cam.mV[VY] - span * 0.5f) / cell) * cell,
                     0.f);

    mWindowData.assign((size_t)WINDOW_RES * WINDOW_RES * 4, 0.f);
    for (size_t t = 0; t < (size_t)WINDOW_RES * WINDOW_RES; ++t)
    {
        mWindowData[t * 4] = WINDOW_NO_SURFACE;
    }
    mWindowFlowData.assign((size_t)WINDOW_RES * WINDOW_RES * 4, 0.f);
    mWindowStateData.assign((size_t)WINDOW_RES * WINDOW_RES * 4, 0.f);
    // <SS:Nexii> The cover window inits to "no verdict" - every cell the loop below does not
    // answer (past the stitched regions) must read as no-answer in the shader, never as open.
    mWindowCoverData.assign((size_t)WINDOW_RES * WINDOW_RES * 4, -1.f);

    bool any = false;
    for (const auto& entry : mFields)
    {
        const Field& fld = entry.second;
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp || fld.mN < 1 || fabsf(fld.mCell - cell) > 0.001f) continue;

        const LLVector3 forigin = regionp->getOriginAgent();
        const S32 off_x = (S32)llround((forigin.mV[VX] - origin.mV[VX]) / cell);
        const S32 off_y = (S32)llround((forigin.mV[VY] - origin.mV[VY]) / cell);

        const S32 x0 = llmax(0, off_x), x1 = llmin(WINDOW_RES, off_x + fld.mN);
        const S32 y0 = llmax(0, off_y), y1 = llmin(WINDOW_RES, off_y + fld.mN);
        if (x0 >= x1 || y0 >= y1) continue;

        auto geom_it = mGeometry.find(entry.first);
        const Geometry* geom = (geom_it != mGeometry.end()) ? &geom_it->second : nullptr;
        const bool have_slope = geom && geom->valid() && geom->mN == fld.mN;

        // <SS:Nexii> The cover window's source: the shared world field's
        // enclosure spectrum. Sampled a quarter metre above the cell's stored
        // surface - the air a fog sample sits in when the covered test gates
        // it - and answered by the flood's touching classification with its
        // sub-band resolution. -1 wherever the world field has no verdict
        // (off, tile stale, point inside a band's implied solid), which the
        // fog shader reads as the old binary covered test. The bulk form
        // resolves the region's tile once per region loop, not once per cell.
        // [interaction: SSWorldField]
        SSWorldField* worldfield = SSWorldField::getInstance();

        for (S32 wy = y0; wy < y1; ++wy)
        {
            const S32 fy = wy - off_y;
            for (S32 wx = x0; wx < x1; ++wx)
            {
                const S32 fx = wx - off_x;
                const size_t fi = (size_t)fy * fld.mN + fx;
                const size_t wi = ((size_t)wy * WINDOW_RES + wx) * 4;
                mWindowData[wi]     = fld.mZ[fi];
                mWindowData[wi + 1] = fld.mWet[fi];
                mWindowData[wi + 2] = fld.mSnow[fi];
                mWindowData[wi + 3] = fld.mPuddle[fi];

                // <SS:Nexii> The state window - ice, frost, stain, age - same lattice, uploaded beside the field data (doc sec 2).
                mWindowStateData[wi]     = fld.mIce.empty() ? 0.f : fld.mIce[fi];
                mWindowStateData[wi + 1] = fld.mFrost.empty() ? 0.f : fld.mFrost[fi];
                mWindowStateData[wi + 2] = fld.mStain.empty() ? 0.f : fld.mStain[fi];
                mWindowStateData[wi + 3] = fld.mAge.empty() ? 0.f : fld.mAge[fi];

                mWindowCoverData[wi] = worldfield->enclosureAtRegion(entry.first, LLVector3(
                    forigin.mV[VX] + ((F32)fx + 0.5f) * fld.mCell,
                    forigin.mV[VY] + ((F32)fy + 0.5f) * fld.mCell,
                    fld.mZ[fi] + 0.25f));

                if (have_slope && geom->solid(fi) && !geom->water(fi))
                {
                    mWindowFlowData[wi]     = geom->mSlopeX[fi];
                    mWindowFlowData[wi + 1] = geom->mSlopeY[fi];
                    mWindowFlowData[wi + 2] =
                        llclamp(geom->mSlope[fi] / SLOPE_RUN_FULL, 0.f, 1.f) * fld.mWet[fi];
                    // <SS:Nexii> Wind carry spare channel (doc sec 8): ssFieldFlowMap.w reads as ground wind 0..1 so the deposit passes can settle snow in a lee the sky-view term alone would leave bare.
                    mWindowFlowData[wi + 3] = fld.mGroundSpeed01.empty() ? 0.f : fld.mGroundSpeed01[fi];
                }
            }
        }
        any = true;
    }

    if (!any)
    {
        mWindowValid = false;
        return;
    }

    if (mWindowTex == 0 || mWindowRes != WINDOW_RES)
    {
        releaseGL();

#if LL_WINDOWS
        if (glTexStorage2D == nullptr)
        {
            LL_WARNS_ONCE("AtmoMagic") << "No glTexStorage2D; the surface field"
                                       << " window cannot be allocated" << LL_ENDL;
            mWindowValid = false;
            return;
        }
#endif

        while (glGetError() != GL_NO_ERROR) { }

        glGenTextures(1, &mWindowTex);
        glBindTexture(GL_TEXTURE_2D, mWindowTex);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, WINDOW_RES, WINDOW_RES);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenTextures(1, &mWindowFlowTex);
        glBindTexture(GL_TEXTURE_2D, mWindowFlowTex);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, WINDOW_RES, WINDOW_RES);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenTextures(1, &mWindowStateTex);
        glBindTexture(GL_TEXTURE_2D, mWindowStateTex);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, WINDOW_RES, WINDOW_RES);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenTextures(1, &mWindowCoverTex);
        glBindTexture(GL_TEXTURE_2D, mWindowCoverTex);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, WINDOW_RES, WINDOW_RES);
        glBindTexture(GL_TEXTURE_2D, 0);

        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            LL_WARNS("AtmoMagic") << "Surface field texture failed to allocate,"
                                     " GL error 0x" << std::hex << (U32)err << std::dec
                                  << "; nothing will shade wet" << LL_ENDL;
            releaseGL();
            mWindowValid = false;
            return;
        }

        mWindowRes = WINDOW_RES;
        LL_INFOS("AtmoMagic") << "Surface field window allocated, " << WINDOW_RES
                              << " cells square" << LL_ENDL;
    }

    {
        LL_RECORD_BLOCK_TIME(FTM_SS_SURFACE_UPLOAD);

        glBindTexture(GL_TEXTURE_2D, mWindowTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WINDOW_RES, WINDOW_RES,
                        GL_RGBA, GL_FLOAT, mWindowData.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindTexture(GL_TEXTURE_2D, mWindowFlowTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WINDOW_RES, WINDOW_RES,
                        GL_RGBA, GL_FLOAT, mWindowFlowData.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindTexture(GL_TEXTURE_2D, mWindowStateTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WINDOW_RES, WINDOW_RES,
                        GL_RGBA, GL_FLOAT, mWindowStateData.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindTexture(GL_TEXTURE_2D, mWindowCoverTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WINDOW_RES, WINDOW_RES,
                        GL_RGBA, GL_FLOAT, mWindowCoverData.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    mWindowCell = cell;
    mWindowOrigin = origin;
    mWindowValid = true;
}

// Binds the field window texture.
bool SSSurfaceField::bindForShader(LLGLSLShader& shader, S32 channel)
{
    // <SS:Nexii> Nothing upstream clamps the running channel count against the driver's actual unit budget - a shader with enough other samplers already bound could hand this an out-of-range unit, which is an out-of-bounds gGL.getTexUnit() index rather than merely "one fewer effect".
    if (!hasWindow() || channel < 0 || channel >= gGLManager.mNumTextureImageUnits) return false;

    static LLStaticHashedString field_map("ssFieldMap");
    static LLStaticHashedString field_origin("ssFieldOrigin");

    gGL.getTexUnit(channel)->activate();
    gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mWindowTex);
    shader.uniform1i(field_map, channel);

    shader.uniform4f(field_origin, mWindowOrigin.mV[VX], mWindowOrigin.mV[VY],
                     mWindowCell, (F32)mWindowRes);
    return true;
}

// Binds the flow window texture.
bool SSSurfaceField::bindFlowForShader(LLGLSLShader& shader, S32 channel)
{
    if (!hasFlowWindow() || channel < 0 || channel >= gGLManager.mNumTextureImageUnits) return false;

    static LLStaticHashedString field_flow_map("ssFieldFlowMap");

    gGL.getTexUnit(channel)->activate();
    gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mWindowFlowTex);
    shader.uniform1i(field_flow_map, channel);

    return true;
}

// Binds the state window texture (ice, frost, stain, age).
bool SSSurfaceField::bindStateForShader(LLGLSLShader& shader, S32 channel)
{
    if (!hasStateWindow() || channel < 0 || channel >= gGLManager.mNumTextureImageUnits) return false;

    static LLStaticHashedString field_state_map("ssFieldStateMap");

    gGL.getTexUnit(channel)->activate();
    gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mWindowStateTex);
    shader.uniform1i(field_state_map, channel);

    return true;
}

// Binds the cover window texture (the world field's enclosure spectrum).
bool SSSurfaceField::bindCoverForShader(LLGLSLShader& shader, S32 channel)
{
    if (!hasCoverWindow() || channel < 0 || channel >= gGLManager.mNumTextureImageUnits) return false;

    static LLStaticHashedString field_cover_map("ssFieldCoverMap");

    gGL.getTexUnit(channel)->activate();
    gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mWindowCoverTex);
    shader.uniform1i(field_cover_map, channel);

    return true;
}

// Uploads the resolved liquid/deposit looks and the plain weather scalars every surface pass shares (doc sec 2/3).
void SSSurfaceField::bindLooksForShader(LLGLSLShader& shader) const
{
    static LLStaticHashedString liquid_look_u("ssLiquidLook");
    static LLStaticHashedString liquid_look2_u("ssLiquidLook2");
    static LLStaticHashedString deposit_look_u("ssDepositLook");
    static LLStaticHashedString deposit_look2_u("ssDepositLook2");
    static LLStaticHashedString intensity_u("ssSurfaceIntensity");
    static LLStaticHashedString temp_u("ssSurfaceTempC");
    static LLStaticHashedString drops_u("ssSurfaceDrops");
    static LLStaticHashedString drop_scale_u("ssSurfaceDropScale");
    static LLStaticHashedString ice_on_u("ssSurfaceIceOn");
    static LLStaticHashedString frost_on_u("ssSurfaceFrostOn");
    static LLStaticHashedString frost_strength_u("ssSurfaceFrostStrength");
    static LLStaticHashedString debug_u("ssSurfaceDebug");
    static LLStaticHashedString wind_u("ssSurfaceWind");
    static LLStaticHashedString time_u("ssTime");

    const SSSurfaceState::LiquidLook liquid = liquidLook();
    const SSSurfaceState::DepositLook deposit = depositLook();

    shader.uniform4f(liquid_look_u, liquid.mTint.r, liquid.mTint.g, liquid.mTint.b, liquid.mOpacity);
    shader.uniform2f(liquid_look2_u, liquid.mStain, liquid.mMetal);
    shader.uniform4f(deposit_look_u, deposit.mTint.r, deposit.mTint.g, deposit.mTint.b, deposit.mSparkle);
    shader.uniform4f(deposit_look2_u, deposit.mTranslucency, deposit.mDepthFull, deposit.mWash, deposit.mMelts);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    // <SS:Nexii> Liquid precipitation intensity only - 0 while a granular type falls (doc sec 3's ssSurfaceIntensity).
    const bool liquidFalling = atmo->hasWeather() && !atmo->preset().isGranular();
    shader.uniform1f(intensity_u, liquidFalling ? llclamp(atmo->precipitation(), 0.f, 1.f) : 0.f);
    shader.uniform1f(temp_u, atmo->temperatureC());

    static LLCachedControl<F32> drops(gSavedSettings, "SSAtmoSurfaceDrops", 1.f);
    static LLCachedControl<F32> drop_scale(gSavedSettings, "SSAtmoSurfaceDropScale", 1.f);
    static LLCachedControl<bool> ice_on(gSavedSettings, "SSAtmoSurfaceIce", true);
    static LLCachedControl<bool> frost_on(gSavedSettings, "SSAtmoSurfaceFrost", true);
    static LLCachedControl<F32> frost_strength(gSavedSettings, "SSAtmoSurfaceFrostStrength", 1.f);
    static LLCachedControl<S32> debug(gSavedSettings, "SSAtmoSurfaceDebug", 0);

    shader.uniform1f(drops_u, llclamp((F32)drops, 0.f, 1.f));
    shader.uniform1f(drop_scale_u, llmax((F32)drop_scale, 0.f));
    shader.uniform1f(ice_on_u, ice_on ? 1.f : 0.f);
    shader.uniform1f(frost_on_u, frost_on ? 1.f : 0.f);
    shader.uniform1f(frost_strength_u, llmax((F32)frost_strength, 0.f));
    shader.uniform1f(debug_u, (F32)(S32)debug);

    const LLVector3 wind = SSWindFlowMap::getInstance()->sampleGround(LLViewerCamera::getInstance()->getOrigin());
    shader.uniform3fv(wind_u, 1, wind.mV);
    shader.uniform1f(time_u, gFrameTimeSeconds);
}

// The analytic ring's clock rate, from the preset's own landing ring and the taste controls that scale it (doc sec 6).
F32 SSSurfaceField::ringRate() const
{
    // <SS:Nexii> The same two controls spawnRipple builds its landing quad from, read the same way (clamped to the same bands), so the analytic ring and the quad it stands in for cannot drift apart: the quad's radius is mRippleSize * scale and it crosses it in mRippleLife / speed seconds. The per-impact strength jitter that also scales a quad is deliberately NOT in here - one rate serves the whole buffer, and a ring that ran at its own speed would need its own uniform per ring.
    static LLCachedControl<F32> ripple_scale_setting(gSavedSettings, "SSAtmoRippleScale", 1.f);
    static LLCachedControl<F32> ripple_speed_setting(gSavedSettings, "SSAtmoRippleSpeed", 2.f);
    const F32 ripple_scale = llclamp((F32)ripple_scale_setting, 0.25f, 3.f);
    const F32 ripple_speed = llclamp((F32)ripple_speed_setting, 0.5f, 5.f);

    const SSPrecipPreset& preset = SSAtmoMagic::getInstance()->preset();
    if (!preset.makesRipples()) return 1.f;

    return SSSurfaceState::ringRate(preset.mRippleSize * ripple_scale, preset.mRippleLife / ripple_speed);
}

// Uploads the live impact ring buffer (doc sec 6) - expire() already ran this frame in idle().
void SSSurfaceField::bindRingsForShader(LLGLSLShader& shader) const
{
    static LLStaticHashedString ring_count_u("ssRingCount");
    static LLStaticHashedString rings_u("ssRings");
    static LLStaticHashedString rings_z_u("ssRingZ");
    static LLStaticHashedString ring_rate_u("ssRingRate");

    shader.uniform1f(ring_rate_u, ringRate());

    // <SS:Nexii> SSAtmoSurfaceRings gates the shader upload only - the buffer itself keeps recording and expiring regardless, so toggling this back on picks up whatever is still live rather than a cold buffer.
    static LLCachedControl<bool> rings_on(gSavedSettings, "SSAtmoSurfaceRings", true);
    const S32 count = rings_on ? mRings.mCount : 0;
    shader.uniform1i(ring_count_u, count);
    if (count <= 0) return;

    F32 rings[SSSurfaceState::RING_MAX * 4];
    F32 z[SSSurfaceState::RING_MAX];
    for (S32 i = 0; i < count; ++i)
    {
        const SSSurfaceState::Ring& r = mRings.mRings[i];
        rings[i * 4]     = r.mX;
        rings[i * 4 + 1] = r.mY;
        rings[i * 4 + 2] = r.mBirth;
        rings[i * 4 + 3] = r.mStrength;
        z[i] = r.mZ;
    }
    shader.uniform4fv(rings_u, count, rings);
    shader.uniform1fv(rings_z_u, count, z);
}

// Records an impact into the ring buffer if it fell within RING_NEAR_M of the camera - farther ones stay ripple quads (doc sec 6).
void SSSurfaceField::noteImpact(const LLVector3& pos_agent, F32 strength)
{
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 dx = pos_agent.mV[VX] - cam.mV[VX];
    const F32 dy = pos_agent.mV[VY] - cam.mV[VY];
    const F32 dz = pos_agent.mV[VZ] - cam.mV[VZ];
    if (dx * dx + dy * dy + dz * dz > SSSurfaceState::RING_NEAR_M * SSSurfaceState::RING_NEAR_M) return;

    SSSurfaceState::Ring ring;
    ring.mX = pos_agent.mV[VX];
    ring.mY = pos_agent.mV[VY];
    ring.mZ = pos_agent.mV[VZ];
    ring.mBirth = gFrameTimeSeconds;
    ring.mStrength = llclamp(strength, 0.f, 1.f);
    mRings.push(ring);
}

// Frees the GL objects.
void SSSurfaceField::releaseGL()
{
    if (mWindowTex)
    {
        glDeleteTextures(1, &mWindowTex);
        mWindowTex = 0;
    }
    if (mWindowFlowTex)
    {
        glDeleteTextures(1, &mWindowFlowTex);
        mWindowFlowTex = 0;
    }
    if (mWindowStateTex)
    {
        glDeleteTextures(1, &mWindowStateTex);
        mWindowStateTex = 0;
    }
    if (mWindowCoverTex)
    {
        glDeleteTextures(1, &mWindowCoverTex);
        mWindowCoverTex = 0;
    }
    mWindowRes = 0;
    mWindowValid = false;
    mScratch.release();
    mScratchNormal.release();
}

// Screen-space pass: darkens and gloss-boosts wet gbuffer surfaces, then commits back into the specular attachment.
void SSSurfaceField::renderWetPass()
{
    static S32 s_state = -1;
    auto note = [](S32 state, const char* what)
    {
        if (s_state != state)
        {
            s_state = state;
            LL_INFOS("AtmoMagic") << "Surface wetness pass: " << what << LL_ENDL;
        }
    };

    if (gCubeSnapshot) return;
    if (!hasWindow()) { note(1, "idle, no field window uploaded"); return; }
    if (!gSSSurfaceWetProgram.isComplete()) { note(2, "idle, shader did not build"); return; }
    if (!gSSSurfaceCommitProgram.isComplete()) { note(6, "idle, commit shader did not build"); return; }

    static LLCachedControl<F32> strength(gSavedSettings, "SSAtmoWetStrength", 1.f);
    const F32 wet_strength = llclamp((F32)strength, 0.f, 2.f);
    static LLCachedControl<bool> wet_on(gSavedSettings, "SSAtmoWetSurfaces", true);
    if (wet_strength <= 0.f || !wet_on) { note(3, "idle, SSAtmoWetStrength is zero or wet surfaces disabled"); return; }

    LLRenderTarget* gbuffer = &gPipeline.mRT->deferredScreen;
    const U32 w = gbuffer->getWidth();
    const U32 h = gbuffer->getHeight();
    if (w == 0 || h == 0 || gbuffer->getNumTextures() < 2)
    {
        note(4, "idle, gbuffer has no specular attachment");
        return;
    }

    if (mScratch.getWidth() != w || mScratch.getHeight() != h)
    {
        mScratch.release();
        if (!mScratch.allocate(w, h, GL_RGBA, false))
        {
            note(5, "idle, could not allocate the scratch target");
            return;
        }

        if (mScratch.getNumTextures() < 1)
        {
            LL_WARNS("AtmoMagic") << "Surface wetness scratch target has no"
                                     " colour attachment after allocate("
                                  << w << "x" << h << ")" << LL_ENDL;
            return;
        }
    }

    if (s_state != 0)
    {
        s_state = 0;
        LL_INFOS("AtmoMagic") << "Surface wetness pass running at " << w << "x" << h
                              << ", field window " << mWindowRes << " at " << mWindowCell
                              << "m" << LL_ENDL;
    }

    // <SS:Nexii> Probe BEFORE the draw: an error already pending here was raised somewhere earlier in the
    // frame, and pinning that boundary is what narrows the hunt for the raiser (observed 2026-09-11: a
    // per-frame GL_INVALID_OPERATION began the second an Atmo environment unloaded and the world fell
    // back to stock EEP).
    {
        const GLenum pending = glGetError();
        if (pending != GL_NO_ERROR)
        {
            LL_WARNS_ONCE("AtmoMagic") << "GL error 0x" << std::hex << (U32)pending << std::dec
                                       << " already pending BEFORE the wetness draw" << LL_ENDL;
        }
    }

    LL_PROFILE_GPU_ZONE("atmo surface wetness");

    mScratch.bindTarget();

    gPipeline.bindDeferredShader(gSSSurfaceWetProgram);

    const S32 field_channel = gSSSurfaceWetProgram.mActiveTextureChannels;
    bindForShader(gSSSurfaceWetProgram, field_channel);
    // <SS:Nexii> Flow and state now bind on every surface program, not just the normal pass - the wet pass needs the wind carry channel (doc sec 8) and the state window for ice/frost/deposit matte (doc sec 3/4/7).
    const S32 wet_flow_channel = field_channel + 1;
    bindFlowForShader(gSSSurfaceWetProgram, wet_flow_channel);
    const S32 wet_state_channel = wet_flow_channel + 1;
    bindStateForShader(gSSSurfaceWetProgram, wet_state_channel);
    bindLooksForShader(gSSSurfaceWetProgram);

    SSAvatarWet::getInstance()->bindForShader(gSSSurfaceWetProgram);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const LLVector3 fall = atmo->rainDirection();

    static LLStaticHashedString inv_view("ssFieldInvView");
    static LLStaticHashedString field_fall("ssFieldFall");
    static LLStaticHashedString field_spread("ssFieldSpread");
    static LLStaticHashedString field_facing("ssFieldFacing");
    static LLStaticHashedString wet_str("ssWetStrength");
    static LLStaticHashedString wet_rough("ssWetRoughness");
    static LLStaticHashedString wet_rough_min("ssWetRoughMin");
    static LLStaticHashedString wet_gloss("ssWetGlossTarget");
    static LLStaticHashedString wet_spec("ssWetSpecular");
    static LLStaticHashedString wet_spec_matte("ssWetSpecularMatte");
    static LLStaticHashedString wet_debug("ssWetDebugForce");
    static LLStaticHashedString wet_puddle_depth("ssWetPuddleDepthFull");
    static LLStaticHashedString wet_puddle_rough("ssWetPuddleRoughness");
    static LLStaticHashedString wet_puddle_rough_min("ssWetPuddleRoughMin");
    static LLStaticHashedString wet_puddle_spec("ssWetPuddleSpecular");
    static LLStaticHashedString wet_puddle_gloss("ssWetPuddleGloss");
    static LLStaticHashedString wet_night("ssWetNight");

    // The night factor the puddle treatment yields to: a near-mirror under a
    // moonless zenith reads as a black hole, so full-puddle patches pull back
    // toward damp after dark. The sun's clamped direction carries the day cycle.
    const F32 sun_z = LLEnvironment::instance().getSunDirection().mV[VZ];
    const F32 night = llclamp(1.f - (sun_z + 0.1f) * 4.f, 0.f, 1.f);

    const glm::mat4 inv = glm::inverse(get_current_modelview());
    gSSSurfaceWetProgram.uniformMatrix4fv(inv_view, 1, GL_FALSE, glm::value_ptr(inv));
    gSSSurfaceWetProgram.uniform3fv(field_fall, 1, fall.mV);

    static LLCachedControl<F32> spread_setting(gSavedSettings, "SSAtmoWetSpread", 0.35f);
    const F32 spread = llclamp((F32)spread_setting, 0.f, 1.f);
    gSSSurfaceWetProgram.uniform1f(field_spread, spread);

    static LLCachedControl<F32> facing(gSavedSettings, "SSAtmoWetFacing", 0.6f);
    gSSSurfaceWetProgram.uniform1f(field_facing, llclamp((F32)facing, 0.f, 1.f));

    static LLCachedControl<F32> rough_mul(gSavedSettings, "SSAtmoWetRoughness", 0.25f);
    static LLCachedControl<F32> rough_min(gSavedSettings, "SSAtmoWetRoughMin", 0.04f);
    static LLCachedControl<F32> gloss_target(gSavedSettings, "SSAtmoWetGloss", 0.55f);
    static LLCachedControl<F32> spec_target(gSavedSettings, "SSAtmoWetSpecular", 0.25f);

    static LLCachedControl<F32> spec_matte(gSavedSettings, "SSAtmoWetSpecularMatte", 0.1f);

    static F32 s_light_vis = 1.f;
    static F64 s_light_vis_at = -1.0;
    const F64 vis_now = SSAtmoMagic::getInstance()->sharedTime();
    if (vis_now - s_light_vis_at > 0.5)
    {
        s_light_vis_at = vis_now;
        SSVolCloud* vol = SSVolCloud::getInstance();
        const LLVector3 cam_pos = LLViewerCamera::getInstance()->getOrigin();
        const LLVector3 toward_light = LLEnvironment::instance().getLightDirection();
        s_light_vis = vol->empty() ? 1.f
            : vol->transmittance(cam_pos, cam_pos + toward_light * 6000.f, 1.f);
    }
    const F32 spec_dim = 0.3f + 0.7f * s_light_vis;

    gSSSurfaceWetProgram.uniform1f(wet_str, wet_strength);
    gSSSurfaceWetProgram.uniform1f(wet_rough, llclamp((F32)rough_mul, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_rough_min, llclamp((F32)rough_min, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_gloss, llclamp((F32)gloss_target, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_spec, llclamp((F32)spec_target, 0.f, 1.f) * spec_dim);
    gSSSurfaceWetProgram.uniform1f(wet_spec_matte, llclamp((F32)spec_matte, 0.f, 1.f) * spec_dim);

    static LLCachedControl<F32> puddle_depth_full(gSavedSettings, "SSAtmoWetPuddleDepthFull", 0.02f);
    static LLCachedControl<F32> puddle_rough(gSavedSettings, "SSAtmoWetPuddleRoughness", 0.02f);
    static LLCachedControl<F32> puddle_rough_min(gSavedSettings, "SSAtmoWetPuddleRoughMin", 0.02f);
    static LLCachedControl<F32> puddle_spec(gSavedSettings, "SSAtmoWetPuddleSpecular", 0.2f);
    static LLCachedControl<F32> puddle_gloss(gSavedSettings, "SSAtmoWetPuddleGloss", 0.9f);
    const F32 puddle_depth_full_m = llmax((F32)puddle_depth_full, 0.001f);
    gSSSurfaceWetProgram.uniform1f(wet_puddle_depth, puddle_depth_full_m);
    gSSSurfaceWetProgram.uniform1f(wet_puddle_rough, llclamp((F32)puddle_rough, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_puddle_rough_min, llclamp((F32)puddle_rough_min, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_puddle_spec, llclamp((F32)puddle_spec, 0.f, 1.f) * spec_dim);
    gSSSurfaceWetProgram.uniform1f(wet_puddle_gloss, llclamp((F32)puddle_gloss, 0.f, 1.f));
    gSSSurfaceWetProgram.uniform1f(wet_night, night);

    {
        static LLStaticHashedString mask_amt_u("ssPuddleMaskAmt");
        static LLStaticHashedString mask_scale_u("ssPuddleMaskScaleM");
        static LLStaticHashedString mask_anchor_u("ssPuddleMaskAnchor");
        static LLCachedControl<F32> m_strength(gSavedSettings, "SSAtmoWetPuddleMask", 0.75f);
        static LLCachedControl<F32> m_scale(gSavedSettings, "SSAtmoWetPuddleMaskScale", 7.f);
        LLVector3 anchor(0.f, 0.f, 0.f);
        if (LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(
                LLViewerCamera::getInstance()->getOrigin()))
        {
            anchor = cam_region->getOriginAgent();
        }
        gSSSurfaceWetProgram.uniform1f(mask_amt_u, llclamp((F32)m_strength, 0.f, 1.f));
        gSSSurfaceWetProgram.uniform1f(mask_scale_u, llmax((F32)m_scale, 1.f));
        gSSSurfaceWetProgram.uniform2f(mask_anchor_u, anchor.mV[VX], anchor.mV[VY]);
    }

    static LLStaticHashedString wet_cos_full("ssWetFlattenCosFull");
    static LLStaticHashedString wet_cos_zero("ssWetFlattenCosZero");
    static LLCachedControl<F32> wet_angle_full(gSavedSettings, "SSAtmoWetFlattenAngleFull", 10.f);
    static LLCachedControl<F32> wet_angle_zero(gSavedSettings, "SSAtmoWetFlattenAngleZero", 30.f);
    gSSSurfaceWetProgram.uniform1f(wet_cos_full,
        cosf(llclamp((F32)wet_angle_full, 0.f, 89.f) * DEG_TO_RAD));
    gSSSurfaceWetProgram.uniform1f(wet_cos_zero,
        cosf(llclamp((F32)wet_angle_zero, 1.f, 90.f) * DEG_TO_RAD));

    static LLCachedControl<F32> debug_force(gSavedSettings, "SSAtmoWetDebugForce", 0.f);
    gSSSurfaceWetProgram.uniform1f(wet_debug, llclamp((F32)debug_force, 0.f, 1.f));

    static LLStaticHashedString wet_skip_exposure("ssWetSkipExposure");
    static LLCachedControl<F32> skip_exposure(gSavedSettings, "SSAtmoWetSkipExposure", 0.f);
    gSSSurfaceWetProgram.uniform1f(wet_skip_exposure, llclamp((F32)skip_exposure, 0.f, 1.f));

    {
        LLGLDepthTest depth(GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    gGL.getTexUnit(field_channel)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(wet_flow_channel)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(wet_state_channel)->unbind(LLTexUnit::TT_TEXTURE);
    gPipeline.unbindDeferredShader(gSSSurfaceWetProgram);

    {
        // <SS:Nexii> Drain the whole queue rather than popping one: several errors can stack within a frame,
        // and a strand left behind reads as "already pending" to every later checker (the wind flow's
        // contextLooksHealthy saw exactly that).
        U32 drained = 0;
        GLenum first = GL_NO_ERROR;
        GLenum err;
        while ((err = glGetError()) != GL_NO_ERROR)
        {
            if (!drained) first = err;
            ++drained;
        }
        if (drained)
        {
            LL_WARNS("AtmoMagic") << "GL error 0x" << std::hex << (U32)first << std::dec
                                  << " after the wetness draw (" << drained
                                  << (drained == 1 ? " error" : " errors") << " this frame)" << LL_ENDL;
        }
    }

    mScratch.flush();

    if (mScratch.getNumTextures() < 1)
    {
        LL_WARNS_ONCE("AtmoMagic") << "Surface wetness scratch target lost its"
                                      " colour attachment before the commit"
                                      " pass could read it" << LL_ENDL;
    }

    static S32 s_normal_state = -1;
    auto note_normal = [](S32 state, const char* what)
    {
        if (s_normal_state != state)
        {
            s_normal_state = state;
            LL_INFOS("AtmoMagic") << "Surface normal flatten pass: " << what << LL_ENDL;
        }
    };

    const bool do_normal = gSSSurfaceNormalProgram.isComplete();
    if (!do_normal)
    {
        note_normal(1, "idle, shader did not build");
    }
    else
    {
        if (mScratchNormal.getWidth() != w || mScratchNormal.getHeight() != h)
        {
            mScratchNormal.release();
            if (!mScratchNormal.allocate(w, h, GL_RGBA, false))
            {
                note_normal(2, "idle, could not allocate the scratch target");
            }
            else if (mScratchNormal.getNumTextures() < 1)
            {
                note_normal(3, "idle, allocate reported success but left no colour attachment");
            }
        }
    }

    if (do_normal && mScratchNormal.getNumTextures() >= 1)
    {
        if (s_normal_state != 0)
        {
            s_normal_state = 0;
            LL_INFOS("AtmoMagic") << "Surface normal flatten pass running" << LL_ENDL;
        }

        LL_PROFILE_GPU_ZONE("atmo surface normal flatten");

        mScratchNormal.bindTarget();
        gPipeline.bindDeferredShader(gSSSurfaceNormalProgram);

        const S32 normal_field_channel = gSSSurfaceNormalProgram.mActiveTextureChannels;
        bindForShader(gSSSurfaceNormalProgram, normal_field_channel);
        const S32 flow_field_channel = normal_field_channel + 1;
        bindFlowForShader(gSSSurfaceNormalProgram, flow_field_channel);

        const S32 wave_channel = flow_field_channel + 1;
        LLSettingsWater::ptr_t pwater = LLEnvironment::instance().getCurrentWater();
        LLUUID wave_id = pwater ? pwater->getNormalMapID() : LLUUID::null;
        if (wave_id.isNull()) wave_id = LLSettingsWater::GetDefaultWaterNormalAssetId();
        LLViewerFetchedTexture* wave_tex = LLViewerTextureManager::getFetchedTexture(wave_id);

        static LLStaticHashedString wave_map("ssWaveMap");
        bool have_wave = false;
        // <SS:Nexii> Same out-of-bounds guard as bindForShader/bindFlowForShader/bindStateForShader - this bind is manual (no field window to gate it) so it needs its own channel-budget check.
        if (wave_tex && wave_channel < gGLManager.mNumTextureImageUnits)
        {
            wave_tex->addTextureStats(1024.f * 1024.f);
            gGL.getTexUnit(wave_channel)->activate();
            gGL.getTexUnit(wave_channel)->bindManual(LLTexUnit::TT_TEXTURE, wave_tex->getTexName());
            gSSSurfaceNormalProgram.uniform1i(wave_map, wave_channel);
            have_wave = true;
        }

        // <SS:Nexii> State after the last texture slot already in use here (field, flow, wave) - the normal pass also needs looks (ice freeze, deposit soften) and the ring buffer (doc sec 3/6/7).
        const S32 normal_state_channel = wave_channel + 1;
        bindStateForShader(gSSSurfaceNormalProgram, normal_state_channel);
        bindLooksForShader(gSSSurfaceNormalProgram);
        bindRingsForShader(gSSSurfaceNormalProgram);

        // <SS:Nexii> The capsules are the only "is this a person" answer a screen-space pass has (ssavatarwet.h): the field's height rejection sits 1.5 cells up - 3 m at the window's 2 m cell, above a body's reach - so without them the normal pass stamps the ground's drop lattices on whoever stands there. Same bind the wet pass gets; uniforms the pass does not declare are skipped silently.
        SSAvatarWet::getInstance()->bindForShader(gSSSurfaceNormalProgram);

        static LLStaticHashedString norm_inv_view("ssFieldInvView");
        static LLStaticHashedString norm_wet_str("ssWetStrength");
        static LLStaticHashedString norm_wet_debug("ssWetDebugForce");
        static LLStaticHashedString norm_wet_skip_exposure("ssWetSkipExposure");
        static LLStaticHashedString norm_flatten("ssWetNormalFlatten");
        static LLStaticHashedString norm_cos_full("ssWetFlattenCosFull");
        static LLStaticHashedString norm_cos_zero("ssWetFlattenCosZero");
        static LLStaticHashedString norm_puddle_depth("ssWetPuddleDepthFull");
        static LLStaticHashedString norm_puddle_flatten("ssWetPuddleFlatten");

        gSSSurfaceNormalProgram.uniformMatrix4fv(norm_inv_view, 1, GL_FALSE, glm::value_ptr(inv));
        gSSSurfaceNormalProgram.uniform1f(norm_wet_str, wet_strength);
        gSSSurfaceNormalProgram.uniform1f(norm_wet_debug, llclamp((F32)debug_force, 0.f, 1.f));
        gSSSurfaceNormalProgram.uniform1f(norm_wet_skip_exposure, llclamp((F32)skip_exposure, 0.f, 1.f));

        static LLCachedControl<F32> flatten_amount(gSavedSettings, "SSAtmoWetNormalFlatten", 0.6f);
        gSSSurfaceNormalProgram.uniform1f(norm_flatten, llclamp((F32)flatten_amount, 0.f, 1.f));

        static LLCachedControl<F32> flatten_angle_full(gSavedSettings, "SSAtmoWetFlattenAngleFull", 10.f);
        static LLCachedControl<F32> flatten_angle_zero(gSavedSettings, "SSAtmoWetFlattenAngleZero", 30.f);
        const F32 cos_full = cosf(llclamp((F32)flatten_angle_full, 0.f, 89.f) * DEG_TO_RAD);
        const F32 cos_zero = cosf(llclamp((F32)flatten_angle_zero, 1.f, 90.f) * DEG_TO_RAD);
        gSSSurfaceNormalProgram.uniform1f(norm_cos_full, cos_full);
        gSSSurfaceNormalProgram.uniform1f(norm_cos_zero, cos_zero);

        gSSSurfaceNormalProgram.uniform1f(norm_puddle_depth, puddle_depth_full_m);
        static LLCachedControl<F32> puddle_flatten(gSavedSettings, "SSAtmoWetPuddleFlatten", 1.f);
        gSSSurfaceNormalProgram.uniform1f(norm_puddle_flatten, llclamp((F32)puddle_flatten, 0.f, 1.f));

        static LLStaticHashedString norm_time("ssTime");
        static LLStaticHashedString norm_flow_scale("ssWetFlowScale");
        static LLStaticHashedString norm_flow_speed("ssWetFlowSpeed");
        static LLStaticHashedString norm_flow_strength("ssWetFlowStrength");
        static LLStaticHashedString norm_flow_rot_sin("ssWetFlowRotSin");
        static LLStaticHashedString norm_flow_rot_cos("ssWetFlowRotCos");
        static LLStaticHashedString norm_flow_min_wet("ssWetFlowMinWet");
        static LLCachedControl<F32> flow_scale(gSavedSettings, "SSAtmoWetFlowScale", 4.f);
        static LLCachedControl<F32> flow_speed(gSavedSettings, "SSAtmoWetFlowSpeed", 0.6f);
        static LLCachedControl<F32> flow_strength(gSavedSettings, "SSAtmoWetFlowStrength", 0.6f);
        static LLCachedControl<F32> flow_rotate(gSavedSettings, "SSAtmoWetFlowRotate", 90.f);
        static LLCachedControl<F32> flow_min_wet(gSavedSettings, "SSAtmoWetFlowMinWet", 0.3f);

        gSSSurfaceNormalProgram.uniform1f(norm_time, gFrameTimeSeconds);
        gSSSurfaceNormalProgram.uniform1f(norm_flow_scale, llmax((F32)flow_scale, 0.1f));
        gSSSurfaceNormalProgram.uniform1f(norm_flow_speed, (F32)flow_speed);
        const F32 flow_rot_rad = (F32)flow_rotate * DEG_TO_RAD;
        gSSSurfaceNormalProgram.uniform1f(norm_flow_rot_sin, sinf(flow_rot_rad));
        gSSSurfaceNormalProgram.uniform1f(norm_flow_rot_cos, cosf(flow_rot_rad));
        const F32 shiver = llclamp(0.3f + SSAtmoMagic::getInstance()->wind().magVec() / 9.f,
                                   0.3f, 1.1f);
        gSSSurfaceNormalProgram.uniform1f(norm_flow_strength,
                                          have_wave ? llclamp((F32)flow_strength * shiver, 0.f, 1.6f) : 0.f);
        gSSSurfaceNormalProgram.uniform1f(norm_flow_min_wet, llclamp((F32)flow_min_wet, 0.f, 0.99f));

        {
            LLGLDepthTest depth(GL_FALSE);
            LLGLDisable blend(GL_BLEND);
            LLGLDisable scissor(GL_SCISSOR_TEST);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }

        gGL.getTexUnit(normal_field_channel)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(flow_field_channel)->unbind(LLTexUnit::TT_TEXTURE);
        if (have_wave) gGL.getTexUnit(wave_channel)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(normal_state_channel)->unbind(LLTexUnit::TT_TEXTURE);
        gPipeline.unbindDeferredShader(gSSSurfaceNormalProgram);

        {
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR)
            {
                LL_WARNS("AtmoMagic") << "GL error 0x" << std::hex << (U32)err << std::dec
                                      << " after the normal flatten draw" << LL_ENDL;
            }
        }

        mScratchNormal.flush();
    }

    static LLStaticHashedString commit_src("ssCommitSource");
    static LLStaticHashedString commit_paint("ssCommitDebugPaint");
    static LLStaticHashedString commit_target("ssCommitTarget");

    gbuffer->bindTarget();

    const GLenum bufs[4] = { GL_NONE, GL_COLOR_ATTACHMENT1, GL_NONE, GL_NONE };
    glDrawBuffers(4, bufs);

    {
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            LL_WARNS_ONCE("AtmoMagic") << "Gbuffer framebuffer incomplete for the"
                                          " commit pass, status 0x" << std::hex
                                       << (U32)status << std::dec << LL_ENDL;
        }

        static LLCachedControl<F32> commit_debug_paint_peek(gSavedSettings, "SSAtmoCommitDebugPaint", 0.f);
        if ((F32)commit_debug_paint_peek > 0.f)
        {
            const GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
            GLint box[4] = { 0, 0, 0, 0 };
            glGetIntegerv(GL_SCISSOR_BOX, box);
            LL_INFOS("AtmoMagic") << "Scissor going into the commit draw: "
                                  << (scissor_was_on ? "ON" : "off") << " box ("
                                  << box[0] << "," << box[1] << "," << box[2]
                                  << "," << box[3] << ")" << LL_ENDL;
        }
    }

    gSSSurfaceCommitProgram.bind();
    gGL.getTexUnit(0)->activate();
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mScratch.getTexture(0));
    gSSSurfaceCommitProgram.uniform1i(commit_src, 0);
    gSSSurfaceCommitProgram.uniform1f(commit_target, 1.f);

    static LLCachedControl<F32> commit_debug_paint_setting(gSavedSettings, "SSAtmoCommitDebugPaint", 0.f);
    const F32 ssCommitDebugPaint = llclamp((F32)commit_debug_paint_setting, 0.f, 1.f);
    gSSSurfaceCommitProgram.uniform1f(commit_paint, ssCommitDebugPaint);

    {
        LLGLDepthTest depth(GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR)
        {
            LL_WARNS("AtmoMagic") << "GL error 0x" << std::hex << (U32)err << std::dec
                                  << " after the commit draw" << LL_ENDL;
        }
    }

    if (ssCommitDebugPaint > 0.f)
    {
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        GLint bound_fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fbo);

        const S32 inset = 4;
        const struct { const char* name; S32 x, y; } points[5] = {
            { "centre", (S32)(w / 2), (S32)(h / 2) },
            { "top-left",     inset,            (S32)h - 1 - inset },
            { "top-right",    (S32)w - 1 - inset, (S32)h - 1 - inset },
            { "bottom-left",  inset,            inset },
            { "bottom-right", (S32)w - 1 - inset, inset },
        };

        std::ostringstream line;
        line << "Commit readback, FBO " << bound_fbo << " tex "
            << gbuffer->getTexture(0) << ":";
        for (const auto& pt : points)
        {
            U8 px[4] = { 0, 0, 0, 0 };
            glReadPixels(pt.x, pt.y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            line << " " << pt.name << "=(" << (int)px[0] << "," << (int)px[1]
                << "," << (int)px[2] << "," << (int)px[3] << ")";
        }
        LL_INFOS("AtmoMagic") << line.str() << LL_ENDL;
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gSSSurfaceCommitProgram.unbind();

    if (do_normal && mScratchNormal.getNumTextures() >= 1)
    {
        // frag_data[2] - the commit shader writes every output, so the mask must route the
        // normal one here (the old layout routed slot 1, which the generalized commit no longer
        // pairs with this target).
        const GLenum normal_bufs[4] = { GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT2, GL_NONE };
        glDrawBuffers(4, normal_bufs);

        gSSSurfaceCommitProgram.bind();
        gGL.getTexUnit(0)->activate();
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mScratchNormal.getTexture(0));
        gSSSurfaceCommitProgram.uniform1i(commit_src, 0);
        gSSSurfaceCommitProgram.uniform1f(commit_target, 2.f);
        gSSSurfaceCommitProgram.uniform1f(commit_paint, 0.f);

        {
            LLGLDepthTest depth(GL_FALSE);
            LLGLDisable blend(GL_BLEND);
            LLGLDisable scissor(GL_SCISSOR_TEST);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }

        {
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR)
            {
                LL_WARNS("AtmoMagic") << "GL error 0x" << std::hex << (U32)err << std::dec
                                      << " after the normal commit draw" << LL_ENDL;
            }
        }

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gSSSurfaceCommitProgram.unbind();
    }

    const GLenum restore[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                                GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, restore);

    gbuffer->flush();
}

// <SS:Nexii> Was renderSnowPass(): now the albedo pass (doc sec 3) - the same screen-space shape as the wet pass - field window in, scratch target, commit back into the gbuffer - but writing the diffuse attachment. Gated on any of wet/snow/ice/frost/stain having accumulated rather than snow depth alone, since ice glaze and frost and a stain all need this pass with nothing settled in mSnow. Program renamed gSSSurfaceAlbedoProgram elsewhere in the shader manager (out of Slice A's file scope) to match. Runs after the wet pass so it covers it.
void SSSurfaceField::renderAlbedoPass()
{
    if (gCubeSnapshot) return;
    if (!hasWindow()) return;
    if (!gSSSurfaceAlbedoProgram.isComplete()) return;
    if (!gSSSurfaceCommitProgram.isComplete()) return;

    // <SS:Nexii> AUDIT (finding 11): SSAtmoSnowSurfaces/Strength used to early-return the WHOLE pass, which also killed wet darkening, ice, frost and stain the moment a user turned "snow surfaces" off - it now only scales the deposit layer itself (ssSnowStrength, read in the shader's item (g)); the pass's own gate stays the peak check below.
    static LLCachedControl<F32> strength(gSavedSettings, "SSAtmoSnowSurfaceStrength", 1.f);
    static LLCachedControl<bool> snow_on(gSavedSettings, "SSAtmoSnowSurfaces", true);
    const F32 snow_strength = snow_on ? llclamp((F32)strength, 0.f, 2.f) : 0.f;
    if (peakWet() <= 0.f && peakSnow() <= 0.f && peakIce() <= 0.f
        && peakFrost() <= 0.f && peakStain() <= 0.f) return;

    LLRenderTarget* gbuffer = &gPipeline.mRT->deferredScreen;
    const U32 w = gbuffer->getWidth();
    const U32 h = gbuffer->getHeight();
    if (w == 0 || h == 0) return;

    if (mScratch.getWidth() != w || mScratch.getHeight() != h)
    {
        mScratch.release();
        if (!mScratch.allocate(w, h, GL_RGBA, false)) return;
    }

    LL_PROFILE_GPU_ZONE("atmo surface albedo");

    mScratch.bindTarget();

    gPipeline.bindDeferredShader(gSSSurfaceAlbedoProgram);

    const S32 field_channel = gSSSurfaceAlbedoProgram.mActiveTextureChannels;
    bindForShader(gSSSurfaceAlbedoProgram, field_channel);
    // <SS:Nexii> Flow (wind carry, doc sec 8) and state bind here too now - the albedo pass paints ice/frost/deposit tint and needs both.
    const S32 albedo_flow_channel = field_channel + 1;
    bindFlowForShader(gSSSurfaceAlbedoProgram, albedo_flow_channel);
    const S32 albedo_state_channel = albedo_flow_channel + 1;
    bindStateForShader(gSSSurfaceAlbedoProgram, albedo_state_channel);
    bindLooksForShader(gSSSurfaceAlbedoProgram);

    static LLStaticHashedString inv_view("ssFieldInvView");
    static LLStaticHashedString snow_strength_u("ssSnowStrength");

    const glm::mat4 inv = glm::inverse(get_current_modelview());
    gSSSurfaceAlbedoProgram.uniformMatrix4fv(inv_view, 1, GL_FALSE, glm::value_ptr(inv));

    gSSSurfaceAlbedoProgram.uniform1f(snow_strength_u, snow_strength);

    // <SS:Nexii> AUDIT (finding 10): same settings, same names, as ssSurfaceWetF.glsl's own upload (renderWetPass) - the albedo pass declares these uniforms (doc section 3/4) but nothing was ever binding them for THIS program, so they read as GL's default zero: ssWetFlattenCosZero/Full both 0 made ssSurfaceLevel's smoothstep(0,0,x) undefined, and ssWetPuddleDepthFull 0 made every puddle read as instantly full. [interaction: renderWetPass's own copy of this block]
    static LLStaticHashedString wet_puddle_depth_u("ssWetPuddleDepthFull");
    static LLStaticHashedString wet_cos_full_u("ssWetFlattenCosFull");
    static LLStaticHashedString wet_cos_zero_u("ssWetFlattenCosZero");
    static LLStaticHashedString mask_amt_u("ssPuddleMaskAmt");
    static LLStaticHashedString mask_scale_u("ssPuddleMaskScaleM");
    static LLStaticHashedString mask_anchor_u("ssPuddleMaskAnchor");
    static LLCachedControl<F32> puddle_depth_full(gSavedSettings, "SSAtmoWetPuddleDepthFull", 0.02f);
    static LLCachedControl<F32> wet_angle_full(gSavedSettings, "SSAtmoWetFlattenAngleFull", 10.f);
    static LLCachedControl<F32> wet_angle_zero(gSavedSettings, "SSAtmoWetFlattenAngleZero", 30.f);
    static LLCachedControl<F32> m_strength(gSavedSettings, "SSAtmoWetPuddleMask", 0.75f);
    static LLCachedControl<F32> m_scale(gSavedSettings, "SSAtmoWetPuddleMaskScale", 7.f);
    gSSSurfaceAlbedoProgram.uniform1f(wet_puddle_depth_u, llmax((F32)puddle_depth_full, 0.001f));
    gSSSurfaceAlbedoProgram.uniform1f(wet_cos_full_u,
        cosf(llclamp((F32)wet_angle_full, 0.f, 89.f) * DEG_TO_RAD));
    gSSSurfaceAlbedoProgram.uniform1f(wet_cos_zero_u,
        cosf(llclamp((F32)wet_angle_zero, 1.f, 90.f) * DEG_TO_RAD));
    gSSSurfaceAlbedoProgram.uniform1f(mask_amt_u, llclamp((F32)m_strength, 0.f, 1.f));
    // <SS:Nexii> ssPuddleMaskScaleM is used by ssPuddleMaskNoise inside ssSurfaceStateF.glsl, not by this pass file directly - but uniform locations are resolved against the whole LINKED program, not the file that happened to declare them, so this program's copy still needs its own upload.
    gSSSurfaceAlbedoProgram.uniform1f(mask_scale_u, llmax((F32)m_scale, 1.f));
    LLVector3 mask_anchor(0.f, 0.f, 0.f);
    if (LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(
            LLViewerCamera::getInstance()->getOrigin()))
    {
        mask_anchor = cam_region->getOriginAgent();
    }
    gSSSurfaceAlbedoProgram.uniform2f(mask_anchor_u, mask_anchor.mV[VX], mask_anchor.mV[VY]);

    {
        LLGLDepthTest depth(GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    gGL.getTexUnit(field_channel)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(albedo_flow_channel)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(albedo_state_channel)->unbind(LLTexUnit::TT_TEXTURE);
    gPipeline.unbindDeferredShader(gSSSurfaceAlbedoProgram);

    mScratch.flush();

    // Commit the lifted albedo into the diffuse attachment.
    gbuffer->bindTarget();

    const GLenum albedo_bufs[4] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE, GL_NONE };
    glDrawBuffers(4, albedo_bufs);

    gSSSurfaceCommitProgram.bind();
    gGL.getTexUnit(0)->activate();
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mScratch.getTexture(0));
    static LLStaticHashedString snow_commit_src("ssCommitSource");
    static LLStaticHashedString snow_commit_target("ssCommitTarget");
    static LLStaticHashedString snow_commit_paint("ssCommitDebugPaint");
    gSSSurfaceCommitProgram.uniform1i(snow_commit_src, 0);
    gSSSurfaceCommitProgram.uniform1f(snow_commit_target, 0.f);
    gSSSurfaceCommitProgram.uniform1f(snow_commit_paint, 0.f);

    {
        LLGLDepthTest depth(GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gSSSurfaceCommitProgram.unbind();

    const GLenum restore_bufs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                                     GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, restore_bufs);

    gbuffer->flush();
}

// Draws the field over the world for inspection. SSAtmoSnowDebug 1 replaces the wet/puddle
// colouring with the transport's per-cell lift figure - what the drift pool's spawn walk reads.
void SSSurfaceField::renderDebug()
{
    if (mFields.empty()) return;

    static LLCachedControl<S32> snow_debug(gSavedSettings, "SSAtmoSnowDebug", 0);
    const bool lift_view = (S32)snow_debug == 1;

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    static LLCachedControl<F32> radius_setting(gSavedSettings, "SSAtmoSurfaceRadius", 64.f);
    const F32 reach = llclamp((F32)radius_setting, 8.f, 256.f);

    for (const auto& entry : mFields)
    {
        const Field& fld = entry.second;
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp || fld.mN < 2) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const S32 n = fld.mN;
        const F32 cell = fld.mCell;
        const F32 half = cell * 0.45f;

        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < (S32)fld.mZ.size(); ++i)
        {
            if (lift_view)
            {
                const F32 lift = fld.mLift.empty() ? 0.f : fld.mLift[i];
                if (lift <= 0.01f) continue;

                const LLVector3 c(origin.mV[VX] + ((F32)(i % n) + 0.5f) * cell,
                                  origin.mV[VY] + ((F32)(i / n) + 0.5f) * cell,
                                  fld.mZ[i] + 0.06f);
                if ((c - cam).magVecSquared() > reach * reach) continue;

                // cold blue at onset through white to hot orange at saturation
                const F32 t = llclamp(fld.mLift[i], 0.f, 1.f);
                const F32 r = lerp(0.15f, 1.f, t);
                const F32 g = lerp(0.35f, 0.85f, llmin(t * 2.f, 1.f)) * (1.f - 0.55f * llmax(0.f, t - 0.5f) * 2.f);
                const F32 b = lerp(1.f, 0.1f, llclamp(t * 2.f, 0.f, 1.f));
                gGL.color4f(r, g, b, 0.35f + 0.6f * t);

                gGL.vertex3f(c.mV[VX] - half, c.mV[VY] - half, c.mV[VZ]);
                gGL.vertex3f(c.mV[VX] + half, c.mV[VY] - half, c.mV[VZ]);
                gGL.vertex3f(c.mV[VX] + half, c.mV[VY] + half, c.mV[VZ]);

                gGL.vertex3f(c.mV[VX] - half, c.mV[VY] - half, c.mV[VZ]);
                gGL.vertex3f(c.mV[VX] + half, c.mV[VY] + half, c.mV[VZ]);
                gGL.vertex3f(c.mV[VX] - half, c.mV[VY] + half, c.mV[VZ]);
                continue;
            }

            const F32 wet = fld.mWet[i];
            const F32 snow = fld.mSnow[i];
            const F32 puddle = fld.mPuddle[i];
            if (wet < 0.01f && snow < 0.001f && puddle < 0.001f) continue;

            const LLVector3 c(origin.mV[VX] + ((F32)(i % n) + 0.5f) * cell,
                              origin.mV[VY] + ((F32)(i / n) + 0.5f) * cell,
                              fld.mZ[i] + 0.04f);
            if ((c - cam).magVecSquared() > reach * reach) continue;

            F32 r, g, b, a;
            if (puddle > 0.001f)
            {
                const F32 t = llclamp(puddle / 0.05f, 0.f, 1.f);
                r = 0.1f; g = 0.7f; b = 0.9f; a = 0.35f + 0.5f * t;
            }
            else if (snow > 0.001f)
            {
                const F32 t = llclamp(snow / 0.1f, 0.f, 1.f);
                r = 0.85f; g = 0.9f; b = 1.f; a = 0.25f + 0.6f * t;
            }
            else
            {
                r = 0.2f; g = 0.35f; b = 0.8f; a = 0.1f + 0.45f * wet;
            }
            gGL.color4f(r, g, b, a);

            gGL.vertex3f(c.mV[VX] - half, c.mV[VY] - half, c.mV[VZ]);
            gGL.vertex3f(c.mV[VX] + half, c.mV[VY] - half, c.mV[VZ]);
            gGL.vertex3f(c.mV[VX] + half, c.mV[VY] + half, c.mV[VZ]);

            gGL.vertex3f(c.mV[VX] - half, c.mV[VY] - half, c.mV[VZ]);
            gGL.vertex3f(c.mV[VX] + half, c.mV[VY] + half, c.mV[VZ]);
            gGL.vertex3f(c.mV[VX] - half, c.mV[VY] + half, c.mV[VZ]);
        }
        gGL.end();
    }

    gGL.flush();
}

// Every shelter edge lip as a tile plus an arrow along its outward
// direction, coloured per the runoff debug view: 0 the eaves themselves,
// 1 the water each holds and how hard it is draining, 2 the first gate
// holding each quiet one back. context_only paints them all dim, as ground
// for the live particle view.
void SSSurfaceField::renderRunoffLips(U32 view, const LLVector3& cam,
                                      F32 radius_sq, F32 budget, bool context_only) const
{
    const bool granular = SSAtmoMagic::getInstance()->granularWeather();

    for (const auto& entry : mGeometry)
    {
        const Geometry& geom = entry.second;
        if (!geom.valid()) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent();

        auto field_it = mFields.find(entry.first);
        const Field* fld = (field_it != mFields.end() && field_it->second.mN == geom.mN)
                               ? &field_it->second : nullptr;

        if (granular)
        {
            // Granular sheds at the lips: the cascade debug is per lip cell.
            if (geom.mEdgeCells.empty()) continue;

            const S32 n = geom.mN;
            const F32 cell = geom.mCell;
            const F32 half = cell * 0.35f;

            gGL.begin(LLRender::TRIANGLES);
            for (const S32 i : geom.mEdgeCells)
            {
                const size_t ui = (size_t)i;

                const F32 cx = origin.mV[VX] + ((F32)(i % n) + 0.5f) * cell;
                const F32 cy = origin.mV[VY] + ((F32)(i / n) + 0.5f) * cell;
                const F32 cz = geom.mZ[ui] + 0.05f;

                F32 r = 0.4f, g = 0.45f, b = 0.5f, a = 0.12f;
                if (!context_only)
                {
                    const F32 store = fld ? fld->mStore[ui] : 0.f;
                    const F32 outflow = store / SHED_DRAIN_TAU;
                    const F32 raw_rate = llmin(outflow / SHED_MERGE, SHED_MAX_RATE);
                    const F32 t = llclamp(raw_rate / SHED_STREAM_MIN, 0.f, 1.f);

                    r = lerp(0.15f, 0.1f, t);
                    g = lerp(0.3f, 0.7f, t);
                    b = lerp(0.6f, 0.95f, t);
                    a = llmax(0.2f + 0.6f * t, (view == 0) ? 0.3f : 0.f);
                }

                gGL.color4f(r, g, b, a);

                gGL.vertex3f(cx - half, cy - half, cz);
                gGL.vertex3f(cx + half, cy - half, cz);
                gGL.vertex3f(cx + half, cy + half, cz);

                gGL.vertex3f(cx - half, cy - half, cz);
                gGL.vertex3f(cx + half, cy + half, cz);
                gGL.vertex3f(cx - half, cy + half, cz);

                // An arrowhead out over the edge: the way the water leaves.
                const F32 ex = geom.mEdgeX[ui], ey = geom.mEdgeY[ui];
                const F32 px = -ey * half * 0.55f, py = ex * half * 0.55f;
                const F32 bx = cx + ex * half, by = cy + ey * half;
                const F32 tx = cx + ex * half * 2.4f, ty = cy + ey * half * 2.4f;

                gGL.vertex3f(bx + px, by + py, cz);
                gGL.vertex3f(bx - px, by - py, cz);
                gGL.vertex3f(tx, ty, cz);
            }
            gGL.end();
            continue;
        }

        const Geometry::Runoff& ro = geom.mRunoff;
        if (!ro.valid() || ro.mRuns.empty()) continue;

        const F32 cell = ro.mCell;

        gGL.begin(LLRender::LINES);
        for (const Geometry::Run& run : ro.mRuns)
        {
            const LLVector3 centroid(run.mCentroid.mV[VX] + origin.mV[VX],
                                     run.mCentroid.mV[VY] + origin.mV[VY],
                                     run.mCentroid.mV[VZ]);

            F32 r = 0.4f, g = 0.45f, b = 0.5f, a = 0.12f;
            if (!context_only)
            {
                if (view == 2)
                {
                    // The first gate that still holds this run back - dry,
                    // beyond the shed radius, starved by the drip budget, or
                    // actually producing.
                    F32 store = 0.f;
                    if (fld)
                    {
                        auto store_it = fld->mRunStore.find(run.mKey);
                        if (store_it != fld->mRunStore.end()) store = store_it->second;
                    }
                    const F32 outflow = store / SHED_DRAIN_TAU;
                    const F32 raw_rate = llmin(outflow / SHED_MERGE, SHED_MAX_RATE);
                    const F32 stream_drive = llclamp((raw_rate - SHED_STREAM_MIN)
                                                         / SHED_STREAM_FULL, 0.f, 1.f);

                    if (outflow <= 0.01f)
                    {
                        r = 0.35f; g = 0.35f; b = 0.38f; a = 0.15f;
                    }
                    else
                    {
                        const LLVector3 delta = centroid - cam;
                        if (delta.magVecSquared() > radius_sq)
                        {
                            r = 1.f; g = 0.25f; b = 0.2f; a = 0.5f;
                        }
                        else if (stream_drive > 0.f)
                        {
                            r = 1.f; g = 1.f; b = 1.f; a = 0.85f;
                        }
                        else if (budget <= 0.f)
                        {
                            r = 1.f; g = 0.2f; b = 1.f; a = 0.6f;
                        }
                        else
                        {
                            r = 0.2f; g = 1.f; b = 0.35f;
                            a = 0.35f + 0.4f * budget;
                        }
                    }
                }
                else if (view == 1)
                {
                    // Water the run holds, ramped the same slate-to-white the
                    // lips drew: filling toward the stream threshold, white
                    // once the run is driving a curtain.
                    F32 store = 0.f;
                    if (fld)
                    {
                        auto store_it = fld->mRunStore.find(run.mKey);
                        if (store_it != fld->mRunStore.end()) store = store_it->second;
                    }
                    const F32 outflow = store / SHED_DRAIN_TAU;
                    const F32 raw_rate = llmin(outflow / SHED_MERGE, SHED_MAX_RATE);
                    const F32 stream_drive = llclamp((raw_rate - SHED_STREAM_MIN)
                                                         / SHED_STREAM_FULL, 0.f, 1.f);
                    const F32 t = llclamp(raw_rate / SHED_STREAM_MIN, 0.f, 1.f);

                    r = lerp(lerp(0.15f, 0.1f, t), 1.f, stream_drive);
                    g = lerp(lerp(0.3f, 0.7f, t), 1.f, stream_drive);
                    b = lerp(lerp(0.6f, 0.95f, t), 1.f, stream_drive);
                    a = llmax(0.2f + 0.6f * t, 0.f);
                }
                else
                {
                    // The network itself: brightening with the catchment the
                    // run collects, so a gutter reads as a gutter between
                    // rains as well as under them.
                    const F32 t = llclamp(run.mCatch / 64.f, 0.f, 1.f);
                    r = lerp(0.15f, 0.1f, t);
                    g = lerp(0.3f, 0.7f, t);
                    b = lerp(0.6f, 0.95f, t);
                    a = 0.2f + 0.6f * t;
                }
            }

            gGL.color4f(r, g, b, a);

            // The run as the connected line it is.
            const size_t members = run.mLips.size();
            for (size_t k = 0; k + 1 < members; ++k)
            {
                const LLVector3& a_lip = run.mLips[k];
                const LLVector3& b_lip = run.mLips[k + 1];
                gGL.vertex3f(a_lip.mV[VX] + origin.mV[VX], a_lip.mV[VY] + origin.mV[VY],
                             a_lip.mV[VZ] + 0.05f);
                gGL.vertex3f(b_lip.mV[VX] + origin.mV[VX], b_lip.mV[VY] + origin.mV[VY],
                             b_lip.mV[VZ] + 0.05f);
            }

            // An arrowhead out over the edge at the run's middle: the way the
            // water leaves.
            const F32 ex = run.mOut.mV[VX], ey = run.mOut.mV[VY];
            const F32 half = cell * 0.5f;
            const F32 px = -ey * half * 0.55f, py = ex * half * 0.55f;
            const F32 bx = centroid.mV[VX] + ex * half, by = centroid.mV[VY] + ey * half;
            const F32 tx = centroid.mV[VX] + ex * half * 2.4f;
            const F32 ty = centroid.mV[VY] + ey * half * 2.4f;
            const F32 tz = centroid.mV[VZ] + 0.05f;

            gGL.vertex3f(bx + px, by + py, tz);
            gGL.vertex3f(bx - px, by - py, tz);
            gGL.vertex3f(tx, ty, tz);
        }
        gGL.end();
    }
}

// The runoff overlay: one stage of the shed per SSAtmoRunoffDebugView - the
// runs and the catchment they collect, the water they hold, the gates that
// quiet them, or the drips and gutter streams they have actually spawned.
// Granular weather sheds at the lips instead, so every view draws the per-lip
// cascade surface there.
void SSSurfaceField::renderRunoffDebug()
{
    static LLCachedControl<U32> view_setting(gSavedSettings, "SSAtmoRunoffDebugView", 0);
    const U32 view = llclamp((U32)view_setting, 0u, 3u);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSPrecipSim* sim = atmo ? atmo->sim() : nullptr;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    static LLCachedControl<F32> radius_setting(gSavedSettings, "SSAtmoRunoffRadius", 48.f);
    const F32 radius = llclamp((F32)radius_setting, 8.f, 128.f);
    const F32 radius_sq = radius * radius;

    const S32 live = sim ? sim->dripCount() : 0;
    const F32 budget = (live >= DRIP_BUDGET) ? 0.f
                     : llmin(1.f, (F32)(DRIP_BUDGET - live) / (F32)(DRIP_BUDGET / 4));

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    if (view == 3)
    {
        // What the shed produced: each live drip as a short trail against
        // its fall, each gutter stream as the span it sweeps from its lip.
        if (sim)
        {
            gGL.begin(LLRender::LINES);
            for (const SSPrecipParticle& p : sim->ripples())
            {
                if (!(p.mFlags & PART_DRIP)) continue;

                LLVector3 fall = p.mVel;
                if (fall.magVecSquared() > 0.0001f) fall.normVec();
                else fall.setVec(0.f, 0.f, -1.f);

                gGL.color4f(1.f, 0.35f, 0.85f, 0.9f);
                gGL.vertex3fv(p.mPos.mV);
                gGL.vertex3fv((p.mPos - fall * 0.25f).mV);
            }

            for (const SSPrecipParticle& s : sim->streams())
            {
                gGL.color4f(0.4f, 1.f, 1.f, 0.8f);
                gGL.vertex3fv(s.mPos.mV);
                gGL.vertex3fv((s.mPos + s.mNormal * (s.mSizeX * 2.f)).mV);
            }
            gGL.end();
        }

        renderRunoffLips(view, cam, radius_sq, budget, true);
    }
    else
    {
        renderRunoffLips(view, cam, radius_sq, budget, false);
    }

    gGL.flush();
}
