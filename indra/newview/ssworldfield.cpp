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
#include "ssatmomagic.h"
#include "ssglreadback.h"

#include "llfasttimer.h"
#include "llrender.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "workqueue.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <queue>

static const F32 NEIGHBOR_REACH   = 64.f;
static const F32 DEPTH_MISS       = 0.9999f;
static const F32 BOUNDARY_EPSILON = 0.05f;
static const F32 NO_SURFACE       = -FLT_MAX;

static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD("Atmo Magic World Field");
static LLTrace::BlockTimerStatHandle FTM_SS_WORLDFIELD_GRID("Atmo Magic World Field Grid");

// Channel interest refcounts. File statics so an Interest handle's deleter
// stays valid for the process's life regardless of singleton teardown
// order - a destroyed handle must always release its count.
static std::map<std::pair<U64, S32>, S32> sInterests;

// <SS:Nexii> The debug overlay's materialised drainage view, cached per region and rebuilt only when the tile's geometry serial moves - the same drop-on-serial-change discipline the real channels will follow, so the overlay never pays a per-frame priority flood just to draw. Declared with the other file statics because evict() drops it alongside the tiles it belongs to.
struct SS_WF_DrainDebug
{
    SSRainShadowMap::SurfaceGrid mGrid;
    SSWorldField::Drainage mDrain;
    S32 mN = 0;
    U32 mSerial = 0;
};
static std::map<U64, SS_WF_DrainDebug> sDrainDebug;

static S32 ss_wf_interest_count(U64 region_handle, S32 channel)
{
    auto it = sInterests.find(std::make_pair(region_handle, channel));
    return (it != sInterests.end()) ? it->second : 0;
}

static bool ss_wf_region_claimed(U64 region_handle)
{
    for (S32 ch = 0; ch <= (S32)SSWorldField::EChannel::ACOUSTIC; ++ch)
    {
        if (ss_wf_interest_count(region_handle, ch) > 0) return true;
    }
    return false;
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
// for the camera region and its neighbours, the same reach the rain shadow
// capture serves today.
bool SSWorldField::surfaceTopDemanded() const
{
    static LLCachedControl<bool> demanded(gSavedSettings, "SSWorldFieldSurfaceTop", false);
    return demanded;
}

// One edit fan-out. Settled prim edits land here; the tile's dirty rectangle
// grows to cover the edit, and the re-peel is scissored to it.
void SSWorldField::markDirty(const LLVector3& pos_agent, F32 radius)
{
    SSWorldField* self = getInstance();
    if (!self) return;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return;

    auto it = self->mTiles.find(regionp->getHandle());
    if (it == self->mTiles.end() || !it->second.mValid) return;

    Tile& tile = it->second;
    tile.mLastTouched = self->mNow;

    const F32 x = pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX];
    const F32 y = pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY];
    const F32 r = llmax(radius, tile.mCell);

    const S32 x0 = llclamp((S32)floorf((x - r) / tile.mCell), 0, tile.mRes - 1);
    const S32 y0 = llclamp((S32)floorf((y - r) / tile.mCell), 0, tile.mRes - 1);
    const S32 x1 = llclamp((S32)ceilf((x + r) / tile.mCell), x0 + 1, tile.mRes);
    const S32 y1 = llclamp((S32)ceilf((y + r) / tile.mCell), y0 + 1, tile.mRes);

    if (tile.mDirty)
    {
        tile.mDirtyX0 = llmin(tile.mDirtyX0, x0);
        tile.mDirtyY0 = llmin(tile.mDirtyY0, y0);
        tile.mDirtyX1 = llmax(tile.mDirtyX1, x1);
        tile.mDirtyY1 = llmax(tile.mDirtyY1, y1);
    }
    else
    {
        tile.mDirtyX0 = x0;
        tile.mDirtyY0 = y0;
        tile.mDirtyX1 = x1;
        tile.mDirtyY1 = y1;
        tile.mDirty = true;
    }

// Bands the edit's own altitude could touch. A removed roof drops its whole
    // column, so a rect re-peel sweeps from the band floor up to just past the
    // edit, not only the bands the edit's box overlaps.
    const S32 band = llclamp((S32)((pos_agent.mV[VZ] + r) / tile.mBandHeight), 0, SSWorldField::MAX_BANDS - 1);
    tile.mBandTarget = llmax(tile.mBandTarget, band + 1);
}

void SSWorldField::clear()
{
    // A read in flight still references mTarget's depth texture - let it land
    // first, then tear the target down from the read's completion.
    if (mReadbackPending) { mClearPending = true; return; }
    mTiles.clear();
    mBuild.mActive = false;
    ++mFloodGeneration;     // a flood in flight lands into nothing
    mTarget.release();
}

bool SSWorldField::tileValid(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    return (it != mTiles.end() && it->second.mValid);
}

U32 SSWorldField::geometrySerial(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    return (it != mTiles.end()) ? it->second.mGeomSerial : 0;
}

F32 SSWorldField::bandHeight() const
{
    static LLCachedControl<F32> band(gSavedSettings, "SSWorldFieldBand", 16.f);
    return llclamp((F32)band, 4.f, 64.f);
}

S32 SSWorldField::bandCount() const
{
    static LLCachedControl<F32> ceiling(gSavedSettings, "SSWorldFieldCeiling", 256.f);
    const S32 count = (S32)ceilf(llmax((F32)ceiling, bandHeight()) / bandHeight());
    return llclamp(count, 1, MAX_BANDS);
}

S32 SSWorldField::resolution() const
{
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid) return entry.second.mRes;
    }
    return 0;
}

F64 SSWorldField::tileAge(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return -1.0;
    return mNow - tile->mCaptureTime;
}

S32 SSWorldField::effectiveBands(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    return tile ? tile->mBandCount : 0;
}

const SSWorldField::Tile* SSWorldField::tileAt(const LLVector3& pos_agent) const
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return nullptr;

    auto it = mTiles.find(regionp->getHandle());
    if (it == mTiles.end()) return nullptr;
    return &it->second;
}

