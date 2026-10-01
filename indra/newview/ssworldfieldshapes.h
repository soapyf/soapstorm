/**
 * @file ssworldfieldshapes.h
 * @brief Atmo Magic: exact declared-shape queries over the census of physics shapes.
 *
 *        The column-span store quantizes indoor geometry to 0.25 m cells; the
 *        consumers that need wall-exact answers (the soundscape's live probes,
 *        shockwave and thunder paths, windflow apertures) read here instead:
 *        one census of the shapes objects declare - physics shape type and
 *        phantom status read respectively, phantom a layer not a filter -
 *        answered by exact segment/ray casts against analytic prims, physics
 *        detail tessellations and mesh decomposition hulls.
 *
 *        The census is build-scoped (doc/atmo_magic_worldfield_competition.md
 *        7.1): a snapshot taken on the main thread inside a query envelope,
 *        rasterized lazily per query, never a persistent mirror. Terrain is
 *        answered analytically off the heightfield at query time. Unknown shape
 *        types of solid, non-temporary roots are requested through the same
 *        batched GetObjectPhysicsData path the physics overlay uses (owner
 *        decision 2026-09-10); until the answer lands they sit on conservative
 *        OBBs. Linksets flagged with a static navmesh role get their exact role
 *        from one ObjectNavMeshProperties request per region.
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

#ifndef SS_WORLDFIELD_SHAPES_H
#define SS_WORLDFIELD_SHAPES_H

#include "llquaternion.h"
#include "llsingleton.h"
#include "v3dmath.h"
#include "v3math.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class LLViewerRegion;

class SSWorldFieldShapes : public LLSingleton<SSWorldFieldShapes>
{
    LLSINGLETON_EMPTY_CTOR(SSWorldFieldShapes);

public:
    // Phantom is a layer, not an exclusion: which consumers may read a
    // declared shape is their call (a drop is stopped by a phantom deck,
    // walkability is not). Provenance records how the shape was derived so
    // consumers can filter by trust - walkability will exclude BBOX/UNFETCHED.
    enum ELayer : U8
    {
        LAYER_DECLARED = 0,         // non-phantom declared geometry
        LAYER_DECLARED_PHANTOM,     // phantom - visible, declared, non-colliding
        LAYER_INVISIBLE_SOLID,      // invisible non-phantom: the builder's collision proxy, solid for every consumer
        LAYER_COUNT
    };

    // <SS:Nexii> The sim's navmesh role for a linkset (ObjectNavMeshProperties), mirrored on every record of the linkset so the navmesh can honour it: static obstacles rasterize unwalkable, exclusion volumes cut walkable area, everything else contributes as geometry. Only linksets carrying FLAGS_AFFECTS_NAVMESH are asked; legacy linksets read NAV_ROLE_UNKNOWN.
    enum ENavRole : U8
    {
        NAV_ROLE_UNKNOWN = 0,
        NAV_ROLE_WALKABLE,
        NAV_ROLE_STATIC_OBSTACLE,
        NAV_ROLE_DYNAMIC_OBSTACLE,
        NAV_ROLE_MATERIAL_VOLUME,
        NAV_ROLE_EXCLUSION_VOLUME,
        NAV_ROLE_DYNAMIC_PHANTOM
    };

    enum EProvenance : U8
    {
        PROV_EXACT = 0,             // closed-form prim (box/sphere/cylinder spec)
        PROV_HULL,                  // convex hull points / decomposition hull
        PROV_TESSELLATED,           // physics-detail volume tessellation
        PROV_BBOX,                  // conservative box of the prim bounds
        PROV_UNFETCHED,             // shape type or mesh physics not arrived yet
        PROV_RENDER,                // no declared shape: the prim's own volume is the geometry
        PROV_TERRAIN                // the heightfield, answered analytically
    };

    struct SegmentHit
    {
        bool mHit = false;          // geometry met between a and b
        F32 mDistance = 0.f;        // metres from a
        LLVector3 mPoint;
        LLVector3 mNormal;
        U8 mLayer = LAYER_DECLARED;
        U8 mProvenance = PROV_EXACT;
    };

    // Per-frame maintenance: staleness checks against the query anchor, the
    // debounced markDirty fan-out, gate off = census dropped entirely.
    void update();

    // The edit fan-out, forwarded from SSWorldField::markDirty; rebuilds are
    // debounced, never per edit.
    static void markDirty(const LLVector3& pos_agent, F32 radius);

    // Exact segment cast a->b. Returns false when the census has no answer
    // (gate off, no census) and the caller keeps its own raycast; a false
    // out.mHit with a true return is a genuine miss along the segment.
    bool segmentCast(const LLVector3& a, const LLVector3& b, SegmentHit& out,
                     bool include_phantom = true);

    // Eight horizontal wall distances at ear height - the exact form of the
    // soundscape's cardinal side probes. False when the census has no answer.
    bool wallProfile(const LLVector3& pos, F32 range_m, F32 out[8],
                     bool include_phantom = true);

    // One declared shape in world space. Analytic classes carry their frame;
    // TRI records carry a baked world-space triangle soup (3 verts per tri),
    // which is what mesh decompositions and physics-detail tessellations boil
    // down to. World-space baking costs memory and buys branch-free queries.
    struct Record
    {
        enum EClass : U8 { CLASS_BOX = 0, CLASS_SPHERE, CLASS_CYLINDER, CLASS_TRI } mClass = CLASS_BOX;

        LLVector3 mCenter;
        LLVector3 mAxes[3];         // BOX/SPHERE: prim local axes in world space
        LLVector3 mHalf;            // BOX: half extents per axis
        LLVector3 mRadii;           // SPHERE: per-axis radii (ellipsoid)
        LLVector3 mAxisU, mAxisV;   // CYLINDER: frame perpendicular to mAxes[2]
        F32 mRadius = 0.f;          // CYLINDER
        F32 mHalfHeight = 0.f;      // CYLINDER

        // <SS:Nexii> CLASS_TRI: world-space soup, shared with the part cache so a rebuild that reuses a part copies a pointer, not two million vertices. Read through tris().
        std::shared_ptr<const std::vector<LLVector3> > mTri;
        const std::vector<LLVector3>& tris() const { static const std::vector<LLVector3> empty; return mTri ? *mTri : empty; }

        LLVector3 mBMin, mBMax;         // world AABB
        U8 mLayer = LAYER_DECLARED;
        U8 mProv = PROV_EXACT;
        U32 mVisit = 0;                 // per-query dedupe stamp
        bool mDynamic = false;          // root moved within the settle window: query-time only, never store-bound
        U8 mNavRole = NAV_ROLE_UNKNOWN; // the linkset's sim navmesh role, when it carries one
        bool mNoPhysics = false;        // <SS:Nexii> physics shape type NONE: render geometry for rain and cover, nothing for the navmesh. Distinct from an unanswered shape type, which stays walkable as its volume. [interaction: SSNavMesh navIgnores]
    };

    // Census read access for downstream rasters (the 3D tile lattice): every
    // record whose AABB meets the world-space box, unfiltered - the caller
    // owns layer and DYNAMIC policy. Linear scan; census-rebuild cadence.
    typedef std::function<void(const Record&)> RecordFn;
    void forEachRecord(const LLVector3& bmin, const LLVector3& bmax, const RecordFn& fn) const;

    // The envelope the resident census was built around.
    const LLVector3& censusAnchor() const { return mCensus.mAnchor; }
    // <SS:Nexii> The envelope radius: the query consumers' SSWorldFieldShapesRange, widened to SSNavMeshRange while the navmesh is on - the navmesh is a stable surface the weather reads, not a bubble around the camera, so it needs the census to reach every column it keeps. [interaction: SSNavMesh schedule]
    static F32 envelopeRange();

    // The census build's stamp - downstream rasters key their schedules to it.
    U64 censusStamp() const { return (U64)(mCensus.mBuildTime * 1000.0); }

    // Stats
    bool censusCurrent() const;
    bool building() const { return mBuilding; }
    S32 cachedPartCount() const { return (S32)mParts.size(); }
    // How the last build decided rest: the sim's pathfinding role, the ROC ledger, or watching the object settle.
    S32 restByFlag() const { return mRestByFlag; }
    S32 restByLedger() const { return mRestByLedger; }
    S32 restByWatching() const { return mRestByWatching; }
    S32 navRoleCount() const { return (S32)mNavRoles.size(); }
    S32 physicsRequested() const { return mPhysicsRequested; }
    bool navRolesInFlight() const { return mRolesInFlight; }
    S32 recordCount() const { return (S32)mCensus.mRecords.size(); }
    S32 triangleCount() const { return mTriCount; }
    F32 lastBuildMS() const { return mLastBuildMS; }

    // The census overlay: record boxes by layer and provenance - view 6 of
    // SSWorldFieldDebugView.
    void renderDebug();

    // <SS:Nexii> The "why is this object ignored" dump: walks a linkset and, for every part, logs the inputs the census decides on (flags, physics type, mesh physics state, hidden test, rest ladder rung, navmesh role, envelope) and re-evaluates addPart's own skip rules without touching any state, then what the part cache holds for it and how the navmesh treats those records. Returns a one-line verdict for the root; the detail goes to out. [interaction: addPart, SSNavMesh::dumpAt]
    std::string dumpObject(const class LLViewerObject* rootp, std::vector<std::string>& out) const;
    // Every census record whose box meets the given box, with how the navmesh treats it (records carry no object id; pair with dumpObject for that).
    void dumpRecordsAt(const LLVector3& bmin, const LLVector3& bmax, std::vector<std::string>& out) const;

private:
    // The build-scoped snapshot: records plus the 64 m bucket grid over them.
    // One census resident at a time - the queries come from one listener.
    struct Census
    {
        U64 mRegionHandle = 0;
        LLVector3 mAnchor;
        F64 mBuildTime = 0.0;
        std::vector<Record> mRecords;
        std::unordered_map<U64, std::vector<U32> > mBuckets;
        U32 mVisitEpoch = 0;
    };

    bool needsRebuild(LLViewerRegion* regionp) const;
    void beginBuild(LLViewerRegion* regionp);
    void continueBuild(LLViewerRegion* regionp);
    void finishBuild();
    void prunePartCache();
    void pruneNavRoles();
    U64 partSignature(class LLVOVolume* vov, const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale,
                      S32 ptype, bool shape_known, bool phantom, bool hidden) const;
    void addPart(class LLVOVolume* vov);
    void trackRest(const class LLViewerObject* rootp, bool& out_dynamic);
    void addOBB(const LLVector3& center, const LLQuaternion& rot, const LLVector3& half, U8 layer, U8 prov);
    void addEllipsoid(const LLVector3& center, const LLQuaternion& rot, const LLVector3& radii, U8 layer, U8 prov);
    void addCylinder(const LLVector3& center, const LLQuaternion& rot, F32 radius, F32 half_height, U8 layer, U8 prov);
    bool addTriangles(const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale,
                      const std::vector<LLVector3>& local_soup, U8 layer, U8 prov);
    void addRecord(Record& rec);
    void requestNavRoles();
    static void onNavRoles(U32 request_id, S32 status, const std::shared_ptr<class LLPathfindingObjectList>& list, U64 region_handle);
    bool castRecord(const Record& rec, const LLVector3& a, const LLVector3& dirn,
                    F32 t_min, F32 t_max, F32& out_t, LLVector3& out_n) const;
    bool castTerrain(const LLVector3& a, const LLVector3& b,
                     F32& out_t, LLVector3& out_normal) const;

    Census mCensus;
    LLVector3 mDirtyCenter;
    F32 mDirtyRadius = 0.f;
    F64 mDirtyAt = 0.0;
    bool mDirty = false;

    // <SS:Nexii> The DYNAMIC classifier's memory (doc/atmo_magic_worldfield_competition.md 11):
    // per-root transform history that outlives the build-scoped census, so an object at rest
    // across the settle window reads at-rest and a mover reads DYNAMIC - excluded from any
    // store-bound raster, resolved at query time, never bumping a geometry serial.
    struct RestState
    {
        LLVector3d mPos;
        LLQuaternion mRot;
        LLVector3 mScale;
        F64 mMovedAt = 0.0;
        bool mDynamic = true;
        bool mSeen = false;         // sighted by the scan in progress (pruning)
        bool mKnown = false;        // has a history at all: the first-sighting test, never the pruning flag
    };
    std::unordered_map<LLUUID, RestState> mRest;
    bool mBuildDynamic = false;     // the part being filed rides its root's rest state
    U8 mBuildNavRole = 0;           // and its root's navmesh role
    bool mBuildNoPhysics = false;   // the part being baked has physics shape type NONE

    // Navmesh roles per linkset root, from ObjectNavMeshProperties; requested once per region when a flagged root
    // turns up without one, re-requested no sooner than a minute later.
    // <SS:Nexii> Each role remembers the region it was fetched for, because the reply is region-wide - far more linksets than the census ever sights - so pruning it to the census would drop roles the next envelope still needs and buy a re-request; pruning by region instead bounds the table across region hops and keeps every role LLWorld still holds a region for. [interaction: SSNavMesh roles]
    struct NavRole
    {
        U8 mRole = 0;
        U64 mRegion = 0;
    };
    std::unordered_map<LLUUID, NavRole> mNavRoles;
    bool mRolesWanted = false;
    bool mRolesInFlight = false;
    bool mRolesChanged = false;
    F64 mRolesRequestedAt = -1000.0;
    U64 mRolesRegion = 0;
    S32 mPhysicsRequested = 0, mPendPhysicsRequested = 0;

    // <SS:Nexii> The part cache and the time-sliced build. A rebuild used to re-tessellate every part in the envelope on one frame (700 ms spikes on dense builds); now every part's baked records live here under a signature of everything that shaped them (transform, volume params, physics type, phantom, hidden, mesh decomposition state), a rebuild reuses any part whose signature holds, and the scan itself fills mPending over frames under SSWorldFieldShapesBudgetMS and swaps in whole - consumers read the previous census meanwhile. [interaction: navmesh schedule keys off censusStamp, which only moves at the swap]
    struct PartCache
    {
        U64 mSig = 0;
        std::vector<Record> mRecords;
        S32 mTris = 0;
        F64 mSeen = 0.0;
    };
    std::unordered_map<LLUUID, PartCache> mParts;
    PartCache* mCurrentPart = nullptr;
    Census mPending;
    bool mBuilding = false;
    S32 mScanIndex = 0;
    S32 mPendingTris = 0;
    F32 mBuildMS = 0.f;
    S32 mRestByFlag = 0, mRestByLedger = 0, mRestByWatching = 0;         // the build in progress
    S32 mPendRestByFlag = 0, mPendRestByLedger = 0, mPendRestByWatching = 0;

    F64 mNow = 0.0;
    F32 mLastBuildMS = 0.f;
    S32 mTriCount = 0;
};

#endif
