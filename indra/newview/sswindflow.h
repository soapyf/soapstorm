/**
 * @file sswindflow.h
 * @brief Atmo Magic: per-region wind flowmap.
 *
 *        A top-down and a bottom-up ortho depth capture give the solid span of
 *        every column. Those are sliced into a handful of adaptive horizontal
 *        slabs, and a divergence-free field is solved over the result on the
 *        GPU, so air accelerates through alleys, piles up against windward
 *        faces, lifts over rooftops and goes calm in courtyards.
 *
 *        Anchored to the region and solved once, the way the rain shadow maps
 *        are: a static flowmap of the build, not a running simulation. It
 *        rebuilds when the build changes underneath it, when the ambient wind
 *        changes, or when the camera moves to a different sky track.
 *
 *        Consumed by the ambient audio mix and by precipitation advection.
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

#ifndef SS_WINDFLOW_H
#define SS_WINDFLOW_H

#include "llrendertarget.h"
#include "llsingleton.h"
#include "ssworldfield.h"
#include "v3math.h"
#include "v4math.h"

#include <glm/mat4x4.hpp>

#include <functional>
#include <map>
#include <memory>
#include <atomic>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

const S32 SS_WIND_MAX_SLICES = 16;
const S32 SS_WIND_MIN_SLICES = 2;

const S32 SS_WIND_MAX_LEVELS = 5;
const S32 SS_WIND_MIN_LEVEL_RES = 24;

const S32 SS_WIND_PROBES = 4;

class SSWindFlowMap : public LLSingleton<SSWindFlowMap>
{
    LLSINGLETON_EMPTY_CTOR(SSWindFlowMap);

public:
    ~SSWindFlowMap() override;

    static bool isSupported();

    void update();

    static void markDirty(const LLVector3& pos_agent, F32 radius);

    void clear();

    void rebuildAll();

    bool isValid() const;

    static bool drivesWind();

    LLVector3 sample(const LLVector3& pos_agent) const;

    // <SS:Nexii> Granular transport reads: the solved field minus the gust layer - gusts are a scalar applied once per tick by every consumer, never per cell, which keeps the erosion tick and spawn walk linear. No "ground slab": the read picks, per column, the first slab whose ceiling clears that column's own surface height, as terrain and builds slope across slabs. pos_agent.z IS that surface - callers pass the cell's stored height or the camera's. The grid variant bulk-samples the region's lattice for SSGranular::step(); surface_z is the field's n*n height array; cells outside the tile answer with the ambient wind at full exposure.
    LLVector3 sampleGround(const LLVector3& pos_agent) const;
    bool sampleGroundGrid(LLViewerRegion* regionp, S32 n, F32 cell, const F32* surface_z,
                          std::vector<LLVector4>& out) const;

    F32 exposure(const LLVector3& pos_agent) const;

    // <SS:Nexii> The boundary-layer wind gradient, for anything scaling the authored 10m wind to another altitude (the cirrus cloud band). windAlpha() is the roughness-derived shear exponent of the current camera region; windGradientScale(z_agl) is the power-law factor v(z)/v_ref = (z/z_ref)^alpha, held constant above the boundary-layer top (~1.5km), the free atmosphere. groundRefZ() is the reference ground z_agl measures from - the water plane on water regions, else the region's true ground. All fall back to the SSAtmoWindFlowGradient setting until a flowmap tile is solved.
    F32 windAlpha() const;
    F32 windGradientScale(F32 z_agl) const;
    F32 groundRefZ() const;

    void gustAt(const LLVector3& pos_agent, F64 time, F32& scale, F32& veer) const;
    F32 gust(const LLVector3& pos_agent) const;

    void renderDebug();

    U64 capturedRegion() const { return mCaptureRegion; }

    // <SS:Nexii> A claim on the worldfield's real-geometry capture for the map's region, so the true-ground read (buildTrueGround) has a valid tile. Refreshed when the camera region changes; released on clear().
    void refreshTrueGroundClaim(U64 region_handle);

    S32 sliceCount() const;
    S32 resolution() const;
    F32 extent() const;
    F32 cellSize() const;
    F32 sliceAltitude(S32 i) const;
    F32 lastSolveMS() const { return mSolveMS; }

    bool surfaceAt(const LLVector3& pos_agent, F32& top) const;

    S32 forEachColumn(const LLVector3& center_agent, F32 radius_m,
                      const std::function<void(const LLVector3& pos_agent, F32 top)>& fn) const;
    F32 carvedFraction() const;
    F32 solidFill() const;
    F64 age() const;
    U32 buildCount() const { return mBuildCount; }
    S32 tileCount() const { return (S32)mTiles.size(); }
    bool lastBuildPartial() const { return mLastBuildPartial; }

    // <SS:Nexii> Rebuild telemetry for the info overlay: whether the solve rode the GL worker thread, the full/partial split, the average partial box as a share of the tile, the last solver block's wall time, and the estimated VRAM the solver textures hold.
    bool workerActive() const { return mWorkerReady; }
    bool lastSolveOnWorker() const { return mLastSolveWorker; }
    S32 partialBuildCount() const { return mPartialBuilds; }
    S32 fullBuildCount() const { return mFullBuilds; }
    F32 partialBoxShare() const;
    F32 workerSolveMS() const { return mSolveBlockMS; }
    F32 vramMB() const;

private:
    struct Tile
    {
        U64 mRegionHandle = 0;

        S32 mRes = 0;
        S32 mSlices = 0;
        F32 mExtent = 0.f;
        F32 mMargin = 0.f;
        LLVector3 mOriginRegion;

        F32 mSliceZ[SS_WIND_MAX_SLICES + 1] = { 0.f };
        LLVector3 mAmbient[SS_WIND_MAX_SLICES];

        // <SS:Nexii> The wind-shear exponent of the boundary-layer power law, derived from how tall and how open the region's surface is (deriveWindAlpha in sswindflow.cpp). The reference wind is authored at 10m above open level ground; this alpha scales it upward through the layer and is the number cloud drift uses to reach the cirrus altitude.
        F32 mAlpha = 0.16f;

        F32 mGroundRef = 0.f;
        F32 mBandTop = 0.f;
        F32 mBandBottom = 0.f;
        S32 mTrack = 0;

        std::vector<LLVector4> mFlow;

        std::vector<U8> mSolid;

        // <SS:Nexii> Partial rebuilds: the last solve's pressure field, kept so an edit rebuild warm-starts the Poisson relaxation from the previous answer instead of from zero - the whole point of re-solving a local change in a box against the old field. Fine level only; the pyramid levels are rebuilt from scratch every time.
        std::vector<F32> mPressure;

        std::vector<F32> mSurfaceTop;

        // <SS:Nexii> The TRUE-GROUND reference per column: the region terrain heightmap, downsampled to tile resolution and floored by the water plane where set. Unlike the captured mSurfaceTop - the topmost solid, a rooftop over a column - this is the ground the wind profile starts from, so the boundary layer is measured against actual terrain. The terrain grid is always complete (objects never occlude it), so no gap fill; a low-resolution read keeps it cheap and smooth. Null until the tile is full-built.
        std::vector<F32> mGroundZ;

        F32 mCarved = 0.f;
        F32 mSolidFill = 0.f;

        LLVector3 mBuiltWind;
        U32 mTuning = 0;
        F64 mBuildTime = 0.0;
        bool mDirty = false;
        // Pending geometry edits, as a box of tile-local cells (inclusive),
        // so a settled rebuild only re-solves the flow it can change (a little
        // upwind, everything downwind) rather than the whole domain. Empty when
        // mDirty is false; a union of every un-settled markDirty that landed.
        S32 mDirtyC0[2] = { -1, -1 };
        S32 mDirtyC1[2] = { -1, -1 };
        bool mValid = false;
        F64 mLastTouched = 0.0;
    };

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    const Tile* tileAt(const LLVector3& pos_agent) const;
    const Tile* cameraTile() const;

    bool needsSolve(const Tile& tile) const;
    std::string solveStaleReason(const Tile& tile) const;
    void evict();

    enum class EStage
    {
        IDLE,
        CAPTURE_TOP,
        CAPTURE_PROBE,
        REDUCE,
        SOLVE_INIT,
        BRIDGE,
        SOLVE_RUN,
        READBACK,
        CONVERT,
        SOLVE_GL,
        COMMIT
    };

    bool advanceBuild();
    bool beginBuild(Tile& tile);

    void abandonBuild();
    void releaseScratch();

    void postWorker(std::function<void()> work, EStage next);
    // The solve+readback+convert unit, submitted to the GL worker thread when
    // one is available (see glWorker()); without a worker the existing staged
    // READBACK/CONVERT path runs instead.
    void postSolveGL(const Tile& tile);
    bool solveRunWorker(const Tile& tile);

    Tile* buildTile();

    bool stageCaptureTop(Tile& tile);
    bool stageCaptureProbe(Tile& tile, S32 which);
    void stageReduce(Tile& tile);
    bool stageSolveInit(Tile& tile);
    void stageBridge(Tile& tile);
    bool stageSolveRun(Tile& tile);
    bool stageReadback(Tile& tile);
    void stageConvert(Tile& tile);
    void stageCommit(Tile& tile);

    bool ensureResources(S32 res, S32 slices);
    void releaseResources();
    bool ensureShaders();

    void chooseBand(Tile& tile, LLViewerRegion* regionp);

    bool captureHeights(Tile& tile);

    bool captureAlong(LLRenderTarget& target, S32 res, const Tile& tile,
                      const LLVector3& dir, const LLVector3& eye,
                      F32 half, F32 range, std::vector<F32>& out, glm::mat4& view_out);

    void beginProbes(Tile& tile);
    bool captureProbe(Tile& tile, S32 which);
    void reconstructHidden(Tile& tile);

    void auditProbes(const Tile& tile) const;

    void renderDebugCapture(S32 which);

    void renderDebugStreamlines();

    F32 slabAlpha(const Tile& tile, S32 k, F32 cam_z) const;

    void buildCarveFlags(const Tile& tile);

    void readMaskForBridge(const Tile& tile);
    void bridgePassages(const Tile& tile);
    void uploadBridgedMask(const Tile& tile);

    // <SS:Nexii> Partial rebuild support. hasPendingEdits() is the box-empty test; partialBoxes() turns a tile's pending edit box into the mask box (where the solid mask can change), the capture footprint around it, and the solve box (mask box + a little headwind + everything downwind); restoreConsumedDirty() returns an abandoned partial build's edits to the tile so they are not lost.
    static bool hasPendingEdits(const Tile& tile)
    {
        return tile.mDirty && tile.mDirtyC0[0] >= 0 && tile.mDirtyC0[1] >= 0;
    }
    bool partialBoxes(const Tile& tile, const LLVector3& wind_h);
    void restoreConsumedDirty();

    void placeSlices(Tile& tile);

    bool solveInit(const Tile& tile);
    bool solveRun(const Tile& tile);

    static S32 levelRes(S32 res, S32 level) { return res >> level; }
    static S32 levelCount(S32 res);

    void readback(Tile& tile);
    void unpackVolume(Tile& tile);

    static size_t index(const Tile& tile, S32 x, S32 y, S32 k)
    {
        return ((size_t)k * tile.mRes + y) * tile.mRes + x;
    }

    static void sliceAt(const Tile& tile, F32 z, S32& k, F32& frac);

    LLVector4 groundCell(const Tile& tile, const LLVector3& tile_origin_agent, F32 cell,
                         const LLVector3& pos_agent) const;

    bool mShadersReady = false;
    bool mShaderFailed = false;

    std::map<U64, Tile> mTiles;

    std::vector<F32> mTop;

    std::vector<F32> mProbeDepth[SS_WIND_PROBES];
    glm::mat4        mProbeView[SS_WIND_PROBES];
    F32              mProbeHalf[SS_WIND_PROBES] = { 0.f };

    S32              mProbeRes = 0;

    // The probe footprint actually captured this build. Always tile-scale for
    // a full solve (probe texels over the whole region, the uProbeRes image the
    // shader indexes); for a partial solve it is the edit box's share of that
    // image, uploaded as a sub-rect, and this is its local resolution.
    S32              mProbeTake = 0;

    struct ProbeFrame
    {
        LLVector3 mEye;
        LLVector3 mDir;
        LLVector3 mRight;
        LLVector3 mUp;
    };
    ProbeFrame       mProbeFrame[SS_WIND_PROBES];

    U64              mCaptureRegion = 0;
    S32              mCaptureRes = 0;
    F32              mCaptureCell = 0.f;
    LLVector3        mCaptureOrigin;

    // The footprint the captures this build covered - the whole tile for a full
    // solve, the edit box plus margins for a partial one. The captures and the
    // solve both hang off this, so a partial build never renders or dispatches
    // outside it.
    S32              mCaptureC0[2] = { 0, 0 };    // tile-local cell of the capture image origin
    S32              mCaptureC1[2] = { -1, -1 };  // last tile-local cell the image covers
    F32              mCaptureExtent = 0.f;        // metres across the captured footprint
    LLVector3        mCaptureCentre;              // agent XY of the footprint centre
    bool             mProbeUsable[SS_WIND_PROBES] = { false };
    F32              mProbeMiss[SS_WIND_PROBES] = { 1.f };

    std::vector<F32> mHidden;

    U32 mHeightTex = 0;
    U32 mProbeTex = 0;
    U32 mSolidTex[SS_WIND_MAX_LEVELS] = { 0 };
    U32 mVelTex[SS_WIND_MAX_LEVELS] = { 0 };
    U32 mDivTex[SS_WIND_MAX_LEVELS] = { 0 };
    U32 mPressureTex[SS_WIND_MAX_LEVELS][2] = {};
    S32 mTexRes = 0;
    S32 mTexSlices = 0;
    S32 mProbeTexRes = 0;
    S32 mTexLevels = 0;

    LLRenderTarget mCapture;
    LLRenderTarget mProbeCapture;

    std::vector<U8> mCarveFlags;

    F64 mLastBuild = 0.0;
    F32 mSolveMS = 0.f;
    U32 mBuildCount = 0;
    bool mLastBuildPartial = false;

    // <SS:Nexii> Rebuild telemetry backing the info overlay accessors.
    bool mWorkerReady = false;           // the GL solve worker is live
    bool mLastSolveWorker = false;       // the last build's block ran on the worker
    U32 mPartialBuilds = 0;              // how many rebuilds were box solves
    U32 mFullBuilds = 0;                 // how many were whole-tile solves
    F64 mPartialAreaSum = 0.0;           // box-area/tile-area, for the average
    U32 mPartialCount = 0;
    std::atomic<F32> mSolveBlockMS{ 0.f }; // solve+readback+convert wall time (worker writes)

    std::string mLastRebuildReason;    // the last logged rebuild driver (throttle)
    F64 mLastRebuildLog = 0.0;

    // <SS:Nexii> The worldfield interest held while this map has a region, so the worldfield builds its real-geometry tile there for buildTrueGround. Re-claimed on region change.
    SSWorldField::Interest mTrueGroundClaim;
    U64 mTrueGroundRegion = 0;

    EStage mStage = EStage::IDLE;

    Tile mBuild;

    U64 mBuildRegion = 0;
    S32 mBuildProbe = 0;
    F64 mBuildStart = 0.0;
    bool mWorkerBusy = false;
    bool mClearPending = false;

    // Partial-build state: whether the in-flight build is a box against the
    // old field rather than a whole-tile solve, and which boxes. All boxes are
    // inclusive tile-local cell ranges.
    bool mPartial = false;
    S32 mBoxC0[2] = { 0, 0 };      // solve box: where pressure/velocity is recomputed
    S32 mBoxC1[2] = { -1, -1 };
    S32 mMaskC0[2] = { 0, 0 };     // mask box: where the solid mask and capture change
    S32 mMaskC1[2] = { -1, -1 };

    // The edit box a partial build consumed, restored to the tile if the build
    // is abandoned so the edits are not lost.
    S32 mConsumedDirty[4] = { -1, -1, -1, -1 };

    S32 mJacobiBuffer = 0;         // pressure buffer the last Jacobi runs left in

    // Readback for the pressure warm-start volume.
    std::vector<F32> mPressureRaw;

    bool glWorker();
    void glSolveDone(U32 generation);

    U32 mBuildGeneration = 0;
    bool mSolveFail = false;

    struct SSWindFlowGLWorker;
    // Raw pointer, not a smart pointer: the worker type is only complete in
    // sswindflow.cpp, and a unique_ptr to an incomplete type would break every
    // TU that includes this header. Created and destroyed only here.
    SSWindFlowGLWorker* mGLWorker = nullptr;
    bool mGLWorkerTried = false;

    // Cross-context sync for the solve worker (GLsync stored as void*): the
    // main thread fences after its init writes, the worker waits on it before
    // touching the shared textures, and the worker fences its readback so the
    // next build's init on the main thread does not race the tail of the last.
    void* mSolveFence = nullptr;
    void* mReadbackFence = nullptr;

    std::vector<U8> mMaskRaw;
    std::vector<U8> mMaskBridged;
    bool mMaskChanged = false;

    std::vector<F32> mVolumeRaw;
    std::vector<U8> mSolidRaw;
};

#endif