SSWorldField::Tile* SSWorldField::tileFor(LLViewerRegion* regionp, bool allow_create)
{
    auto it = mTiles.find(regionp->getHandle());
    if (it != mTiles.end()) return &it->second;
    if (!allow_create) return nullptr;

    static LLCachedControl<F32> cell_setting(gSavedSettings, "SSWorldFieldCell", 2.f);
    const F32 cell = llclamp((F32)cell_setting, 0.5f, 8.f);
    const F32 width = regionp->getWidth();
    const S32 res = llclamp((S32)llround(width / cell), 32, 256);

    Tile& tile = mTiles[regionp->getHandle()];
    tile.mRegionHandle = regionp->getHandle();
    tile.mRes = res;
    tile.mCell = width / (F32)res;
    tile.mBandHeight = bandHeight();
    tile.mBandTop.assign((size_t)MAX_BANDS * res * res, NO_SURFACE);
    tile.mBandFlags.assign((size_t)MAX_BANDS * res * res, 0);
    return &tile;
}

// Whether a tile is worth (re)building: never built, edited, stale, or
// captured under a cell/band setting that has since changed.
bool SSWorldField::needsBuild(const Tile& tile) const
{
    if (!tile.mValid) return true;

    static LLCachedControl<F32> max_age(gSavedSettings, "SSWorldFieldMaxAge", 120.f);
    if (mNow - tile.mCaptureTime > (F64)llmax(5.f, (F32)max_age)) return true;

    if (tile.mDirty && mNow - tile.mCaptureTime > DIRTY_MIN_INTERVAL) return true;

    static LLCachedControl<F32> cell_setting(gSavedSettings, "SSWorldFieldCell", 2.f);
    const F32 cell = llclamp((F32)cell_setting, 0.5f, 8.f);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;
    const S32 res = llclamp((S32)llround(regionp->getWidth() / cell), 32, 256);
    if (res != tile.mRes) return true;

    if (fabsf(tile.mBandHeight - bandHeight()) > 0.01f) return true;

    return false;
}

// Most deserving tile first: the camera's region, then any region within
// neighbour reach that has a claimed channel or serves the demanded surface
// top. Nothing builds while nothing demands anything.
SSWorldField::Tile* SSWorldField::pickBuildTarget()
{
    if (!surfaceTopDemanded() && sInterests.empty()) return nullptr;

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);
    if (cam_region)
    {
        Tile* tile = tileFor(cam_region, true);
        tile->mLastTouched = mNow;
        if (needsBuild(*tile)) return tile;
    }

    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp || regionp == cam_region) continue;
        if (!ss_wf_region_claimed(regionp->getHandle()) && !surfaceTopDemanded()) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const F32 width = regionp->getWidth();
        const F32 dx = llmax(origin.mV[VX] - cam.mV[VX], cam.mV[VX] - (origin.mV[VX] + width), 0.f);
        const F32 dy = llmax(origin.mV[VY] - cam.mV[VY], cam.mV[VY] - (origin.mV[VY] + width), 0.f);
        if (dx * dx + dy * dy > NEIGHBOR_REACH * NEIGHBOR_REACH) continue;

        Tile* tile = tileFor(regionp, true);
        tile->mLastTouched = mNow;
        if (needsBuild(*tile)) return tile;
    }

    return nullptr;
}

// Per-frame drive: evict departed regions, keep stepping the live build one
// band at a time, and begin a new build when a tile deserves one.
void SSWorldField::update()
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD);

    mNow = SSAtmoMagic::getInstance()->sharedTime();

    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldField", true);
    if (!enabled || !SSAtmoMagic::getInstance()->hasWeather())
    {
        if (!mTiles.empty() || mBuild.mActive) clear();
        return;
    }

    evict();

// Catch-up for the connectivity labels: a tile that committed while a flood
    // was in flight was skipped rather than queued; this is where it gets its
    // turn - one tile per call. Oldest-first is unnecessary at this scale
    // (four tiles).
    if (!mFloodBusy)
    {
        for (auto& entry : mTiles)
        {
            Tile& tile = entry.second;
            if (tile.mValid && tile.mAirSerial != tile.mGeomSerial && !tile.mDirty)
            {
                scheduleFlood(tile);
                break;
            }
        }
    }

    if (mBuild.mActive)
    {
        if (mNow - mLastBandAt < BAND_MIN_INTERVAL) return;
        mLastBandAt = mNow;

        LLTimer band_timer;
        advanceBuild();
        mLastCaptureMS = band_timer.getElapsedTimeF32() * 1000.f;
        return;
    }

    Tile* target = pickBuildTarget();
    if (!target) return;

// Begin a build. A dirty tile re-peels only its dirty rectangle's frustum,
    // but still sweeps every band from the floor to the highest band the edits
    // could touch - a removed roof drops its whole column, so band scoping below
    // the edit is not safe.
    mBuild.mActive = true;
    mBuild.mRegionHandle = target->mRegionHandle;
    mBuild.mBand = 0;
    mBuild.mEmptyRun = 0;
    mBuild.mChanged = false;
    mBuild.mRectOnly = target->mDirty;

    if (mBuild.mRectOnly)
    {
        mBuild.mRectX0 = target->mDirtyX0;
        mBuild.mRectY0 = target->mDirtyY0;
        mBuild.mRectX1 = target->mDirtyX1;
        mBuild.mRectY1 = target->mDirtyY1;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(target->mRegionHandle);
        const LLVector3 origin = regionp ? regionp->getOriginAgent() : LLVector3::zero;

        mBuild.mRectCentre.setVec(origin.mV[VX] + 0.5f * (F32)(mBuild.mRectX0 + mBuild.mRectX1) * target->mCell,
                                  origin.mV[VY] + 0.5f * (F32)(mBuild.mRectY0 + mBuild.mRectY1) * target->mCell,
                                  0.f);

// Rect capture resources: a square frustum covering the rect's wider axis,
        // with the short side's extra texels spilling outside the rect and skipped
        // at splice time.
        const S32 rw = mBuild.mRectX1 - mBuild.mRectX0;
        const S32 rh = mBuild.mRectY1 - mBuild.mRectY0;
        mBuild.mRectRes = llclamp(llmax(rw, rh), 4, target->mRes);
        mBuild.mRectHalf = 0.5f * (F32)mBuild.mRectRes * target->mCell;
    }
    else
    {
        target->mBandTarget = bandCount();
    }

    target->mBandTarget = llmax(llmax(target->mBandTarget, target->mBandCount), 1);
    mBuild.mBand = 0;
    mBuild.mEmptyRun = 0;
    mBuild.mChanged = false;
    mLastBandAt = mNow;
}

// Drops tiles for departed regions, then the least recently used beyond the
// cache cap. Erasing a tile also moves the flood generation - a walk in
// flight for the departed tile must not land on a fresh tile the same region
// handle re-created (restarted at geometry serial 1, so the serial gate alone
// would not stop it) - and drops its cached debug views.
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
    while ((S32)mTiles.size() > MAX_TILES)
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
    if (erased) ++mFloodGeneration;
}

