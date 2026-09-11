/**
 * @file ssworldfield.cpp
 * @brief See ssworldfield.h.
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

#include "ssworldfield.h"
#include "ssworldfieldcore.h"
#include "ssatmomagic.h"
#include "ssworldfieldshapes.h"
#include "ssnavmesh.h"

#include "llfasttimer.h"
#include "llrender.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "workqueue.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"        // the overlay's LLGLEnable/LLGLDepthTest

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <queue>

static const F32 NEIGHBOR_REACH   = 64.f;
static const F32 NO_SURFACE       = -FLT_MAX;

// <SS:Nexii> The enclosure spectrum's floor constant: the shortest saturation
// length the ramp ever uses. The real one is the local gap's own cot(theta)
// reach (see SS_WF_OPEN_K), so a hall or the underside of a sky platform
// saturates over tens of metres while a crawlspace saturates over one; this is
// what stops a gap thinner than a person from saturating instantly.
static const F32 SS_WF_ENCLOSURE_TAU_M = 4.f;

// <SS:Nexii> How many solid spans a cell may hold. Real content runs two to
// five; a cell that resolves past the cap merges its smallest air gap rather
// than dropping a body. Must match the navmesh sheet's own budget - the sheets
// are the geometry, and a mismatch would silently truncate them.
static constexpr S32 SS_WF_MAX_SPANS = 6;
static_assert(SS_WF_MAX_SPANS == SSNavMesh::SpanSheet::SPANS,
              "the cell grid's span budget is the navmesh sheet's span budget");
// <SS:Nexii> The core mirrors these as plain constants so it never includes this
// header; this is the lockstep check. [interaction: SSWorldFieldCore]
static_assert((U8)SSWorldField::AIR_SOLID     == SSWorldFieldCore::AIR_SOLID
           && (U8)SSWorldField::AIR_OUTDOORS  == SSWorldFieldCore::AIR_OUTDOORS
           && (U8)SSWorldField::AIR_SHELTERED == SSWorldFieldCore::AIR_SHELTERED
           && (U8)SSWorldField::AIR_INTERIOR  == SSWorldFieldCore::AIR_INTERIOR
           && (U8)SSWorldField::AIR_UNKNOWN   == SSWorldFieldCore::AIR_UNKNOWN,
              "SSWorldFieldCore's air labels are SSWorldField::EAirLabel");
static_assert((U16)SSWorldField::AIR_DEPTH_UNREACHED == SSWorldFieldCore::DEPTH_UNREACHED,
              "SSWorldFieldCore's unreached sentinel is SSWorldField's");

// <SS:Nexii> Headroom above the highest band the navmesh published: the grid's
// ceiling. It only sets where a column's top gap ends, and that gap is open
// sky by construction, so the value is a bound rather than a measurement.
static const F32 SS_WF_CEILING_HEADROOM_M = 8.f;

static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD("Atmo Magic World Field");
static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD_GRID("Atmo Magic World Field Grid");

// Channel interest refcounts. File statics so an Interest handle's deleter
// stays valid for the process's life regardless of singleton teardown
// order - a destroyed handle must always release its count.
static std::map<std::pair<U64, S32>, S32> sInterests;

// <SS:Nexii> The debug overlay's materialised drainage view, cached per region and rebuilt only when the grid's geometry serial moves - the same drop-on-serial-change discipline the real channels follow, so the overlay never pays a per-frame priority flood just to draw. Declared with the other file statics because evict() drops it alongside the grids it belongs to.
struct SS_WF_DrainDebug
{
    SSRainShadowMap::SurfaceGrid mGrid;
    SSWorldField::Drainage mDrain;
    U32 mSerial = 0;
};
static std::map<U64, SS_WF_DrainDebug> sDrainDebug;

static S32 ss_wf_interest_count(U64 region_handle, S32 channel)
{
    auto it = sInterests.find(std::make_pair(region_handle, channel));
    return (it != sInterests.end()) ? it->second : 0;
}

// The grid's cell size in metres: 0.25 is the navmesh sheet's own cell, so it
// costs no resampling and keeps door- and window-scale openings resolved for
// the reach walk. Coarser unions whole sheet cells together.
static F32 ss_wf_cell_setting()
{
    static LLCachedControl<F32> cell(gSavedSettings, "SSWorldFieldCell", 0.25f);
    return llclamp((F32)cell, 0.25f, 8.f);
}

// cot(theta) for the outdoors reach: how many metres of covered space one
// metre of gap height hands inward before the sky stops being overhead.
static F32 ss_wf_open_k()
{
    static LLCachedControl<F32> angle(gSavedSettings, "SSWorldFieldOpenAngle", 45.f);
    const F32 t = llclamp((F32)angle, 5.f, 85.f) * DEG_TO_RAD;
    return llclamp(cosf(t) / llmax(sinf(t), 0.01f), 0.1f, 12.f);
}

// How far a query point with no air at its own height may walk DOWN looking
// for one before the field calls it open air.
static F32 ss_wf_ground_reach()
{
    static LLCachedControl<F32> reach(gSavedSettings, "SSWorldFieldGroundReach", 6.f);
    return llclamp((F32)reach, 0.5f, 64.f);
}

SSWorldField::Interest SSWorldField::claim(U64 region_handle, EChannel channel)
{
    const std::pair<U64, S32> key(region_handle, (S32)channel);
    ++sInterests[key];

    return Interest(std::shared_ptr<void>((void*)1, [key](void*)
    {
        auto it = sInterests.find(key);
        if (it != sInterests.end() && --(it->second) <= 0)
        {
            sInterests.erase(it);
        }
    }));
}

// The wet field's source switch - while on, SURFACE_TOP counts as claimed
// for the camera region and its neighbours.
bool SSWorldField::surfaceTopDemanded() const
{
    static LLCachedControl<bool> demanded(gSavedSettings, "SSWorldFieldSurfaceTop", false);
    return demanded;
}

// <SS:Nexii> The edit fan-out is a pass-through now. The field's geometry is the navmesh's band sheets and the navmesh already rebuilds the bands an edit's census change touches; there is no capture here to scissor and no store here to splice, so marking a rectangle dirty would only duplicate work the census does better. The field learns about the edit when the region settles again with a different sheet-set stamp. [interaction: SSWorldFieldShapes::markDirty, SSNavMesh::collectSheets]
void SSWorldField::markDirty(const LLVector3& pos_agent, F32 radius)
{
    SSWorldFieldShapes::markDirty(pos_agent, radius);
}

void SSWorldField::clear()
{
    mTiles.clear();
    sDrainDebug.clear();
    ++mGridGeneration;      // a classification in flight lands into nothing
}

// The field holds no GL resources; the display teardown path still calls this, so it is clear().
void SSWorldField::shutdownGL()
{
    clear();
}

// The serial of the PUBLISHED grid - the spans and labels a query is about to
// read - or 0 when the region has none.
U32 SSWorldField::gridSerial(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    return (it != mTiles.end() && it->second.hasGrid()) ? it->second.mGridSerial : 0;
}

// A rebuild is owed: the navmesh's sheet set moved since the published grid was classified.
bool SSWorldField::gridStale(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    return (it != mTiles.end()) && !current(it->second);
}

S32 SSWorldField::resolution() const
{
    for (const auto& entry : mTiles)
    {
        if (entry.second.hasGrid()) return entry.second.mRes;
    }
    return 0;
}

F32 SSWorldField::cellSize() const
{
    return ss_wf_cell_setting();
}

F32 SSWorldField::ceilingAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    return (tile && tile->hasGrid()) ? tile->mCeiling : 0.f;
}

S32 SSWorldField::sheetsAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    return tile ? tile->mSheets : 0;
}

F64 SSWorldField::tileAge(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->hasGrid()) return -1.0;
    return mNow - tile->mBuiltAt;
}

const SSWorldField::Tile* SSWorldField::tileAt(const LLVector3& pos_agent) const
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return nullptr;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end()) return nullptr;
    return &it->second;
}

void SSWorldField::dumpColumn(const LLVector3& pos_agent, std::vector<std::string>& out) const
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    const Tile* tile = tileAt(pos_agent);
    if (!regionp || !tile) { out.push_back("world field: no grid under the point"); return; }
    out.push_back(llformat("world field grid: %s, %d cells/axis (%.2f m), ceiling %.1f m, %d band sheets, %.0f%% surveyed, geometry serial %u, grid serial %u%s",
                           tile->hasGrid() ? "published" : "NOT BUILT YET", tile->mRes, tile->mCell, tile->mCeiling, tile->mSheets,
                           airCoverage(tile->mRegionHandle) * 100.f,
                           tile->mGeomSerial, tile->mGridSerial, current(*tile) ? "" : " (REBUILD PENDING)"));
    if (!tile->hasGrid()) return;
    const LLVector3 rp = regionp->getPosRegionFromAgent(pos_agent);
    const S32 x = llclamp((S32)(rp.mV[VX] / tile->mCell), 0, tile->mRes - 1), y = llclamp((S32)(rp.mV[VY] / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)y * tile->mRes + x, layer = (size_t)tile->mRes * tile->mRes;
    static const char* AIR_NAME[] = {"solid", "outdoors", "sheltered", "interior", "unknown"};
    if (!(*tile->mSurveyed)[col])
    {
        out.push_back(llformat("  cell %d,%d: NOT SURVEYED - no band sheet covers it, so it has no geometry and every gap reads unknown", x, y));
        return;
    }
    auto depth_str = [&](U16 d) { return (d == (U16)AIR_DEPTH_UNREACHED) ? std::string("unreached") : llformat("%.1f m covered", (F32)d * 0.1f); };
    S32 n = 0;
    for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
    {
        const size_t si = (size_t)k * layer + col;
        if ((*tile->mSpanTop)[si] <= NO_SURFACE * 0.5f) break;
        const U8 gl = (*tile->mGapLabel)[col * (SS_WF_MAX_SPANS + 1) + k];
        out.push_back(llformat("  gap %d below: %s, %s; span %d: z %.2f..%.2f flags 0x%02x%s%s", k, AIR_NAME[llclamp((S32)gl, 0, 4)],
                               depth_str((*tile->mGapDepth)[col * (SS_WF_MAX_SPANS + 1) + k]).c_str(), k, (*tile->mSpanBottom)[si], (*tile->mSpanTop)[si], (*tile->mSpanFlags)[si],
                               ((*tile->mSpanFlags)[si] & SSRainShadowMap::SURF_FALLBACK) ? " terrain" : "", ((*tile->mSpanFlags)[si] & SSRainShadowMap::SURF_WATER) ? " water" : ""));
        ++n;
    }
    const U8 top_gl = (*tile->mGapLabel)[col * (SS_WF_MAX_SPANS + 1) + n];
    out.push_back(llformat("  gap %d above: %s, %s (cell %d,%d, %d spans)", n, AIR_NAME[llclamp((S32)top_gl, 0, 4)],
                           depth_str((*tile->mGapDepth)[col * (SS_WF_MAX_SPANS + 1) + n]).c_str(), x, y, n));
    out.push_back(llformat("  at the point: air %s, enclosure %.2f", AIR_NAME[llclamp((S32)airLabelAt(pos_agent), 0, 4)], enclosureAt(pos_agent)));
}

// The camera's region and any region within NEIGHBOR_REACH of the camera - the reach the field keeps grids for.
bool SSWorldField::regionNear(const LLViewerRegion* regionp, const LLVector3& cam) const
{
    const LLVector3 origin = regionp->getOriginAgent();
    const F32 width = regionp->getWidth();
    const F32 dx = llmax(origin.mV[VX] - cam.mV[VX], cam.mV[VX] - (origin.mV[VX] + width), 0.f);
    const F32 dy = llmax(origin.mV[VY] - cam.mV[VY], cam.mV[VY] - (origin.mV[VY] + width), 0.f);
    return dx * dx + dy * dy <= NEIGHBOR_REACH * NEIGHBOR_REACH;
}

// ---------------------------------------------------------------------------- the cell grid

// <SS:Nexii> The adapter between the navmesh's published sheets and the core's
// materialisation: region-local corners, raw pointers, and the census's terrain flag.
// All the arithmetic lives in SSWorldFieldCore::buildGrid, where the harness pins it -
// a half-cell shift in that mapping would silently poison every downstream answer and
// nothing in the viewer would notice. A sheet whose vectors are the wrong size is one
// whose band build bailed out before filling it; it is skipped, and the cells it would
// have covered stay UNSURVEYED rather than reading as empty sky.
// [interaction: SSWorldFieldCore::buildGrid, SSNavMesh::extractSpanSheet]
static void ss_wf_build_grid(S32 res, F32 cell, const LLVector3& region_origin,
                             const std::vector<SSNavMesh::BandSheet>& sheets,
                             std::vector<F32>& span_bottom, std::vector<F32>& span_top,
                             std::vector<U8>& span_flags, std::vector<U8>& surveyed)
{
    const size_t layer = (size_t)res * res;
    span_bottom.assign((size_t)SS_WF_MAX_SPANS * layer, NO_SURFACE);
    span_top.assign((size_t)SS_WF_MAX_SPANS * layer, NO_SURFACE);
    span_flags.assign((size_t)SS_WF_MAX_SPANS * layer, (U8)0);
    surveyed.assign(layer, (U8)0);

    const S32 R = SSNavMesh::SpanSheet::RES, K = SSNavMesh::SpanSheet::SPANS;

    std::vector<SSWorldFieldCore::SheetRef> refs;
    refs.reserve(sheets.size());
    for (const SSNavMesh::BandSheet& bs : sheets)
    {
        const SSNavMesh::SpanSheet* sheet = bs.mSheet.get();
        if (!sheet) continue;
        if (sheet->mCount.size() != (size_t)R * R || sheet->mBottom.size() != sheet->mCount.size() * K
            || sheet->mTop.size() != sheet->mBottom.size() || sheet->mFlags.size() != sheet->mBottom.size())
        {
            continue;
        }

        SSWorldFieldCore::SheetRef ref;
        ref.mX0 = bs.mOriginAgent.mV[VX] - region_origin.mV[VX];
        ref.mY0 = bs.mOriginAgent.mV[VY] - region_origin.mV[VY];
        ref.mExtent = SSNavMesh::TILE_M;
        ref.mRes = R;
        ref.mSpans = K;
        ref.mCount = sheet->mCount.data();
        ref.mBottom = sheet->mBottom.data();
        ref.mTop = sheet->mTop.data();
        ref.mFlags = sheet->mFlags.data();
        refs.push_back(ref);
    }

    SSWorldFieldCore::buildGrid(res, cell, SS_WF_MAX_SPANS, refs.data(), (S32)refs.size(),
                                (U8)SSRainShadowMap::SURF_FALLBACK,
                                span_bottom.data(), span_top.data(), span_flags.data(), surveyed.data());
}

// ---------------------------------------------------------------------------- per frame

// <SS:Nexii> Follow the navmesh: a grid slot for every region in reach (the tile cap decides how many), and a region the navmesh has finished with - nothing queued, nothing building, no schedule pending - whose sheet-set stamp differs from the published grid's gets its geometry serial moved, which is what sends the classification job over it. An unchanged stamp is a no-op: a settle that rebuilt nothing must not re-run a region-wide walk. [interaction: SSNavMesh::regionSettled, scheduleGrid]
void SSWorldField::navSettle()
{
    SSNavMesh* nav = SSNavMesh::getInstance();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 cell = ss_wf_cell_setting();

    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || !regionNear(regionp, cam)) continue;
        auto it = mTiles.find(regionp->getHandle());
        if (it == mTiles.end())
        {
            if (mTiles.size() >= MAX_TILES)
            {
                // Make room with a grid out of reach; none out of reach means
                // this region waits its turn.
                auto victim = mTiles.end();
                for (auto vt = mTiles.begin(); vt != mTiles.end(); ++vt)
                {
                    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromHandle(vt->first);
                    if (!r || !regionNear(r, cam)) { victim = vt; break; }
                }
                if (victim == mTiles.end()) continue;
                sDrainDebug.erase(victim->first);
                mTiles.erase(victim);
                ++mGridGeneration;
            }
            Tile& fresh = mTiles[regionp->getHandle()];
            fresh.mRegionHandle = regionp->getHandle();
            it = mTiles.find(regionp->getHandle());
        }
        Tile& tile = it->second;
        tile.mLastTouched = mNow;

        if (!nav->regionSettled(tile.mRegionHandle)) continue;

        std::vector<SSNavMesh::BandSheet> sheets;
        U64 stamp = 0;
        if (!nav->collectSheets(tile.mRegionHandle, sheets, stamp)) continue;

        // <SS:Nexii> No sheets is NO ANSWER, never an empty world. The census envelope has not reached this region yet (or has left it), and classifying nothing would publish a grid of pure sky that told every consumer "outdoors" with full confidence - exactly the wrong direction, and it would retire the raycast fallbacks the readers keep for this case. Drop whatever was published and go back to having no grid. [interaction: airLabelAt returning AIR_UNKNOWN]
        if (sheets.empty())
        {
            if (tile.mValid || tile.mGeomSerial != 0)      // mValid, not hasGrid: reset even a half-published one
            {
                tile = Tile();
                tile.mRegionHandle = regionp->getHandle();
                tile.mLastTouched = mNow;
                ++mGridGeneration;      // a classification in flight for this region lands into nothing
                sDrainDebug.erase(tile.mRegionHandle);
            }
            continue;
        }

        // <SS:Nexii> The cell setting joins the stamp: a grid built at 0.25 m does not describe the world at 1 m, and nothing else would notice the change.
        stamp ^= (U64)llround(cell * 1024.f) * 0x100000001B3ull;
        if (stamp == tile.mWantStamp) continue;

        tile.mWantStamp = stamp;
        tile.mSettledAt = mNow;
        tile.mGeomSerial = (tile.mGeomSerial == 0xFFFFFFFFu) ? 1 : tile.mGeomSerial + 1;
    }
}

// Per-frame drive: drop grids for regions that left, follow the navmesh's
// settles, and hand the one region that owes a classification to the worker.
void SSWorldField::update()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD);

    mNow = SSAtmoMagic::getInstance()->sharedTime();

    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldField", true);
    // <SS:Nexii> Master switch, not hasWeather(): the soundscape and the fog consume the field in calm weather too.
    if (!enabled)
    {
        if (!mTiles.empty()) clear();
        mWasEnabled = false;
        return;
    }
    // <SS:Nexii> Off->on: every band the navmesh rebuilt while the switch was down
    // holds no sheet, and nothing would ever ask it for one again - schedule() only
    // enqueues a band whose geometry signature moved. Their cells would read as
    // unsurveyed for the rest of the session. Ask the navmesh to rebuild them.
    // [interaction: SSNavMesh::resheet]
    if (!mWasEnabled)
    {
        mWasEnabled = true;
        if (SSNavMesh::instanceExists())
        {
            const S32 queued = SSNavMesh::getInstance()->resheet();
            if (queued) LL_INFOS("SSWorldField") << "world field enabled: asking the navmesh to rebuild " << queued << " bands that hold no span sheet" << LL_ENDL;
        }
    }
    // <SS:Nexii> The Atmo gate flickers: a death teleport or an altitude with no track resolves no environment for a while, and clearing the store on every dip threw away every grid and every consumer's overlay with it. The setting still clears; the Atmo gate only pauses. Both edges log.
    const bool gate_on = SSAtmoMagic::getInstance()->isEnabled();
    if (!gate_on)
    {
        if (!mGateWasOff) LL_INFOS("SSWorldField") << "world field paused: Atmo Magic resolved no environment (" << mTiles.size() << " grids kept)" << LL_ENDL;
        mGateWasOff = true;
        return;
    }
    if (mGateWasOff)
    {
        mGateWasOff = false;
        LL_INFOS("SSWorldField") << "world field resumed: Atmo Magic environment back, " << mTiles.size() << " grids held" << LL_ENDL;
    }

    evict();

    // <SS:Nexii> The navmesh is the field's only geometry source now, and both it and the census it needs default OFF. A field switched on over a navmesh that is not running would otherwise do nothing, for ever, in silence - and every consumer would quietly sit on its raycast fallback wondering why. Say it once. [interaction: SSNavMesh, SSWorldFieldShapes]
    if (!SSNavMesh::instanceExists() || !SSNavMesh::getInstance()->active())
    {
        if (!mWarnedNoNavMesh)
        {
            mWarnedNoNavMesh = true;
            LL_WARNS("SSWorldField") << "world field is on but the census navmesh is not running: it has no geometry source and will answer nothing. "
                                     << "Needs SSNavMesh, which needs SSWorldFieldShapes." << LL_ENDL;
        }
        if (!mTiles.empty()) clear();
        return;
    }
    mWarnedNoNavMesh = false;
    navSettle();

    // One classification at a time. A region that settled while one ran simply
    // gets its turn here - oldest settle first is unnecessary at this scale
    // (four grids), so the first that owes one wins.
    if (mBuildBusy)
    {
        // <SS:Nexii> The watchdog. A dropped completion - a closing queue, a shutdown
        // race - would otherwise leave the flag set and retire the field for the rest
        // of the session with no symptom but silence. Moving the generation first
        // means a completion that does eventually land is refused rather than
        // overwriting whatever the retry publishes.
        if (mNow - mBuildStartedAt < BUILD_WATCHDOG_S) return;
        LL_WARNS("SSWorldField") << "classification job has not returned in " << (mNow - mBuildStartedAt)
                                 << " s; abandoning it and retrying" << LL_ENDL;
        ++mGridGeneration;
        mBuildBusy = false;
    }
    for (auto& entry : mTiles)
    {
        Tile& tile = entry.second;
        if (tile.mGeomSerial == 0) continue;                        // the navmesh has never settled this region
        if (tile.mGridSerial == tile.mGeomSerial) continue;         // the grid is current
        if (mNow - tile.mSettledAt < SETTLE_DEBOUNCE) continue;     // bands still trickling in
        scheduleGrid(tile);
        break;
    }
}

// Drops grids for departed regions, then the least recently used beyond the
// cache cap. Erasing a grid also moves the generation - a classification in
// flight for the departed region must not land on a fresh grid the same region
// handle re-created (restarted at serial 0, so the serial gate alone would not
// stop it) - and drops its cached debug views.
void SSWorldField::evict()
{
    bool erased = false;
    for (auto it = mTiles.begin(); it != mTiles.end();)
    {
        if (!LLWorld::getInstance()->getRegionFromHandle(it->first))
        {
            sDrainDebug.erase(it->first);
            it = mTiles.erase(it);
            erased = true;
        }
        else
        {
            ++it;
        }
    }
    while ((S32)mTiles.size() > (S32)MAX_TILES)
    {
        auto oldest = mTiles.begin();
        for (auto it = mTiles.begin(); it != mTiles.end(); ++it)
        {
            if (it->second.mLastTouched < oldest->second.mLastTouched) oldest = it;
        }
        sDrainDebug.erase(oldest->first);
        mTiles.erase(oldest);
        erased = true;
    }
    if (erased) ++mGridGeneration;
}

// Resolves the landing-surface grid for a region - SSRainShadowMap's exact
// contract, sourced from the cell grid, not a private capture. The first thing
// a falling drop meets is the cell's highest solid span, so the span list is
// scanned top-down and the first hit wins.
bool SSWorldField::buildSurfaceGrid(U64 region_handle, S32 n, SSRainShadowMap::SurfaceGrid& out)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD_GRID);

    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.hasGrid()) return false;

    const Tile& tile = it->second;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return false;

    n = llclamp(n, 16, 512);
    const F32 width = regionp->getWidth();

    out.mRegionHandle = region_handle;
    out.mN = n;
    out.mCell = width / (F32)n;
    out.mGeomSerial = tile.mGridSerial;
    out.mZ.assign((size_t)n * n, -FLT_MAX);
    out.mFlags.assign((size_t)n * n, 0);
    out.mAbove.assign((size_t)n * n, 0.f);

    const F32 water_z = regionp->getWaterHeight();
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool sky = atmo->isSkyTrack();
    const F32 sky_floor = atmo->groundZero();

    for (S32 gy = 0; gy < n; ++gy)
    {
        for (S32 gx = 0; gx < n; ++gx)
        {
            const S32 cx = llclamp((S32)(((F32)gx + 0.5f) * (F32)tile.mRes / (F32)n), 0, tile.mRes - 1);
            const S32 cy = llclamp((S32)(((F32)gy + 0.5f) * (F32)tile.mRes / (F32)n), 0, tile.mRes - 1);

            const size_t col = (size_t)cy * tile.mRes + cx;
            const size_t oidx = (size_t)gy * n + gx;

            F32 z = -FLT_MAX;
            U8 flags = 0;

            // The cell's landing surface: the highest solid span's top. A cell no
            // sheet covered has none, and drops through to the heightmap fallback
            // below rather than reporting the sky it appears to be.
            for (S32 k = (*tile.mSurveyed)[col] ? SS_WF_MAX_SPANS - 1 : -1; k >= 0; --k)
            {
                const size_t si = (size_t)k * (size_t)tile.mRes * (size_t)tile.mRes + col;
                if ((*tile.mSpanTop)[si] > -FLT_MAX * 0.5f)
                {
                    z = (*tile.mSpanTop)[si];
                    flags = (*tile.mSpanFlags)[si];
                    break;
                }
            }

            if (z > -FLT_MAX * 0.5f)
            {
                if (!sky && z < water_z)
                {
                    out.mZ[oidx] = water_z;
                    out.mFlags[oidx] = SSRainShadowMap::SURF_MAPPED | SSRainShadowMap::SURF_WATER;
                }
                else
                {
                    out.mZ[oidx] = z;
                    out.mFlags[oidx] = flags | SSRainShadowMap::SURF_MAPPED;

// Same figure the rain shadow builder derives: metres over the
                    // terrain-or-water reference (the sky track floor in a skybox),
                    // so both sources hand consumers identical ground-vs-structure
                    // data and the SSWorldFieldSurfaceTop switch stays behaviour-
                    // neutral.
                    F32 ground;
                    if (sky)
                    {
                        ground = sky_floor;
                    }
                    else
                    {
                        const LLVector3 centre(regionp->getOriginAgent().mV[VX] + out.axis(gx),
                                               regionp->getOriginAgent().mV[VY] + out.axis(gy),
                                               water_z);
                        ground = llmax(LLWorld::getInstance()->resolveLandHeightAgent(centre), water_z);
                    }
                    out.mAbove[oidx] = llmax(z - ground, 0.f);
                }
            }
            else if (sky)
            {
                out.mZ[oidx] = sky_floor;
                out.mFlags[oidx] = 0;
            }
            else
            {
                const LLVector3 centre(regionp->getOriginAgent().mV[VX] + out.axis(gx),
                                       regionp->getOriginAgent().mV[VY] + out.axis(gy),
                                       water_z);
                const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(centre);
                out.mZ[oidx] = llmax(land, water_z);
                out.mFlags[oidx] = SSRainShadowMap::SURF_FALLBACK | ((water_z > land) ? SSRainShadowMap::SURF_WATER : 0);
            }
        }
    }

    return true;
}

void SSWorldField::validTiles(std::vector<std::pair<U64, U32> >& out) const
{
    out.clear();
    out.reserve(mTiles.size());
    for (const auto& entry : mTiles)
    {
        if (entry.second.hasGrid())
        {
            out.emplace_back(entry.first, entry.second.mGridSerial);
        }
    }
}

bool SSWorldField::surfaceTop(const LLVector3& pos_agent, F32& z, U8& flags) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->hasGrid()) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);

    const size_t col = (size_t)cy * tile->mRes + cx;
    if (!(*tile->mSurveyed)[col]) return false;     // no sheet covered this cell: no answer, not "no surface"
    for (S32 k = SS_WF_MAX_SPANS - 1; k >= 0; --k)
    {
        const size_t si = (size_t)k * (size_t)tile->mRes * (size_t)tile->mRes + col;
        if ((*tile->mSpanTop)[si] > -FLT_MAX * 0.5f)
        {
            z = (*tile->mSpanTop)[si];
            flags = (*tile->mSpanFlags)[si];
            return true;
        }
    }
    return false;
}

bool SSWorldField::coverageDetail(const LLVector3& pos_agent, bool& covered,
                                  F32& ceiling_z, F32& column_top_z) const
{
    covered = false;
    ceiling_z = 0.f;
    column_top_z = 0.f;

    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->hasGrid()) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)cy * tile->mRes + cx;
    if (!(*tile->mSurveyed)[col]) return false;     // no sheet covered this cell: no answer, not "no surface"

    // The half metre of grace keeps the surface being stood on from reading
    // as its own ceiling - the cell's floor under the camera can land a hair
    // above the camera's own feet.
    const F32 over = pos_agent.mV[VZ] + 0.5f;

    // <SS:Nexii> The cell's spans, lowest first: the lowest span whose BOTTOM
    // clears the camera is the ceiling of the space the camera stands in, and
    // the highest span's top is the column top. The underside, not the top
    // face: the sheets carry both faces of every span, where the depth-peel
    // capture only ever resolved tops and so read a roof's slab as headroom.
    const size_t layer = (size_t)tile->mRes * tile->mRes;
    bool any = false;
    for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
    {
        const size_t si = (size_t)k * layer + col;
        const F32 top = (*tile->mSpanTop)[si];
        if (top <= -FLT_MAX * 0.5f) break;
        const F32 bottom = (*tile->mSpanBottom)[si];

        any = true;
        column_top_z = llmax(column_top_z, top);
        if (bottom > over && (!covered || bottom < ceiling_z))
        {
            covered = true;
            ceiling_z = bottom;
        }
    }

    return any;
}

// Which air gap of a cell contains z - the core's own lookup, so a query and the
// classification can never disagree about which gap a point is in.
S32 SSWorldField::gapAt(const Tile& tile, size_t col, F32 z, F32& g0, F32& g1) const
{
    const size_t layer = (size_t)tile.mRes * tile.mRes;
    return SSWorldFieldCore::gapIndexAt(tile.mSpanTop->data(), tile.mSpanBottom->data(),
                                        SS_WF_MAX_SPANS, layer, col, z, tile.mCeiling, g0, g1);
}

// <SS:Nexii> The point query of decision 2. A 3D point usually lands in a gap of
// its own cell - a flying avatar, rain at 200 m and a listener on a sky platform
// all sit in their column's top gap, which the grid extends past its ceiling
// precisely so those answers exist. The one case with no air at its own height is
// a point INSIDE a body (a camera pushed into a wall, an ear inside a floor slab),
// and for that the query walks DOWN from z to the first gap within
// SSWorldFieldGroundReach metres and answers with that gap's verdict. Nothing
// within reach is open air, which the callers read as outdoors. [interaction: airLabelAt, enclosureInRegion]
S32 SSWorldField::resolveGap(const Tile& tile, size_t col, F32 z, F32& g0, F32& g1) const
{
    S32 gap = gapAt(tile, col, z, g0, g1);
    if (gap >= 0) return gap;

    const size_t layer = (size_t)tile.mRes * tile.mRes;
    const F32 floor_z = z - ss_wf_ground_reach();
    // Walking down from inside a body, the first air met is the gap beneath the
    // body the point sits in - the highest span whose bottom is below the point.
    for (S32 k = SS_WF_MAX_SPANS - 1; k >= 0; --k)
    {
        const size_t si = (size_t)k * layer + col;
        if ((*tile.mSpanTop)[si] <= NO_SURFACE * 0.5f) continue;
        const F32 bottom = (*tile.mSpanBottom)[si];
        if (bottom > z) continue;                       // that span is above the point
        if (bottom < floor_z) break;                    // further down than the reach allows
        gap = gapAt(tile, col, bottom - 0.02f, g0, g1);
        if (gap >= 0) return gap;
        break;                                          // the gap below is itself body: nothing to find
    }
    return -1;
}

// <SS:Nexii> Air connectivity lookup: the gap of the cell that contains the
// point (or the first one below it within reach), read from the labels the
// classification stored, or AIR_UNKNOWN when nothing is current - no grid for
// the region, or a rebuild owed since the navmesh moved. A point with no air
// within the ground reach reads AIR_OUTDOORS: the field ran out of structure
// to stand under, not out of confidence.
U8 SSWorldField::airLabelAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !current(*tile)) return AIR_UNKNOWN;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return AIR_UNKNOWN;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)cy * tile->mRes + cx;

    if (!(*tile->mSurveyed)[col]) return AIR_UNKNOWN;    // nobody has looked at this cell
    F32 g0, g1;
    const S32 gap = resolveGap(*tile, col, pos_agent.mV[VZ], g0, g1);
    if (gap < 0) return AIR_OUTDOORS;
    const size_t gi = col * (SS_WF_MAX_SPANS + 1) + (size_t)gap;
    return (gi < tile->mGapLabel->size()) ? (*tile->mGapLabel)[gi] : (U8)AIR_UNKNOWN;
}

// The covered distance behind airLabelAt, rounded to metres: how far the
// opening's reach had to carry to get here. Same gate, same walk-down.
U32 SSWorldField::airDepthAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !current(*tile)) return AIR_DEPTH_UNREACHED;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return AIR_DEPTH_UNREACHED;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)cy * tile->mRes + cx;

    if (!(*tile->mSurveyed)[col]) return AIR_DEPTH_UNREACHED;
    F32 g0, g1;
    const S32 gap = resolveGap(*tile, col, pos_agent.mV[VZ], g0, g1);
    if (gap < 0) return 0;                              // nothing overhead within reach: open air
    const size_t gi = col * (SS_WF_MAX_SPANS + 1) + (size_t)gap;
    if (gi >= tile->mGapDepth->size()) return AIR_DEPTH_UNREACHED;
    const U16 dm = (*tile->mGapDepth)[gi];
    return (dm == (U16)AIR_DEPTH_UNREACHED) ? AIR_DEPTH_UNREACHED : (U32)llround((F32)dm * 0.1f);
}

// <SS:Nexii> The enclosure spectrum at a point: the gap the point resolves to
// answers with its own label and covered distance. Returns -1 whenever there is
// no current answer and the caller keeps its own probe answer for that.
F32 SSWorldField::enclosureAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !current(*tile)) return -1.f;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return -1.f;

    return enclosureInRegion(regionp, *tile, pos_agent);
}

// The bulk form for callers that walk one region's cells (the surface field's
// window stitch): same answer, region and grid resolved once instead of per
// point.
F32 SSWorldField::enclosureAtRegion(U64 region_handle, const LLVector3& pos_agent) const
{
    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !current(it->second)) return -1.f;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return -1.f;

    return enclosureInRegion(regionp, it->second, pos_agent);
}

// <SS:Nexii> The ramp: d / (d + tau) on the covered distance d, with tau the LOCAL
// gap's own cot(theta) reach floored at SS_WF_ENCLOSURE_TAU_M. The same geometry
// that decides how far an opening carries outdoors decides how fast the ramp
// saturates, so one setting governs both and a tall space never reads as enclosed
// as a low one at the same distance from its opening.
F32 SSWorldField::enclosureInRegion(const LLViewerRegion* regionp, const Tile& tile,
                                    const LLVector3& pos_agent) const
{
    if (!current(tile)) return -1.f;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile.mCell), 0, tile.mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile.mCell), 0, tile.mRes - 1);
    const size_t layer = (size_t)tile.mRes * tile.mRes;
    const size_t col = (size_t)cy * tile.mRes + cx;
    if ((size_t)(SS_WF_MAX_SPANS + 1) * layer > tile.mGapLabel->size()) return -1.f;
    if (!(*tile.mSurveyed)[col]) return -1.f;           // nobody has looked at this cell: no verdict

    F32 g0, g1;
    const S32 gap = resolveGap(tile, col, pos_agent.mV[VZ], g0, g1);
    if (gap < 0) return 0.f;    // no air within the ground reach: open sky by the decision-2 rule

    const size_t gi = col * (SS_WF_MAX_SPANS + 1) + (size_t)gap;
    switch ((*tile.mGapLabel)[gi])
    {
        case AIR_OUTDOORS: return 0.f;
        case AIR_INTERIOR: return 1.f;
        case AIR_SHELTERED:
        {
            const U16 dm = (*tile.mGapDepth)[gi];
            if (dm == (U16)AIR_DEPTH_UNREACHED) return 1.f;
            const F32 metres = (F32)dm * 0.1f;
            const F32 tau = llmax(SS_WF_ENCLOSURE_TAU_M, (g1 - g0) * ss_wf_open_k());
            return metres / (metres + tau);
        }
        default: return -1.f;
    }
}

// <SS:Nexii> The wall profile at a point, from the gap-anchored probe bake: the
// nearest probe of the listener's lattice cell (its 8 neighbours back it up when the
// cell is probe-less - a pillar's answer is never stored, so a probe-less cell must
// reach sideways for one), its 8-direction profile's four cardinals out in the
// side-probe contract (metres, saturated at the reach cap). The ring lattice this
// once answered from sampled whatever altitude a fixed 4 m band landed on; the probe
// profile is taken at a real, ear-height z in real air.
bool SSWorldField::acousticAt(const LLVector3& pos_agent, F32 wall[4]) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->hasGrid()) return false;

    const Tile::Acoustic& ac = tile->mAcoustic;
    if (ac.mLatRes < 1 || !current(*tile)
        || ac.mProbes.empty() || ac.mCellStart.size() != (size_t)ac.mLatRes * (size_t)ac.mLatRes + 1)
    {
        return false;
    }

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const F32 lx_f = (pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / ac.mLatCell;
    const F32 ly_f = (pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / ac.mLatCell;
    const S32 lx = llclamp((S32)lx_f, 0, ac.mLatRes - 1);
    const S32 ly = llclamp((S32)ly_f, 0, ac.mLatRes - 1);

    // Own cell first, then the 8-neighbour ring: nearest probe by 3D distance.
    S32 best = -1;
    F32 best_d2 = FLT_MAX;
    for (S32 ring = 0; ring <= 1 && best < 0; ++ring)
    {
        const S32 r0 = (ring == 0) ? 0 : -1;
        const S32 r1 = (ring == 0) ? 0 : 1;
        for (S32 oy = r0; oy <= r1; ++oy)
        {
            for (S32 ox = r0; ox <= r1; ++ox)
            {
                const S32 nx = lx + ox;
                const S32 ny = ly + oy;
                if (nx < 0 || ny < 0 || nx >= ac.mLatRes || ny >= ac.mLatRes) continue;
                const S32 cell = ny * ac.mLatRes + nx;
                for (S32 pi = ac.mCellStart[cell]; pi < ac.mCellStart[cell + 1]; ++pi)
                {
                    const SSAcoustic::Probe& p = ac.mProbes[(size_t)pi];
                    const F32 dx = (regionp->getOriginAgent().mV[VX] + p.mX) - pos_agent.mV[VX];
                    const F32 dy = (regionp->getOriginAgent().mV[VY] + p.mY) - pos_agent.mV[VY];
                    const F32 dz = p.mZ - pos_agent.mV[VZ];
                    const F32 d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < best_d2) { best_d2 = d2; best = pi; }
                }
            }
        }
    }

    if (best < 0) return false;
    const SSAcoustic::Probe& p = ac.mProbes[(size_t)best];
    wall[0] = p.mWall[0];
    wall[1] = p.mWall[2];
    wall[2] = p.mWall[4];
    wall[3] = p.mWall[6];
    return true;
}

// <SS:Nexii> The occlusion trace over the cell grid - the doc's Part 3, the brief's
// realtime ask. Resolves one region's grid (both endpoints must live in it; the
// grid has no verdict past its border) and hands the segment to the core's DDA.
bool SSWorldField::traceSolid(const LLVector3& a, const LLVector3& b,
                              F32& solid_m, S32& crossings) const
{
    solid_m = 0.f;
    crossings = 0;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(a);
    if (!regionp) return false;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end() || !it->second.hasGrid()) return false;

    const Tile& tile = it->second;
    const size_t live = (size_t)SS_WF_MAX_SPANS * (size_t)tile.mRes * (size_t)tile.mRes;
    if (tile.mRes < 1 || tile.mSpanTop->size() < live || tile.mSpanBottom->size() < live) return false;

    SSAcoustic::Snap snap;
    snap.mTop = tile.mSpanTop->data();
    snap.mBottom = tile.mSpanBottom->data();
    snap.mFlags = tile.mSpanFlags->data();
    snap.mSurveyed = tile.mSurveyed->data();      // <SS:Nexii> so the trace can say it crossed cells nobody surveyed [interaction: SSAcoustic::Trace::mUnsurveyed]
    snap.mRes = tile.mRes;
    snap.mCell = tile.mCell;
    snap.mCeiling = tile.mCeiling;
    snap.mMaxSpans = SS_WF_MAX_SPANS;

    const LLVector3& origin = regionp->getOriginAgent();
    const F32 A[3] = { a.mV[VX] - origin.mV[VX], a.mV[VY] - origin.mV[VY], a.mV[VZ] };
    const F32 B[3] = { b.mV[VX] - origin.mV[VX], b.mV[VY] - origin.mV[VY], b.mV[VZ] };

    SSAcoustic::Trace t;
    if (!SSAcoustic::traceSolid(snap, A, B, t)) return false;
    // <SS:Nexii> The segment crossed cells no band sheet ever covered. The count is a
    // floor, not an answer, and reporting it would read as confidently open air - the
    // optimistic direction for audio and the one that suppresses the caller's own
    // raycast. [interaction: SSAcoustic::Trace::mUnsurveyed]
    if (t.mUnsurveyed) return false;

    solid_m = t.mSolidM;
    crossings = t.mCrossings;
    return true;
}

// <SS:Nexii> The bulk read, and the only supported way to take world-field geometry
// off the main thread. It copies six shared_ptrs and some scalars; the arrays behind
// them are immutable for their whole life, because a classification builds a whole new
// set and the completion swaps them in rather than editing these. A worker may
// therefore hold this view across a reclassification and across this region's
// eviction, and gate its result on mSerial when it lands. Main thread, as every query
// is - it resolves a region. [interaction: SSWindFlowMap, SSSoundscape]
bool SSWorldField::snapshotGrid(U64 region_handle, GridView& out) const
{
    out = GridView();

    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.hasGrid()) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return false;

    const Tile& tile = it->second;
    out.mSpanBottom = tile.mSpanBottom;
    out.mSpanTop = tile.mSpanTop;
    out.mSpanFlags = tile.mSpanFlags;
    out.mGapLabel = tile.mGapLabel;
    out.mGapDepth = tile.mGapDepth;
    out.mSurveyed = tile.mSurveyed;
    out.mOriginAgent = regionp->getOriginAgent();
    out.mRes = tile.mRes;
    out.mCell = tile.mCell;
    out.mCeiling = tile.mCeiling;
    out.mMaxSpans = SS_WF_MAX_SPANS;
    out.mSerial = tile.mGridSerial;
    out.mStale = !current(tile);
    return true;
}

// <SS:Nexii> The listener blend's probe set, shared by probesAt and acousticDebug:
// the probes of the listener's own gap in its lattice cell (vertical overlap with
// the listener's gap, the crouch figure) plus their graph-adjacent probes, deduped,
// at most max_out. Falls back to all of the cell's probes when none overlaps (a
// listener inside a body's sliver, say), and to the nearest probe of the
// neighbourhood when the cell is probe-less.
S32 SSWorldField::listenerProbeSet(const Tile& tile, const LLVector3& pos_agent,
                                   S32* out, S32 max_out) const
{
    const Tile::Acoustic& ac = tile.mAcoustic;
    if (ac.mLatRes < 1 || ac.mProbes.empty()
        || ac.mCellStart.size() != (size_t)ac.mLatRes * (size_t)ac.mLatRes + 1
        || ac.mAdjStart.size() != ac.mProbes.size() + 1)
    {
        return 0;
    }

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return 0;

    const S32 lx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / ac.mLatCell),
                           0, ac.mLatRes - 1);
    const S32 ly = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / ac.mLatCell),
                           0, ac.mLatRes - 1);
    const S32 cell = ly * ac.mLatRes + lx;

    // The listener's own gap, from the capture column it stands in.
    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile.mCell),
                           0, tile.mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile.mCell),
                           0, tile.mRes - 1);
    const size_t col = (size_t)cy * (size_t)tile.mRes + (size_t)cx;
    F32 g0 = 0.f, g1 = 0.f;
    const S32 gap = gapAt(tile, col, pos_agent.mV[VZ], g0, g1);

    // Own-gap probes of the cell: vertical overlap with the listener's gap.
    S32 n = 0;
    for (S32 pi = ac.mCellStart[cell]; pi < ac.mCellStart[cell + 1] && n < max_out; ++pi)
    {
        const SSAcoustic::Probe& p = ac.mProbes[(size_t)pi];
        if (gap < 0 || llmin(p.mGapTop, g1) - llmax(p.mGapBottom, g0) >= SSAcoustic::LINK_MIN_OVERLAP_M)
        {
            out[n++] = pi;
        }
    }

    // Graph-adjacent expansion (the connectivity-aware half: a probe behind a wall
    // reaches the blend only through a validated link), deduped by a stamp.
    // <SS:Nexii> The stamp plane is a member, grown once and never cleared: a fresh
    // allocate-and-zero of the full probe count ran on every probesAt, on the main
    // thread, at the soundscape's 50 ms cycle - including on the paths that then
    // answered false. A monotonic counter retires the clear; the only cost is a wrap
    // check. [interaction: probesAt, acousticDebug]
    if (mProbeMark.size() < ac.mProbes.size()) mProbeMark.assign(ac.mProbes.size(), 0);
    if (++mProbeStamp == 0) { std::fill(mProbeMark.begin(), mProbeMark.end(), 0u); mProbeStamp = 1; }
    const U32 mark = mProbeStamp;
    for (S32 i = 0; i < n; ++i) mProbeMark[(size_t)out[i]] = mark;
    const S32 own_n = n;
    for (S32 i = 0; i < own_n && n < max_out; ++i)
    {
        const S32 pi = out[i];
        for (S32 e = ac.mAdjStart[(size_t)pi]; e < ac.mAdjStart[(size_t)pi + 1] && n < max_out; ++e)
        {
            const S32 np = ac.mAdjNode[(size_t)e];
            if (mProbeMark[(size_t)np] != mark)
            {
                mProbeMark[(size_t)np] = mark;
                out[n++] = np;
            }
        }
    }

    // Nothing at all in the cell: the nearest probe of the 8-neighbour ring.
    if (n == 0)
    {
        F32 best_d2 = FLT_MAX;
        S32 best = -1;
        for (S32 oy = -1; oy <= 1; ++oy)
        {
            for (S32 ox = -1; ox <= 1; ++ox)
            {
                const S32 nx = lx + ox;
                const S32 ny = ly + oy;
                if (nx < 0 || ny < 0 || nx >= ac.mLatRes || ny >= ac.mLatRes) continue;
                const S32 ncell = ny * ac.mLatRes + nx;
                for (S32 pi = ac.mCellStart[ncell]; pi < ac.mCellStart[ncell + 1]; ++pi)
                {
                    const SSAcoustic::Probe& p = ac.mProbes[(size_t)pi];
                    const F32 dx = (regionp->getOriginAgent().mV[VX] + p.mX) - pos_agent.mV[VX];
                    const F32 dy = (regionp->getOriginAgent().mV[VY] + p.mY) - pos_agent.mV[VY];
                    const F32 dz = p.mZ - pos_agent.mV[VZ];
                    const F32 d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < best_d2) { best_d2 = d2; best = pi; }
                }
            }
        }
        if (best >= 0) out[n++] = best;
    }

    return n;
}

// <SS:Nexii> The listener blend (the doc's Part 3): the probes of the listener's own
// gap plus graph-adjacent probes, inverse-distance weighted. Blended figures: RT60,
// sky openness, room volume, wall profile, travel-to-outdoors, and the space/size
// classes - everything the soundscape's classification asked its raycasts for.
bool SSWorldField::probesAt(const LLVector3& pos_agent, ProbeSample out[4], S32& count) const
{
    count = 0;
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->hasGrid()) return false;
    if (!current(*tile)) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    S32 picks[16];
    const S32 n = listenerProbeSet(*tile, pos_agent, picks, 16);
    if (n <= 0) return false;

    // Top four by inverse-distance weight.
    S32 best[4] = { -1, -1, -1, -1 };
    F32 best_w[4] = { -1.f, -1.f, -1.f, -1.f };
    for (S32 i = 0; i < n; ++i)
    {
        const SSAcoustic::Probe& p = tile->mAcoustic.mProbes[(size_t)picks[i]];
        const F32 dx = (regionp->getOriginAgent().mV[VX] + p.mX) - pos_agent.mV[VX];
        const F32 dy = (regionp->getOriginAgent().mV[VY] + p.mY) - pos_agent.mV[VY];
        const F32 dz = p.mZ - pos_agent.mV[VZ];
        const F32 w = 1.f / (dx * dx + dy * dy + dz * dz + 1.f);
        for (S32 k = 0; k < 4; ++k)
        {
            if (w > best_w[k])
            {
                for (S32 j = 3; j > k; --j) { best[j] = best[j - 1]; best_w[j] = best_w[j - 1]; }
                best[k] = picks[i];
                best_w[k] = w;
                break;
            }
        }
    }

    for (S32 k = 0; k < 4; ++k)
    {
        if (best[k] < 0) break;
        const SSAcoustic::Probe& p = tile->mAcoustic.mProbes[(size_t)best[k]];
        ProbeSample& s = out[count++];
        s.mPos = regionp->getOriginAgent() + LLVector3(p.mX, p.mY, p.mZ);
        s.mWeight = best_w[k];
        s.mRT60 = p.mRT60;
        s.mSkyOpen = p.mSkyOpen;
        s.mVolume = p.mVolume;
        s.mTravelM = p.mTravelM;
        s.mSpaceClass = p.mSpaceClass;
        s.mSizeClass = p.mSizeClass;
        for (S32 i = 0; i < 8; ++i) s.mWall[i] = p.mWall[i];
    }
    return count > 0;
}

// <SS:Nexii> Per-source propagation (the doc's Part 3): Dijkstra over the baked
// probe graph with early exit at the listener's probe. Reads only the immutable
// baked graph - safe on the main thread between floods - and the serial gate keeps
// a stale graph from ever answering. The figures drive thunder's travel time
// (path metres, not euclidean), its muffle (portals and the path-vs-direct ratio),
// and its arrival direction (the listener's probe toward its Dijkstra parent: sound
// entering through a doorway is rendered from the doorway).
bool SSWorldField::propagationQuery(const LLVector3& source, const LLVector3& listener,
                                    Propagation& out) const
{
    out.mDirectM = 0.f;
    out.mPathM = 0.f;
    out.mCostM = 0.f;
    out.mPortals = 0;
    out.mMuffle = 0.f;
    out.mHaveArrival = false;
    out.mArrivalDir.setVec(0.f, 0.f, 1.f);
    out.mPath.clear();

    const LLVector3 mid = (source + listener) * 0.5f;
    const Tile* tile = tileAt(mid);
    if (!tile || !tile->hasGrid()) return false;
    if (!current(*tile)) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile->mRegionHandle);
    if (!regionp) return false;

    // <SS:Nexii> Both ends have to stand in the tile's own region: the tile comes from the midpoint, and listenerProbeSet clamps a position's lattice and column indices into whatever tile it is handed, so an end across a region line used to snap silently onto the border probe and answer with a path it never walked. A false return is the caller's cue to fall back on its own heuristic, which is the honest answer here. [interaction: sssoundscape propagation]
    LLViewerRegion* src_regionp = LLWorld::getInstance()->getRegionFromPosAgent(source);
    LLViewerRegion* lis_regionp = LLWorld::getInstance()->getRegionFromPosAgent(listener);
    if (!src_regionp || src_regionp->getHandle() != tile->mRegionHandle) return false;
    if (!lis_regionp || lis_regionp->getHandle() != tile->mRegionHandle) return false;

    const Tile::Acoustic& ac = tile->mAcoustic;
    if (ac.mProbes.empty()
        || ac.mAdjStart.size() != ac.mProbes.size() + 1
        || ac.mAdjNode.size() != ac.mAdjCost.size()) return false;

    const LLVector3& origin = regionp->getOriginAgent();

    // Snap source and listener each to the nearest probe of their gap in their
    // lattice cell: the blend set's first pick is exactly that probe.
    S32 src_picks[16];
    S32 dst_picks[16];
    const S32 src_n = listenerProbeSet(*tile, source, src_picks, 16);
    const S32 dst_n = listenerProbeSet(*tile, listener, dst_picks, 16);
    if (src_n <= 0 || dst_n <= 0) return false;

    auto nearest = [&](const S32* picks, S32 n, const LLVector3& pos) -> S32
    {
        S32 best = -1;
        F32 best_d2 = FLT_MAX;
        for (S32 i = 0; i < n; ++i)
        {
            const SSAcoustic::Probe& p = ac.mProbes[(size_t)picks[i]];
            const F32 dx = (origin.mV[VX] + p.mX) - pos.mV[VX];
            const F32 dy = (origin.mV[VY] + p.mY) - pos.mV[VY];
            const F32 dz = p.mZ - pos.mV[VZ];
            const F32 d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d2) { best_d2 = d2; best = picks[i]; }
        }
        return best;
    };

    const S32 src = nearest(src_picks, src_n, source);
    const S32 dst = nearest(dst_picks, dst_n, listener);
    if (src < 0 || dst < 0) return false;

    out.mDirectM = dist_vec(source, listener);

    // The solve. Cached by the caller's own move/serial discipline; a one-shot query
    // (thunder) pays its well-under-a-millisecond outright.
    const S32 n = (S32)ac.mProbes.size();
    std::vector<S32> parent((size_t)n);
    std::vector<F32> dist((size_t)n);
    if (!SSAcoustic::dijkstra(n, ac.mAdjStart.data(), ac.mAdjNode.data(), ac.mAdjCost.data(),
                              src, dst, parent.data(), dist.data()))
    {
        return false;   // no connected path: the caller keeps its guess
    }

    // Walk the chain listener <- ... <- source, then reverse.
    std::vector<S32> chain;
    {
        S32 cur = dst;
        S32 guard = 0;
        while (cur >= 0 && guard++ <= n)
        {
            chain.push_back(cur);
            if (cur == src) break;
            cur = parent[(size_t)cur];
        }
        if (chain.empty() || chain.back() != src) return false;
    }
    std::reverse(chain.begin(), chain.end());

    // Geometric path metres: the endpoint hops plus the node-to-node hops.
    const SSAcoustic::Probe& src_p = ac.mProbes[(size_t)src];
    const SSAcoustic::Probe& dst_p = ac.mProbes[(size_t)dst];
    F32 path = dist_vec(source, origin + LLVector3(src_p.mX, src_p.mY, src_p.mZ));
    for (size_t i = 1; i < chain.size(); ++i)
    {
        const SSAcoustic::Probe& a = ac.mProbes[(size_t)chain[i - 1]];
        const SSAcoustic::Probe& b = ac.mProbes[(size_t)chain[i]];
        path += sqrtf((b.mX - a.mX) * (b.mX - a.mX) + (b.mY - a.mY) * (b.mY - a.mY)
                      + (b.mZ - a.mZ) * (b.mZ - a.mZ));
    }
    path += dist_vec(origin + LLVector3(dst_p.mX, dst_p.mY, dst_p.mZ), listener);

    // Portals on the chain: consecutive nodes whose labels differ across the
    // OUTDOORS boundary - the same flag the links carry, recomputed from the pair.
    S32 portals = 0;
    for (size_t i = 1; i < chain.size(); ++i)
    {
        const U8 la = ac.mProbes[(size_t)chain[i - 1]].mLabel;
        const U8 lb = ac.mProbes[(size_t)chain[i]].mLabel;
        if ((la == 1) != (lb == 1)) ++portals;
    }

    out.mPathM = path;
    out.mCostM = dist[(size_t)dst];
    out.mPortals = portals;

    // Muffle from portals and the path-vs-direct ratio: around-buildings sound is
    // softened and darkened; a straight shot through portals less so.
    const F32 ratio = (out.mDirectM > 1.f) ? path / out.mDirectM : 1.f;
    out.mMuffle = llclamp((F32)portals * 0.25f + llmax(ratio - 1.f, 0.f) * 0.3f, 0.f, 0.9f);

    // Arrival direction: the listener's probe toward its Dijkstra parent - sound
    // entering through a doorway is rendered from the doorway.
    if (chain.size() >= 2)
    {
        const SSAcoustic::Probe& from = ac.mProbes[(size_t)chain[chain.size() - 2]];
        const LLVector3 dir((origin.mV[VX] + from.mX) - (origin.mV[VX] + dst_p.mX),
                            (origin.mV[VY] + from.mY) - (origin.mV[VY] + dst_p.mY),
                            from.mZ - dst_p.mZ);
        if (dir.magVecSquared() > 1.0e-4f)
        {
            out.mArrivalDir = dir;
            out.mArrivalDir.normVec();
            out.mHaveArrival = true;
        }
    }

    // The path for the debug layer, capped: stride when the chain runs long.
    const size_t cap = 64;
    const size_t stride = (chain.size() > cap) ? (chain.size() + cap - 1) / cap : 1;
    for (size_t i = 0; i < chain.size(); i += stride)
    {
        const SSAcoustic::Probe& p = ac.mProbes[(size_t)chain[i]];
        out.mPath.push_back(origin + LLVector3(p.mX, p.mY, p.mZ));
    }
    if ((chain.size() - 1) % stride != 0)
    {
        const SSAcoustic::Probe& p = ac.mProbes[(size_t)chain.back()];
        out.mPath.push_back(origin + LLVector3(p.mX, p.mY, p.mZ));
    }

    return true;
}

// <SS:Nexii> The debug export the V10 info view draws: probes and links in range,
// portal flags resolved per probe, the listener's blend set named. Read-only over
// the baked channel; a stale or unbaked channel reads mValid false and the view
// says so instead of drawing yesterday's room.
bool SSWorldField::acousticDebug(U64 region_handle, const LLVector3& centre_agent, F32 range_m,
                                 AcousticDebug& out) const
{
    out.mValid = false;
    out.mLatRes = 0;
    out.mLatCell = 0.f;
    out.mProbeCount = 0;
    out.mBundleCount = 0;
    out.mLinkCount = 0;
    out.mPortalCount = 0;
    out.mProbes.clear();
    out.mLinks.clear();
    out.mListenerProbes.clear();

    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.hasGrid()) return false;

    const Tile& tile = it->second;
    const Tile::Acoustic& ac = tile.mAcoustic;
    if (ac.mLatRes < 1 || !current(tile) || ac.mProbes.empty()) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return false;

    const LLVector3& origin = regionp->getOriginAgent();
    const F32 centre_x = centre_agent.mV[VX] - origin.mV[VX];
    const F32 centre_y = centre_agent.mV[VY] - origin.mV[VY];
    const F32 r2 = range_m * range_m;

    // Range-cull probes, remembering the old->new index map.
    const S32 count = (S32)ac.mProbes.size();
    std::vector<S32> remap((size_t)count, -1);
    std::vector<U8> portal_node((size_t)count, 0);
    for (const SSAcoustic::Link& l : ac.mLinks)
    {
        if (l.mPortal && l.mA >= 0 && l.mA < count) portal_node[(size_t)l.mA] = 1;
        if (l.mPortal && l.mB >= 0 && l.mB < count) portal_node[(size_t)l.mB] = 1;
    }

    for (S32 i = 0; i < count; ++i)
    {
        const SSAcoustic::Probe& p = ac.mProbes[(size_t)i];
        const F32 dx = p.mX - centre_x;
        const F32 dy = p.mY - centre_y;
        if (dx * dx + dy * dy > r2) continue;

        AcousticDebug::Probe o;
        o.mPos = origin + LLVector3(p.mX, p.mY, p.mZ);
        o.mGapBottom = p.mGapBottom;
        o.mGapTop = p.mGapTop;
        o.mLabel = p.mLabel;
        o.mRT60 = p.mRT60;
        o.mSkyOpen = p.mSkyOpen;
        o.mVolume = p.mVolume;
        o.mTravelM = p.mTravelM;
        o.mSpaceClass = p.mSpaceClass;
        o.mSizeClass = p.mSizeClass;
        o.mPortal = portal_node[(size_t)i] != 0;
        o.mHaveBundle = p.mHaveBundle != 0;
        remap[(size_t)i] = (S32)out.mProbes.size();
        out.mProbes.push_back(o);
    }

    for (const SSAcoustic::Link& l : ac.mLinks)
    {
        if (l.mA < 0 || l.mB < 0 || l.mA >= count || l.mB >= count) continue;
        const S32 ra = remap[(size_t)l.mA];
        const S32 rb = remap[(size_t)l.mB];
        if (ra < 0 || rb < 0) continue;
        AcousticDebug::Link o;
        o.mA = ra;
        o.mB = rb;
        o.mPortal = l.mPortal != 0;
        out.mLinks.push_back(o);
        if (o.mPortal) ++out.mPortalCount;
    }

    // The listener's own blend set, by node index.
    S32 picks[16];
    const S32 n = listenerProbeSet(tile, centre_agent, picks, 16);
    for (S32 i = 0; i < n; ++i)
    {
        const S32 r = (picks[i] >= 0 && picks[i] < count) ? remap[(size_t)picks[i]] : -1;
        if (r >= 0) out.mListenerProbes.push_back(r);
    }

    out.mValid = true;
    out.mLatRes = ac.mLatRes;
    out.mLatCell = ac.mLatCell;
    out.mProbeCount = count;
    for (const SSAcoustic::Probe& p : ac.mProbes)
    {
        if (p.mHaveBundle) ++out.mBundleCount;
    }
    out.mLinkCount = (S32)ac.mLinks.size();
    return true;
}

// <SS:Nexii> The share of the region's cells the field has actually surveyed - and
// therefore the build-progress figure the HUD wants, where the old "share of gaps
// carrying a label" was 0 or 1 and nothing else. The count is taken once by the
// classification worker and stored, because the figure it replaced walked a million
// cells times six spans on the MAIN THREAD every frame the info view was open.
F32 SSWorldField::airCoverage(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.hasGrid()) return 0.f;   // what the PUBLISHED grid surveyed, stale or not

    const Tile& tile = it->second;
    const size_t layer = (size_t)tile.mRes * tile.mRes;
    if (layer == 0) return 0.f;
    const S32 unsurveyed = llclamp(tile.mUnsurveyed, 0, (S32)layer);
    return (F32)((S32)layer - unsurveyed) / (F32)layer;
}

// The DRAINAGE_NETWORK core over one landing surface. Barnes' priority flood
// is the O(n log n) way to fill every depression to its spill elevation:
// drains (the grid border, water, unmapped sky) seed the heap at their own
// height, each cell pops once at the lowest spill reaching it, and a cell
// whose spill stands meaningfully above its own surface is standing water.
// Flow directions then run down the FILLED surface, so a pool's water heads
// for its outlet instead of into its own floor - with one exception the raw
// surface contributes: an EAVE. A step down steeper than a roof pitch and at
// least the eave-drop tall is a discontinuity in the capture, not a slope -
// water arriving there leaves into the air, so the drop is not a descent the
// flow may take and the cell holding it ends the surface. That is what stops
// a roof's catchment from pouring through the wall it borders and arriving
// invisibly on the street below, and it is what makes the accumulation
// terminate at the edges the shed reads. Accumulation itself is the
// hydrology-standard descending-fill pass: on the filled surface water only
// ever moves to a strictly lower cell, so one visit per cell in descending
// spill order sees every upstream contribution first, and each cell is left
// holding the area that drains through it in square metres.
bool SSWorldField::buildDrainage(const SSRainShadowMap::SurfaceGrid& grid, Drainage& out)
{
    out.mSpill.clear();
    out.mPool.clear();
    out.mD8.clear();
    out.mCatch.clear();

    const S32 n = grid.mN;
    if (n < 3 || grid.mZ.size() < (size_t)n * n) return false;

    const size_t count = (size_t)n * n;
    out.mSpill.assign(count, -FLT_MAX);
    out.mPool.assign(count, 0);
    out.mD8.assign(count, 4);
    out.mCatch.assign(count, 0.f);

    // The hydrological domain: cells with any surface flag, water excluded -
    // water, unmapped sky and the grid border are drains the fill opens out at.
    // The height guard keeps a NODATA cell that somehow carried flags from
    // seeding a -FLT_MAX spill that would poison every fill elevation it
    // reached.
    auto land = [&](size_t i)
    {
        const U8 f = grid.mFlags[i];
        return (f & SSRainShadowMap::SURF_WATER) == 0 && f != 0
            && grid.mZ[i] > -FLT_MAX * 0.5f;
    };

    static const S32 DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    static const S32 DY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

    struct Node
    {
        F32 mSpill;
        S32 mIndex;
    };
    struct NodeHeapOrder
    {
        bool operator()(const Node& a, const Node& b) const
        {
            // Min-heap on spill; index tie-break keeps the walk deterministic.
            if (a.mSpill != b.mSpill) return a.mSpill > b.mSpill;
            return a.mIndex > b.mIndex;
        }
    };
    std::priority_queue<Node, std::vector<Node>, NodeHeapOrder> heap;

    std::vector<U8> visited(count, 0);

    auto seed = [&](S32 i)
    {
        if (visited[i] || !land((size_t)i)) return;
        visited[i] = 1;
        out.mSpill[i] = grid.mZ[i];
        heap.push({ out.mSpill[i], i });
    };

    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            const S32 i = y * n + x;
            if (!land((size_t)i)) continue;

            if (x == 0 || y == 0 || x == n - 1 || y == n - 1)
            {
                seed(i);
                continue;
            }

            for (S32 d = 0; d < 8; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
                if (!land((size_t)ny * n + nx))
                {
                    seed(i);
                    break;
                }
            }
        }
    }

    while (!heap.empty())
    {
        const S32 c = heap.top().mIndex;
        heap.pop();

        const F32 spill_c = out.mSpill[(size_t)c];
        const S32 cx = c % n;
        const S32 cy = c / n;

        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

            const S32 ni = ny * n + nx;
            if (visited[ni] || !land((size_t)ni)) continue;

            visited[ni] = 1;
            out.mSpill[ni] = llmax(spill_c, grid.mZ[(size_t)ni]);
            heap.push({ out.mSpill[ni], ni });
        }
    }

// Pool membership: the fill is standing water where it rises clear of the
    // surface - the depression's depth, not a sampled dip. Five centimetres sits
    // below the puddle thresholds that consume the mask, so this only gates
    // genuine standing water, never a capture texel's jitter.
    static const F32 POOL_FILL_EPS = 0.05f;

    // Flow: D8 down the filled surface, 3x3-indexed ((dy+1)*3 + (dx+1)), 4 when
    // nothing is lower - a pool floor, a sink, or a drain cell. The eave rule
    // removes the one descent a raw surface drop can offer that is not a
    // descent at all: the step off an edge.
    static const F32 EAVE_DROP_M  = 0.75f;  // at least a storey-step of fall
    static const F32 EAVE_SLOPE   = 2.0f;   // steeper than any roof pitch (~63 deg)

    const F32 cell = grid.mCell;
    const F32 eave_run = (cell > 0.f) ? EAVE_SLOPE * cell : FLT_MAX;

    for (S32 c = 0; c < (S32)count; ++c)
    {
        const size_t i = (size_t)c;
        if (!land(i)) continue;

        if (out.mSpill[i] - grid.mZ[i] > POOL_FILL_EPS)
        {
            out.mPool[i] = 1;
        }

        const S32 cx = c % n;
        const S32 cy = c / n;
        const F32 here = out.mSpill[i];
        const F32 z_here = grid.mZ[i];

        S32 best_dir = 4;
        F32 best_z = here;
        bool has_eave = false;
        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

            const size_t ni = (size_t)ny * n + nx;
            if (!land(ni)) continue;

            // The eave rule, tested on the RAW surface: the filled surface
            // smooths the very discontinuity that ends it. A neighbour over a
            // genuine cliff is not downhill, it is off the surface - water
            // goes into the air there rather than onto whatever is below.
            const F32 drop = z_here - grid.mZ[ni];
            if (drop >= EAVE_DROP_M && drop >= eave_run)
            {
                has_eave = true;
                continue;
            }

            // Dropping the FILLED elevation from the outlet chain windows out
            // the water-plane cases a raw-surface D8 gets wrong: a pool run
            // into its own floor.
            const F32 nz = out.mSpill[ni];
            if (nz < best_z - 0.01f)
            {
                best_z = nz;
                best_dir = (DY[d] + 1) * 3 + (DX[d] + 1);
            }
        }

        // A cell with an eave side is where the surface ends for the water
        // standing on it: it leaves there, whatever other descents the cell
        // could offer. Terminal here is what keeps a sloped gutter's lip line
        // from routing its catchment along itself and counting it twice.
        if (has_eave) best_dir = 4;

        out.mD8[i] = (U8)best_dir;
    }

    // Accumulation: contributing area in square metres, routed down the D8 in
    // descending spill order. On the filled surface water only ever moves to a
    // strictly lower cell, so one pass visits every cell after everything that
    // drains into it - no cycles, no second pass. A cell whose outlet chain
    // ends (an eave, a pool floor, the border) keeps what arrived: that is the
    // catchment the shed reads at the lips, and because an eave cell is
    // terminal, every lip of a run holds a disjoint share of the roof - the
    // run can sum its members' catchments without counting a sloped gutter
    // twice.
    std::vector<S32> order;
    order.reserve(count / 4);
    for (S32 c = 0; c < (S32)count; ++c)
    {
        if (land((size_t)c)) order.push_back(c);
    }
    std::sort(order.begin(), order.end(), [&out](S32 a, S32 b)
    {
        const F32 sa = out.mSpill[(size_t)a];
        const F32 sb = out.mSpill[(size_t)b];
        if (sa != sb) return sa > sb;
        return a > b;
    });

    const F32 area = cell * cell;
    for (const S32 c : order)
    {
        const size_t i = (size_t)c;
        out.mCatch[i] += area;

        const U8 dir = out.mD8[i];
        if (dir == 4) continue;

        const S32 dx = (dir % 3) - 1;
        const S32 dy = (dir / 3) - 1;
        const S32 nx = (c % n) + dx;
        const S32 ny = (c / n) + dy;
        if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

        out.mCatch[(size_t)ny * n + nx] += out.mCatch[i];
    }

    return true;
}

// <SS:Nexii> The ACOUSTIC channel's bake (doc/atmo_magic_acoustics.md Parts 1-4),
// over the snapshot the flood just walked. Replaces the shipped ring lattice whole:
// fixed rings sampled altitudes nothing stands at - a ring inside a floor slab
// answers for nobody - while the span store knows where listeners actually stand,
// so probes anchor to air gaps instead. Per lattice cell (the same ~8 m resolution
// the rings ran at), an anchor column picked by spiralling out from the centre until
// one carries air at ear height, then per gap of that column: an ear probe at floor
// + 1.2 m, a ceiling probe under a roof, intermediates every ~4 m for tall gaps, and
// the tier A statistic bake per probe - the 8-direction wall profile at the probe's
// REAL z (the 4 side-raycasts' answer), sky openness, the bounded room flood's
// volume and area at lattice resolution, Sabine's RT60, the space/size classes the
// ambient loops already read, and the flood's own travel-to-outdoors. The probe graph
// rides the same walk: vertical links by construction, horizontal links between
// probes whose gaps overlap by at least the crouch figure, each validated by a
// span-store ray (at 8 m spacing a wall thinner than a lattice cell is invisible to
// gap overlap alone, and the ray is what keeps the graph from teleporting sound
// through it), aperture factors from the overlap clamped against the midpoint
// clearance, and portal flags across the OUTDOORS boundary. Probes are placed from
// the gap list alone, ignoring the flood's labels - a sealed room still gets probes,
// because a listener teleporting into it still deserves its reverb; labels ride
// along as data, not as placement gates.
static void ss_wf_acoustic_build(S32 res, S32 max_spans, F32 cell_m, F32 ceiling, S32 lat_res,
                                 const std::vector<F32>& span_top, const std::vector<F32>& span_bottom,
                                 const std::vector<U8>& gap_label, const std::vector<U16>& gap_depth,
                                 const std::vector<U8>& surveyed,
                                 std::vector<SSAcoustic::Probe>& out_probes,
                                 std::vector<S32>& out_cell_start,
                                 std::vector<SSAcoustic::Link>& out_links,
                                 std::vector<S32>& out_adj_start,
                                 std::vector<S32>& out_adj_node,
                                 std::vector<F32>& out_adj_cost)
{
    out_probes.clear();
    out_cell_start.clear();
    out_links.clear();
    out_adj_start.clear();
    out_adj_node.clear();
    out_adj_cost.clear();
    if (lat_res < 1 || lat_res > res || cell_m <= 0.f || res < 1) return;

    SSAcoustic::Snap snap;
    snap.mTop = span_top.data();
    snap.mBottom = span_bottom.data();
    snap.mGapLabel = gap_label.data();
    snap.mGapDepth = gap_depth.data();
    // <SS:Nexii> The bake needs the survey mask as much as traceSolid does. An
    // unsurveyed column has no spans, so columnHasEarAir answers true on its
    // ceiling-minus-floor gap and the anchor spiral happily lands on the void: the
    // probe that comes back has AIR_UNKNOWN, travel -1, every wall at the 64 m
    // saturation cap, and therefore a huge room volume and a long Sabine RT60 - which
    // SSAcoustic::classify, having no case for label 4, calls SPACE_SHELTERED and size
    // class open. probesAt and acousticAt would then serve that fabrication as an
    // answer. [interaction: the anchor spiral below, SSAcoustic::columnHasEarAir]
    snap.mSurveyed = surveyed.size() == (size_t)res * (size_t)res ? surveyed.data() : nullptr;
    snap.mRes = res;
    snap.mCell = cell_m;
    snap.mCeiling = ceiling;
    snap.mMaxSpans = max_spans;

    const size_t layer = (size_t)res * (size_t)res;
    const size_t per = (size_t)max_spans + 1;
    const F32 lat_cell = (F32)res * cell_m / (F32)lat_res;

    // The anchor spiral, resolved once: candidate offsets ordered by ring then angle.
    constexpr S32 SPIRAL_R = SSAcoustic::ANCHOR_SPIRAL_CELLS;
    S32 spiral_dx[(2 * SPIRAL_R + 1) * (2 * SPIRAL_R + 1)];
    S32 spiral_dy[(2 * SPIRAL_R + 1) * (2 * SPIRAL_R + 1)];
    const S32 spiral_n = SSAcoustic::anchorSpiral(spiral_dx, spiral_dy);

    SSAcoustic::LatGaps lat;
    lat.build(lat_res, max_spans);

    out_cell_start.assign((size_t)lat_res * (size_t)lat_res + 1, 0);

    for (S32 ly = 0; ly < lat_res; ++ly)
    {
        for (S32 lx = 0; lx < lat_res; ++lx)
        {
            const S32 cell = ly * lat_res + lx;
            out_cell_start[cell] = (S32)out_probes.size();

            // Anchor column: the centre, then the spiral, first column whose gap
            // list has air at ear height. None: the cell stays probe-less and its
            // neighbours' interpolation covers it.
            const S32 cx = llclamp((S32)(((F32)lx + 0.5f) * (F32)res / (F32)lat_res), 0, res - 1);
            const S32 cy = llclamp((S32)(((F32)ly + 0.5f) * (F32)res / (F32)lat_res), 0, res - 1);
            size_t anchor = ~(size_t)0;
            for (S32 i = 0; i < spiral_n; ++i)
            {
                const S32 nx = llclamp(cx + spiral_dx[i], 0, res - 1);
                const S32 ny = llclamp(cy + spiral_dy[i], 0, res - 1);
                const size_t col = (size_t)ny * (size_t)res + (size_t)nx;
                // <SS:Nexii> Never anchor on a column nobody surveyed. The spiral starts
                // at the lattice cell's own centre, so one unsurveyed centre column in
                // an otherwise surveyed 8 m cell would anchor on the void and never look
                // at the real geometry two cells away.
                if (snap.mSurveyed && !snap.mSurveyed[col]) continue;
                if (SSAcoustic::columnHasEarAir(snap, col))
                {
                    anchor = col;
                    break;
                }
            }
            if (anchor == ~(size_t)0) continue;

            SSAcoustic::fillLatGaps(snap, anchor, cell, lat);

            const F32 px = ((F32)(anchor % (size_t)res) + 0.5f) * cell_m;
            const F32 py = ((F32)(anchor / (size_t)res) + 0.5f) * cell_m;
            const S32 span_n = SSAcoustic::spanCount(snap, anchor);

            SSAcoustic::ProbeDef defs[16];
            for (S32 k = 0; k <= max_spans; ++k)
            {
                if (k > span_n) break;
                const F32 g0 = (k == 0) ? 0.f : span_top[(size_t)(k - 1) * layer + anchor];
                const F32 g1 = (k == span_n) ? ceiling : span_bottom[(size_t)k * layer + anchor];
                const S32 count = SSAcoustic::placeGapProbes(g0, g1, k == span_n, defs, 16);
                for (S32 i = 0; i < count; ++i)
                {
                    SSAcoustic::Probe p;
                    p.mX = px;
                    p.mY = py;
                    p.mZ = defs[i].mZ;
                    p.mGapBottom = defs[i].mGapBottom;
                    p.mGapTop = defs[i].mGapTop;
                    p.mCell = cell;
                    p.mGap = k;
                    p.mLabel = gap_label[anchor * per + (size_t)k];
                    p.mRoofed = defs[i].mRoofed ? 1 : 0;
                    const U16 dep = gap_depth[anchor * per + (size_t)k];
                    p.mGapDepth = dep;
                    p.mTravelM = (dep != 0xFFFF) ? (F32)dep * 0.1f : -1.f;

                    // Tier A, all of it bake-time: the profile at the probe's real z,
                    // the column stack above, the bounded room flood, Sabine over it,
                    // and the classes the soundscape's enum already names.
                    SSAcoustic::wallProfile(snap, px, py, defs[i].mZ, p.mWall);
                    p.mSkyOpen = SSAcoustic::skyOpenness(snap, anchor, k);
                    SSAcoustic::roomEstimate(snap, lat, cell, k, p.mVolume, p.mArea);
                    p.mRT60 = SSAcoustic::sabineRT60(p.mVolume, p.mArea);
                    SSAcoustic::classify(defs[i].mRoofed, p.mLabel, p.mWall, p.mSpaceClass, p.mSizeClass);

                    out_probes.push_back(p);
                }
            }
        }
    }
    out_cell_start[(size_t)lat_res * (size_t)lat_res] = (S32)out_probes.size();

    if (out_probes.empty()) return;

    // ---- the graph ----

    // Vertical links: probes of the same gap in the same cell, consecutive by z -
    // connected by construction, cost the vertical distance.
    for (size_t i = 1; i < out_probes.size(); ++i)
    {
        SSAcoustic::Probe& a = out_probes[i - 1];
        SSAcoustic::Probe& b = out_probes[i];
        if (a.mCell != b.mCell || a.mGap != b.mGap) continue;
        SSAcoustic::Link l;
        l.mA = (S32)(i - 1);
        l.mB = (S32)i;
        l.mLen = fabsf(b.mZ - a.mZ);
        l.mAperture = 1.f;
        l.mPortal = 0;
        out_links.push_back(l);
    }

    // Horizontal links: per 4-neighbour lattice cell pair (+X and +Y only, so each
    // unordered pair validates once), candidate links between probes whose gaps
    // vertically overlap by the crouch figure, each validated by a span-store ray.
    for (S32 ly = 0; ly < lat_res; ++ly)
    {
        for (S32 lx = 0; lx < lat_res; ++lx)
        {
            const S32 cell = ly * lat_res + lx;
            static const S32 NDX[2] = { 1, 0 };
            static const S32 NDY[2] = { 0, 1 };
            for (S32 d = 0; d < 2; ++d)
            {
                const S32 nx = lx + NDX[d];
                const S32 ny = ly + NDY[d];
                if (nx >= lat_res || ny >= lat_res) continue;
                const S32 ncell = ny * lat_res + nx;

                for (S32 ai = out_cell_start[cell]; ai < out_cell_start[cell + 1]; ++ai)
                {
                    for (S32 bi = out_cell_start[ncell]; bi < out_cell_start[ncell + 1]; ++bi)
                    {
                        SSAcoustic::Probe& a = out_probes[(size_t)ai];
                        SSAcoustic::Probe& b = out_probes[(size_t)bi];
                        if (!SSAcoustic::gapsOverlap(a, b)) continue;

                        const F32 seg_a[3] = { a.mX, a.mY, a.mZ };
                        const F32 seg_b[3] = { b.mX, b.mY, b.mZ };
                        SSAcoustic::Trace tr;
                        if (!SSAcoustic::traceSolid(snap, seg_a, seg_b, tr)
                            || tr.mSolidM > SSAcoustic::LINK_BLOCK_SOLID_M)
                        {
                            continue;   // blocked: the pair may still connect via
                                        // another gap's probes or a longer path,
                                        // which is the point of a graph
                        }

                        SSAcoustic::Link l;
                        l.mA = ai;
                        l.mB = bi;
                        const F32 ddx = b.mX - a.mX;
                        const F32 ddy = b.mY - a.mY;
                        const F32 ddz = b.mZ - a.mZ;
                        l.mLen = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);

                        // Aperture: the overlap clamped against a horizontal clearance
                        // sample at the link's midpoint, the nearest wall distance
                        // perpendicular to the link.
                        const F32 mid[3] = { (a.mX + b.mX) * 0.5f, (a.mY + b.mY) * 0.5f,
                                             (a.mZ + b.mZ) * 0.5f };
                        const F32 ilen = (l.mLen > 1.0e-4f) ? 1.f / sqrtf(ddx * ddx + ddy * ddy) : 0.f;
                        const F32 perp_x = -ddy * ilen;
                        const F32 perp_y = ddx * ilen;
                        const F32 clear = llmin(
                            SSAcoustic::wallDistDir(snap, mid[0], mid[1], mid[2], perp_x, perp_y, lat_cell),
                            SSAcoustic::wallDistDir(snap, mid[0], mid[1], mid[2], -perp_x, -perp_y, lat_cell));
                        l.mAperture = SSAcoustic::apertureOf(a, b, clear);

                        // Portal: the endpoints' labels differ across the OUTDOORS
                        // boundary - the edge thunder and the muffle question care
                        // about, free from the labels.
                        l.mPortal = (U8)(((a.mLabel == 1) != (b.mLabel == 1)) ? 1 : 0);
                        out_links.push_back(l);
                    }
                }
            }
        }
    }

    // CSR adjacency: every link appears from both ends, cost precomputed once so the
    // runtime solve reads flat arrays only.
    const S32 node_count = (S32)out_probes.size();
    out_adj_start.assign((size_t)node_count + 1, 0);
    for (const SSAcoustic::Link& l : out_links)
    {
        if (l.mA >= 0 && l.mA < node_count) ++out_adj_start[(size_t)l.mA + 1];
        if (l.mB >= 0 && l.mB < node_count) ++out_adj_start[(size_t)l.mB + 1];
    }
    for (size_t i = 1; i < out_adj_start.size(); ++i)
    {
        out_adj_start[i] += out_adj_start[i - 1];
    }
    out_adj_node.resize((size_t)out_adj_start[node_count]);
    out_adj_cost.resize((size_t)out_adj_start[node_count]);
    {
        std::vector<S32> cursor(out_adj_start.begin(), out_adj_start.end() - 1);
        for (const SSAcoustic::Link& l : out_links)
        {
            const F32 cost = SSAcoustic::linkCost(l);
            if (l.mA >= 0 && l.mA < node_count)
            {
                out_adj_node[(size_t)cursor[(size_t)l.mA]] = l.mB;
                out_adj_cost[(size_t)cursor[(size_t)l.mA]++] = cost;
            }
            if (l.mB >= 0 && l.mB < node_count)
            {
                out_adj_node[(size_t)cursor[(size_t)l.mB]] = l.mA;
                out_adj_cost[(size_t)cursor[(size_t)l.mB]++] = cost;
            }
        }
    }
}

// <SS:Nexii> One region's classification, start to finish, as a single worker
// job. The main thread does three things and no more: snapshot the navmesh's
// band sheets (shared_ptr copies - the sheets are immutable once published, so
// nothing is copied and nothing can be rewritten under the walk), read every
// setting the job needs, and remember the serial and generation the job was
// started at. The worker then materialises the cell grid from the sheets, runs
// the air classification over it and bakes the acoustic probes; the completion
// swaps the finished arrays in whole, and only if BOTH gates still hold - the
// generation (clear() and eviction move it, so a job for a dropped region can
// never land on a grid the same region handle re-created) and the geometry
// serial (the navmesh settled again mid-walk, so the answer describes a world
// that has moved). Nothing here mutates a published grid: a reader on the main
// thread always sees either the previous grid entire or the new one entire.
void SSWorldField::scheduleGrid(Tile& tile)
{
    if (mBuildBusy) return;                     // the next update gives this region its turn
    if (!SSNavMesh::instanceExists()) return;

    std::vector<SSNavMesh::BandSheet> sheets;
    U64 stamp = 0;
    if (!SSNavMesh::getInstance()->collectSheets(tile.mRegionHandle, sheets, stamp)) return;
    if (sheets.empty()) return;     // no geometry is no answer, not an empty world; navSettle drops the grid for it

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return;

    const F32 cell = ss_wf_cell_setting();
    const S32 res = llclamp((S32)llround(regionp->getWidth() / cell), 32, 1024);
    const LLVector3 region_origin = regionp->getOriginAgent();

    // The grid's ceiling: above the highest band anything the navmesh knows
    // about, plus headroom, so every cell's top gap is genuinely open sky.
    F32 ceiling = 32.f;
    for (const SSNavMesh::BandSheet& bs : sheets) ceiling = llmax(ceiling, bs.mZMax);
    ceiling += SS_WF_CEILING_HEADROOM_M;

    // <SS:Nexii> The acoustic bake rides the same job (same snapshot, same gates);
    // tier B is opt-in quality AND a claimed ACOUSTIC channel - nobody pays for
    // analysed reverb nobody asked for. Every setting is read here on the main
    // thread, so the worker job and the batch fan-out below never touch one.
    static LLCachedControl<bool> acoustics(gSavedSettings, "SSWorldFieldAcoustics", true);
    static LLCachedControl<U32> acoustic_quality(gSavedSettings, "SSWorldFieldAcousticsQuality", 0);
    const bool do_acoustic = (bool)acoustics;
    const bool tier_b = do_acoustic && llmin((U32)acoustic_quality, 1u) >= 1
                     && ss_wf_interest_count(tile.mRegionHandle, (S32)EChannel::ACOUSTIC) > 0;
    const F32 open_k = ss_wf_open_k();

    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    LL::WorkQueue::ptr_t main = LL::WorkQueue::getInstance("mainloop");
    if (!general || !main) return;              // no worker: labels stay AIR_UNKNOWN, consumers cope

    mBuildBusy = true;
    mBuildStartedAt = mNow;
    const U32 generation = mGridGeneration;
    const U64 region = tile.mRegionHandle;
    const U32 serial = tile.mGeomSerial;
    const S32 sheet_count = (S32)sheets.size();
    (void)stamp;

    auto sheet_set = std::make_shared<std::vector<SSNavMesh::BandSheet> >(std::move(sheets));
    auto span_bottom = std::make_shared<std::vector<F32> >();
    auto span_top = std::make_shared<std::vector<F32> >();
    auto span_flags = std::make_shared<std::vector<U8> >();
    auto surveyed = std::make_shared<std::vector<U8> >();
    auto unsurveyed_count = std::make_shared<S32>(0);
    auto gap_labels = std::make_shared<std::vector<U8> >();
    auto gap_depths = std::make_shared<std::vector<U16> >();
    auto probes = std::make_shared<std::vector<SSAcoustic::Probe> >();
    auto cell_start = std::make_shared<std::vector<S32> >();
    auto links = std::make_shared<std::vector<SSAcoustic::Link> >();
    auto adj_start = std::make_shared<std::vector<S32> >();
    auto adj_node = std::make_shared<std::vector<S32> >();
    auto adj_cost = std::make_shared<std::vector<F32> >();
    auto mip_top = std::make_shared<std::vector<F32> >();
    auto mip_bottom = std::make_shared<std::vector<F32> >();
    auto mip_flags = std::make_shared<std::vector<U8> >();
    auto worker_ms = std::make_shared<F32>(0.f);

    // The acoustic lattice runs at a coarse multiple of the cell grid - near
    // 8 m per probe, never finer than the grid itself.
    const S32 lat_res = llmin(llclamp((S32)llround((F32)res * cell / 8.f), 8, 64), res);

    main->postTo(
        general,
        [res, cell, region_origin, ceiling, open_k, lat_res, do_acoustic, tier_b,
         max_spans = SS_WF_MAX_SPANS, sheet_set, span_bottom, span_top, span_flags,
         surveyed, unsurveyed_count,
         gap_labels, gap_depths, probes, cell_start, links, adj_start, adj_node, adj_cost,
         mip_top, mip_bottom, mip_flags, worker_ms]()
        {
            LLTimer t;
            ss_wf_build_grid(res, cell, region_origin, *sheet_set, *span_bottom, *span_top, *span_flags, *surveyed);
            sheet_set->clear();     // the sheets have been read; drop the references before the long walks
            for (U8 v : *surveyed) if (!v) ++(*unsurveyed_count);
            // <SS:Nexii> The survey mask goes in with the geometry: a cell no sheet covered has no spans, and its single 0..ceiling gap would otherwise read as confidently OUTDOORS. It is labelled AIR_UNKNOWN instead, and it neither seeds the flood nor blocks it. [interaction: SSWorldFieldCore::classify]
            SSWorldFieldCore::classify(res, max_spans, cell, ceiling, open_k,
                                       span_top->data(), span_bottom->data(),
                                       *gap_labels, *gap_depths, surveyed->data());
            if (do_acoustic)
            {
                ss_wf_acoustic_build(res, max_spans, cell, ceiling, lat_res,
                                     *span_top, *span_bottom, *gap_labels, *gap_depths, *surveyed,
                                     *probes, *cell_start, *links, *adj_start, *adj_node, *adj_cost);
                if (tier_b && !probes->empty())
                {
                    // The 4x-coarsened span mip tier B traces against (reverb
                    // statistics do not need 0.25 m walls) - built here while the
                    // grid is hot, handed to the batches through the shared
                    // buffers below.
                    SSAcoustic::Snap snap;
                    snap.mTop = span_top->data();
                    snap.mBottom = span_bottom->data();
                    snap.mRes = res;
                    snap.mCell = cell;
                    snap.mCeiling = ceiling;
                    snap.mMaxSpans = max_spans;
                    SSAcoustic::Snap mip = SSAcoustic::buildMip(snap, *mip_top, *mip_bottom, *mip_flags);
                    (void)mip;
                }
            }
            *worker_ms = t.getElapsedTimeF32() * 1000.f;
            return true;
        },
        [this, generation, region, serial, res, cell, ceiling, lat_res, sheet_count, tier_b,
         main, general,
         span_bottom, span_top, span_flags, surveyed, unsurveyed_count,
         gap_labels, gap_depths,
         probes, cell_start, links, adj_start, adj_node, adj_cost,
         mip_top, mip_bottom, mip_flags, worker_ms](bool)
        {
            mBuildBusy = false;
            if (generation != mGridGeneration) return;

            auto it = mTiles.find(region);
            if (it == mTiles.end()) return;
            if (it->second.mGeomSerial != serial) return;   // the navmesh settled again mid-walk; the next update rebuilds

            Tile& t = it->second;
            t.mRes = res;
            t.mCell = cell;
            t.mCeiling = ceiling;
            t.mSheets = sheet_count;
            t.mSpanBottom = span_bottom;
            t.mSpanTop = span_top;
            t.mSpanFlags = span_flags;
            t.mGapLabel = gap_labels;
            t.mGapDepth = gap_depths;
            t.mSurveyed = surveyed;
            t.mUnsurveyed = *unsurveyed_count;

            Tile::Acoustic& ac = t.mAcoustic;
            ac.mLatRes = lat_res;
            ac.mLatCell = (lat_res > 0) ? (F32)res * cell / (F32)lat_res : 0.f;
            ac.mCeiling = ceiling;
            ac.mProbes = std::move(*probes);
            ac.mCellStart = std::move(*cell_start);
            ac.mLinks = std::move(*links);
            ac.mAdjStart = std::move(*adj_start);
            ac.mAdjNode = std::move(*adj_node);
            ac.mAdjCost = std::move(*adj_cost);

            t.mGridSerial = serial;
            t.mBuiltAt = mNow;
            t.mValid = true;
            mLastBuildMS = *worker_ms;
            ++mGridBuilds;

            // <SS:Nexii> Tier B behind the classification: every probe's bundle is
            // independent, so the bake fans probe BATCHES across the general queue
            // as separate jobs - the classification stays the one-at-a-time job it
            // is, tier B is many small ones behind it, each store-back serial-gated
            // individually (a churning region drops the stale batch and the next
            // classification re-runs the whole walk). The mip the batches trace
            // against was built in the worker above; its dimensions follow from
            // buildMip's own formula, so nothing is plumbed back.
            if (tier_b && !ac.mProbes.empty() && !mip_top->empty()
                && mip_top->size() == mip_bottom->size())
            {
                const S32 mip_res = res / SSAcoustic::BUNDLE_MIP;
                const S32 mip_spans = SS_WF_MAX_SPANS + 2;
                const F32 mip_cell = cell * (F32)SSAcoustic::BUNDLE_MIP;
                if (mip_res >= 1 && mip_top->size() == (size_t)mip_spans * (size_t)mip_res * (size_t)mip_res)
                {
                    auto probe_snap = std::make_shared<std::vector<SSAcoustic::Probe> >(ac.mProbes);
                    constexpr S32 BATCH = 256;
                    const S32 count = (S32)ac.mProbes.size();
                    for (S32 start = 0; start < count; start += BATCH)
                    {
                        const S32 end = llmin(start + BATCH, count);
                        auto bundles = std::make_shared<std::vector<SSAcoustic::Bundle> >((size_t)(end - start));
                        main->postTo(
                            general,
                            [probe_snap, mip_top, mip_bottom, mip_flags, start, end,
                             mip_res, mip_spans, mip_cell, ceiling, bundles]()
                            {
                                SSAcoustic::Snap ms;
                                ms.mTop = mip_top->data();
                                ms.mBottom = mip_bottom->data();
                                ms.mFlags = mip_flags->data();
                                ms.mRes = mip_res;
                                ms.mCell = mip_cell;
                                ms.mCeiling = ceiling;
                                ms.mMaxSpans = mip_spans;
                                for (S32 i = start; i < end; ++i)
                                {
                                    SSAcoustic::Bundle b;
                                    SSAcoustic::traceBundle(ms, (*probe_snap)[(size_t)i],
                                                            SSAcoustic::BUNDLE_RAYS, SSAcoustic::BUNDLE_BOUNCES, b);
                                    (*bundles)[(size_t)(i - start)] = b;
                                }
                                return true;
                            },
                            [this, generation, region, serial, start, end, bundles](bool)
                            {
                                if (generation != mGridGeneration) return;
                                auto cit = mTiles.find(region);
                                if (cit == mTiles.end() || !cit->second.hasGrid()) return;
                                if (cit->second.mGridSerial != serial) return;
                                if (cit->second.mAcoustic.mProbes.size() < (size_t)end) return;

                                for (S32 i = start; i < end; ++i)
                                {
                                    const SSAcoustic::Bundle& b = (*bundles)[(size_t)(i - start)];
                                    SSAcoustic::Probe& p = cit->second.mAcoustic.mProbes[(size_t)i];
                                    p.mMFP = b.mMFP;
                                    p.mRT60 = b.mRT60;
                                    p.mEcho = b.mEcho;
                                    p.mFirstDelay = b.mFirstDelay;
                                    for (S32 k = 0; k < 3; ++k) p.mFirstDir[k] = b.mFirstDir[k];
                                    for (S32 k = 0; k < 8; ++k) p.mOpenness[k] = b.mOpenness[k];
                                    p.mHaveBundle = 1;
                                }
                            });
                    }
                }
            }
        });
}

// A span's hue for the overlay: altitude reads as colour - blue at the floor
// through green to ember red at the grid's ceiling.
static LLColor4 ss_wf_band_hue(F32 t, F32 alpha)
{
    t = llclamp(t, 0.f, 1.f);
    const F32 c0[3] = { 0.25f, 0.5f, 1.f };
    const F32 c1[3] = { 0.3f, 1.f, 0.4f };
    const F32 c2[3] = { 1.f, 0.45f, 0.2f };
    const F32* lo = (t < 0.5f) ? c0 : c1;
    const F32* hi = (t < 0.5f) ? c1 : c2;
    const F32 u = (t < 0.5f) ? t * 2.f : (t - 0.5f) * 2.f;
    return LLColor4(lerp(lo[0], hi[0], u), lerp(lo[1], hi[1], u),
                    lerp(lo[2], hi[2], u), alpha);
}

// The world field's own overlay: what the cell grid holds, what the air
// classification decided with its covered distance, and what the drainage pass
// reads - view picked by SSWorldFieldDebugView, distance-thinned like the wind
// flowmap's.
void SSWorldField::renderDebug()
{
    static LLCachedControl<U32> view(gSavedSettings, "SSWorldFieldDebugView", 1);
    const S32 which = llclamp((S32)view, 1, 7);
    // <SS:Nexii> View 7: the census navmesh - Detour polygon edges by band (doc/atmo_magic_navmesh.md). It is its own store, so it draws whether or not this field holds tiles, and it must run before the tile gate below.
    if (which == 7)
    {
        SSNavMesh::getInstance()->renderDebug(true);
        return;
    }
    if (mTiles.empty()) return;

    static LLCachedControl<F32> range_setting(gSavedSettings, "SSAtmoWindFlowDebugRange", 24.f);
    const F32 full = llclamp((F32)range_setting, 16.f, 4096.f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    auto mark = [&](const LLVector3& p, const LLColor4& c, F32 size)
    {
        gGL.color4fv(c.mV);
        gGL.vertex3f(p.mV[VX] - size, p.mV[VY], p.mV[VZ]);
        gGL.vertex3f(p.mV[VX] + size, p.mV[VY], p.mV[VZ]);
        gGL.vertex3f(p.mV[VX], p.mV[VY] - size, p.mV[VZ]);
        gGL.vertex3f(p.mV[VX], p.mV[VY] + size, p.mV[VZ]);
    };

    // <SS:Nexii> Glyphs for the band-surfaces view: the shape says what the air above the surface is - a circle for outdoors, a triangle for sheltered, a square for interior, the plain cross where the flood has no label yet - so a stack of storeys reads by outline, not by hue alone. All line segments, drawn inside the LINES batch. [interaction: view 1]
    auto circle = [&](const LLVector3& p, const LLColor4& c, F32 r)
    {
        gGL.color4fv(c.mV);
        for (S32 i = 0; i < 8; ++i)
        {
            const F32 a0 = F_TWO_PI * (F32)i / 8.f, a1 = F_TWO_PI * (F32)(i + 1) / 8.f;
            gGL.vertex3f(p.mV[VX] + cosf(a0) * r, p.mV[VY] + sinf(a0) * r, p.mV[VZ]);
            gGL.vertex3f(p.mV[VX] + cosf(a1) * r, p.mV[VY] + sinf(a1) * r, p.mV[VZ]);
        }
    };
    auto triangle = [&](const LLVector3& p, const LLColor4& c, F32 r)
    {
        gGL.color4fv(c.mV);
        const LLVector3 a(p.mV[VX], p.mV[VY] + r, p.mV[VZ]), b(p.mV[VX] - r * 0.866f, p.mV[VY] - r * 0.5f, p.mV[VZ]), d(p.mV[VX] + r * 0.866f, p.mV[VY] - r * 0.5f, p.mV[VZ]);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV);
        gGL.vertex3fv(b.mV); gGL.vertex3fv(d.mV);
        gGL.vertex3fv(d.mV); gGL.vertex3fv(a.mV);
    };
    auto square = [&](const LLVector3& p, const LLColor4& c, F32 r)
    {
        gGL.color4fv(c.mV);
        const F32 x0 = p.mV[VX] - r, x1 = p.mV[VX] + r, y0 = p.mV[VY] - r, y1 = p.mV[VY] + r;
        gGL.vertex3f(x0, y0, p.mV[VZ]); gGL.vertex3f(x1, y0, p.mV[VZ]);
        gGL.vertex3f(x1, y0, p.mV[VZ]); gGL.vertex3f(x1, y1, p.mV[VZ]);
        gGL.vertex3f(x1, y1, p.mV[VZ]); gGL.vertex3f(x0, y1, p.mV[VZ]);
        gGL.vertex3f(x0, y1, p.mV[VZ]); gGL.vertex3f(x0, y0, p.mV[VZ]);
    };

    auto strideFor = [&](F32 wx, F32 wy) -> S32
    {
        const F32 away = llmax(fabsf(wx - cam.mV[VX]), fabsf(wy - cam.mV[VY]));
        return (away < full) ? 1 : (away < full * 2.f) ? 2 : 4;
    };

    gGL.begin(LLRender::LINES);

    for (const auto& entry : mTiles)
    {
        const Tile& tile = entry.second;
        if (!tile.hasGrid()) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const F32 cell = tile.mCell;
        const size_t layer = (size_t)tile.mRes * tile.mRes;

        // The grid's footprint, so a region the navmesh has not reached yet
        // reads as an empty box rather than as nothing at all.
        {
            const F32 x1 = origin.mV[VX] + (F32)tile.mRes * cell;
            const F32 y1 = origin.mV[VY] + (F32)tile.mRes * cell;
            const F32 z0 = 0.f;
            const F32 z1 = tile.mCeiling;
            gGL.color4f(0.5f, 0.55f, 0.7f, 0.35f);
            const F32 bx[4] = { origin.mV[VX], x1, x1, origin.mV[VX] };
            const F32 by[4] = { origin.mV[VY], origin.mV[VY], y1, y1 };
            for (S32 i = 0; i < 4; ++i)
            {
                const S32 j = (i + 1) % 4;
                gGL.vertex3f(bx[i], by[i], z0); gGL.vertex3f(bx[j], by[j], z0);
                gGL.vertex3f(bx[i], by[i], z1); gGL.vertex3f(bx[j], by[j], z1);
                gGL.vertex3f(bx[i], by[i], z0); gGL.vertex3f(bx[i], by[i], z1);
            }
        }

        if (which == 1)
        {
// Every solid span the grid holds, standing at the altitude it holds
            // it at, hue by altitude so stacked storeys read separately instead of
            // fusing into one roof.
            const F32 ceiling = llmax(tile.mCeiling, 1.f);
            const bool labelled = current(tile)
                                  && tile.mGapLabel->size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer;
            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t col = (size_t)y * tile.mRes + x;
                    for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
                    {
                        const F32 z = (*tile.mSpanTop)[(size_t)k * layer + col];
                        if (z <= -FLT_MAX * 0.5f) break;

                        const LLVector3 p(wx, wy, z);
                        const LLColor4 c = ss_wf_band_hue(z / ceiling, 0.85f);
                        const U8 lab = labelled ? (*tile.mGapLabel)[col * (SS_WF_MAX_SPANS + 1) + (size_t)k + 1] : (U8)AIR_UNKNOWN;   // the gap above span k
                        if (lab == AIR_OUTDOORS) circle(p, c, cell * 0.4f);
                        else if (lab == AIR_SHELTERED) triangle(p, c, cell * 0.45f);
                        else if (lab == AIR_INTERIOR) square(p, c, cell * 0.35f);
                        else mark(p, c, cell * 0.4f);
                    }
                }
            }
        }
        else if (which == 2)
        {
            // The touching classification, per air gap: outdoors air green,
            // sheltered air amber fading with occlusion depth (how far the
            // opening's reach still carries), interior air red. A column's
            // gaps sit between its spans, below the lowest and above the
            // highest. Current means the labels cover every live gap: a
            // no-op re-peel can shift the spans without moving the serial,
            // and the flood refills them later.
            const bool have = SSWorldField::current(tile)
                                 && tile.mGapLabel->size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer
                                 && tile.mGapDepth->size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer;
            if (!have) continue;

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t col = (size_t)y * tile.mRes + x;
                    F32 g0 = 0.f;
                    for (S32 k = 0; k <= SS_WF_MAX_SPANS; ++k)
                    {
                        const F32 stop = (k < SS_WF_MAX_SPANS) ? (*tile.mSpanTop)[(size_t)k * layer + col]
                                                               : tile.mCeiling;
                        const F32 g1 = (stop > -FLT_MAX * 0.5f) ? stop : tile.mCeiling;
                        if (g1 - g0 > 0.05f)
                        {
                            const size_t gi = col * (SS_WF_MAX_SPANS + 1) + (size_t)k;
                            const U8 lab = (*tile.mGapLabel)[gi];
                            const F32 z = (g0 + g1) * 0.5f;
                            if (lab == AIR_OUTDOORS)
                            {
                                mark(LLVector3(wx, wy, z), LLColor4(0.3f, 1.f, 0.4f, 0.85f), cell * 0.4f);
                            }
                            else if (lab == AIR_SHELTERED)
                            {
                                const F32 d_m = (F32)(*tile.mGapDepth)[gi] * 0.1f;   // decimetres
                                const F32 a = llmax(0.9f / (1.f + d_m), 0.08f);
                                mark(LLVector3(wx, wy, z), LLColor4(1.f, 0.8f, 0.2f, a), cell * 0.4f);
                            }
                            else if (lab == AIR_INTERIOR)
                            {
                                mark(LLVector3(wx, wy, z), LLColor4(1.f, 0.25f, 0.25f, 0.85f), cell * 0.4f);
                            }
                        }
                        if (stop <= -FLT_MAX * 0.5f) break;   // past the column's spans
                        g0 = stop;
                    }
                }
            }
        }
        else if (which == 3)
        {
            // Drainage topology at the tile's own resolution: standing water
            // blue, and an arrow per cell down the filled surface's D8 - the
            // outlet chain a pool's water will actually follow, ending where
            // the eave rule ends the surface. The grid clamps its resolution,
            // so the walk and the spacing follow the grid's own answers, not
            // the tile's.
            auto& cached = sDrainDebug[tile.mRegionHandle];
            if (cached.mSerial != tile.mGridSerial)
            {
                cached.mGrid = SSRainShadowMap::SurfaceGrid();
                cached.mDrain = Drainage();
                cached.mSerial = tile.mGridSerial;
                if (!buildSurfaceGrid(tile.mRegionHandle, tile.mRes, cached.mGrid)
                    || !buildDrainage(cached.mGrid, cached.mDrain))
                {
                    cached.mSerial = 0;
                    continue;
                }
            }
            const SSRainShadowMap::SurfaceGrid& grid = cached.mGrid;
            const Drainage& drain = cached.mDrain;

            const S32 gn = grid.mN;
            const F32 gcell = grid.mCell;
            if (gn < 3 || gcell <= 0.f) continue;

            for (S32 y = 0; y < gn; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * gcell;
                for (S32 x = 0; x < gn; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * gcell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t i = (size_t)y * gn + x;
                    const U8 f = grid.mFlags[i];
                    if (f == 0 || (f & SSRainShadowMap::SURF_WATER)) continue;

                    const F32 z = grid.mZ[i];
                    if (drain.mPool[i])
                    {
                        mark(LLVector3(wx, wy, z), LLColor4(0.2f, 0.5f, 1.f, 0.9f), gcell * 0.45f);
                    }
                    else if (drain.mD8[i] != 4)
                    {
                        const S32 di = drain.mD8[i];
                        const F32 len = gcell * 0.7f;
                        const F32 dx = (F32)((di % 3) - 1) * len;
                        const F32 dy = (F32)((di / 3) - 1) * len;
                        gGL.color4f(0.7f, 0.85f, 1.f, 0.55f);
                        gGL.vertex3f(wx, wy, z + 0.4f);
                        gGL.vertex3f(wx + dx, wy + dy, z + 0.4f);
                    }
                }
            }
        }
        else if (which == 5)
        {
            // The column spans themselves: each solid span drawn as two flat
            // rects - its floor and its ceiling - coloured by the air state
            // standing on it (the gap above it), with a dim line joining
            // ceiling to floor through the solid. A column's stack reads as a
            // ladder of state-coloured plates on one spine; the air gaps
            // between spans stay empty, which is exactly where the flood
            // walks.
            const bool have = SSWorldField::current(tile)
                                 && tile.mGapLabel->size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer
                                 && tile.mGapDepth->size() >= (size_t)(SS_WF_MAX_SPANS + 1) * layer;
            const F32 ceiling = llmax(tile.mCeiling, 1.f);

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t col = (size_t)y * tile.mRes + x;
                    const F32 s = cell * 0.4f;
                    auto span_rect = [&](F32 z)
                    {
                        gGL.vertex3f(wx - s, wy - s, z); gGL.vertex3f(wx + s, wy - s, z);
                        gGL.vertex3f(wx + s, wy - s, z); gGL.vertex3f(wx + s, wy + s, z);
                        gGL.vertex3f(wx + s, wy + s, z); gGL.vertex3f(wx - s, wy + s, z);
                        gGL.vertex3f(wx - s, wy + s, z); gGL.vertex3f(wx - s, wy - s, z);
                    };
                    auto state_color = [&](U8 st)
                    {
                        switch (st)
                        {
                            case AIR_OUTDOORS:  gGL.color4f(0.3f, 1.f, 0.4f, 0.8f); break;
                            case AIR_SHELTERED: gGL.color4f(1.f, 0.8f, 0.2f, 0.8f); break;
                            case AIR_INTERIOR:  gGL.color4f(1.f, 0.25f, 0.25f, 0.85f); break;
                            default:            gGL.color4f(0.55f, 0.6f, 0.7f, 0.5f); break;
                        }
                    };

                    for (S32 k = 0; k < SS_WF_MAX_SPANS; ++k)
                    {
                        const size_t si = (size_t)k * layer + col;
                        const F32 z1 = (*tile.mSpanTop)[si];
                        if (z1 <= -FLT_MAX * 0.5f) break;   // past the column's spans
                        const F32 z0 = (*tile.mSpanBottom)[si];
                        if (z1 - z0 < 0.01f) continue;

                        // The state of the air the body holds up: the gap
                        // above it in the column.
                        U8 st = AIR_UNKNOWN;
                        if (have)
                        {
                            st = (*tile.mGapLabel)[col * (SS_WF_MAX_SPANS + 1) + (size_t)(k + 1)];
                        }

                        state_color(st);
                        span_rect(z1);
                        span_rect(z0);

                        // The spine: ceiling to floor, through the solid.
                        gGL.color4f(0.6f, 0.65f, 0.75f, 0.35f);
                        gGL.vertex3f(wx, wy, z1);
                        gGL.vertex3f(wx, wy, z0);
                    }
                }
            }
        }
    }

    gGL.end();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    // <SS:Nexii> View 6: the declared-shape census overlay - what the exact
    // query layer holds, boxes by layer and provenance
    // (doc/atmo_magic_worldfield_competition.md 7.8).
    if (which == 6)
    {
        SSWorldFieldShapes::getInstance()->renderDebug();
    }

    // Drop debug views for regions the field no longer holds.
    for (auto it = sDrainDebug.begin(); it != sDrainDebug.end();)
    {
        it = mTiles.count(it->first) ? std::next(it) : sDrainDebug.erase(it);
    }
}

