/**
 * @file ssnavmesh.h
 * @brief Atmo Magic: the census navmesh - Recast/Detour over the declared-shape census.
 *
 *        Replaces the homebrew 3D-tiled census raster (ssworldfieldtiles, removed
 *        2026-09-09) with recastnavigation: the census records are emitted as
 *        triangles per 32 m column, each column split into height bands wherever
 *        an air gap of SSNavMeshBandGap separates the geometry (a skybox never
 *        shares a build with the ground, and a band boundary never cuts a floor,
 *        which is what kept Detour's tile portals intact in the benchmark), and
 *        every band is one DetourTileCache layer. Rasterization runs on the
 *        General work queue; publishing, obstacles and queries stay on the main
 *        thread. Benchmark and design: doc/atmo_magic_navmesh.md section 10.
 *
 *        DYNAMIC records (movers) never enter a layer: they ride the tile cache
 *        as temporary box obstacles, re-synced on every census rebuild.
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

#ifndef SS_NAVMESH_H
#define SS_NAVMESH_H

#include "llsingleton.h"
#include "v3dmath.h"
#include "v3math.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class dtTileCache;
struct SSNavMeshImpl;

class SSNavMesh : public LLSingleton<SSNavMesh>
{
    LLSINGLETON(SSNavMesh);
    ~SSNavMesh();

public:
    // <SS:Nexii> 0.125 m cells (owner decision 2026-09-10: doorways, stairs and thin ledges in user content need it) and 16 m columns, because DetourTileCache stores a layer's width in a byte, so a tile is at most 255 cells across. A power of two keeps every lattice line exact across neighbouring tiles.
    static constexpr F32 TILE_M = 16.f;         // column edge in metres
    static constexpr S32 TILE_CELLS = 128;      // cells per column edge
    static constexpr F32 CELL = 0.125f;
    static constexpr S32 MAX_BANDS = 12;        // height bands per column; 12 x SS_NAV_MAX_LAYERS_PER_BAND layers must fit the tile cache's 512-per-column buffer

    // Per-frame maintenance: follow census rebuilds, schedule changed bands,
    // launch worker builds under the frame budget, publish finished layers,
    // sync mover obstacles and pump the tile cache.
    void update();

    // Queries in agent coordinates. nearestPoint snaps to the navmesh within
    // reach; findPath fills a straight-path polyline, partial when the goal was
    // unreachable and the path ends at the closest point found.
    bool nearestPoint(const LLVector3& pos_agent, F32 reach, LLVector3& out_agent) const;
    bool findPath(const LLVector3& from_agent, const LLVector3& to_agent, std::vector<LLVector3>& out_points, bool& out_partial) const;
    bool onNavMesh(const LLVector3& pos_agent, F32 reach = 1.f) const;

    // The overlay: filled polygons and edges by band, tile links, mover obstacles, the census and the test path,
    // each behind its SSNavMeshShow* switch (the floater's View tab). force_navmesh draws the polygons regardless -
    // view 7 of SSWorldFieldDebugView.
    void renderDebug(bool force_navmesh = false);
    static bool overlayEnabled();

    // Drop every band and build again from the current census (the floater's Rebuild button).
    void rebuildAll();

    // <SS:Nexii> The per-cell sheet: one band's solid spans over the column's 16 m interior at 0.25 m cells, unioned from the raster's cells and clipped to the sheet's span budget. The worker fills it beside the layers and publish KEEPS it on the band, so the navmesh's own spatial structure carries the world field's per-cell data - the sheet set of a region is the field's only geometry source. Immutable once published: a rebuilt band swaps in a whole new sheet, so a reader holding a shared_ptr is never rewritten under it, on any thread. [interaction: SSWorldField::scheduleGrid]
    struct SpanSheet
    {
        static constexpr S32 RES = 64;          // 16 m at 0.25 m
        static constexpr S32 SPANS = 6;         // the field's SS_WF_MAX_SPANS
        std::vector<U8> mCount;                 // [row * RES + col] spans in the column
        std::vector<F32> mBottom, mTop;         // [(row * RES + col) * SPANS + k], local z metres
        std::vector<U8> mFlags;                 // SSRainShadowMap::SURF_* per span
    };

    // <SS:Nexii> One published band's sheet with the frame it lives in: the column's south-west corner in AGENT space and the band's z range (local z is agent z, the census frame only re-bases XY). This is what SSWorldField snapshots - a vector of these, shared_ptr copies only, no geometry copied - before posting its classification job. [interaction: SSWorldField::scheduleGrid]
    struct BandSheet
    {
        S32 mTx = 0, mTy = 0;
        F32 mZMin = 0.f, mZMax = 0.f;
        LLVector3 mOriginAgent;                 // the column's (x0, y0, 0) corner, agent space
        std::shared_ptr<const SpanSheet> mSheet;
    };
    // <SS:Nexii> Whether band builds keep a world-field sheet - the field's master
    // switch, read per launch (an LLCachedControl, so it follows a live change; the
    // bands already built under the old answer are what resheet() exists for).
    static bool sheetsWanted();
    // <SS:Nexii> Invalidate the signature of every published band holding no sheet, so
    // schedule() rebuilds it. schedule() only enqueues a band whose signature moved,
    // and launch() decides sheets once per build, so a band that built while the field
    // was off would otherwise hold no sheet FOR EVER - and its cells would read as
    // unsurveyed, permanently, with no way back short of a teleport. Called on the
    // off->on edge. Returns how many it queued. [interaction: SSWorldField::update]
    S32 resheet();
    // No band of a column inside the region is queued or building, and no schedule is pending: the field may bump its serial.
    bool regionSettled(U64 region_handle) const;
    // <SS:Nexii> Every published band whose column centre falls inside the region, newest sheet per band, plus a stamp over the band keys and their geometry signatures: an unchanged stamp means an unchanged sheet set, which is how the field decides a rebuild is a no-op. Main thread only (it walks the live band map); the sheets themselves are immutable and safe to read from a worker afterwards. [interaction: SSWorldField::navSettle]
    bool collectSheets(U64 region_handle, std::vector<BandSheet>& out, U64& out_stamp) const;
    U32 sheetsHeld() const { return mSheetsHeld; }
    // The dump's navmesh half: every column the agent-space box touches, its band slots (z-range, alive, tiles, age), what is queued or building for it, whether it sits in the envelope, and a nearest-point probe over the box's top. [interaction: SSWorldFieldShapes::dumpObject]
    void dumpAt(const LLVector3& bmin_agent, const LLVector3& bmax_agent, std::vector<std::string>& out) const;
    // Every tile-border (portal) edge within the radius, linked or not, with its endpoints in agent space. [interaction: renderDebug links]
    void dumpLinksAt(const LLVector3& pos_agent, F32 radius, std::vector<std::string>& out) const;
    // A marked spot (the console's Mark location): drawn by the overlay so a dump and the view line up.
    void setMark(const LLVector3& pos_agent) { mMark = pos_agent; mHasMark = true; }
    void clearMark() { mHasMark = false; }

    // The floater's Test path tab: endpoints in agent space, Detour's answer drawn in the overlay.
    void setTestStart(const LLVector3& pos_agent) { mTestStart = pos_agent; mHasTestStart = true; mTestValid = false; }
    void setTestEnd(const LLVector3& pos_agent) { mTestEnd = pos_agent; mHasTestEnd = true; mTestValid = false; }
    void clearTestPath() { mHasTestStart = mHasTestEnd = mTestValid = false; mTestPath.clear(); }
    bool hasTestStart() const { return mHasTestStart; }
    bool hasTestEnd() const { return mHasTestEnd; }
    bool runTestPath(std::string& out_status);

    // What the last schedule saw in the census.
    S32 lastScheduleSeen() const { return mLastSeen; }
    S32 lastScheduleDynamic() const { return mLastDynamic; }
    S32 lastSchedulePhantom() const { return mLastPhantom; }

    // Stats
    bool active() const { return mNavMesh != nullptr; }
    S32 columnCount() const { return (S32)mColumns.size(); }
    S32 bandCount() const { return (S32)mBands.size(); }
    S32 pendingCount() const { return (S32)mWorklist.size(); }
    S32 inFlightCount() const { return mInFlight; }
    S32 obstacleCount() const { return (S32)mObstacles.size(); }
    U32 buildCount() const { return mBuildCount; }
    F32 lastBuildMS() const { return mLastBuildMS; }
    F32 lastPublishMS() const { return mLastPublishMS; }
    U32 polyCount() const;
    size_t layerBytes() const { return mLayerBytes; }
    U32 layersDropped() const { return mLayersDropped; }    // walkable layers currently without a tile, summed over live bands

private:
    // One band of one column: its geometry signature and the compressed tiles it published.
    struct Band
    {
        U64 mSig = 0;
        F32 mZMin = 0.f;
        F32 mZMax = 0.f;
        std::vector<U32> mRefs;             // dtCompressedTileRef per layer
        bool mAlive = false;                // touched by the latest schedule
        F64 mPublishedAt = -100.0;          // when its layers last landed, for the overlay's rebuild flash
        S32 mLayersDropped = 0;             // walkable layers this band produced past SS_NAV_MAX_LAYERS_PER_BAND
        S32 mUnderTerrain = 0;              // walkable spans under the land last publish, so the warning only fires on change
        std::shared_ptr<const SpanSheet> mSheet;    // the world field's per-cell read of this band, kept for the life of the band
    };

    // A band waiting for a worker build.
    struct Job
    {
        S32 mTx = 0, mTy = 0, mBand = 0;
        F32 mZMin = 0.f, mZMax = 0.f;
        U64 mSig = 0;
    };

    // What a worker hands back: compressed layers for one band.
    struct Result
    {
        Job mJob;
        U32 mGeneration = 0;
        std::vector<std::vector<U8> > mLayers;
        S32 mLayersDropped = 0;             // walkable layers past SS_NAV_MAX_LAYERS_PER_BAND that got no tile
        S32 mUnderTerrain = 0;              // walkable spans nulled under the land; zero when the build kept everything
        std::shared_ptr<SpanSheet> mSheet;  // the world field's span read, when it wants one
        F32 mMS = 0.f;
        bool mOk = false;
        std::string mLog;                   // Recast errors and warnings from the build; empty when clean
    };

    bool ensureInit();
    void teardown();
    void schedule();
    void launch(const Job& job);
    void publish(const std::shared_ptr<Result>& result);
    void removeBand(U64 key, Band& band);
    void syncObstacles();
    void pumpObstacles();
    bool terrainZLocal(F32 x, F32 y, F32& z) const;
    LLVector3 toLocal(const LLVector3& pos_agent) const;
    LLVector3 fromLocal(const LLVector3& pos_local) const;
    static U64 bandKey(S32 tx, S32 ty, S32 band);
    static U64 columnKey(S32 tx, S32 ty);

    // <SS:Nexii> Shared, not unique: a band build on the General queue uses the impl's compressor, so the job keeps the impl alive past the singleton if logout races a build. mAlive is the continuation's token - a mainloop callback that finds it expired never touches this. [interaction: launch, publish]
    std::shared_ptr<SSNavMeshImpl> mImpl;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    dtNavMesh* mNavMesh = nullptr;
    dtTileCache* mTileCache = nullptr;
    dtNavMeshQuery* mQuery = nullptr;

    // <SS:Nexii> The build frame: agent coordinates shift by a region on every border crossing, so tiles are keyed in a frame pinned to the region origin at init and every agent-space position is re-based through toLocal; Detour then never sees a key change on a crossing. [interaction: census anchor]
    LLVector3d mOriginGlobal;
    U64 mCensusStamp = 0;
    U32 mGeneration = 0;                        // bumped on teardown so late worker results are dropped
    std::unordered_map<U64, Band> mBands;
    std::unordered_map<U64, S32> mColumns;      // column key -> band count
    std::vector<Job> mWorklist;
    // Mover obstacles: the live set plus the adds and removals still to be handed to the tile cache under the
    // per-frame request budget (dtTileCache queues at most 64 requests between its updates).
    struct Obstacle
    {
        U32 mRef = 0;                           // dtObstacleRef
        F32 mMin[3] = {0, 0, 0};                // Recast-space box
        F32 mMax[3] = {0, 0, 0};
    };
    std::vector<Obstacle> mObstacles;
    std::vector<Obstacle> mObstacleAdds;
    std::vector<U32> mObstacleRemovals;
    S32 mInFlight = 0;
    std::vector<Job> mInFlightJobs;         // what the workers hold, for regionSettled
    U32 mSheetsHeld = 0;                    // live bands carrying a world-field sheet
    U32 mBuildCount = 0;
    F32 mLastBuildMS = 0.f;
    F32 mLastPublishMS = 0.f;               // main-thread cost of the last publish: tile build with its detail mesh
    size_t mLayerBytes = 0;
    U32 mLayersDropped = 0;
    S32 mLastSeen = 0, mLastDynamic = 0, mLastPhantom = 0;

    LLVector3 mMark;
    bool mHasMark = false;

    // Test path state (agent space).
    LLVector3 mTestStart, mTestEnd;
    bool mHasTestStart = false, mHasTestEnd = false, mTestValid = false, mTestPartial = false;
    std::vector<LLVector3> mTestPath;

};

#endif