// One band step: capture, splice, then advance, stop early on empty sky, or
// commit. The depth readback is async (SSGLReadback): capture renders and
// submits, applyBand runs a step after the texels land, and the build waits a
// step while one is in flight.
bool SSWorldField::advanceBuild()
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(mBuild.mRegionHandle);
    Tile* tile = regionp ? tileFor(regionp, false) : nullptr;
    if (!tile)
    {
        mBuild.mActive = false;
        return false;
    }

    // A band readback is still in flight: neither splice it (the texels are
    // not here yet) nor render the shared capture target into again.
    if (mReadbackPending) return true;

    // The previous band's capture rendered and its readback landed; splice it
    // in and advance the build state that depends on it.
    if (mBuild.mJustCaptured)
    {
        mBuild.mJustCaptured = false;
        applyBand(*tile);
        ++mBuild.mBand;

// Full builds stop early once the sky has been genuinely empty for a few
        // consecutive bands; rect builds run to their target so the spliced columns
        // stay consistent with their neighbours.
        if (!mBuild.mRectOnly && mBuild.mEmptyRun >= EMPTY_BANDS_TO_STOP)
        {
            commitBuild(*tile);
            return false;
        }

        if (mBuild.mBand >= tile->mBandTarget)
        {
            commitBuild(*tile);
            return false;
        }

        return true;
    }

    if (mBuild.mBand >= tile->mBandTarget)
    {
        commitBuild(*tile);
        return false;
    }

    if (!captureBand(*tile))
    {
        // GL trouble - abandon rather than spin. The tile keeps its previous
        // contents and stays dirty, so the next update tries again.
        mBuild.mActive = false;
        return false;
    }

    mBuild.mJustCaptured = true;
    return true;
}

// One band capture: an ortho straight-down depth render whose frustum starts
// at the band's top, so everything above the band is behind the near plane
// and the readback is the highest surface *inside the band*.
bool SSWorldField::captureBand(Tile& tile)
{
    LL_PROFILE_GPU_ZONE("atmo world field band");

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp) return false;

    const F32 band_top = bandTopZ(mBuild.mBand, tile.mBandHeight);
    const F32 range = tile.mBandHeight + 2.f;

    S32 res;
    F32 half, centre_x, centre_y;
    if (mBuild.mRectOnly)
    {
        res = mBuild.mRectRes;
        half = mBuild.mRectHalf;
        centre_x = mBuild.mRectCentre.mV[VX];
        centre_y = mBuild.mRectCentre.mV[VY];
    }
    else
    {
        res = tile.mRes;
        half = regionp->getWidth() * 0.5f + 8.f;
        centre_x = regionp->getOriginAgent().mV[VX] + regionp->getWidth() * 0.5f;
        centre_y = regionp->getOriginAgent().mV[VY] + regionp->getWidth() * 0.5f;
    }

    const LLVector3 eye(centre_x, centre_y, band_top);

    const glm::mat4 saved_view = get_current_modelview();
    const glm::mat4 saved_proj = get_current_projection();
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;

    const glm::mat4 view = glm::lookAt(
        glm::vec3(eye.mV[VX], eye.mV[VY], eye.mV[VZ]),
        glm::vec3(eye.mV[VX], eye.mV[VY], eye.mV[VZ] - 1.f),
        glm::vec3(0.f, 1.f, 0.f));
    const glm::mat4 proj = glm::ortho(-half, half, -half, half, 0.f, range);

    set_current_modelview(view);
    set_current_projection(proj);
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_SUN_SHADOW3;

    LLCamera cam = *LLViewerCamera::getInstance();
    cam.setOrigin(eye);
    cam.setFar(range);

    LLVector3 frust[8];
    frust[0] = eye + LLVector3(-half, -half, 0.f);
    frust[1] = eye + LLVector3(half, -half, 0.f);
    frust[2] = eye + LLVector3(half, half, 0.f);
    frust[3] = eye + LLVector3(-half, half, 0.f);
    for (U32 i = 0; i < 4; i++)
    {
        frust[i + 4] = frust[i] + LLVector3(0.f, 0.f, -range);
    }
    cam.calcAgentFrustumPlanes(frust);
    cam.mFrustumCornerDist = 0.f;

    bool ok = true;
    if (mTarget.getWidth() != (U32)res)
    {
        mTarget.release();
        ok = mTarget.allocate(res, res, 0, true);
    }
    else
    {
        ok = true;
    }

    if (ok)
    {
        mTarget.bindTarget();
        mTarget.getViewport(gGLViewport);
        mTarget.clear();

        {
            static LLCullResult cull_result;

            gPipeline.pushRenderTypeMask();
            gPipeline.clearRenderTypeMask(LLPipeline::RENDER_TYPE_AVATAR,
                                          LLPipeline::RENDER_TYPE_CONTROL_AV,
                                          LLPipeline::END_RENDER_TYPES);
            gPipeline.renderShadow(view, proj, cam, cull_result, true);
            gPipeline.popRenderTypeMask();
        }

        mTarget.flush();

        // <SS:Nexii> The band's depth lands via the shared SSGLReadback worker: the synchronous glReadPixels that used to block becomes a glGetTexImage on a dedicated GL thread, and applyBand() runs the step after the texels come back (see mJustCaptured). The worker writes only its own buffer; mDone copies into mBuild.mDepth on the main thread, so the Build never sees a partial read.
        mBuild.mDepth.assign((size_t)res * res, 0.f);
        mReadbackPending = true;
        const U32 tres = (U32)res;

        SSGLReadback::Job job;
        job.mTexture = mTarget.getDepth();
        job.mTarget = GL_TEXTURE_2D;
        job.mWidth = tres;
        job.mHeight = tres;
        job.mFormat = GL_DEPTH_COMPONENT;
        job.mType = GL_FLOAT;
        job.mDone = [this, tres](const U8* data, size_t bytes)
        {
            mReadbackPending = false;
            if (mClearPending)
            {
                mClearPending = false;
                clear();
                return;
            }
            const size_t n = (size_t)tres * tres;
            if (bytes >= n * sizeof(F32) && mBuild.mDepth.size() >= n)
            {
                memcpy(mBuild.mDepth.data(), data, n * sizeof(F32));
            }
        };
        if (!SSGLReadback::getInstance()->submit(job))
        {
            // Could not even stage the read - GL trouble. Abandon rather than
            // leave mReadbackPending stuck and the build spinning.
            mReadbackPending = false;
            mBuild.mJustCaptured = false;
            ok = false;
        }
    }

    set_current_modelview(saved_view);
    set_current_projection(saved_proj);
    LLViewerCamera::sCurCameraID = saved_camera;

    return ok;
}

