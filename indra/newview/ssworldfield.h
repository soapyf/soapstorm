/**
 * @file ssworldfield.h
 * @brief Atmo Magic: the shared world field.
 *
 *        One region-anchored capture of the world's solid structure, shared by
 *        every system that currently captures its own. A tile is captured as a
 *        stack of horizontal Z bands; each band is one top-down ortho depth
 *        pass whose frustum clips everything above the band, so per column and
 *        per band it yields the highest surface inside that band - the
 *        band-sliced form of a depth peel, produced with the exact machinery
 *        the rain shadow and wind captures already use.
 *
 *        The store is spans-shaped: per column, per band, a surface altitude
 *        and surface flags. Everything downstream is a materialised view over
 *        it. The first view is SURFACE_TOP - the landing-surface grid the
 *        surface field, runoff and snow read - produced with the same shape
 *        and serial semantics as SSRainShadowMap::SurfaceGrid so consumers
 *        migrate by swapping their source. COVERAGE (indoor vs outdoor,
 *        burial depth) reads the band stack directly.
 *
 *        Captures are staged across frames like the wind flowmap's build, one
 *        band per step, and a prim edit re-peels only the dirty rectangle.
 *        Tiles are built only where a channel is claimed; nothing runs for a
 *        region nobody asked about.
 *
 *        See doc/atmo_magic_worldfield.md.
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

#ifndef SS_WORLDFIELD_H
#define SS_WORLDFIELD_H

#include "llrendertarget.h"
#include "llsingleton.h"
#include "ssrainshadow.h"
#include "v3math.h"
#include "v3dmath.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

class SSWorldField : public LLSingleton<SSWorldField>
{
    LLSINGLETON_EMPTY_CTOR(SSWorldField);

public:
    // What a consumer wants out of the store. The capture's band depth and
    // later its probe passes are the union of every currently claimed
    // channel's needs; today that is SURFACE_TOP alone.
    enum class EChannel
    {
        SURFACE_TOP = 0,
        SOLID_VOLUME_3D,
        COVERAGE,
        DRAINAGE_NETWORK,
        WALKABLE,
        ACOUSTIC
    };

    // A consumer's stake in a channel. Ref-counted per (region, channel);
    // dropping the last handle stops the region paying for the channel.
    // Deliberately a handle rather than a bool: a prototype that wants a
    // channel for a minute of testing drops it and the field notices.
    class Interest
    {
    public:
        Interest() = default;

        explicit operator bool() const { return mHold != nullptr; }

    private:
        friend class SSWorldField;
        explicit Interest(std::shared_ptr<void> hold) : mHold(std::move(hold)) {}

        std::shared_ptr<void> mHold;
    };

    Interest claim(U64 region_handle, EChannel channel);

    // The one edit fan-out. Settled prim edits land here; the field marks the
    // tile's dirty rectangle and the re-peel is scissored to it.
    static void markDirty(const LLVector3& pos_agent, F32 radius);

    void clear();

    void update();

    // The landing-surface view - SSRainShadowMap::buildSurfaceGrid's exact
    // contract (region-anchored n x n grid, first thing a falling drop meets,
    // water and heightmap fallbacks included, geometry serial for the retrace
    // gate). Consumers switch sources without changing anything else.
    bool buildSurfaceGrid(U64 region_handle, S32 n, SSRainShadowMap::SurfaceGrid& out);

    // <SS:Nexii> The TRUE-GROUND reference for the wind profile: per column, the real ground the boundary layer is measured from - the topmost real surface (olivine/green-blue ground tiles), with TALL-STRUCTURE columns (purple tiles: a building or skybox far above the surrounding ground) voided and gap-filled from the surrounding true ground; water columns hold the sea plane. Unlike the ancient Linden terrain heightmap (resolveHeightRegion), this reads the worldfield's real-geometry captures, so streets, mesh terrain and prim ground built ON TOP of the old heightmap become the ground. n is the output resolution (region-anchored like buildSurfaceGrid); false when the tile is not yet valid.
    bool buildTrueGround(U64 region_handle, S32 n, std::vector<F32>& out);

    void validTiles(std::vector<std::pair<U64, U32> >& out) const;

    // The topmost surface at a point, absolute Z. False if the column is
    // unmapped (tile absent, not yet built, or fully sky).
    bool surfaceTop(const LLVector3& pos_agent, F32& z, U8& flags) const;

    // Whether the point has structure above it (sheltered), and if so how far
    // up to the column's sky-open top - the burial measure the soundscape
    // currently derives from the wind tile's column top.
    bool coverageAt(const LLVector3& pos_agent, bool& outdoor, F32& buried_depth) const;

    // The richer form the soundscape's probe cycle wants: whether anything
    // stands over the point at all, the nearest such surface's altitude (the
    // space's ceiling, to band precision), and the column's sky-open top.
    // Burial (the build stacked between ceiling and sky) is their difference,
    // floor-count aware where the single column-top subtraction never was.
    // False when no valid tile covers the point; the caller keeps its
    // raycasts for that.
    bool coverageDetail(const LLVector3& pos_agent, bool& covered,
                        F32& ceiling_z, F32& column_top_z) const;

    // <SS:Nexii> Air connectivity, the flood-fill pass of the worldfield design: every air cell of a tile's band stack is labelled by whether it can actually be reached from the sky or the tile's horizontal borders. A courtyard, an underpass and the gap under a skyway are OUTSIDE - the flood walks in; a sealed room is INTERIOR. Solved on the general worker queue from a snapshot of the band stack, so a build committing on the main thread never races the walk; stored against the tile's geometry serial so a stale answer is never served after an edit. The same job carries the occlusion depth: each outside-connected air cell's graph distance to the frontier, for the "how enclosed is this point" consumers (the sparse-air-solve and acoustic occlusion figures).
    enum EAirLabel : U8
    {
        AIR_SOLID = 0,      // a surface occupies the band here
        AIR_OUTSIDE,        // air the flood reached from sky or border
        AIR_INTERIOR,       // air the flood could not reach
        AIR_UNKNOWN         // no tile, no labels yet, or stale
    };
    U8 airLabelAt(const LLVector3& pos_agent) const;

    // The occlusion depth behind airLabelAt: graph distance in cells from the
    // point's air band-cell to the nearest AIR_OUTSIDE cell (0 when the point
    // itself is outside-connected air), or AIR_DEPTH_UNREACHED when the cell
    // is interior, off-tile, or the labels are not current. Band and
    // horizontal steps count one cell each, so the figure is a graph hop
    // count over the store's own grid, not metres.
    static constexpr U32 AIR_DEPTH_UNREACHED = 0xFFFFu;
    U32 airDepthAt(const LLVector3& pos_agent) const;

    // The share of a tile's air cells the flood actually labelled - 1.0 once
    // the labels are current, less before the first flood or after an edit.
    F32 airCoverage(U64 region_handle) const;

    // <SS:Nexii> Drainage topology over one landing-surface grid - the DRAINAGE_NETWORK channel core, materialised synchronously over whatever SurfaceGrid the caller already holds (the surface field's geometry, at its own n). A Barnes priority flood fills every depression to its spill elevation; a cell below that level is a pool member (standing water, the figure that retires the surface field's local dips check); flow directions are D8 down the *filled* surface, so a pool's water drains toward its spill outlet rather than into its own floor. Nothing is cached here: the grid carries the geometry serial and the caller already gates retraces on it. Per-span levels wait on the multi-peel store; this is the landing-surface level the design ships first.
    struct Drainage
    {
        std::vector<F32> mSpill;      // fill elevation per cell, NODATA where unmapped
        std::vector<U8> mPool;        // 1 = cell sits under its spill level (depression member)
        std::vector<U8> mD8;          // outflow cell on the filled surface, 3x3-indexed
                                      // ((dy+1)*3 + (dx+1)); 4 = no outflow (sink or drain edge)
    };
    bool buildDrainage(const SSRainShadowMap::SurfaceGrid& grid, Drainage& out) const;

    bool tileValid(U64 region_handle) const;
    U32 geometrySerial(U64 region_handle) const;

    // Stats
    S32 tileCount() const { return (S32)mTiles.size(); }
    U32 captureCount() const { return mCaptureCount; }
    U32 dirtyCaptureCount() const { return mDirtyCaptures; }
    F32 lastCaptureMS() const { return mLastCaptureMS; }
    F32 bandHeight() const;
    S32 bandCount() const;
    S32 resolution() const;
    F64 tileAge(const LLVector3& pos_agent) const;
    S32 effectiveBands(const LLVector3& pos_agent) const;

    // <SS:Nexii> The world field's own overlay: what the capture saw, what the air flood decided, and what the drainage pass reads - view picked by SSWorldFieldDebugView, distance-thinned like the wind flowmap's overlay.
    void renderDebug();

private:
    static constexpr S32 MAX_BANDS = 24;
    static constexpr U32 MAX_TILES = 4;
    static constexpr F64 DIRTY_MIN_INTERVAL = 2.0;
    static constexpr F64 BAND_MIN_INTERVAL = 0.05;
    // This many consecutive empty bands end a full build: bands are swept
    // bottom-up, so empty runs only occur above all content the ceiling
    // setting covers. Skyboxes above the resulting ceiling are invisible to
    // the field until SSWorldFieldCeiling is raised - the same practical
    // shape as the wind map's band.
    static constexpr S32 EMPTY_BANDS_TO_STOP = 3;

    struct Tile
    {
        U64 mRegionHandle = 0;

        S32 mRes = 0;
        F32 mCell = 0.f;

        S32 mBandCount = 0;        // effective bands; bands [0, mBandCount) are live
        F32 mBandHeight = 16.f;

        // Per band, per column: the highest surface inside the band, absolute
        // Z, NO_SURFACE where the band is open there. Flat [band][y * res + x].
        std::vector<F32> mBandTop;
        std::vector<U8> mBandFlags;

        // Dirty rectangle in cells; empty = whole tile. The re-peel renders
        // only this sub-frustum and splices only these columns.
        S32 mDirtyX0 = 0, mDirtyY0 = 0, mDirtyX1 = 0, mDirtyY1 = 0;

        // Air connectivity labels, EAirLabel per band-cell, same layout as
        // mBandTop. Valid only while mAirSerial matches mGeomSerial - an edit
        // invalidates them until the flood re-runs on the next commit.
        std::vector<U8> mAirLabel;

        // Air-connectivity occlusion depth, same layout and validity gate as
        // mAirLabel: per air band-cell the shortest 6-connected distance in
        // in cells to an AIR_OUTSIDE cell (0 on the frontier itself). Interior
        // cells never reach the outside through air, so they keep
        // AIR_DEPTH_UNREACHED - "maximally enclosed" is exactly the number the
        // sealed-room consumers want. Produced by the same worker job as the
        // labels, stored together; distances saturate at UNREACHED - 1 so the
        // sentinel stays exclusive to "not walked".
        std::vector<U16> mAirDepth;
        U32 mAirSerial = 0;

        U32 mGeomSerial = 1;

        S32 mBandTarget = 0;       // bands the next/active build runs

        F64 mCaptureTime = 0.0;
        F64 mLastTouched = 0.0;
        bool mDirty = false;
        bool mValid = false;
    };

    struct Build
    {
        bool mActive = false;
        U64 mRegionHandle = 0;
        S32 mBand = 0;
        bool mRectOnly = false;    // re-peeling the dirty rectangle only
        S32 mRectX0 = 0, mRectY0 = 0, mRectX1 = 0, mRectY1 = 0;
        S32 mRectRes = 0;          // square capture resolution covering the rect
        F32 mRectHalf = 0.f;       // world half-extent of the rect frustum
        LLVector3 mRectCentre;     // agent-space centre of the rect
        std::vector<F32> mDepth;   // last band's depth readback
        S32 mEmptyRun = 0;         // consecutive empty bands seen by the live build
        bool mChanged = false;     // any spliced column differed from what was stored
        bool mJustCaptured = false;// a band was rendered and its readback landed; apply it next step
    };

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    const Tile* tileAt(const LLVector3& pos_agent) const;

    bool needsBuild(const Tile& tile) const;
    Tile* pickBuildTarget();

    bool advanceBuild();

    bool captureBand(Tile& tile);
    void applyBand(Tile& tile);
    void commitBuild(Tile& tile);

    // Post the connectivity flood for a committed tile to the general worker
    // queue. Snapshot in, labels out; the completion stores them only if the
    // tile's geometry serial has not moved underneath the walk.
    void scheduleFlood(Tile& tile);

    void evict();

    static F32 bandTopZ(S32 band, F32 band_height) { return (F32)(band + 1) * band_height; }

    std::map<U64, Tile> mTiles;
    Build mBuild;

    // Registered when the surface-field source switch is on; the field reads
    // the setting rather than holding a handle, so the switch is one settings
    // entry and the wet field's plumbing stays untouched. The interest
    // refcounts live in file statics so a handle's deleter stays valid for
    // the process's life regardless of singleton teardown order.
    bool surfaceTopDemanded() const;

    LLRenderTarget mTarget;

    // <SS:Nexii> One band readback in flight, served by SSGLReadback. mTarget must not be re-rendered (or torn down) until the outstanding read lands: advanceBuild()/capture gate on mReadbackPending, and a clear requested mid-read defers to the read's completion.
    bool mReadbackPending = false;
    bool mClearPending = false;

    // One flood in flight at a time; a tile committing while one runs simply
    // schedules again on its own commit. A generation counter, not a pointer,
    // decides whether a finished walk still applies - clear() and eviction
    // both move it.
    bool mFloodBusy = false;
    U32 mFloodGeneration = 0;

    F64 mNow = 0.0;
    F64 mLastBandAt = 0.0;
    F32 mLastCaptureMS = 0.f;
    U32 mCaptureCount = 0;
    U32 mDirtyCaptures = 0;
};

#endif
