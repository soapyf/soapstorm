/**
 * @file ssrainshadow.h
 * @brief Atmo Magic: rain shadow depth maps.
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

#ifndef SS_RAINSHADOW_H
#define SS_RAINSHADOW_H

#include "llrendertarget.h"
#include "llsingleton.h"
#include "v3math.h"
#include "v3dmath.h"
#include "v4coloru.h"

#include <map>
#include <vector>

class LLViewerObject;
class LLViewerRegion;

class SSRainShadowMap : public LLSingleton<SSRainShadowMap>
{
    LLSINGLETON_EMPTY_CTOR(SSRainShadowMap);

public:
    void capture();

    void markDirty(const LLVector3& pos_agent, F32 radius);
    void clearCache();

    S32 tileCount() const { return (S32)mTiles.size(); }
    S32 voidTileCount() const { return (S32)mVoidTiles.size(); }
    U32 resolution() const;

    U32 captureCount() const { return mCaptureCount; }
    U32 dirtyCaptureCount() const { return mDirtyCaptures; }
    U32 dirtyTileCount() const;
    F32 lastCaptureMS() const { return mLastCaptureMS; }
    F64 lastCaptureAge() const;

    void renderDebug();

    // <SS:Nexii> The rain pane's column trace: one line per spawn cell, from the ground it would land on up toward the weather source, colour by what the fall meets - the "can it actually rain here" answer itself, where the debug views above show the map that answer is read from.
    void renderColumnTrace();

    // <SS:Nexii> The debug views, in SSAtmoShadowDebugView order. Cloud and shelter are the same capture read two ways - the raw texels, and the resampled grid its consumers take - so a disagreement is resampling loss. Map is the capture itself, volume the frame it was taken in.
    enum EDebugView
    {
        DEBUG_CLOUD = 0,
        DEBUG_SHELTER,
        DEBUG_MAP,
        DEBUG_VOLUME,
        DEBUG_VIEW_COUNT
    };

    enum
    {
        SURF_MAPPED   = 0x01,
        SURF_WATER    = 0x02,
        SURF_FALLBACK = 0x04
    };

    struct SurfaceGrid
    {
        U64 mRegionHandle = 0;
        S32 mN = 0;
        F32 mCell = 0.f;
        std::vector<F32> mZ;
        std::vector<U8> mFlags;

        // <SS:Nexii> Metres the surface stands above the ground reference under it - the terrain heightmap (or water where higher; the sky track floor in a skybox). Zero is true ground, anything meaningfully positive is standing structure. The same figure the debug texel cloud colours by, kept per cell because consumers want it too: puddles don't stand on tower roofs, deep snow piles belong at grade, and a drip line belongs on a roof edge, not a terrain ledge.
        std::vector<F32> mAbove;

        U32 mGeomSerial = 0;

        F32 axis(S32 i) const { return ((F32)i + 0.5f) * mCell; }
        F32 above(size_t i) const { return mAbove.empty() ? 0.f : mAbove[i]; }
    };

    bool buildSurfaceGrid(U64 region_handle, S32 n, SurfaceGrid& out);

    bool refineEdge(U64 region_handle, const LLVector3& from_agent, const LLVector3& out_dir,
                    F32 max_dist, F32 tolerance, LLVector3& refined_agent) const;

    void validTiles(std::vector<std::pair<U64, U32> >& out) const;

    bool resolveColumn(const LLVector3& pos_agent, LLVector3& hit_pos_agent, bool& on_water,
                       LLVector3* hit_normal = nullptr);

private:
    struct Tile
    {
        U64 mRegionHandle = 0;
        U32 mRes = 0;
        std::vector<F32> mDepth;

        LLVector3 mEyeRegion;
        LLVector3 mDir, mRight, mUp;
        F32 mHalfW = 0.f, mHalfH = 0.f;
        F32 mNear = 0.f, mFar = 0.f;
        F32 mBandTop = 0.f, mBandBottom = 0.f;

        // <SS:Nexii> Voidscape tile: no region anchors it, so mEyeRegion hangs off the
        // global super-grid square the key encodes instead of a region origin.
        bool mVoid = false;
        LLVector3d mVoidOrigin;

        F64 mCaptureTime = 0.0;
        bool mDirty = false;
        bool mValid = false;
        F64 mLastTouched = 0.0;

        U32 mGeomSerial = 1;
        U32 mCapturedSerial = 0;
    };

    // <SS:Nexii> The debug view is the depth map itself, not a re-trace: every kept texel unprojects along the fall direction to the world point it actually saw, drawn as the quad footprint it covers. Holes, eaves, vertical smear and the tilt overscan are then visible directly rather than inferred.
    struct DebugCloud
    {
        std::vector<LLVector3> mPos;
        std::vector<LLColor4U> mColor;

        LLVector3 mRight, mUp;
        F32 mHalf = 0.f;

        F64 mBuiltFrom = -1.0;
        F32 mBuiltStride = 0.f;
        U32 mBuiltRes = 0;
        F32 mBuiltFloor = 0.f;
        bool mBuiltSky = false;
    };

    void buildDebugCloud(const Tile& tile, DebugCloud& cloud);

    // The resampled landing grid, coloured by how far each cell's landing sits above the terrain under it - so the cells the capture never reached, which every consumer silently takes off the heightmap, are visible as themselves.
    struct DebugGrid
    {
        SurfaceGrid mGrid;
        std::vector<LLColor4U> mColor;

        F64 mBuiltFrom = -1.0;
        F32 mBuiltFloor = 0.f;
        bool mBuiltSky = false;
    };

    void buildDebugGrid(const Tile& tile, DebugGrid& grid);

    void drawTexelCloud();
    void drawShelterGrid();
    void drawDepthMap();
    void drawCaptureVolume();

    Tile* tileFor(LLViewerRegion* regionp, bool allow_create);
    bool needsCapture(const Tile& tile) const;
    bool captureTile(Tile& tile);
    void evict();

    // <SS:Nexii> Agent-frame corner of a tile's footprint: a region tile hangs off its region's origin, a void tile off the global super-grid square its key encodes.
    LLVector3 tileOriginAgent(const Tile& tile) const;

    std::map<U64, Tile> mTiles;
    // <SS:Nexii> Voidscape captures: the same Tile over the squares past the rendered
    // regions, keyed by the square's region handle with the void flag bit set, so the two
    // key spaces can never collide. The void carries its own geometry - objects, mega
    // sculpties and giant mesh builds reach kilometres past a region - so these capture it
    // like any other tile, just coarser, lower priority, and on a much longer rebake clock.
    std::map<U64, Tile> mVoidTiles;
    std::map<U64, DebugCloud> mDebugCloud;
    std::map<U64, DebugGrid> mDebugGrid;

    // <SS:Nexii> The on-screen map view's upload of a tile's depth texels. A raw GL name rather than an LLRenderTarget because nothing renders into it - only an upload of CPU-side depth the readback already delivered.
    U32 mDebugMapTex = 0;
    U64 mDebugMapRegion = 0;
    F64 mDebugMapFrom = -1.0;
    U32 mDebugMapRes = 0;
    LLRenderTarget mTarget;
    // <SS:Nexii> The void tiles' own small target: they capture at a quarter of the shadow
    // resolution or coarser, and sharing the region target would reallocate it every time
    // the two alternate.
    LLRenderTarget mVoidTarget;
    F64 mLastCapture = 0.0;

    // <SS:Nexii> One readback in flight, served by SSGLReadback. The shared mTarget must not be re-rendered (or torn down) until the outstanding read lands; mReadbackPending gates capture() and evict(), and a clear requested mid-read is deferred to the read's completion.
    bool mReadbackPending = false;
    bool mClearPending = false;
    U64 mReadbackRegion = 0;

    U32 mCaptureCount = 0;
    U32 mDirtyCaptures = 0;
    F32 mLastCaptureMS = 0.f;
};

#endif