// Splices the captured band into the tile: per column, the highest surface
// inside the band, projected out of the depth readback. Full builds write
// every column; rect builds only the dirty rectangle's columns.
void SSWorldField::applyBand(Tile& tile)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD_GRID);

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
    if (!regionp || mBuild.mDepth.empty()) return;

    const S32 band = mBuild.mBand;
    const F32 band_top = bandTopZ(band, tile.mBandHeight);
    const F32 range = tile.mBandHeight + 2.f;
    const F32 hi = band_top - BOUNDARY_EPSILON;

    const S32 x0 = mBuild.mRectOnly ? mBuild.mRectX0 : 0;
    const S32 y0 = mBuild.mRectOnly ? mBuild.mRectY0 : 0;
    const S32 x1 = mBuild.mRectOnly ? mBuild.mRectX1 : tile.mRes;
    const S32 y1 = mBuild.mRectOnly ? mBuild.mRectY1 : tile.mRes;
    const S32 cap_res = mBuild.mRectOnly ? mBuild.mRectRes : tile.mRes;
    const F32 half = mBuild.mRectOnly ? mBuild.mRectHalf : regionp->getWidth() * 0.5f + 8.f;
    const F32 centre_x = mBuild.mRectOnly ? mBuild.mRectCentre.mV[VX]
                                          : regionp->getOriginAgent().mV[VX] + regionp->getWidth() * 0.5f;
    const F32 centre_y = mBuild.mRectOnly ? mBuild.mRectCentre.mV[VY]
                                          : regionp->getOriginAgent().mV[VY] + regionp->getWidth() * 0.5f;

    const F32 texel = (2.f * half) / (F32)cap_res;
    const F32 frust_min_x = centre_x - half;
    const F32 frust_min_y = centre_y - half;

    const F32 water_z = regionp->getWaterHeight();
    const bool sky = SSAtmoMagic::getInstance()->isSkyTrack();
    const F32 sky_floor = SSAtmoMagic::getInstance()->groundZero();

    const S32 stride = tile.mRes;
    F32* top_z = &tile.mBandTop[(size_t)band * (size_t)tile.mRes * (size_t)tile.mRes];
    U8* top_flags = &tile.mBandFlags[(size_t)band * tile.mRes * tile.mRes];

    U32 hits = 0;

    for (S32 cy = y0; cy < y1; ++cy)
    {
        for (S32 cx = x0; cx < x1; ++cx)
        {
            const F32 wx = regionp->getOriginAgent().mV[VX] + ((F32)cx + 0.5f) * tile.mCell;
            const F32 wy = regionp->getOriginAgent().mV[VY] + ((F32)cy + 0.5f) * tile.mCell;

            const size_t idx = (size_t)cy * stride + cx;

            F32 z = NO_SURFACE;
            U8 flags = 0;

            const F32 u = (wx - frust_min_x) / (2.f * half);
            const F32 v = (wy - frust_min_y) / (2.f * half);
            if (u >= 0.f && u < 1.f && v >= 0.f && v < 1.f)
            {
                const S32 tx = llmin((S32)(u * (F32)cap_res), cap_res - 1);
                const S32 ty = llmin((S32)(v * (F32)cap_res), cap_res - 1);
                const F32 d = mBuild.mDepth[(size_t)ty * cap_res + tx];
                if (d < DEPTH_MISS)
                {
                    z = band_top - d * range;
                    if (z > hi) z = hi;
                    flags = SSRainShadowMap::SURF_MAPPED;
                    ++hits;
                }
            }

            if (z > NO_SURFACE + 1.f)
            {
                // Water is the one surface the depth pass does not draw, so a
                // hit under the waterline is the seabed and the cell belongs
                // to the water above it - the rain shadow rule, at every band.
                if (!sky && z < water_z)
                {
                    z = water_z;
                    flags = SSRainShadowMap::SURF_MAPPED | SSRainShadowMap::SURF_WATER;
                }
            }
            else if (band == 0)
            {
                // The ground band falls back to the heightmap, exactly as the
                // rain shadow capture does for what it missed. Higher bands
                // leave unmapped cells open instead.
                if (sky)
                {
                    z = sky_floor;
                    flags = 0;
                }
                else
                {
                    const LLVector3 probe(wx, wy, water_z);
                    const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(probe);
                    z = llmax(land, water_z);
                    flags = SSRainShadowMap::SURF_FALLBACK
                            | ((water_z > land) ? SSRainShadowMap::SURF_WATER : 0);
                }
            }
            else
            {
                z = NO_SURFACE;
                flags = 0;
            }

            // Splice, and notice when the column actually changed so a
            // no-op edit does not bump the geometry serial.
            if (fabsf(top_z[idx] - z) > 0.01f || top_flags[idx] != flags)
            {
                mBuild.mChanged = true;
            }
            top_z[idx] = z;
            top_flags[idx] = flags;
        }
    }

    // Bands with content extend the tile's live band stack; a run of
    // genuinely empty bands ends a full build early.
    if (hits > 0)
    {
        if (band + 1 > tile.mBandCount) tile.mBandCount = band + 1;
        mBuild.mEmptyRun = 0;
    }
    else
    {
        ++mBuild.mEmptyRun;
    }
}

void SSWorldField::commitBuild(Tile& tile)
{
    if (mBuild.mChanged)
    {
        if (tile.mGeomSerial == 0xFFFFFFFFu)
        {
            tile.mGeomSerial = 1;
        }
        else
        {
            ++tile.mGeomSerial;
        }
    }

    if (!mBuild.mRectOnly)
    {
        ++mCaptureCount;
        tile.mValid = true;
    }
    else
    {
        ++mDirtyCaptures;
    }

    // Rect cleared, target reset, timestamps refreshed. The dirty rect is a
    // one-shot: the re-peel splices exactly what was marked.
    tile.mDirtyX0 = tile.mDirtyY0 = 0;
    tile.mDirtyX1 = tile.mDirtyY1 = 0;
    tile.mDirty = false;
    tile.mBandTarget = 0;
    tile.mCaptureTime = mNow;
    tile.mLastTouched = mNow;
    mBuild.mActive = false;

// The connectivity labels follow every commit, not only the ones that changed
    // something: they also serve their first fill, and a commit that changed
    // nothing left mAirSerial already matching, so scheduleFlood's serial check
    // makes the walk a no-op store.
    if (tile.mAirSerial != tile.mGeomSerial)
    {
        scheduleFlood(tile);
    }
}

