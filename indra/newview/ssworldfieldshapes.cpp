/**
 * @file ssworldfieldshapes.cpp
 * @brief See ssworldfieldshapes.h.
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

#include "ssworldfieldshapes.h"
#include "ssnavmesh.h"

#include "indra_constants.h"
#include "llfasttimer.h"
#include "llframetimer.h"
#include "llgl.h"
#include "lldrawable.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llprimitive.h"
#include "llphysicsshapebuilderutil.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llsurface.h"
#include "lltimer.h"
#include "llvector4a.h"
#include "llvolumemgr.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvovolume.h"
#include "llworld.h"
#include "lltextureentry.h"
#include "object_flags.h"
#include "ssrocledger.h"
#include "llpathfindinglinkset.h"
#include "llpathfindingmanager.h"
#include "llpathfindingobjectlist.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

// <SS:Nexii> The census envelope and rebuild policy (doc/atmo_magic_worldfield_competition.md 7.1):
// one snapshot around the query anchor, rebuilt when it rots - anchor drift, age, or a debounced
// edit fan-out - never a persistent mirror. The bucket grid is the broadphase; 64 m keeps a
// megaprim a handful of cells, not per-column work.
static constexpr F32 SS_SHAPES_BUCKET_M = 64.f;
static constexpr F64 SS_SHAPES_MAX_AGE = 10.0;
static constexpr F64 SS_SHAPES_REBUILD_AGE = 8.0;       // start the periodic rebuild before the census expires: it now spans frames
static constexpr F64 SS_SHAPES_PART_CACHE_TTL = 60.0;   // seconds a part's baked records outlive its last sighting
static constexpr S32 SS_SHAPES_SCAN_CHUNK = 32;         // objects between budget checks
static constexpr F32 SS_SHAPES_REBUILD_MOVE = 48.f;
static constexpr F64 SS_SHAPES_DIRTY_DEBOUNCE = 2.0;
static constexpr S32 SS_SHAPES_PART_TRIS = 8192;
static constexpr S32 SS_SHAPES_PART_TRIS_LARGE = 65536;
static constexpr S32 SS_SHAPES_TOTAL_TRIS = 6000000;    // baked world-space soups; the navmesh envelope reaches whole regions now
static constexpr F32 SS_SHAPES_LARGE_PART_M = 16.f;
static constexpr F32 SS_SHAPES_TERRAIN_STEP_M = 4.f;
static constexpr S32 SS_SHAPES_TERRAIN_SAMPLES = 96;
static constexpr S32 SS_SHAPES_DDA_GUARD = 512;
// The DYNAMIC classifier's settle window and slop - the settleEdits pattern's values.
static constexpr F64 SS_SHAPES_SETTLE_SECONDS = 4.0;
static constexpr F32 SS_SHAPES_REST_SLOP = 0.25f;
static constexpr S32 SS_SHAPES_DEBUG_TRIS = 512;

static LLTrace::BlockTimerStatHandle FTM_SS_SHAPES_CENSUS("SS Shapes Census");

// The overlay's 12 AABB edges, shared by the classes that draw their bounds.
static void ss_debug_aabb(const LLVector3& mn, const LLVector3& mx)
{
    gGL.vertex3fv(mn.mV); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mn.mV[VZ]);
    gGL.vertex3fv(mn.mV); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mn.mV[VZ]);
    gGL.vertex3fv(mn.mV); gGL.vertex3f(mn.mV[VX], mn.mV[VY], mx.mV[VZ]);
    gGL.vertex3fv(mx.mV); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mx.mV[VZ]);
    gGL.vertex3fv(mx.mV); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mx.mV[VZ]);
    gGL.vertex3fv(mx.mV); gGL.vertex3f(mx.mV[VX], mx.mV[VY], mn.mV[VZ]);
    gGL.vertex3f(mn.mV[VX], mx.mV[VY], mn.mV[VZ]); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mx.mV[VZ]);
    gGL.vertex3f(mn.mV[VX], mn.mV[VY], mx.mV[VZ]); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mx.mV[VZ]);
    gGL.vertex3f(mx.mV[VX], mn.mV[VY], mn.mV[VZ]); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mx.mV[VZ]);
    gGL.vertex3f(mx.mV[VX], mn.mV[VY], mx.mV[VZ]); gGL.vertex3f(mx.mV[VX], mx.mV[VY], mx.mV[VZ]);
    gGL.vertex3f(mx.mV[VX], mx.mV[VY], mn.mV[VZ]); gGL.vertex3f(mn.mV[VX], mx.mV[VY], mn.mV[VZ]);
    gGL.vertex3f(mx.mV[VX], mx.mV[VY], mn.mV[VZ]); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mn.mV[VZ]);
    gGL.vertex3f(mn.mV[VX], mn.mV[VY], mx.mV[VZ]); gGL.vertex3f(mx.mV[VX], mn.mV[VY], mx.mV[VZ]);
}

// Terrain height under an agent-space point: the heightfield of whichever
// region contains the sample - neighbor terrain included, so surrounds and
// border-crossing casts see the real land. Void (between regions) has none.
static bool ss_terrain_z(const LLVector3& pos_agent, F32& out_z)
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;
    LLVector3 region_pos = regionp->getPosRegionFromAgent(pos_agent);
    region_pos.mV[VX] = llclamp(region_pos.mV[VX], 0.f, 255.9f);
    region_pos.mV[VY] = llclamp(region_pos.mV[VY], 0.f, 255.9f);
    out_z = regionp->getLand().resolveHeightRegion(region_pos.mV[VX], region_pos.mV[VY]);
    return true;
}

// <SS:Nexii> Convex hull soups arrive with whatever winding the decomposition produced, and Recast marks a triangle walkable only when its normal faces up - a hull roof wound clockwise is a roof the navmesh never sees. A convex hull has one outside, so every triangle is flipped to face away from the hull's own centroid. Tessellated volumes and authored physics meshes keep their winding. [interaction: SSNavMesh emitRecord]
static void ss_orient_outward(std::vector<LLVector3>& soup, size_t begin)
{
    const size_t end = soup.size() - (soup.size() - begin) % 3;
    if (end <= begin) return;
    LLVector3 centroid;
    for (size_t i = begin; i < end; ++i) centroid += soup[i];
    centroid *= 1.f / (F32)(end - begin);
    for (size_t i = begin; i + 2 < end; i += 3)
    {
        const LLVector3 n = (soup[i + 1] - soup[i]) % (soup[i + 2] - soup[i]);
        const LLVector3 c = (soup[i] + soup[i + 1] + soup[i + 2]) * (1.f / 3.f);
        if (n * (c - centroid) < 0.f) std::swap(soup[i + 1], soup[i + 2]);
    }
}

// Volume-space vertex to a plain vector.
static LLVector3 ss_vert(const LLVector4a& vert)
{
    return LLVector3(vert.getF32ptr());
}

// Fully hidden: every face carries alpha-zero colour, an invisiprim texture, or the
// default transparent texture. A phantom that is invisible everywhere is not part
// of the world at all - no shelter, no block, no record.
static bool ss_part_fully_hidden(LLVOVolume* vov)
{
    const U8 n = vov->getNumTEs();
    if (n <= 0) return false;
    for (U8 i = 0; i < n; ++i)
    {
        const LLTextureEntry* te = vov->getTE(i);
        if (!te) return false;
        if (te->getColor().mV[3] > 0.01f
            && !LLViewerTexture::isInvisiprim(te->getID())
            && te->getID() != IMG_TRANSPARENT)
        {
            return false;
        }
    }
    return true;
}

// World rotation of a part: the drawable's xform-maintained world frame - the
// same one the physics debug renderer draws through (xform.cpp composes
// world = local * parent, which a hand-rolled parent-first walk gets wrong).
static LLQuaternion ss_world_rotation(const LLViewerObject* vobj)
{
    if (vobj->mDrawable)
    {
        return vobj->mDrawable->getWorldRotation();
    }
    LLQuaternion rot = vobj->getRotation();
    const LLViewerObject* cur = vobj;
    while (cur->getParent())
    {
        cur = (LLViewerObject*)cur->getParent();
        rot = rot * cur->getRotation();
    }
    return rot;
}

// <SS:Nexii> The object's world rotation as the sim knows it, without the client-side spin llTargetOmega adds: the viewer keeps getRotation() = sim rotation * mAngularVelocityRot (llviewerobject.cpp, processUpdateMessage and applyAngularVelocity both compose that way), so the spin divides back out per link. Composed parent-first like ss_world_rotation's fallback. [interaction: addPart spinners]
static LLQuaternion ss_world_rotation_unspun(const LLViewerObject* vobj)
{
    LLQuaternion rot = vobj->getRotation() * ~vobj->ssAngularVelocityRot();
    const LLViewerObject* cur = vobj;
    while (cur->getParent())
    {
        cur = (LLViewerObject*)cur->getParent();
        rot = rot * (cur->getRotation() * ~cur->ssAngularVelocityRot());
    }
    return rot;
}

// 64 m bucket key from cell coordinates, biased into unsigned space.
static U64 ss_bucket_key(S32 x, S32 y, S32 z)
{
    return ((U64)(U32)(x + (1 << 20)) << 42)
         | ((U64)(U32)(y + (1 << 20)) << 21)
         | (U64)(U32)(z + (1 << 20));
}

// Segment vs world AABB in segment parameter space [t0, t1] (0..1); tightens both bounds.
static bool ss_ray_aabb(const LLVector3& a, const LLVector3& dir,
                        const LLVector3& bmin, const LLVector3& bmax,
                        F32& t0, F32& t1)
{
    F32 lo = t0, hi = t1;
    for (U32 i = 0; i < 3; ++i)
    {
        const F32 d = dir.mV[i];
        if (fabsf(d) < 1e-9f)
        {
            if (a.mV[i] < bmin.mV[i] || a.mV[i] > bmax.mV[i]) return false;
            continue;
        }
        F32 tn = (bmin.mV[i] - a.mV[i]) / d;
        F32 tf = (bmax.mV[i] - a.mV[i]) / d;
        if (tn > tf) { F32 tmp = tn; tn = tf; tf = tmp; }
        if (tn > lo) lo = tn;
        if (tf < hi) hi = tf;
        if (lo > hi) return false;
    }
    t0 = lo;
    t1 = hi;
    return true;
}

// Segment vs triangle, two-sided Moeller-Trumbore.
static bool ss_ray_tri(const LLVector3& v0, const LLVector3& v1, const LLVector3& v2,
                       const LLVector3& a, const LLVector3& dir,
                       F32 t_min, F32 t_max, F32& out_t, LLVector3& out_n)
{
    const LLVector3 e1 = v1 - v0;
    const LLVector3 e2 = v2 - v0;
    const LLVector3 p = dir % e2;
    const F32 det = e1 * p;
    if (fabsf(det) < 1e-9f) return false;
    const F32 inv = 1.f / det;
    const LLVector3 tv = a - v0;
    const F32 u = (tv * p) * inv;
    if (u < -1e-6f || u > 1.000001f) return false;
    const LLVector3 q = tv % e1;
    const F32 v = (dir * q) * inv;
    if (v < -1e-6f || u + v > 1.000001f) return false;
    const F32 t = (e2 * q) * inv;
    if (t < t_min || t > t_max) return false;
    LLVector3 n = e1 % e2;
    if (n.magVecSquared() < 1e-18f) return false;
    n.normVec();
    if (n * dir > 0.f) n *= -1.f;
    out_t = t;
    out_n = n;
    return true;
}

// One frame of maintenance: gate off drops the census entirely; gate on rebuilds
// a rotted snapshot. Cheap every frame - the scan only runs on rebuild.
void SSWorldFieldShapes::update()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldFieldShapes", false);
    mNow = LLFrameTimer::getTotalSeconds();
    if (!enabled)
    {
        if (!mCensus.mRecords.empty() || mBuilding || !mParts.empty())
        {
            mCensus = Census();
            mPending = Census();
            mParts.clear();
            mCurrentPart = nullptr;
            mBuilding = false;
            mTriCount = 0;
        }
        return;
    }

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(LLViewerCamera::getInstance()->getOrigin());
    if (mBuilding)
    {
        continueBuild(regionp);
    }
    else if (needsRebuild(regionp))
    {
        beginBuild(regionp);
        continueBuild(regionp);
    }
}

// The edit fan-out: record the dirty sphere; the next update past the debounce
// rebuilds if it touches the envelope. Never per-edit work here.
void SSWorldFieldShapes::markDirty(const LLVector3& pos_agent, F32 radius)
{
    SSWorldFieldShapes* self = getInstance();
    self->mDirtyCenter = pos_agent;
    self->mDirtyRadius = radius;
    self->mDirtyAt = LLFrameTimer::getTotalSeconds();
    self->mDirty = true;
}

// Whether the resident snapshot is stale or absent: no region, region switch,
// anchor drift, age, or an accepted dirty sphere inside the envelope.
bool SSWorldFieldShapes::needsRebuild(LLViewerRegion* regionp) const
{
    if (!regionp) return false;
    const F32 env = envelopeRange();
    const LLVector3 anchor = LLViewerCamera::getInstance()->getOrigin();
    if (mCensus.mRegionHandle != regionp->getHandle()) return true;
    if (mRolesChanged) return true;
    if ((anchor - mCensus.mAnchor).magVec() > SS_SHAPES_REBUILD_MOVE) return true;
    if (mNow - mCensus.mBuildTime > SS_SHAPES_REBUILD_AGE) return true;
    if (mDirty && (mNow - mDirtyAt >= SS_SHAPES_DIRTY_DEBOUNCE))
    {
        if ((mDirtyCenter - mCensus.mAnchor).magVec() - mDirtyRadius < env) return true;
    }
    return false;
}

// The envelope radius, widened to the navmesh's while it runs.
F32 SSWorldFieldShapes::envelopeRange()
{
    static LLCachedControl<F32> range(gSavedSettings, "SSWorldFieldShapesRange", 192.f);
    static LLCachedControl<bool> nav_enabled(gSavedSettings, "SSNavMesh", false);
    static LLCachedControl<F32> nav_range(gSavedSettings, "SSNavMeshRange", 512.f);
    F32 env = (F32)range;
    if (nav_enabled) env = llmax(env, (F32)nav_range);
    return llclamp(env, 32.f, 1024.f);
}

// The census scan, time-sliced: beginBuild opens the pending snapshot, continueBuild
// walks the object list under the frame budget, finishBuild swaps it in. Records land
// in object-list order as of each slice; a completed census is internally consistent
// and downstream tie-breaks are deterministic per census, not across rebuilds.
void SSWorldFieldShapes::beginBuild(LLViewerRegion* regionp)
{
    if (!regionp) return;
    mPending = Census();
    mPending.mRegionHandle = regionp->getHandle();
    mPending.mAnchor = LLViewerCamera::getInstance()->getOrigin();
    mPending.mBuildTime = mNow;
    mPendingTris = 0;
    mScanIndex = 0;
    mBuildMS = 0.f;
    mPendRestByFlag = mPendRestByLedger = mPendRestByWatching = 0;
    mPendPhysicsRequested = 0;
    mRolesChanged = false;
    if (mRolesRegion != mPending.mRegionHandle) mRolesInFlight = false;
    mCurrentPart = nullptr;
    mDirty = false;
    mBuilding = true;

    // Rest-state seen flags: false going in, set by trackRest during the scan,
    // and whatever stayed unseen left the envelope or the region - pruned at the swap.
    for (auto& rest : mRest)
    {
        rest.second.mSeen = false;
    }
}

// One budgeted slice of the scan. A region switch mid-scan would mix two agent
// frames, so it abandons the pending snapshot and lets needsRebuild restart.
void SSWorldFieldShapes::continueBuild(LLViewerRegion* regionp)
{
    LL_RECORD_BLOCK_TIME(FTM_SS_SHAPES_CENSUS);
    if (!regionp || regionp->getHandle() != mPending.mRegionHandle)
    {
        mPending = Census();
        mCurrentPart = nullptr;
        mBuilding = false;
        prunePartCache();    // an abort must not leave the cache growing across region hops
        return;
    }
    static LLCachedControl<F32> budget_ms(gSavedSettings, "SSWorldFieldShapesBudgetMS", 3.f);
    const F32 env = envelopeRange();
    const F32 budget = llclamp((F32)budget_ms, 0.5f, 50.f) * 0.001f;
    LLTimer slice;

    S32 n = gObjectList.getNumObjects();
    while (mScanIndex < n)
    {
        for (S32 k = 0; k < SS_SHAPES_SCAN_CHUNK && mScanIndex < n; ++k, ++mScanIndex)
        {
            LLViewerObject* vobj = gObjectList.getObject(mScanIndex);
            if (!vobj || vobj->isDead() || vobj->isOrphaned()) continue;
            // No region filter: the envelope is world-space and owns the census.
            // Neighbor-region parts and border-straddling geometry belong here -
            // sim surrounds and landscape assets are exactly the structure the
            // region-anchored sweep can never see.
            if (vobj->isAvatar() || vobj->isAttachment()) continue;
            const U32 pcode = vobj->getPCode();
            if (pcode == LLViewerObject::LL_VO_WATER || pcode == LLViewerObject::LL_VO_VOID_WATER) continue;
            if (pcode != LL_PCODE_VOLUME) continue;

            LLVOVolume* vov = (LLVOVolume*)vobj;
            if (vov->isFlexible() || vov->isRiggedMesh()) continue;
            if (!vov->getVolume()) continue;

            const LLVector3 pos = vov->getPositionAgent();
            const LLVector3 scale = vov->getScale();
            // Half-diagonal reach: a rotated or huge part (a 1 km surround mesh
            // centred well outside the envelope) must still test against it.
            const F32 approx = 0.5f * scale.magVec() + 1.f;
            if ((pos - mPending.mAnchor).magVec() - approx > env) continue;

            addPart(vov);
        }
        if (slice.getElapsedTimeF32() >= budget)
        {
            mBuildMS += slice.getElapsedTimeF32() * 1000.f;
            return;
        }
        n = gObjectList.getNumObjects();
    }
    mBuildMS += slice.getElapsedTimeF32() * 1000.f;
    finishBuild();
}

// The swap: prune the rest states and cache entries nothing sighted, publish the
// pending snapshot as the census - its stamp moves here and only here.
void SSWorldFieldShapes::finishBuild()
{
    for (auto it = mRest.begin(); it != mRest.end();)
    {
        if (it->second.mSeen) { ++it; } else { it = mRest.erase(it); }
    }
    prunePartCache();
    pruneNavRoles();
    mCurrentPart = nullptr;
    mPending.mBuildTime = mNow;
    mCensus = std::move(mPending);
    mPending = Census();
    mTriCount = mPendingTris;
    mLastBuildMS = mBuildMS;
    mRestByFlag = mPendRestByFlag; mRestByLedger = mPendRestByLedger; mRestByWatching = mPendRestByWatching;
    mPhysicsRequested = mPendPhysicsRequested;
    mBuilding = false;
    if (mRolesWanted) requestNavRoles();
}

// One ObjectNavMeshProperties request for the region answers every flagged linkset at once; a minute between
// requests so a region full of unanswerable roots (no capability) costs one call a minute, not one per rebuild.
void SSWorldFieldShapes::requestNavRoles()
{
    mRolesWanted = false;
    // The pathfinding manager drops a deferred request silently when the region changes before its capabilities
    // arrive, so an in-flight flag older than 30 s is a dead request, not a pending one.
    if (mRolesInFlight && mNow - mRolesRequestedAt < 30.0) return;
    if (mNow - mRolesRequestedAt < 60.0 && mRolesRegion == mCensus.mRegionHandle) return;
    if (!LLPathfindingManager::instanceExists()) return;
    LLPathfindingManager* mgr = LLPathfindingManager::getInstance();
    if (!mgr->isPathfindingEnabledForCurrentRegion()) return;
    mRolesRequestedAt = mNow;
    mRolesRegion = mCensus.mRegionHandle;
    mRolesInFlight = true;
    static U32 request_id = 0x53530000u;
    const U64 region_handle = mRolesRegion;     // travels with the request: a second request can overwrite mRolesRegion before a slow reply lands
    mgr->requestGetLinksets(++request_id, [region_handle](U32 id, LLPathfindingManager::ERequestStatus status, LLPathfindingObjectListPtr list)
    {
        SSWorldFieldShapes::onNavRoles(id, (S32)status, list, region_handle);
    });
}

// The reply: every linkset's role filed by root id; a changed table forces the next rebuild so the records carry it.
void SSWorldFieldShapes::onNavRoles(U32, S32 status, const std::shared_ptr<LLPathfindingObjectList>& list, U64 region_handle)
{
    if (status == (S32)LLPathfindingManager::kRequestStarted) return;
    if (!SSWorldFieldShapes::instanceExists()) return;
    SSWorldFieldShapes* self = SSWorldFieldShapes::getInstance();
    self->mRolesInFlight = false;
    if (status != (S32)LLPathfindingManager::kRequestCompleted || !list) return;
    for (LLPathfindingObjectList::const_iterator it = list->begin(); it != list->end(); ++it)
    {
        const LLPathfindingLinkset* linkset = dynamic_cast<const LLPathfindingLinkset*>(it->second.get());
        if (!linkset || linkset->isTerrain()) continue;
        U8 role = NAV_ROLE_UNKNOWN;
        switch (linkset->getLinksetUse())
        {
            case LLPathfindingLinkset::kWalkable: role = NAV_ROLE_WALKABLE; break;
            case LLPathfindingLinkset::kStaticObstacle: role = NAV_ROLE_STATIC_OBSTACLE; break;
            case LLPathfindingLinkset::kDynamicObstacle: role = NAV_ROLE_DYNAMIC_OBSTACLE; break;
            case LLPathfindingLinkset::kMaterialVolume: role = NAV_ROLE_MATERIAL_VOLUME; break;
            case LLPathfindingLinkset::kExclusionVolume: role = NAV_ROLE_EXCLUSION_VOLUME; break;
            case LLPathfindingLinkset::kDynamicPhantom: role = NAV_ROLE_DYNAMIC_PHANTOM; break;
            default: break;
        }
        NavRole& slot = self->mNavRoles[linkset->getUUID()];
        slot.mRegion = region_handle;           // the region this reply answered for - what pruneNavRoles ages the entry against
        if (slot.mRole != role) { slot.mRole = role; self->mRolesChanged = true; }
    }
}

// Drop roles whose region LLWorld no longer holds: the reply is region-wide, so hopping regions would otherwise
// stack a whole sim's table per hop; neighbours stay resident, and with them every role the census can still ask for.
void SSWorldFieldShapes::pruneNavRoles()
{
    if (mNavRoles.empty() || !LLWorld::instanceExists()) return;
    LLWorld* worldp = LLWorld::getInstance();
    for (auto it = mNavRoles.begin(); it != mNavRoles.end();)
    {
        if (it->second.mRegion && !worldp->getRegionFromHandle(it->second.mRegion))
        {
            it = mNavRoles.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// Drop cached parts nothing has sighted within the TTL.
void SSWorldFieldShapes::prunePartCache()
{
    for (auto it = mParts.begin(); it != mParts.end();)
    {
        if (mNow - it->second.mSeen > SS_SHAPES_PART_CACHE_TTL) { it = mParts.erase(it); } else { ++it; }
    }
}

// Everything that shapes a part's records, hashed: a cache hit means the baked
// records are still exactly what addPart would produce.
U64 SSWorldFieldShapes::partSignature(LLVOVolume* vov, const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale,
                                      S32 ptype, bool shape_known, bool phantom, bool hidden) const
{
    auto fnv = [](U64 h, U64 v) { h ^= v; h *= 1099511628211ull; return h; };
    auto fnvf = [&](U64 h, F32 f) { return fnv(h, (U64)(S64)llround(f * 1000.f)); };
    U64 h = 14695981039346656037ull;
    for (U32 c = 0; c < 3; ++c) { h = fnvf(h, pos.mV[c]); h = fnvf(h, scale.mV[c]); }
    for (U32 c = 0; c < 4; ++c) h = fnvf(h, rot.mQ[c] * 10.f);
    const LLVolumeParams& params = vov->getVolume()->getParams();
    const LLPathParams& path = params.getPathParams();
    const LLProfileParams& prof = params.getProfileParams();
    h = fnv(h, path.getCurveType()); h = fnvf(h, path.getBegin()); h = fnvf(h, path.getEnd());
    h = fnvf(h, path.getScale().mV[0]); h = fnvf(h, path.getScale().mV[1]);
    h = fnvf(h, path.getShear().mV[0]); h = fnvf(h, path.getShear().mV[1]);
    h = fnvf(h, path.getTwist()); h = fnvf(h, path.getTwistBegin()); h = fnvf(h, path.getRadiusOffset());
    h = fnvf(h, path.getTaper().mV[0]); h = fnvf(h, path.getTaper().mV[1]);
    h = fnvf(h, path.getRevolutions()); h = fnvf(h, path.getSkew());
    h = fnv(h, prof.getCurveType()); h = fnvf(h, prof.getBegin()); h = fnvf(h, prof.getEnd()); h = fnvf(h, prof.getHollow());
    const LLUUID& sculpt = params.getSculptID();
    for (U32 i = 0; i < 4; ++i) h = fnv(h, ((const U32*)sculpt.mData)[i]);
    h = fnv(h, params.getSculptType());
    h = fnv(h, (U64)(ptype + 2) | ((U64)shape_known << 8) | ((U64)phantom << 9) | ((U64)hidden << 10) | ((U64)vov->isMesh() << 11) | ((U64)mBuildNavRole << 12));
    if (vov->isMesh())
    {
        // The decomposition state decides hull vs tessellation vs placeholder box; its arrival must miss the cache.
        LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(sculpt);
        if (decomp)
        {
            h = fnv(h, 1 + (U64)decomp->mHull.size());
            h = fnv(h, (U64)decomp->mMesh.size());
            h = fnv(h, (U64)decomp->mPhysicsShapeMesh.mPositions.size());
            h = fnv(h, (U64)decomp->mBaseHullMesh.mPositions.size());
        }
    }
    return h;
}

// The DYNAMIC classifier: an object at rest across the settle window reads
// at-rest; any transform change re-marks it dynamic and restarts the window
// (the settleEdits pattern, per-root). State outlives the build-scoped census
// so a jittering stack never re-enters anything store-bound.
void SSWorldFieldShapes::trackRest(const LLViewerObject* rootp, bool& out_dynamic)
{
    RestState& state = mRest[rootp->getID()];
    state.mSeen = true;

    // <SS:Nexii> The sim's own word first: a pathfinding character or a physical root is a mover whatever it is doing right now, and a linkset set to a static navmesh role (Walkable, Static obstacle, a material or exclusion volume - the FLAGS_AFFECTS_NAVMESH bit) is landscape from its first sighting. Only the default role (Movable obstacle, no bit) is undecided. [interaction: SSNavMesh obstacles]
    LLViewerObject* mutable_root = const_cast<LLViewerObject*>(rootp);
    if (rootp->flagCharacter() || rootp->flagUsePhysics())
    {
        state.mKnown = true;
        state.mDynamic = true;
        state.mMovedAt = mNow;
        out_dynamic = true;
        ++mPendRestByFlag;
        return;
    }
    if (mutable_root->getFlags() & FLAGS_AFFECTS_NAVMESH)
    {
        state.mKnown = true;
        state.mDynamic = false;
        state.mMovedAt = mNow;
        state.mPos = LLVector3d(rootp->getPositionAgent());
        state.mRot = rootp->getRotation();
        state.mScale = rootp->getScale();
        out_dynamic = false;
        ++mPendRestByFlag;
        return;
    }

    // <SS:Nexii> Then the Region Object Cache ledger: a root it has watched sit still across visits, or promoted, is landscape now rather than after a settle window; one it has seen move, or disqualified, stays a mover. Silence falls through to watching. [interaction: ssrocledger restVerdict]
    if (!state.mKnown && SSROCLedger::instanceExists() && rootp->getRegion())
    {
        bool is_static = false;
        if (SSROCLedger::getInstance()->restVerdict(rootp->getRegion()->getHandle(), rootp->getID(), is_static))
        {
            state.mKnown = true;
            state.mDynamic = !is_static;
            state.mMovedAt = mNow;
            state.mPos = LLVector3d(rootp->getPositionAgent());
            state.mRot = rootp->getRotation();
            state.mScale = rootp->getScale();
            out_dynamic = state.mDynamic;
            ++mPendRestByLedger;
            return;
        }
    }
    ++mPendRestByWatching;
    const LLVector3d pos(rootp->getPositionAgent());
    const LLQuaternion rot = rootp->getRotation();
    const LLVector3 scale = rootp->getScale();

    // <SS:Nexii> First sighting is "no history", never "not yet scanned this build": mSeen is reset before every scan for pruning, and testing it here re-marked every root DYNAMIC on every build, so nothing ever settled and the navmesh saw terrain only. [interaction: SSNavMesh bands, obstacles]
    if (!state.mKnown)
    {
        // First sighting: unknown history - dynamic until the window proves still.
        state.mKnown = true;
        state.mMovedAt = mNow;
        state.mDynamic = true;
    }
    else if ((state.mPos - pos).magVecSquared() > SS_SHAPES_REST_SLOP * SS_SHAPES_REST_SLOP
             || state.mRot != rot
             || state.mScale != scale)
    {
        state.mMovedAt = mNow;
        state.mDynamic = true;
    }
    else if (state.mDynamic && mNow - state.mMovedAt >= SS_SHAPES_SETTLE_SECONDS)
    {
        state.mDynamic = false;
    }

    state.mPos = pos;
    state.mRot = rot;
    state.mScale = scale;
    state.mSeen = true;
    out_dynamic = state.mDynamic;
}

// One volume part in: classify its declared shape exactly the way the physics
// debug renderer does (LLPhysicsShapeBuilderUtil + get_physics_detail), collect
// local-space triangles where the class needs them, and file the record.
// The dump mirrors addPart's decisions read-only; keep the two in step when a rule changes.
std::string SSWorldFieldShapes::dumpObject(const LLViewerObject* obj, std::vector<std::string>& out) const
{
    if (!obj) return "no object";
    const LLViewerObject* rootp = obj;
    while (rootp->getParent()) rootp = (const LLViewerObject*)rootp->getParent();
    std::string verdict;
    if (rootp->getPCode() != LL_PCODE_VOLUME)
    {
        out.push_back(llformat("=== %s local %u: not a volume prim (pcode 0x%02x: land patch, tree, grass or avatar) - the census never files these",
                               rootp->getID().asString().c_str(), rootp->getLocalID(), rootp->getPCode()));
        return "not a volume prim: the census never files these";
    }
    bool spinning = false;
    for (const LLViewerObject* o = rootp; o; o = (const LLViewerObject*)o->getParent())
    {
        if (o->getAngularVelocity().magVecSquared() > 1e-6f) { spinning = true; break; }
    }
    if (spinning) out.push_back("root spins (llTargetOmega): filed as a mover at its unspun pose");

    const U32 root_flags = rootp->getFlags();
    const bool phantom = rootp->flagPhantom();
    out.push_back(llformat("=== linkset root %s local %u at (%.1f, %.1f, %.1f) region %s, %d children",
                           rootp->getID().asString().c_str(), rootp->getLocalID(),
                           rootp->getPositionAgent().mV[VX], rootp->getPositionAgent().mV[VY], rootp->getPositionAgent().mV[VZ],
                           rootp->getRegion() ? rootp->getRegion()->getName().c_str() : "none", (S32)rootp->getChildren().size()));
    out.push_back(llformat("root flags 0x%08x: phantom %d, use_physics %d, character %d, affects_navmesh %d, volume_detect %d, temp_on_rez %d",
                           root_flags, (S32)phantom, (S32)rootp->flagUsePhysics(), (S32)rootp->flagCharacter(),
                           (S32)((root_flags & FLAGS_AFFECTS_NAVMESH) != 0), (S32)((root_flags & FLAGS_VOLUME_DETECT) != 0),
                           (S32)((root_flags & FLAGS_TEMPORARY_ON_REZ) != 0)));

    // Navmesh role, as addPart resolves it.
    U8 nav_role = NAV_ROLE_UNKNOWN;
    static const char* ROLE_NAME[] = {"unknown", "walkable", "static obstacle", "dynamic obstacle", "material volume", "exclusion volume", "dynamic phantom"};
    if (root_flags & FLAGS_AFFECTS_NAVMESH)
    {
        auto role_it = mNavRoles.find(rootp->getID());
        if (role_it != mNavRoles.end()) nav_role = role_it->second.mRole;
        out.push_back(llformat("navmesh role: %s%s", ROLE_NAME[llclamp((S32)nav_role, 0, 6)],
                               role_it != mNavRoles.end() ? "" : (mRolesInFlight ? " (flag set, role not fetched yet - request in flight)" : " (flag set, role not fetched yet - request not in flight)")));
    }
    else
    {
        out.push_back("navmesh role: none (default 'movable obstacle' - the rest ladder decides)");
    }

    // Rest ladder, as trackRest ranks it.
    std::string rest;
    if (rootp->flagCharacter() || rootp->flagUsePhysics()) rest = "MOVER by sim flag (character/physical)";
    else if (root_flags & FLAGS_AFFECTS_NAVMESH) rest = "STATIC by sim flag (navmesh role set)";
    else
    {
        bool is_static = false;
        if (SSROCLedger::instanceExists() && rootp->getRegion()
            && SSROCLedger::getInstance()->restVerdict(rootp->getRegion()->getHandle(), rootp->getID(), is_static))
        {
            rest = is_static ? "STATIC by ROC ledger" : "MOVER by ROC ledger";
        }
        else
        {
            rest = "watching (no flag, ledger silent)";
        }
    }
    auto rest_it = mRest.find(rootp->getID());
    if (rest_it != mRest.end())
    {
        out.push_back(llformat("rest ladder: %s; tracked state: %s, known %d, last moved %.1f s ago (settle window %.0f s)",
                               rest.c_str(), rest_it->second.mDynamic ? "DYNAMIC (obstacle only, no navmesh floor)" : "static",
                               (S32)rest_it->second.mKnown, mNow - rest_it->second.mMovedAt, (F32)SS_SHAPES_SETTLE_SECONDS));
    }
    else
    {
        out.push_back(llformat("rest ladder: %s; never tracked (the census has not scanned this root)", rest.c_str()));
    }
    const bool dynamic_now = rest_it != mRest.end() ? rest_it->second.mDynamic : true;

    // Envelope.
    const F32 env = envelopeRange();
    const F32 dist = (rootp->getPositionAgent() - mCensus.mAnchor).magVec();
    out.push_back(llformat("census envelope: root %.0f m from the anchor, range %.0f m, census %s (%d records), part cache %d entries",
                           dist, env, censusCurrent() ? "current" : "stale", recordCount(), cachedPartCount()));

    // Linkset-level skips.
    if (root_flags & (FLAGS_VOLUME_DETECT | FLAGS_TEMPORARY_ON_REZ)) verdict = "SKIPPED by census: volume-detect or temporary-on-rez linkset";
    else if (phantom && nav_role != NAV_ROLE_EXCLUSION_VOLUME) verdict = "SKIPPED by census: phantom linkset (only exclusion volumes pass)";
    else if (nav_role == NAV_ROLE_EXCLUSION_VOLUME) verdict = "filed as an exclusion volume: cuts walkable area, no geometry";
    else if (dynamic_now) verdict = "filed but DYNAMIC: navmesh carves it as a mover obstacle, never floor";
    else if (nav_role == NAV_ROLE_STATIC_OBSTACLE) verdict = "filed as a static obstacle: blocks and shadows, never floor";

    // Every part.
    std::vector<const LLViewerObject*> parts;
    parts.push_back(rootp);
    for (const LLPointer<LLViewerObject>& child : rootp->getChildren()) parts.push_back(child.get());
    static const char* PTYPE_NAME[] = {"prim", "none", "convex hull"};
    static const char* CLASS_NAME[] = {"box", "sphere", "cylinder", "tri"};
    static const char* PROV_NAME[] = {"exact", "hull", "tessellated", "bbox", "unfetched", "render", "terrain"};
    static const char* LAYER_NAME[] = {"declared", "phantom", "invisible solid"};
    LLVector3 all_min(FLT_MAX, FLT_MAX, FLT_MAX), all_max(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const LLViewerObject* part : parts)
    {
        if (!part || part->getPCode() != LL_PCODE_VOLUME) { out.push_back(llformat("part %s: not a volume (pcode %u)", part ? part->getID().asString().c_str() : "?", part ? part->getPCode() : 0)); continue; }
        LLVOVolume* vov = (LLVOVolume*)const_cast<LLViewerObject*>(part);
        LLVolume* vol = vov->getVolume();
        const LLVector3 pos = vov->getPositionAgent();
        const LLVector3 scale = vov->getScale();
        std::string line = llformat("part %s local %u: pos (%.1f, %.1f, %.1f) scale (%.2f, %.2f, %.2f)%s%s%s",
                                    part->getID().asString().c_str(), part->getLocalID(),
                                    pos.mV[VX], pos.mV[VY], pos.mV[VZ], scale.mV[VX], scale.mV[VY], scale.mV[VZ],
                                    vov->isMesh() ? " MESH" : (vov->isSculpted() ? " SCULPT" : " PRIM"),
                                    vov->isFlexible() ? " FLEXIBLE(skipped)" : "", vov->isRiggedMesh() ? " RIGGED(skipped)" : "");
        out.push_back(line);
        if (!vol) { out.push_back("  no volume yet: the scan skips it"); continue; }
        const LLVolumeParams& params = vol->getParams();
        const bool shape_known = !vov->getPhysicsShapeUnknown();
        const S32 ptype = shape_known ? vov->getPhysicsShapeType() : -1;
        const bool hidden = ss_part_fully_hidden(vov);
        const bool geometry_only = (ptype == LLViewerObject::PHYSICS_SHAPE_NONE) || !shape_known;
        out.push_back(llformat("  physics type: %s%s, fully hidden %d, path 0x%02x profile 0x%02x, sculpt/mesh id %s",
                               shape_known ? PTYPE_NAME[llclamp(ptype, 0, 2)] : "UNKNOWN (not fetched)",
                               geometry_only ? " -> render geometry (provenance RENDER)" : "",
                               (S32)hidden, params.getPathParams().getCurveType(), params.getProfileParams().getCurveType(),
                               params.getSculptID().asString().c_str()));
        if (vov->isMesh())
        {
            const LLUUID mesh_id = params.getSculptID();
            LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);
            if (decomp)
            {
                S32 hull_tris = 0;
                for (size_t h = 0; h < decomp->mMesh.size(); ++h) hull_tris += (S32)(decomp->mMesh[h].mPositions.size() / 3);
                out.push_back(llformat("  mesh physics: decomposition present, %d hulls (%d built tris), physics mesh %s, base hull %s, header says physics shape %d",
                                       (S32)decomp->mHull.size(), hull_tris,
                                       decomp->mPhysicsShapeMesh.empty() ? "absent" : llformat("%d tris", (S32)(decomp->mPhysicsShapeMesh.mPositions.size() / 3)).c_str(),
                                       decomp->mBaseHullMesh.empty() ? "absent" : llformat("%d tris", (S32)(decomp->mBaseHullMesh.mPositions.size() / 3)).c_str(),
                                       (S32)gMeshRepo.hasPhysicsShapeInHeader(mesh_id)));
            }
            else
            {
                out.push_back(llformat("  mesh physics: no decomposition yet (header physics shape %d) -> bounding box, provenance UNFETCHED", (S32)gMeshRepo.hasPhysicsShapeInHeader(mesh_id)));
            }
        }
        if (hidden && (phantom || ptype == LLViewerObject::PHYSICS_SHAPE_NONE) && nav_role != NAV_ROLE_EXCLUSION_VOLUME)
        {
            out.push_back("  SKIPPED by census: fully hidden and phantom or physics NONE");
            if (verdict.empty() && parts.size() == 1) verdict = "SKIPPED by census: fully hidden and phantom or physics NONE";
            continue;
        }
        auto cache_it = mParts.find(part->getID());
        if (cache_it == mParts.end())
        {
            out.push_back("  part cache: NO ENTRY - the scan never filed it (flexible/rigged, outside the envelope, skipped above, or not scanned since it appeared)");
            continue;
        }
        const PartCache& pc = cache_it->second;
        out.push_back(llformat("  part cache: %d records, %d tris, seen %.1f s ago, sig %016llx", (S32)pc.mRecords.size(), pc.mTris, mNow - pc.mSeen, (unsigned long long)pc.mSig));
        for (const Record& r : pc.mRecords)
        {
            const bool inverted = r.mBMin.mV[VX] > r.mBMax.mV[VX] || r.mBMin.mV[VY] > r.mBMax.mV[VY] || r.mBMin.mV[VZ] > r.mBMax.mV[VZ];
            const bool convex = r.mClass != Record::CLASS_TRI || r.mProv == PROV_HULL || r.mProv == PROV_BBOX || r.mProv == PROV_UNFETCHED;
            const char* treatment = (r.mLayer == LAYER_DECLARED_PHANTOM && nav_role != NAV_ROLE_EXCLUSION_VOLUME) ? "navmesh skips (phantom layer)"
                                  : (r.mNoPhysics && nav_role != NAV_ROLE_EXCLUSION_VOLUME) ? "navmesh skips (physics NONE: render geometry is for rain and cover only)"
                                  : dynamic_now ? "navmesh carves a box obstacle (DYNAMIC)"
                                  : nav_role == NAV_ROLE_STATIC_OBSTACLE ? (convex ? "solid block, never floor" : "surface block, never floor")
                                  : convex ? "solid fill, walkable on up-facing tops within the slope" : "surface raster, walkable within the slope";
            out.push_back(llformat("    record %s/%s/%s: %d tris, aabb (%.1f, %.1f, %.1f)-(%.1f, %.1f, %.1f) -> %s%s",
                                   CLASS_NAME[llclamp((S32)r.mClass, 0, 3)], PROV_NAME[llclamp((S32)r.mProv, 0, 6)], LAYER_NAME[llclamp((S32)r.mLayer, 0, 2)],
                                   (S32)(r.tris().size() / 3), r.mBMin.mV[VX], r.mBMin.mV[VY], r.mBMin.mV[VZ], r.mBMax.mV[VX], r.mBMax.mV[VY], r.mBMax.mV[VZ], treatment,
                                   inverted ? "  ** INVERTED AABB: no query can match this record **" : ""));
            for (S32 c = 0; c < 3; ++c) { all_min.mV[c] = llmin(all_min.mV[c], r.mBMin.mV[c]); all_max.mV[c] = llmax(all_max.mV[c], r.mBMax.mV[c]); }
        }
    }
    if (verdict.empty()) verdict = all_min.mV[VX] < all_max.mV[VX] ? "filed as static geometry: see the navmesh columns below" : "no records on file for this linkset";
    if (all_min.mV[VX] < all_max.mV[VX] && SSNavMesh::instanceExists())
    {
        SSNavMesh::getInstance()->dumpAt(all_min, all_max, out);
    }
    else
    {
        // No records: still say what the navmesh has under the root.
        const LLVector3 c = rootp->getPositionAgent();
        if (SSNavMesh::instanceExists()) SSNavMesh::getInstance()->dumpAt(c - LLVector3(1.f, 1.f, 1.f), c + LLVector3(1.f, 1.f, 1.f), out);
    }
    out.push_back("verdict: " + verdict);
    return verdict;
}

void SSWorldFieldShapes::dumpRecordsAt(const LLVector3& bmin, const LLVector3& bmax, std::vector<std::string>& out) const
{
    static const char* CLASS_NAME[] = {"box", "sphere", "cylinder", "tri"};
    static const char* PROV_NAME[] = {"exact", "hull", "tessellated", "bbox", "unfetched", "render", "terrain"};
    static const char* LAYER_NAME[] = {"declared", "phantom", "invisible solid"};
    static const char* ROLE_NAME[] = {"none", "walkable", "static obstacle", "dynamic obstacle", "material volume", "exclusion volume", "dynamic phantom"};
    S32 n = 0, shown = 0;
    forEachRecord(bmin, bmax, [&](const Record& r)
    {
        ++n;
        if (shown >= 40) return;
        ++shown;
        const bool convex = r.mClass != Record::CLASS_TRI || r.mProv == PROV_HULL || r.mProv == PROV_BBOX || r.mProv == PROV_UNFETCHED;
        out.push_back(llformat("  record %s/%s/%s role %s%s: %d tris, aabb (%.1f, %.1f, %.1f)-(%.1f, %.1f, %.1f), %s",
                               CLASS_NAME[llclamp((S32)r.mClass, 0, 3)], PROV_NAME[llclamp((S32)r.mProv, 0, 6)], LAYER_NAME[llclamp((S32)r.mLayer, 0, 2)],
                               ROLE_NAME[llclamp((S32)r.mNavRole, 0, 6)], r.mDynamic ? " DYNAMIC" : "",
                               (S32)(r.tris().size() / 3), r.mBMin.mV[VX], r.mBMin.mV[VY], r.mBMin.mV[VZ], r.mBMax.mV[VX], r.mBMax.mV[VY], r.mBMax.mV[VZ],
                               (r.mLayer == LAYER_DECLARED_PHANTOM && r.mNavRole != NAV_ROLE_EXCLUSION_VOLUME) ? "skipped (phantom)"
                               : (r.mNoPhysics && r.mNavRole != NAV_ROLE_EXCLUSION_VOLUME) ? "skipped (physics NONE)"
                               : r.mDynamic ? "box obstacle"
                               : r.mNavRole == NAV_ROLE_EXCLUSION_VOLUME ? "exclusion cut" : r.mNavRole == NAV_ROLE_STATIC_OBSTACLE ? "block, never floor"
                               : convex ? "solid fill" : "surface raster"));
    });
    out.push_back(llformat("  census records meeting the box: %d%s", n, n > shown ? " (first 40 listed)" : ""));
}

void SSWorldFieldShapes::addPart(LLVOVolume* vov)
{
    LLVolume* vol = vov->getVolume();
    if (!vol) return;

    const LLVolumeParams& params = vol->getParams();
    const LLVector3 pos = vov->getPositionAgent();
    LLQuaternion rot = ss_world_rotation(vov);
    const LLVector3 scale = vov->getScale();
    LLViewerObject* rootp = vov;
    while (rootp->getParent()) rootp = (LLViewerObject*)rootp->getParent();

    // The DYNAMIC bit rides the root: a mover's parts are query-time only and
    // never store-bound, whatever their layer.
    bool dynamic = false;
    trackRest(rootp, dynamic);
    // <SS:Nexii> A part spinning under llTargetOmega, or under a spinning ancestor, turns client-side: the drawable's world rotation changes every frame while the root never moves, so the rest ladder called it landscape and its records' boxes changed every census - which rebuilt its navmesh band every census, forever, and with it the world field's serial, so the flood never landed. It is a mover: filed at its unspun pose so its obstacle box is stable, never floor. [interaction: SSNavMesh band signature, syncObstacles]
    bool spinning = false;
    for (const LLViewerObject* o = vov; o; o = (const LLViewerObject*)o->getParent())
    {
        if (o->getAngularVelocity().magVecSquared() > 1e-6f) { spinning = true; break; }
    }
    if (spinning) { dynamic = true; rot = ss_world_rotation_unspun(vov); }
    mBuildDynamic = dynamic;

    const bool phantom = rootp->flagPhantom();
    const U32 root_flags = rootp->getFlags();

    // The linkset's navmesh role: known from the table, wanted when the flag says there is one to fetch.
    mBuildNavRole = NAV_ROLE_UNKNOWN;
    mBuildNoPhysics = false;
    if (root_flags & FLAGS_AFFECTS_NAVMESH)
    {
        auto role_it = mNavRoles.find(rootp->getID());
        if (role_it != mNavRoles.end()) mBuildNavRole = role_it->second.mRole; else mRolesWanted = true;
    }

    // <SS:Nexii> Not physics shapes, so not census (owner decision 2026-09-10): volume-detect and temporary-on-rez linksets, and phantom ones - except an exclusion volume, the one phantom the navmesh must see, which carries no geometry but cuts walkable area. The phantom LAYER therefore holds exclusion volumes only. [interaction: SSNavMesh exclusions]
    if (root_flags & (FLAGS_VOLUME_DETECT | FLAGS_TEMPORARY_ON_REZ)) return;
    if (phantom && mBuildNavRole != NAV_ROLE_EXCLUSION_VOLUME) return;

    if (mBuildNavRole == NAV_ROLE_EXCLUSION_VOLUME)
    {
        // The navmesh reads only its bounds (a convex cut over the walkable area); nothing else reads it at all.
        mCurrentPart = nullptr;
        addOBB(pos, rot, scale * 0.5f, (U8)LAYER_DECLARED_PHANTOM, PROV_BBOX);
        return;
    }

    const bool shape_known = !vov->getPhysicsShapeUnknown();
    // <SS:Nexii> Ask for an unknown part's shape type: getPhysicsShapeType() files the request itself, the object list batches every stale id into one GetObjectPhysicsData call, and the arrival changes the part's signature so the next build re-tessellates it. [interaction: part cache]
    if (!shape_known)
    {
        vov->getPhysicsShapeType();
        ++mPendPhysicsRequested;
    }
    const S32 ptype = shape_known ? vov->getPhysicsShapeType() : -1;
    // <SS:Nexii> Set before the part-cache reuse below, not after: addRecord stamps every record - reused ones too - from this, and stamping it later left the flag on the cached copy only. The store's records read false, the navmesh saw no-physics trees as floor, and the schedule log counted 0 skipped while the dump (which reads the cache) said 'navmesh skips'. [interaction: addRecord, navIgnores]
    mBuildNoPhysics = (ptype == LLViewerObject::PHYSICS_SHAPE_NONE);

    // Hidden parts: phantom (no collision declared) or shape-NONE (collision
    // explicitly declined) with no visible face anywhere affect nothing at all
    // - no shelter, no block, no record. Invisible solids stay: the builder's
    // collision proxy where a mesh lacks a good physics shape - every consumer
    // reads them, the layer only marks them for the overlay. An unknown type
    // stays a proxy until its data arrives saying NONE.
    const bool hidden = ss_part_fully_hidden(vov);
    // An exclusion volume is normally an invisible phantom: it carries no geometry but it must reach the navmesh.
    if (hidden && (phantom || ptype == LLViewerObject::PHYSICS_SHAPE_NONE) && mBuildNavRole != NAV_ROLE_EXCLUSION_VOLUME) return;

    // The part cache: reuse the baked records when nothing that shaped them changed.
    mCurrentPart = nullptr;
    const U64 sig = partSignature(vov, pos, rot, scale, ptype, shape_known, phantom, hidden);
    PartCache& part = mParts[vov->getID()];
    part.mSeen = mNow;
    if (part.mSig == sig && !part.mRecords.empty())
    {
        if (mPendingTris + part.mTris <= SS_SHAPES_TOTAL_TRIS)
        {
            for (const Record& cached : part.mRecords)
            {
                Record rec = cached;
                addRecord(rec);
            }
            mPendingTris += part.mTris;
            return;
        }
        // Over the triangle budget this pass: build the fallback below without disturbing the valid entry.
    }
    else
    {
        part.mSig = sig;
        part.mRecords.clear();
        part.mTris = 0;
        mCurrentPart = &part;
    }
    const U8 layer = phantom ? (U8)LAYER_DECLARED_PHANTOM
                             : (hidden ? (U8)LAYER_INVISIBLE_SOLID : (U8)LAYER_DECLARED);

    // No declared shape, or a shape type nobody ever queried: the prim's own
    // volume is the geometry, provenance RENDER - the same math the server
    // shape builder runs, without a server shape behind it. A tapered prim
    // keeps its taper this way; a bbox would erase it. Unknown MESH physics
    // still needs its asset, so those keep the fetch-then-box path below.
    // Never fired an ObjectPhysicsProperties request here.
    const bool geometry_only = (ptype == LLViewerObject::PHYSICS_SHAPE_NONE) || !shape_known;

    const S32 tri_cap = (llmax(scale.mV[VX], llmax(scale.mV[VY], scale.mV[VZ])) > SS_SHAPES_LARGE_PART_M)
                            ? SS_SHAPES_PART_TRIS_LARGE : SS_SHAPES_PART_TRIS;

    if (vov->isMesh())
    {
        const LLUUID mesh_id = params.getSculptID();
        LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);
        const bool has_decomp = decomp && !decomp->mHull.empty();
        LLPhysicsVolumeParams phys_params(params, ptype == LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);
        LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification spec;
        LLPhysicsShapeBuilderUtil::determinePhysicsShape(phys_params, scale, has_decomp, spec);
        const S32 st = (S32)spec.getType();

        if (decomp)
        {
            if (!decomp->mHull.empty())
            {
                gMeshRepo.buildPhysicsMesh(*decomp);
                // <SS:Nexii> One record per hull: each hull is convex, and the navmesh fills convex records solid so a building's interior never reads as a room. Concatenating hulls into one soup would lose that. [interaction: SSNavMesh convex fill]
                S32 total_tris = 0;
                for (size_t h = 0; h < decomp->mMesh.size(); ++h) total_tris += (S32)(decomp->mMesh[h].mPositions.size() / 3);
                if (total_tris > 0 && total_tris <= tri_cap && mPendingTris + total_tris <= SS_SHAPES_TOTAL_TRIS)
                {
                    // All or nothing: a partial set of hulls plus a box would double-file the building.
                    std::vector<LLVector3> soup;
                    for (size_t h = 0; h < decomp->mMesh.size(); ++h)
                    {
                        soup = decomp->mMesh[h].mPositions;
                        ss_orient_outward(soup, 0);
                        addTriangles(pos, rot, scale, soup, layer, geometry_only ? (U8)PROV_RENDER : (U8)PROV_HULL);
                    }
                    return;
                }
                addOBB(pos, rot, scale * 0.5f, layer, PROV_BBOX);
                return;
            }
            if (!decomp->mPhysicsShapeMesh.empty())
            {
                if (addTriangles(pos, rot, scale, decomp->mPhysicsShapeMesh.mPositions, layer,
                                 geometry_only ? (U8)PROV_RENDER : (U8)PROV_TESSELLATED))
                {
                    return;
                }
            }
            else if (!decomp->mBaseHullMesh.empty())
            {
                if (addTriangles(pos, rot, scale, decomp->mBaseHullMesh.mPositions, layer,
                                 geometry_only ? (U8)PROV_RENDER : (U8)PROV_HULL))
                {
                    return;
                }
            }
            else
            {
                gMeshRepo.fetchPhysicsShape(mesh_id);
            }
        }
        addOBB(pos, rot, scale * 0.5f, layer, PROV_UNFETCHED);
        return;
    }

    // Prim: closed form for the three implicit specs, physics-detail geometry
    // for everything cut, hollow, twisted or otherwise non-implicit.
    const U8 prov_base = geometry_only ? (U8)PROV_RENDER : (U8)PROV_EXACT;
    LLPhysicsVolumeParams phys_params(params, ptype == LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);
    LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification spec;
    LLPhysicsShapeBuilderUtil::determinePhysicsShape(phys_params, scale, false, spec);
    const S32 st = (S32)spec.getType();
    const LLVector3 spec_center = pos + (spec.getCenter() * rot);
    const LLVector3 spec_half = spec.getScale() * 0.5f;

    if (st == (S32)LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::BOX)
    {
        addOBB(spec_center, rot, spec_half, layer, prov_base);
    }
    else if (st == (S32)LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::SPHERE)
    {
        addEllipsoid(spec_center, rot, spec_half, layer, prov_base);
    }
    else if (st == (S32)LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification::CYLINDER)
    {
        const F32 radius = llmax(spec_half.mV[VX], spec_half.mV[VY]);
        addCylinder(spec_center, rot, radius, spec_half.mV[VZ], layer, prov_base);
    }
    else
    {
        // PRIM_MESH / PRIM_CONVEX / SCULPT / USER_CONVEX: the physics-detail
        // tessellation is the shape; hull points when Havok already built them.
        const S32 detail = get_physics_detail(params, scale);
        LLVolume* phys_vol = LLPrimitive::sVolumeManager->refVolume(params, detail);
        if (!phys_vol)
        {
            addOBB(pos, rot, scale * 0.5f, layer, PROV_BBOX);
            return;
        }

        std::vector<LLVector3> soup;
        bool truncated = false;

        if (phys_vol->mHullPoints && phys_vol->mHullIndices && phys_vol->mNumHullIndices >= 3)
        {
            for (S32 k = 0; k + 2 < phys_vol->mNumHullIndices; k += 3)
            {
                if ((S32)(soup.size() / 3) >= tri_cap) { truncated = true; break; }
                soup.push_back(ss_vert(phys_vol->mHullPoints[phys_vol->mHullIndices[k]]));
                soup.push_back(ss_vert(phys_vol->mHullPoints[phys_vol->mHullIndices[k + 1]]));
                soup.push_back(ss_vert(phys_vol->mHullPoints[phys_vol->mHullIndices[k + 2]]));
            }
            ss_orient_outward(soup, 0);
        }
        if (soup.empty())
        {
            const S32 nf = phys_vol->getNumVolumeFaces();
            for (S32 f = 0; f < nf; ++f)
            {
                const LLVolumeFace& vf = phys_vol->getVolumeFace(f);
                if (!vf.mPositions || !vf.mIndices || vf.mNumIndices < 3 || vf.mNumIndices > 65535) continue;
                for (S32 k = 0; k + 2 < vf.mNumIndices; k += 3)
                {
                    if ((S32)(soup.size() / 3) >= tri_cap) { truncated = true; break; }
                    soup.push_back(ss_vert(vf.mPositions[vf.mIndices[k]]));
                    soup.push_back(ss_vert(vf.mPositions[vf.mIndices[k + 1]]));
                    soup.push_back(ss_vert(vf.mPositions[vf.mIndices[k + 2]]));
                }
                if (truncated) break;
            }
        }
        const bool from_hull = phys_vol->mHullPoints && phys_vol->mHullIndices && phys_vol->mNumHullIndices >= 3;
        LLPrimitive::sVolumeManager->unrefVolume(phys_vol);

        if (!truncated && !soup.empty()
            && addTriangles(pos, rot, scale, soup, layer,
                            geometry_only ? (U8)PROV_RENDER
                                          : (from_hull ? (U8)PROV_HULL : (U8)PROV_TESSELLATED)))
        {
            return;
        }
        addOBB(pos, rot, scale * 0.5f, layer, PROV_BBOX);
    }
}

// Analytic record builders - AABBs derived from the frame so the bucket grid
// and the query pretests stay cheap.
// <SS:Nexii> World AABB half-extent of an oriented box: per world axis, the sum over the body's axes of |axis component| times that axis' half size. The old form multiplied each axis vector component-wise by the half-size vector and summed the signed results, which for any rotation that flips an axis (a prim turned past 90 degrees) produced a box with min above max - a record no query could ever match, so whole rotated buildings went missing from the navmesh and every other census reader. [interaction: addRecord buckets, forEachRecord]
static LLVector3 ss_obb_extent(const LLVector3* axes, const LLVector3& half)
{
    LLVector3 ext;
    for (U32 i = 0; i < 3; ++i)
    {
        const F32 h = fabsf(half.mV[i]);
        ext.mV[VX] += fabsf(axes[i].mV[VX]) * h;
        ext.mV[VY] += fabsf(axes[i].mV[VY]) * h;
        ext.mV[VZ] += fabsf(axes[i].mV[VZ]) * h;
    }
    return ext;
}

void SSWorldFieldShapes::addOBB(const LLVector3& center, const LLQuaternion& rot,
                                const LLVector3& half, U8 layer, U8 prov)
{
    Record rec;
    rec.mClass = Record::CLASS_BOX;
    rec.mCenter = center;
    rec.mAxes[0] = LLVector3(1.f, 0.f, 0.f) * rot;
    rec.mAxes[1] = LLVector3(0.f, 1.f, 0.f) * rot;
    rec.mAxes[2] = LLVector3(0.f, 0.f, 1.f) * rot;
    rec.mHalf = half;
    const LLVector3 ext = ss_obb_extent(rec.mAxes, half);
    rec.mBMin = center - ext;
    rec.mBMax = center + ext;
    rec.mLayer = layer;
    rec.mProv = prov;
    addRecord(rec);
}

void SSWorldFieldShapes::addEllipsoid(const LLVector3& center, const LLQuaternion& rot,
                                      const LLVector3& radii, U8 layer, U8 prov)
{
    Record rec;
    rec.mClass = Record::CLASS_SPHERE;
    rec.mCenter = center;
    rec.mAxes[0] = LLVector3(1.f, 0.f, 0.f) * rot;
    rec.mAxes[1] = LLVector3(0.f, 1.f, 0.f) * rot;
    rec.mAxes[2] = LLVector3(0.f, 0.f, 1.f) * rot;
    rec.mRadii = radii;
    LLVector3 ext;
    for (U32 i = 0; i < 3; ++i)
    {
        ext += LLVector3(fabsf(rec.mAxes[i].mV[VX]), fabsf(rec.mAxes[i].mV[VY]), fabsf(rec.mAxes[i].mV[VZ])) * fabsf(radii.mV[i]);
    }
    rec.mBMin = center - ext;
    rec.mBMax = center + ext;
    rec.mLayer = layer;
    rec.mProv = prov;
    addRecord(rec);
}

void SSWorldFieldShapes::addCylinder(const LLVector3& center, const LLQuaternion& rot,
                                     F32 radius, F32 half_height, U8 layer, U8 prov)
{
    Record rec;
    rec.mClass = Record::CLASS_CYLINDER;
    rec.mCenter = center;
    rec.mAxisU = LLVector3(1.f, 0.f, 0.f) * rot;
    rec.mAxisV = LLVector3(0.f, 1.f, 0.f) * rot;
    rec.mAxes[2] = LLVector3(0.f, 0.f, 1.f) * rot;
    rec.mRadius = radius;
    rec.mHalfHeight = half_height;
    const LLVector3 abs_axes[3] = {rec.mAxisU, rec.mAxisV, rec.mAxes[2]};
    const LLVector3 ext = ss_obb_extent(abs_axes, LLVector3(radius, radius, half_height));
    rec.mBMin = center - ext;
    rec.mBMax = center + ext;
    rec.mLayer = layer;
    rec.mProv = prov;
    addRecord(rec);
}

// Triangle soup in (local, unit-space verts): transform to world, budget-check,
// AABB, file. Returns false when the soup was empty or over budget - the caller
// falls back to a box.
bool SSWorldFieldShapes::addTriangles(const LLVector3& pos, const LLQuaternion& rot,
                                      const LLVector3& scale, const std::vector<LLVector3>& local_soup,
                                      U8 layer, U8 prov)
{
    const size_t verts = local_soup.size() - local_soup.size() % 3;
    if (verts < 3) return false;
    if (mPendingTris + (S32)(verts / 3) > SS_SHAPES_TOTAL_TRIS) return false;

    Record rec;
    rec.mClass = Record::CLASS_TRI;
    std::shared_ptr<std::vector<LLVector3> > soup = std::make_shared<std::vector<LLVector3> >(verts);
    rec.mBMin.setVec(FLT_MAX, FLT_MAX, FLT_MAX);
    rec.mBMax.setVec(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (size_t i = 0; i < verts; ++i)
    {
        const LLVector3 w = pos + (local_soup[i].scaledVec(scale) * rot);
        (*soup)[i] = w;
        for (U32 c = 0; c < 3; ++c)
        {
            rec.mBMin.mV[c] = llmin(rec.mBMin.mV[c], w.mV[c]);
            rec.mBMax.mV[c] = llmax(rec.mBMax.mV[c], w.mV[c]);
        }
    }
    rec.mTri = soup;
    rec.mLayer = layer;
    rec.mProv = prov;
    mPendingTris += (S32)(verts / 3);
    if (mCurrentPart) mCurrentPart->mTris += (S32)(verts / 3);
    addRecord(rec);
    return true;
}

// File a record and register its AABB with every bucket cell it touches.
void SSWorldFieldShapes::addRecord(Record& rec)
{
    const F32 inv = 1.f / SS_SHAPES_BUCKET_M;
    S32 x0 = (S32)floorf(rec.mBMin.mV[VX] * inv);
    S32 y0 = (S32)floorf(rec.mBMin.mV[VY] * inv);
    S32 z0 = (S32)floorf(rec.mBMin.mV[VZ] * inv);
    S32 x1 = (S32)floorf(rec.mBMax.mV[VX] * inv);
    S32 y1 = (S32)floorf(rec.mBMax.mV[VY] * inv);
    S32 z1 = (S32)floorf(rec.mBMax.mV[VZ] * inv);
    if (x1 - x0 > 512) x1 = x0 + 512;
    if (y1 - y0 > 512) y1 = y0 + 512;
    if (z1 - z0 > 512) z1 = z0 + 512;

    const U32 index = (U32)mPending.mRecords.size();
    rec.mDynamic = mBuildDynamic;
    rec.mNavRole = mBuildNavRole;
    rec.mNoPhysics = mBuildNoPhysics;
    if (mCurrentPart) mCurrentPart->mRecords.push_back(rec);
    mPending.mRecords.push_back(rec);
    for (S32 z = z0; z <= z1; ++z)
    {
        for (S32 y = y0; y <= y1; ++y)
        {
            for (S32 x = x0; x <= x1; ++x)
            {
                mPending.mBuckets[ss_bucket_key(x, y, z)].push_back(index);
            }
        }
    }
}

// Exact segment cast: bucket DDA for candidates, analytic/triangle exact tests
// against each, heightfield march for terrain. Deterministic per census - the
// object-list build order and strict-closest tie-break fix the answer.
bool SSWorldFieldShapes::segmentCast(const LLVector3& a, const LLVector3& b, SegmentHit& out,
                                     bool include_phantom)
{
    out = SegmentHit();
    static LLCachedControl<bool> enabled(gSavedSettings, "SSWorldFieldShapes", false);
    if (!enabled) return false;

    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(a);
    if (!regionp && mCensus.mRegionHandle)
    {
        // A cast from over the void still reads the resident census - the
        // envelope is world-space; the context region only anchors rebuilds.
        regionp = LLWorld::getInstance()->getRegionFromHandle(mCensus.mRegionHandle);
    }
    if (!regionp) return false;
    // <SS:Nexii> A stale census never rebuilds inside a query any more (that was a 700 ms stall on a soundscape probe): it only opens the sliced build, and the cast answers from the resident snapshot. [interaction: SSWorldFieldShapesBudgetMS]
    if (!mBuilding && needsRebuild(regionp)) beginBuild(regionp);
    if (mCensus.mRegionHandle != regionp->getHandle())
    {
        const F32 env = envelopeRange();
        if ((a - mCensus.mAnchor).magVec() > env) return false;
    }

    const LLVector3 dir = b - a;
    const F32 len = dir.magVec();
    if (len < 1e-6f) return true;
    const LLVector3 dirn = dir * (1.f / len);

    F32 best_t = 1.f;
    LLVector3 best_n;
    U8 best_layer = LAYER_DECLARED;
    U8 best_prov = PROV_TERRAIN;
    bool hit = false;

    F32 terr_t = 0.f;
    LLVector3 terr_n;
    if (castTerrain(a, b, terr_t, terr_n))
    {
        best_t = terr_t;
        best_n = terr_n;
        hit = true;
    }

    const U32 epoch = ++mCensus.mVisitEpoch;
    S32 cx = (S32)floorf(a.mV[VX] / SS_SHAPES_BUCKET_M);
    S32 cy = (S32)floorf(a.mV[VY] / SS_SHAPES_BUCKET_M);
    S32 cz = (S32)floorf(a.mV[VZ] / SS_SHAPES_BUCKET_M);
    const S32 stepx = dir.mV[VX] > 0.f ? 1 : (dir.mV[VX] < 0.f ? -1 : 0);
    const S32 stepy = dir.mV[VY] > 0.f ? 1 : (dir.mV[VY] < 0.f ? -1 : 0);
    const S32 stepz = dir.mV[VZ] > 0.f ? 1 : (dir.mV[VZ] < 0.f ? -1 : 0);
    const F32 tdx = fabsf(dir.mV[VX]) > 1e-9f ? SS_SHAPES_BUCKET_M / fabsf(dir.mV[VX]) : F32_MAX;
    const F32 tdy = fabsf(dir.mV[VY]) > 1e-9f ? SS_SHAPES_BUCKET_M / fabsf(dir.mV[VY]) : F32_MAX;
    const F32 tdz = fabsf(dir.mV[VZ]) > 1e-9f ? SS_SHAPES_BUCKET_M / fabsf(dir.mV[VZ]) : F32_MAX;
    F32 tmaxx = fabsf(dir.mV[VX]) > 1e-9f ? ((dir.mV[VX] > 0.f ? (F32)(cx + 1) * SS_SHAPES_BUCKET_M - a.mV[VX]
                                                               : a.mV[VX] - (F32)cx * SS_SHAPES_BUCKET_M) / fabsf(dir.mV[VX])) : F32_MAX;
    F32 tmaxy = fabsf(dir.mV[VY]) > 1e-9f ? ((dir.mV[VY] > 0.f ? (F32)(cy + 1) * SS_SHAPES_BUCKET_M - a.mV[VY]
                                                               : a.mV[VY] - (F32)cy * SS_SHAPES_BUCKET_M) / fabsf(dir.mV[VY])) : F32_MAX;
    F32 tmaxz = fabsf(dir.mV[VZ]) > 1e-9f ? ((dir.mV[VZ] > 0.f ? (F32)(cz + 1) * SS_SHAPES_BUCKET_M - a.mV[VZ]
                                                               : a.mV[VZ] - (F32)cz * SS_SHAPES_BUCKET_M) / fabsf(dir.mV[VZ])) : F32_MAX;

    S32 guard = 0;
    while (true)
    {
        auto it = mCensus.mBuckets.find(ss_bucket_key(cx, cy, cz));
        if (it != mCensus.mBuckets.end())
        {
            for (U32 idx : it->second)
            {
                Record& rec = mCensus.mRecords[idx];
                if (rec.mVisit == epoch) continue;
                rec.mVisit = epoch;
                if (rec.mNavRole == NAV_ROLE_EXCLUSION_VOLUME) continue;    // a navmesh cut, never a surface to hit
                if (!include_phantom && rec.mLayer == LAYER_DECLARED_PHANTOM) continue;

                F32 t0 = 0.f, t1 = best_t;
                if (!ss_ray_aabb(a, dirn, rec.mBMin, rec.mBMax, t0, t1)) continue;

                F32 t = 0.f;
                LLVector3 n;
                if (castRecord(rec, a, dirn, t0, best_t, t, n))
                {
                    best_t = t;
                    best_n = n;
                    best_layer = rec.mLayer;
                    best_prov = rec.mProv;
                    hit = true;
                }
            }
        }

        if (tmaxx <= tmaxy && tmaxx <= tmaxz)
        {
            if (tmaxx > 1.f) break;
            cx += stepx;
            tmaxx += tdx;
        }
        else if (tmaxy <= tmaxz)
        {
            if (tmaxy > 1.f) break;
            cy += stepy;
            tmaxy += tdy;
        }
        else
        {
            if (tmaxz > 1.f) break;
            cz += stepz;
            tmaxz += tdz;
        }
        if (++guard > SS_SHAPES_DDA_GUARD) break;
    }

    if (hit)
    {
        out.mHit = true;
        out.mDistance = best_t * len;
        out.mPoint = a + dir * best_t;
        out.mNormal = best_n;
        out.mLayer = best_layer;
        out.mProvenance = best_prov;
    }
    return true;
}

// One record's exact test in segment parameter space; t_max doubles as the
// running best so triangle soups early-out.
bool SSWorldFieldShapes::castRecord(const Record& rec, const LLVector3& a, const LLVector3& dirn,
                                    F32 t_min, F32 t_max, F32& out_t, LLVector3& out_n) const
{
    switch (rec.mClass)
    {
        case Record::CLASS_BOX:
        {
            F32 t0 = t_min, t1 = t_max;
            S32 enter = -1;
            F32 enter_sign = 0.f;
            const LLVector3 rel = a - rec.mCenter;
            for (U32 i = 0; i < 3; ++i)
            {
                const F32 o = rel * rec.mAxes[i];
                const F32 d = dirn * rec.mAxes[i];
                const F32 h = rec.mHalf.mV[i];
                if (fabsf(d) < 1e-9f)
                {
                    if (fabsf(o) > h) return false;
                    continue;
                }
                F32 tn = (-h - o) / d;
                F32 tf = (h - o) / d;
                if (tn > tf) { F32 tmp = tn; tn = tf; tf = tmp; }
                if (tn > t0) { t0 = tn; enter = (S32)i; enter_sign = d > 0.f ? -1.f : 1.f; }
                if (tf < t1) t1 = tf;
                if (t0 > t1) return false;
            }
            if (enter < 0) return false;
            out_t = t0;
            out_n = rec.mAxes[enter] * enter_sign;
            return true;
        }
        case Record::CLASS_SPHERE:
        {
            const LLVector3 rel = a - rec.mCenter;
            F32 o[3], d[3];
            for (U32 i = 0; i < 3; ++i)
            {
                o[i] = (rel * rec.mAxes[i]) / rec.mRadii.mV[i];
                d[i] = (dirn * rec.mAxes[i]) / rec.mRadii.mV[i];
            }
            const F32 A = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            const F32 B = 2.f * (o[0] * d[0] + o[1] * d[1] + o[2] * d[2]);
            const F32 C = o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - 1.f;
            const F32 disc = B * B - 4.f * A * C;
            if (disc < 0.f || A < 1e-12f) return false;
            const F32 sq = sqrtf(disc);
            F32 t = (-B - sq) / (2.f * A);
            if (t < t_min) t = (-B + sq) / (2.f * A);
            if (t < t_min || t > t_max) return false;
            LLVector3 n;
            for (U32 i = 0; i < 3; ++i)
            {
                n += rec.mAxes[i] * ((o[i] + t * d[i]) / rec.mRadii.mV[i]);
            }
            n.normVec();
            out_t = t;
            out_n = n;
            return true;
        }
        case Record::CLASS_CYLINDER:
        {
            const LLVector3 rel = a - rec.mCenter;
            const F32 e0 = rel * rec.mAxisU, e1 = rel * rec.mAxisV, e2 = rel * rec.mAxes[2];
            const F32 d0 = dirn * rec.mAxisU, d1 = dirn * rec.mAxisV, d2 = dirn * rec.mAxes[2];
            const F32 r = rec.mRadius, h = rec.mHalfHeight;
            const F32 A = d0 * d0 + d1 * d1;
            if (A > 1e-12f)
            {
                const F32 B = 2.f * (e0 * d0 + e1 * d1);
                const F32 C = e0 * e0 + e1 * e1 - r * r;
                const F32 disc = B * B - 4.f * A * C;
                if (disc >= 0.f)
                {
                    const F32 sq = sqrtf(disc);
                    for (S32 k = 0; k < 2; ++k)
                    {
                        const F32 t = k == 0 ? (-B - sq) / (2.f * A) : (-B + sq) / (2.f * A);
                        if (t < t_min || t > t_max) continue;
                        const F32 z = e2 + t * d2;
                        if (fabsf(z) > h) continue;
                        LLVector3 n = rec.mAxisU * (e0 + t * d0) + rec.mAxisV * (e1 + t * d1);
                        n.normVec();
                        out_n = n;
                        out_t = t;
                        return true;
                    }
                }
            }
            if (fabsf(d2) > 1e-9f)
            {
                const F32 tc0 = (-h - e2) / d2;
                const F32 tc1 = (h - e2) / d2;
                const F32 t_lo = llmin(tc0, tc1);
                const F32 t_hi = llmax(tc0, tc1);
                const F32 tc = t_lo >= t_min ? t_lo : (t_hi >= t_min && t_hi <= t_max ? t_hi : F32_MAX);
                if (tc <= t_max)
                {
                    const F32 px = e0 + tc * d0, py = e1 + tc * d1;
                    if (px * px + py * py <= r * r)
                    {
                        out_n = rec.mAxes[2] * (d2 > 0.f ? -1.f : 1.f);
                        out_t = tc;
                        return true;
                    }
                }
            }
            return false;
        }
        case Record::CLASS_TRI:
        {
            const std::vector<LLVector3>& tri = rec.tris();
            const size_t nt = tri.size() / 3;
            // <SS:Nexii> The nearest triangle, never the first one met: ss_ray_tri is two-sided, so a hull's far exit face can sit ahead of its near entry face in the soup and returning early handed the caller a wall behind the one it was standing at. Each call is capped at the best t so far - the same shrinking window segmentCast runs its records under - and ss_ray_tri writes out_t/out_n only when it accepts a hit, so the locals survive every rejected triangle.
            F32 tri_t = t_max;
            LLVector3 tri_n;
            bool tri_hit = false;
            for (size_t k = 0; k < nt; ++k)
            {
                F32 t = 0.f;
                LLVector3 n;
                if (ss_ray_tri(tri[k * 3], tri[k * 3 + 1], tri[k * 3 + 2],
                               a, dirn, t_min, tri_t, t, n))
                {
                    tri_t = t;
                    tri_n = n;
                    tri_hit = true;
                }
            }
            if (!tri_hit) return false;
            out_t = tri_t;
            out_n = tri_n;
            return true;
        }
    }
    return false;
}

// Heightfield march: 4 m samples resolved per sample against whichever region
// contains the point - neighbor terrain included, void counts as no ground -
// sign flip, 10 bisection refinements; the normal from finite differences.
bool SSWorldFieldShapes::castTerrain(const LLVector3& a, const LLVector3& b,
                                     F32& out_t, LLVector3& out_normal) const
{
    const LLVector3 d = b - a;
    const F32 len = d.magVec();
    if (len < 1e-4f) return false;

    const S32 steps = llmin(SS_SHAPES_TERRAIN_SAMPLES, (S32)(len / SS_SHAPES_TERRAIN_STEP_M) + 1);
    F32 prev_t = 0.f;
    F32 z = 0.f;
    bool have_prev = false;
    if (ss_terrain_z(a, z))
    {
        if (a.mV[VZ] - z <= 0.f)
        {
            out_t = 0.f;
            out_normal.setVec(0.f, 0.f, 1.f);
            return true;
        }
        have_prev = true;
    }

    for (S32 i = 1; i <= steps; ++i)
    {
        const F32 t = (F32)i / (F32)steps;
        const LLVector3 p = a + d * t;
        F32 land;
        if (!ss_terrain_z(p, land))
        {
            // Over void: no ground, the crossing hunt restarts at the next sample.
            have_prev = false;
            prev_t = t;
            continue;
        }
        const F32 dz = p.mV[VZ] - land;
        if (dz <= 0.f)
        {
            F32 lo = have_prev ? prev_t : t;
            F32 hi = t;
            for (S32 k = 0; k < 10; ++k)
            {
                const F32 mid = 0.5f * (lo + hi);
                const LLVector3 pm = a + d * mid;
                F32 mz;
                if (!ss_terrain_z(pm, mz) || pm.mV[VZ] - mz > 0.f) lo = mid; else hi = mid;
            }
            out_t = 0.5f * (lo + hi);

            const LLVector3 ph = a + d * out_t;
            F32 hx0, hx1, hy0, hy1;
            const bool ok = ss_terrain_z(ph + LLVector3(1.f, 0.f, 0.f), hx0)
                         && ss_terrain_z(ph + LLVector3(-1.f, 0.f, 0.f), hx1)
                         && ss_terrain_z(ph + LLVector3(0.f, 1.f, 0.f), hy0)
                         && ss_terrain_z(ph + LLVector3(0.f, -1.f, 0.f), hy1);
            if (ok)
            {
                out_normal.setVec(-(hx0 - hx1), -(hy0 - hy1), 2.f);
                out_normal.normVec();
            }
            else
            {
                out_normal.setVec(0.f, 0.f, 1.f);
            }
            return true;
        }
        have_prev = true;
        prev_t = t;
    }
    return false;
}

// Eight horizontal wall distances - the exact form of the soundscape's side probes.
bool SSWorldFieldShapes::wallProfile(const LLVector3& pos, F32 range_m, F32 out[8],
                                     bool include_phantom)
{
    SegmentHit probe_hit;
    if (!segmentCast(pos, pos + LLVector3(1.f, 0.f, 0.f) * range_m, probe_hit, include_phantom))
    {
        return false;
    }
    for (S32 i = 0; i < 8; ++i)
    {
        const F32 ang = (F32)i * (F_PI / 4.f);
        SegmentHit h;
        if (segmentCast(pos, pos + LLVector3(cosf(ang), sinf(ang), 0.f) * range_m, h, include_phantom)
            && h.mHit)
        {
            out[i] = h.mDistance;
        }
        else
        {
            out[i] = range_m;
        }
    }
    return true;
}

bool SSWorldFieldShapes::censusCurrent() const
{
    // A census being refreshed stays current for a bounded grace: the pending one replaces it whole within a few frames.
    return mCensus.mRegionHandle != 0 && (mNow - mCensus.mBuildTime) <= SS_SHAPES_MAX_AGE + (mBuilding ? SS_SHAPES_MAX_AGE : 0.0);
}

// Records overlapping a world-space box, unfiltered - the tiles raster and
// any later downstream walk own their layer and DYNAMIC policy. Linear scan:
// census-rebuild cadence, thousands of records at most.
void SSWorldFieldShapes::forEachRecord(const LLVector3& bmin, const LLVector3& bmax, const RecordFn& fn) const
{
    for (const Record& rec : mCensus.mRecords)
    {
        if (rec.mBMax.mV[VX] < bmin.mV[VX] || rec.mBMin.mV[VX] > bmax.mV[VX]) continue;
        if (rec.mBMax.mV[VY] < bmin.mV[VY] || rec.mBMin.mV[VY] > bmax.mV[VY]) continue;
        if (rec.mBMax.mV[VZ] < bmin.mV[VZ] || rec.mBMin.mV[VZ] > bmax.mV[VZ]) continue;
        fn(rec);
    }
}

// The census overlay: record boxes by layer and provenance, distance-thinned
// like the rest of the debug views.
void SSWorldFieldShapes::renderDebug()
{
    if (mCensus.mRecords.empty()) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::LINES);

    for (const Record& rec : mCensus.mRecords)
    {
        const LLVector3 centre = (rec.mBMin + rec.mBMax) * 0.5f;
        if ((centre - cam).magVec() > 256.f) continue;

        if (rec.mLayer == LAYER_DECLARED_PHANTOM)
        {
            gGL.color4f(1.f, 0.82f, 0.3f, 0.45f);
        }
        else if (rec.mLayer == LAYER_INVISIBLE_SOLID)
        {
            // The builder's collision proxy - invisible in the world, drawn
            // bright here because it is load-bearing.
            gGL.color4f(0.95f, 0.95f, 1.f, 0.65f);
        }
        else
        {
            switch (rec.mProv)
            {
                case PROV_EXACT:        gGL.color4f(0.7f, 0.9f, 1.f, 0.45f); break;
                case PROV_HULL:         gGL.color4f(0.5f, 0.8f, 0.9f, 0.45f); break;
                case PROV_TESSELLATED:  gGL.color4f(0.5f, 1.f, 0.7f, 0.45f); break;
                case PROV_BBOX:         gGL.color4f(1.f, 0.6f, 0.3f, 0.45f); break;
                case PROV_RENDER:       gGL.color4f(0.95f, 0.95f, 0.55f, 0.45f); break;
                default:                gGL.color4f(1.f, 0.4f, 1.f, 0.45f); break;
            }
        }

        const LLVector3& mn = rec.mBMin;
        const LLVector3& mx = rec.mBMax;

        if (rec.mClass == Record::CLASS_BOX)
        {
            // The true oriented frame - the AABB alone would read as zero rotation.
            const LLVector3 hx = rec.mAxes[0] * rec.mHalf.mV[VX];
            const LLVector3 hy = rec.mAxes[1] * rec.mHalf.mV[VY];
            const LLVector3 hz = rec.mAxes[2] * rec.mHalf.mV[VZ];
            const LLVector3 p[8] = {
                rec.mCenter - hx - hy - hz, rec.mCenter + hx - hy - hz,
                rec.mCenter + hx + hy - hz, rec.mCenter - hx + hy - hz,
                rec.mCenter - hx - hy + hz, rec.mCenter + hx - hy + hz,
                rec.mCenter + hx + hy + hz, rec.mCenter - hx + hy + hz,
            };
            static const S32 edges[12][2] = {
                {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
            };
            for (S32 e = 0; e < 12; ++e)
            {
                gGL.vertex3fv(p[edges[e][0]].mV);
                gGL.vertex3fv(p[edges[e][1]].mV);
            }
        }
        else if (rec.mClass == Record::CLASS_CYLINDER)
        {
            // Axis frame: two cap rings and one cross spoke.
            const LLVector3 ax = rec.mAxes[2] * rec.mHalfHeight;
            const LLVector3 c0 = rec.mCenter - ax;
            const LLVector3 c1 = rec.mCenter + ax;
            LLVector3 prev0, prev1;
            for (S32 k = 0; k <= 8; ++k)
            {
                const F32 ang = (F32)k * (F_TWO_PI / 8.f);
                const LLVector3 off = (rec.mAxisU * cosf(ang) + rec.mAxisV * sinf(ang)) * rec.mRadius;
                const LLVector3 q0 = c0 + off;
                const LLVector3 q1 = c1 + off;
                if (k > 0)
                {
                    gGL.vertex3fv(prev0.mV); gGL.vertex3fv(q0.mV);
                    gGL.vertex3fv(prev1.mV); gGL.vertex3fv(q1.mV);
                    if (k == 4)
                    {
                        gGL.vertex3fv(q0.mV); gGL.vertex3fv(q1.mV);
                    }
                }
                prev0 = q0;
                prev1 = q1;
            }
        }
        else if (rec.mClass == Record::CLASS_TRI)
        {
            // The actual physics-detail soup - an AABB would hide tapered
            // edges behind a box. Overlay-capped so dense soups stay cheap.
            const std::vector<LLVector3>& tri = rec.tris();
            const size_t nt = tri.size() / 3;
            if (nt <= SS_SHAPES_DEBUG_TRIS)
            {
                for (size_t k = 0; k < nt; ++k)
                {
                    gGL.vertex3fv(tri[k * 3].mV);     gGL.vertex3fv(tri[k * 3 + 1].mV);
                    gGL.vertex3fv(tri[k * 3 + 1].mV); gGL.vertex3fv(tri[k * 3 + 2].mV);
                    gGL.vertex3fv(tri[k * 3 + 2].mV); gGL.vertex3fv(tri[k * 3].mV);
                }
            }
            else
            {
                ss_debug_aabb(mn, mx);
            }
        }
        else if (rec.mClass == Record::CLASS_SPHERE)
        {
            // Three great circles in the record's own frame: an ellipsoid reads as one, not as its box.
            static const S32 SEGS = 24;
            for (S32 ring = 0; ring < 3; ++ring)
            {
                const S32 ia = (ring + 1) % 3, ib = (ring + 2) % 3;
                LLVector3 prev = rec.mCenter + rec.mAxes[ia] * rec.mRadii.mV[ia];
                for (S32 k = 1; k <= SEGS; ++k)
                {
                    const F32 a = F_TWO_PI * (F32)k / (F32)SEGS;
                    const LLVector3 p = rec.mCenter + rec.mAxes[ia] * (cosf(a) * rec.mRadii.mV[ia]) + rec.mAxes[ib] * (sinf(a) * rec.mRadii.mV[ib]);
                    gGL.vertex3fv(prev.mV);
                    gGL.vertex3fv(p.mV);
                    prev = p;
                }
            }
        }
        else
        {
            ss_debug_aabb(mn, mx);
        }
    }

    gGL.end();
}