// Resolves the landing-surface grid for a region - SSRainShadowMap's exact
// contract, sourced from the band stack, not a private capture. The first
// thing a falling drop meets is the column's highest surface, so bands are
// scanned top-down and the first hit wins.
bool SSWorldField::buildSurfaceGrid(U64 region_handle, S32 n, SSRainShadowMap::SurfaceGrid& out)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_WORLDFIELD_GRID);

    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid) return false;

    const Tile& tile = it->second;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp) return false;

    n = llclamp(n, 16, 512);
    const F32 width = regionp->getWidth();

    out.mRegionHandle = region_handle;
    out.mN = n;
    out.mCell = width / (F32)n;
    out.mGeomSerial = tile.mGeomSerial;
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

            for (S32 b = tile.mBandCount - 1; b >= 0; --b)
            {
                const size_t bi = (size_t)b * (size_t)tile.mRes * (size_t)tile.mRes + col;
                if (tile.mBandTop[bi] > -FLT_MAX * 0.5f)
                {
                    z = tile.mBandTop[bi];
                    flags = tile.mBandFlags[bi];
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

// The TRUE-GROUND reference for the wind profile: per column, the topmost real surface from
// buildSurfaceGrid (olivine/green-blue ground tiles), except tall-structure columns (a building
// or skybox far above the surrounding ground) are voided and gap-filled from the surrounding
// true ground; water columns hold the sea plane. Captured from the real geometry (streets, mesh
// terrain, prim ground) - not the ancient Linden heightmap resolveHeightRegion reads - so prim
// terrain built on the old terrain becomes the ground from which the wind boundary layer is
// measured.
bool SSWorldField::buildTrueGround(U64 region_handle, S32 n, std::vector<F32>& out)
{
    SSRainShadowMap::SurfaceGrid grid;
    if (!buildSurfaceGrid(region_handle, n, grid)) return false;

    n = grid.mN;
    const size_t cells = (size_t)n * n;
    out.assign(cells, 0.f);

    // Columns that are real ground: not a tall structure. Water is ground (the sea plane). A
    // column whose topmost captured surface stands well above the local surrounding ground is a
    // tall structure - voided, filled from its neighbours below. A generous threshold keeps
    // genuine raised mesh/prim ground (a street deck a couple of metres up) as ground while
    // catching buildings and skyboxes.
    static const F32 SS_TALL_STRUCTURE_M = 12.f;
    std::vector<U8> valid(cells, 0);

    for (S32 gy = 0; gy < n; ++gy)
    {
        for (S32 gx = 0; gx < n; ++gx)
        {
            const size_t i = (size_t)gy * n + gx;
            const F32 z = grid.mZ[i];
            const U8 f = grid.mFlags[i];
            if (f == 0) continue;                 // unmapped - leave for gap fill
            if (f & SSRainShadowMap::SURF_WATER)  // the sea plane is ground
            {
                out[i] = z;
                valid[i] = 1;
                continue;
            }
            if (grid.above(i) <= SS_TALL_STRUCTURE_M)  // at or near the local ground
            {
                out[i] = z;
                valid[i] = 1;
            }
            // else: tall structure - voided, filled below
        }
    }

// Gap-fill the voided (tall-structure) columns from the surrounding true ground, in rings
    // outward, so a building column reads its neighbours' street/mesh ground. Each pass fills
    // every still-void column that has a valid neighbour, taking that neighbour's ground.
    bool progressed = true;
    while (progressed)
    {
        progressed = false;
        for (S32 gy = 0; gy < n; ++gy)
        {
            for (S32 gx = 0; gx < n; ++gx)
            {
                const size_t i = (size_t)gy * n + gx;
                if (valid[i]) continue;

                F32 sum = 0.f;
                S32 cnt = 0;
                for (S32 dy = -1; dy <= 1; ++dy)
                {
                    for (S32 dx = -1; dx <= 1; ++dx)
                    {
                        if (dx == 0 && dy == 0) continue;
                        const S32 nx = gx + dx, ny = gy + dy;
                        if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;
                        const size_t ni = (size_t)ny * n + nx;
                        if (valid[ni])
                        {
                            sum += out[ni];
                            ++cnt;
                        }
                    }
                }
                if (cnt > 0)
                {
                    out[i] = sum / (F32)cnt;
                    valid[i] = 1;
                    progressed = true;
                }
            }
        }
    }

// A column with no valid neighbour anywhere (fully enclosed, or a field of unmapped) keeps
    // its buildSurfaceGrid value - the heightmap/water fallback it already carries.
    for (size_t i = 0; i < cells; ++i)
    {
        if (!valid[i]) out[i] = grid.mZ[i];
    }
    return true;
}

void SSWorldField::validTiles(std::vector<std::pair<U64, U32> >& out) const
{
    out.clear();
    out.reserve(mTiles.size());
    for (const auto& entry : mTiles)
    {
        if (entry.second.mValid)
        {
            out.emplace_back(entry.first, entry.second.mGeomSerial);
        }
    }
}

bool SSWorldField::surfaceTop(const LLVector3& pos_agent, F32& z, U8& flags) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);

    const size_t col = (size_t)cy * tile->mRes + cx;
    for (S32 b = tile->mBandCount - 1; b >= 0; --b)
    {
        const size_t bi = (size_t)b * (size_t)tile->mRes * (size_t)tile->mRes + col;
        if (tile->mBandTop[bi] > -FLT_MAX * 0.5f)
        {
            z = tile->mBandTop[bi];
            flags = tile->mBandFlags[bi];
            return true;
        }
    }
    return false;
}

bool SSWorldField::coverageAt(const LLVector3& pos_agent, bool& outdoor, F32& buried_depth) const
{
    outdoor = true;
    buried_depth = 0.f;

    F32 top = 0.f;
    U8 flags = 0;
    if (!surfaceTop(pos_agent, top, flags)) return false;

    // Standing on the top surface counts as outdoors; anything below it is
    // under the column's sky-open top by however much.
    if (top < pos_agent.mV[VZ] - 0.01f)
    {
        return true;
    }

    outdoor = false;
    buried_depth = llmax(0.f, top - pos_agent.mV[VZ]);
    return true;
}

bool SSWorldField::coverageDetail(const LLVector3& pos_agent, bool& covered,
                                  F32& ceiling_z, F32& column_top_z) const
{
    covered = false;
    ceiling_z = 0.f;
    column_top_z = 0.f;

    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const size_t col = (size_t)cy * tile->mRes + cx;

    // The half metre of grace keeps the surface being stood on from reading
    // as its own ceiling - a capture texel of the floor under the camera can
    // land a hair above the camera's own feet.
    const F32 over = pos_agent.mV[VZ] + 0.5f;

    bool any = false;
    for (S32 b = 0; b < tile->mBandCount; ++b)
    {
        const size_t bi = (size_t)b * (size_t)tile->mRes * (size_t)tile->mRes + col;
        const F32 z = tile->mBandTop[bi];
        if (z <= -FLT_MAX * 0.5f) continue;

        any = true;
        column_top_z = llmax(column_top_z, z);
        if (z > over && (!covered || z < ceiling_z))
        {
            covered = true;
            ceiling_z = z;
        }
    }

    return any;
}

// <SS:Nexii> Air connectivity lookup: the band the point stands in, read from the labels the flood stored, or AIR_UNKNOWN when nothing is current - after an edit, before the first flood, or off-tile.
U8 SSWorldField::airLabelAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return AIR_UNKNOWN;
    if (tile->mAirLabel.empty() || tile->mAirSerial != tile->mGeomSerial) return AIR_UNKNOWN;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return AIR_UNKNOWN;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const S32 band = llclamp((S32)(pos_agent.mV[VZ] / tile->mBandHeight), 0, tile->mBandCount - 1);

    const size_t bi = ((size_t)band * tile->mRes + cy) * tile->mRes + cx;
    return (bi < tile->mAirLabel.size()) ? tile->mAirLabel[bi] : (U8)AIR_UNKNOWN;
}

// Occlusion depth behind airLabelAt: the flood's distance walk per band-cell,
// gated by the same serial check so a stale walk is never served.
U32 SSWorldField::airDepthAt(const LLVector3& pos_agent) const
{
    const Tile* tile = tileAt(pos_agent);
    if (!tile || !tile->mValid) return AIR_DEPTH_UNREACHED;
    if (tile->mAirDepth.empty() || tile->mAirSerial != tile->mGeomSerial) return AIR_DEPTH_UNREACHED;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return AIR_DEPTH_UNREACHED;

    const S32 cx = llclamp((S32)((pos_agent.mV[VX] - regionp->getOriginAgent().mV[VX]) / tile->mCell), 0, tile->mRes - 1);
    const S32 cy = llclamp((S32)((pos_agent.mV[VY] - regionp->getOriginAgent().mV[VY]) / tile->mCell), 0, tile->mRes - 1);
    const S32 band = llclamp((S32)(pos_agent.mV[VZ] / tile->mBandHeight), 0, tile->mBandCount - 1);

    const size_t bi = ((size_t)band * tile->mRes + cy) * tile->mRes + cx;
    return (bi < tile->mAirDepth.size()) ? (U32)tile->mAirDepth[bi] : AIR_DEPTH_UNREACHED;
}

// Share of a tile's band-cells carrying a current label - 1.0 once the first
// flood has landed and nothing has edited since. The overlay reads this to
// tell a settled field from one still catching up.
F32 SSWorldField::airCoverage(U64 region_handle) const
{
    auto it = mTiles.find(region_handle);
    if (it == mTiles.end() || !it->second.mValid) return 0.f;

    const Tile& tile = it->second;
    if (tile.mAirLabel.empty() || tile.mAirSerial != tile.mGeomSerial) return 0.f;
    if (tile.mBandCount < 1 || tile.mRes < 1) return 0.f;

    const size_t cells = (size_t)tile.mBandCount * (size_t)tile.mRes * (size_t)tile.mRes;
    if (tile.mAirLabel.size() < cells) return 0.f;

    size_t labelled = 0;
    for (size_t i = 0; i < cells; ++i)
    {
        const U8 l = tile.mAirLabel[i];
        if (l != AIR_UNKNOWN) ++labelled;
    }
    return (F32)labelled / (F32)cells;
}

// The DRAINAGE_NETWORK core over one landing surface. Barnes' priority flood
// is the O(n log n) way to fill every depression to its spill elevation:
// drains (the grid border, water, unmapped sky) seed the heap at their own
// height, each cell pops once at the lowest spill reaching it, and a cell
// whose spill stands meaningfully above its own surface is standing water.
// Flow directions then run down the FILLED surface, so a pool's water heads
// for its outlet instead of into its own floor.
bool SSWorldField::buildDrainage(const SSRainShadowMap::SurfaceGrid& grid, Drainage& out) const
{
    out.mSpill.clear();
    out.mPool.clear();
    out.mD8.clear();

    const S32 n = grid.mN;
    if (n < 3 || grid.mZ.size() < (size_t)n * n) return false;

    const size_t count = (size_t)n * n;
    out.mSpill.assign(count, -FLT_MAX);
    out.mPool.assign(count, 0);
    out.mD8.assign(count, 4);

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
    // nothing is lower - a pool floor, a sink, or a drain cell.
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

        S32 best_dir = 4;
        F32 best_z = here;
        for (S32 d = 0; d < 8; ++d)
        {
            const S32 nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue;

            const size_t ni = (size_t)ny * n + nx;
            if (!land(ni)) continue;

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
        out.mD8[i] = (U8)best_dir;
    }

    return true;
}

// The flood itself, run on the general worker queue against a snapshot. A
// band-cell is solid where the capture found a surface in that band; air
// otherwise. Every air cell in the top band, and every air cell on the
// horizontal border, starts OUTSIDE; the flood walks 6-connected through air.
// Whatever air is left is INTERIOR - a room, a sealed box - exactly what the
// wind solve wants to skip and the soundscape wants to know it is standing in.
// Evidence-only in the same spirit as the probe carve: a passage narrower
// than a cell stays uncounted rather than invented. The second walk measures
// occlusion: every outside-connected air cell's graph distance in cells to
// the nearest frontier cell, the "how enclosed is this air" figure the
// acoustic channel's travel times and sparse air solve both read. Interior
// cells never reach the frontier through air, so they keep
// AIR_DEPTH_UNREACHED - sealed is maximally enclosed by construction.
static void ss_wf_flood(S32 res, S32 bands, const std::vector<F32>& band_top,
                        std::vector<U8>& label, std::vector<U16>& depth)
{
    const size_t layer = (size_t)res * res;
    const size_t cells = layer * (size_t)bands;
    label.assign(cells, SSWorldField::AIR_INTERIOR);
    depth.assign(cells, (U16)SSWorldField::AIR_DEPTH_UNREACHED);

    std::vector<S32> queue;
    queue.reserve(cells / 8);

    auto isAir = [&](size_t i) { return band_top[i] <= -FLT_MAX * 0.5f; };

    for (size_t i = 0; i < cells; ++i)
    {
        if (!isAir(i))
        {
            label[i] = SSWorldField::AIR_SOLID;
            continue;
        }

        const S32 b = (S32)(i / layer);
        const S32 y = (S32)((i % layer) / res);
        const S32 x = (S32)(i % res);
        if (b == bands - 1 || x == 0 || y == 0 || x == res - 1 || y == res - 1)
        {
            label[i] = SSWorldField::AIR_OUTSIDE;
            queue.push_back((S32)i);
        }
    }

    for (size_t head = 0; head < queue.size(); ++head)
    {
        const S32 i = queue[head];
        const S32 b = (S32)((size_t)i / layer);
        const S32 y = (S32)(((size_t)i % layer) / res);
        const S32 x = (S32)((size_t)i % res);

        static const S32 DX[6] = { 1, -1, 0, 0, 0, 0 };
        static const S32 DY[6] = { 0, 0, 1, -1, 0, 0 };
        static const S32 DB[6] = { 0, 0, 0, 0, 1, -1 };

        for (S32 d = 0; d < 6; ++d)
        {
            const S32 nx = x + DX[d], ny = y + DY[d], nb = b + DB[d];
            if (nx < 0 || ny < 0 || nb < 0 || nx >= res || ny >= res || nb >= bands) continue;

            const size_t j = ((size_t)nb * res + ny) * (size_t)res + nx;
            if (label[j] != SSWorldField::AIR_INTERIOR) continue;

            label[j] = SSWorldField::AIR_OUTSIDE;
            queue.push_back((S32)j);
        }
    }

    // Occlusion depth, one BFS from the whole outside frontier over air. The
    // outside set was found by walking 6-connected air from that same frontier,
    // so this reaches exactly it; interior air is unreachable and stays marked.
// Distances saturate at AIR_DEPTH_UNREACHED - 1: the sentinel must stay
// exclusive to "unvisited", or a cell whose true distance hit 0xFFFF would
// read as never visited and the walk would loop on it forever.
    {
        std::vector<S32> depth_q;
        depth_q.reserve(queue.size());
        for (const S32 i : queue)
        {
            depth[i] = 0;
            depth_q.push_back(i);
        }

        for (size_t head = 0; head < depth_q.size(); ++head)
        {
            const S32 i = depth_q[head];
            const S32 b = (S32)((size_t)i / layer);
            const S32 y = (S32)(((size_t)i % layer) / res);
            const S32 x = (S32)((size_t)i % res);

            static const S32 DX[6] = { 1, -1, 0, 0, 0, 0 };
            static const S32 DY[6] = { 0, 0, 1, -1, 0, 0 };
            static const S32 DB[6] = { 0, 0, 0, 0, 1, -1 };

            for (S32 d = 0; d < 6; ++d)
            {
                const S32 nx = x + DX[d], ny = y + DY[d], nb = b + DB[d];
                if (nx < 0 || ny < 0 || nb < 0 || nx >= res || ny >= res || nb >= bands) continue;

                const size_t j = ((size_t)nb * res + ny) * (size_t)res + nx;
                if (label[j] != SSWorldField::AIR_OUTSIDE) continue;
                if (depth[j] != SSWorldField::AIR_DEPTH_UNREACHED) continue;

                depth[j] = (U16)llmin((U32)depth[i] + 1u,
                                      SSWorldField::AIR_DEPTH_UNREACHED - 1u);
                depth_q.push_back((S32)j);
            }
        }
    }
}

void SSWorldField::scheduleFlood(Tile& tile)
{
    if (mFloodBusy) return;                     // this commit's successor will reschedule
    if (tile.mBandCount < 1 || tile.mRes < 1) return;

    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    LL::WorkQueue::ptr_t main = LL::WorkQueue::getInstance("mainloop");
    if (!general || !main) return;              // no worker: labels stay AIR_UNKNOWN, consumers cope

    mFloodBusy = true;
    const U32 generation = mFloodGeneration;
    const U64 region = tile.mRegionHandle;
    const U32 serial = tile.mGeomSerial;
    const S32 res = tile.mRes;
    const S32 bands = tile.mBandCount;

    // Snapshot: the walk must never read the live band stack, which the next
    // build splices on the main thread while the worker is mid-flood.
    auto snapshot = std::make_shared<std::vector<F32> >(
        tile.mBandTop.begin(),
        tile.mBandTop.begin() + (size_t)bands * res * res);
    auto labels = std::make_shared<std::vector<U8> >();
    auto depths = std::make_shared<std::vector<U16> >();

    main->postTo(
        general,
        [res, bands, snapshot, labels, depths]()
        {
            ss_wf_flood(res, bands, *snapshot, *labels, *depths);
            return true;
        },
        [this, generation, region, serial, labels, depths](bool)
        {
            mFloodBusy = false;
            if (generation != mFloodGeneration) return;

            auto it = mTiles.find(region);
            if (it == mTiles.end() || !it->second.mValid) return;
            if (it->second.mGeomSerial != serial) return;   // edited mid-walk; the next commit refloods

            it->second.mAirLabel = std::move(*labels);
            it->second.mAirDepth = std::move(*depths);
            it->second.mAirSerial = serial;
        });
}

// A band's surface hue for the overlay: the store's vertical resolution
// reads as colour - blue at the floor through green to ember red at the
// ceiling.
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

// The world field's own overlay: what the capture resolved, what the air flood
// decided with its occlusion depth, and what the drainage pass reads - view
// picked by SSWorldFieldDebugView, distance-thinned like the wind flowmap's.
void SSWorldField::renderDebug()
{
    static LLCachedControl<U32> view(gSavedSettings, "SSWorldFieldDebugView", 1);
    const S32 which = llclamp((S32)view, 1, 4);
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

    auto strideFor = [&](F32 wx, F32 wy) -> S32
    {
        const F32 away = llmax(fabsf(wx - cam.mV[VX]), fabsf(wy - cam.mV[VY]));
        return (away < full) ? 1 : (away < full * 2.f) ? 2 : 4;
    };

    gGL.begin(LLRender::LINES);

    for (const auto& entry : mTiles)
    {
        const Tile& tile = entry.second;
        if (!tile.mValid) continue;

        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(tile.mRegionHandle);
        if (!regionp) continue;

        const LLVector3 origin = regionp->getOriginAgent();
        const F32 cell = tile.mCell;
        const F32 h = tile.mBandHeight;

        // The tile's stack footprint, so a region nobody has captured yet reads
        // as an empty box rather than as nothing at all.
        {
            const F32 x1 = origin.mV[VX] + (F32)tile.mRes * cell;
            const F32 y1 = origin.mV[VY] + (F32)tile.mRes * cell;
            const F32 z0 = 0.f;
            const F32 z1 = (F32)llmax(tile.mBandCount, 1) * h;
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
// Every band the capture gave a surface, standing at the altitude the store
            // holds it at, hue by band so stacked storeys read separately instead of
            // fusing into one roof.
            const S32 b_last = llmax(tile.mBandCount - 1, 1);
            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    for (S32 b = 0; b < tile.mBandCount; ++b)
                    {
                        const size_t bi = ((size_t)b * tile.mRes + y) * (size_t)tile.mRes + x;
                        const F32 z = tile.mBandTop[bi];
                        if (z <= -FLT_MAX * 0.5f) continue;

                        mark(LLVector3(wx, wy, z),
                             ss_wf_band_hue((F32)b / (F32)b_last, 0.85f), cell * 0.4f);
                    }
                }
            }
        }
        else if (which == 2)
        {
            // The air flood: outside-connected air green and fading with
            // occlusion depth, sealed interior air red. Solid cells draw
            // nothing - the surfaces view shows those.
            const bool current = !tile.mAirLabel.empty() && tile.mAirSerial == tile.mGeomSerial;
            if (!current) continue;

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    for (S32 b = 0; b < tile.mBandCount; ++b)
                    {
                        const size_t bi = ((size_t)b * tile.mRes + y) * (size_t)tile.mRes + x;
                        const U8 lab = tile.mAirLabel[bi];
                        if (lab == AIR_SOLID || lab == AIR_UNKNOWN) continue;

                        const F32 z = ((F32)b + 0.5f) * h;
                        if (lab == AIR_OUTSIDE)
                        {
                            const U16 d = tile.mAirDepth[bi];
                            const F32 a = llmax(0.9f / (1.f + (F32)d * 0.25f), 0.08f);
                            mark(LLVector3(wx, wy, z), LLColor4(0.3f, 1.f, 0.4f, a), cell * 0.4f);
                        }
                        else
                        {
                            mark(LLVector3(wx, wy, z), LLColor4(1.f, 0.25f, 0.25f, 0.85f), cell * 0.4f);
                        }
                    }
                }
            }
        }
        else if (which == 4)
        {
// TRUE GROUND: the wind profile's reference ground per column - the topmost real
            // surface (olivine/green-blue ground tiles), tall-structure columns voided and
            // filled from their neighbours, water held at the sea plane. Hue rises with the
            // ground's height, so a raised mesh/prim street deck reads distinct from the plain.
            std::vector<F32> ground;
            if (!buildTrueGround(tile.mRegionHandle, tile.mRes, ground)) continue;

            F32 g_lo = ground[0], g_hi = ground[0];
            for (F32 g : ground)
            {
                g_lo = llmin(g_lo, g);
                g_hi = llmax(g_hi, g);
            }
            const F32 g_span = llmax(g_hi - g_lo, 1.f);

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t i = (size_t)y * tile.mRes + x;
                    const F32 z = ground[i];
                    mark(LLVector3(wx, wy, z),
                         ss_wf_band_hue((z - g_lo) / g_span, 0.9f), cell * 0.5f);
                }
            }
        }
        else if (which == 3)
        {
            // Drainage topology at the tile's own resolution: standing water
            // blue, and an arrow per cell down the filled surface's D8 - the
            // outlet chain a pool's water will actually follow.
            auto& cached = sDrainDebug[tile.mRegionHandle];
            if (cached.mSerial != tile.mGeomSerial || cached.mN != tile.mRes)
            {
                cached.mGrid = SSRainShadowMap::SurfaceGrid();
                cached.mDrain = Drainage();
                cached.mN = tile.mRes;
                cached.mSerial = tile.mGeomSerial;
                if (!buildSurfaceGrid(tile.mRegionHandle, tile.mRes, cached.mGrid)
                    || !buildDrainage(cached.mGrid, cached.mDrain))
                {
                    cached.mSerial = 0;
                    continue;
                }
            }
            const SSRainShadowMap::SurfaceGrid& grid = cached.mGrid;
            const Drainage& drain = cached.mDrain;

            for (S32 y = 0; y < tile.mRes; ++y)
            {
                const F32 wy = origin.mV[VY] + ((F32)y + 0.5f) * cell;
                for (S32 x = 0; x < tile.mRes; ++x)
                {
                    const F32 wx = origin.mV[VX] + ((F32)x + 0.5f) * cell;
                    const S32 step = strideFor(wx, wy);
                    if ((x % step) || (y % step)) continue;

                    const size_t i = (size_t)y * tile.mRes + x;
                    const U8 f = grid.mFlags[i];
                    if (f == 0 || (f & SSRainShadowMap::SURF_WATER)) continue;

                    const F32 z = grid.mZ[i];
                    if (drain.mPool[i])
                    {
                        mark(LLVector3(wx, wy, z), LLColor4(0.2f, 0.5f, 1.f, 0.9f), cell * 0.45f);
                    }
                    else if (drain.mD8[i] != 4)
                    {
                        const S32 di = drain.mD8[i];
                        const F32 len = cell * 0.7f;
                        const F32 dx = (F32)((di % 3) - 1) * len;
                        const F32 dy = (F32)((di / 3) - 1) * len;
                        gGL.color4f(0.7f, 0.85f, 1.f, 0.55f);
                        gGL.vertex3f(wx, wy, z + 0.4f);
                        gGL.vertex3f(wx + dx, wy + dy, z + 0.4f);
                    }
                }
            }
        }
    }

    gGL.end();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    // Drop debug views for regions the field no longer holds.
    for (auto it = sDrainDebug.begin(); it != sDrainDebug.end();)
    {
        it = mTiles.count(it->first) ? std::next(it) : sDrainDebug.erase(it);
    }
}

