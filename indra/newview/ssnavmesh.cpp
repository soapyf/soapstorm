/**
 * @file ssnavmesh.cpp
 * @brief See ssnavmesh.h.
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

#include "ssnavmesh.h"

#include "ssworldfieldshapes.h"
#include "ssrainshadow.h"

#include "llagent.h"
#include "llfasttimer.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llrender.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "workqueue.h"

#include "Recast.h"
#include "RecastAlloc.h"
#include "DetourAlloc.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"

#ifdef LL_USESYSTEMLIBS
#include <zlib.h>
#else
#include "zlib-ng/zlib.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

static LLTrace::BlockTimerStatHandle FTM_SS_NAVMESH_UPDATE("SS NavMesh Update");
static LLTrace::BlockTimerStatHandle FTM_SS_NAVMESH_PUBLISH("SS NavMesh Publish");

// Build-side constants: the Recast config the benchmark ran with (doc/atmo_magic_navmesh.md 10). The agent
// parameters are settings; the rest is fixed until a second agent class exists.
static constexpr F32 SS_NAV_MAX_SIMPLIFICATION_ERROR = 1.3f;
static constexpr F32 SS_NAV_MIN_ISLAND_M = 2.f;        // <SS:Nexii> walkable islands smaller than this square are culled per layer before compression. The tile-cache path never reads rcConfig's minRegionArea/mergeRegionArea/maxEdgeLen (dtBuildTileCacheRegions does its own monotone partition and largest-neighbour merge, dtBuildTileCacheContours takes only maxError), so the sliver cull Recast's classic path gives for free is done here on the rcHeightfieldLayer instead. Islands touching the layer's edge or a layer portal are kept: the neighbour tile, or another layer of this one, may carry the rest of that surface, and culling one side would open a seam. [interaction: seams]
static constexpr F32 SS_NAV_LAYER_MERGE_M = 2.f;         // <SS:Nexii> rcBuildHeightfieldLayers merges non-overlapping regions within 4 x this into one layer; it used to ride the agent height, and the 0.5 m crawl agent quartered the merge distance to 2 m, so every ledge became its own layer (57 in one band). Kept at the 2 m walker figure so layering does not follow the agent class. [interaction: SS_NAV_MAX_LAYERS_PER_BAND]
static constexpr S32 SS_NAV_MAX_LAYERS_PER_BAND = 32;      // walkable layers a band may publish, largest by walkable cells first; the tile-cache layer index is an int, the only ceiling is dtTileCache::buildNavMeshTilesAt's 512-per-column buffer (12 bands x 32 = 384)
static constexpr S32 SS_NAV_TERRAIN_NODES = 21;     // 1 m grid over the bordered column: 16 m + 2 x 2 m margin, plus one
static constexpr F32 SS_NAV_TERRAIN_MARGIN_M = 2.f;
static constexpr F32 SS_NAV_TERRAIN_SLOP_M = 0.5f;  // raster-vs-grid slack; walkable surface more than this under the land is buried
static constexpr S32 SS_NAV_MAX_TILES = 32768;             // 64-bit poly refs (DT_POLYREF64): the tile budget is memory, not id bits; 16 m columns over 512 m need it
static constexpr S32 SS_NAV_MAX_OBSTACLES = 512;
static constexpr S32 SS_NAV_OBSTACLE_REQUESTS_PER_FRAME = 48;   // under dtTileCache's 64-request queue, drained once per update
static constexpr S32 SS_NAV_QUERY_NODES = 4096;
static constexpr S32 SS_NAV_MAX_PATH = 512;
static constexpr F32 SS_NAV_DETAIL_SAMPLE_M = 1.f;         // height detail: sample spacing over a polygon
static constexpr F32 SS_NAV_DETAIL_ERROR_M = 0.25f;        // and the height error that earns a sample its vertex

// ---------------------------------------------------------------------------- Recast-side helpers

namespace
{
    // zlib-backed DetourTileCache compressor: the tile cache owns compressed layers and inflates on rebuild.
    struct SSNavCompressor : public dtTileCacheCompressor
    {
        int maxCompressedSize(const int bufferSize) override { return (int)compressBound((uLong)bufferSize); }
        dtStatus compress(const unsigned char* buffer, const int bufferSize, unsigned char* compressed, const int maxCompressedSize, int* compressedSize) override
        {
            uLongf out = (uLongf)maxCompressedSize;
            if (compress2(compressed, &out, buffer, (uLong)bufferSize, Z_BEST_SPEED) != Z_OK) return DT_FAILURE;
            *compressedSize = (int)out;
            return DT_SUCCESS;
        }
        dtStatus decompress(const unsigned char* compressed, const int compressedSize, unsigned char* buffer, const int maxBufferSize, int* bufferSize) override
        {
            uLongf out = (uLongf)maxBufferSize;
            if (uncompress(buffer, &out, compressed, (uLong)compressedSize) != Z_OK) return DT_FAILURE;
            *bufferSize = (int)out;
            return DT_SUCCESS;
        }
    };

    // <SS:Nexii> Recast's own errors and warnings, collected per build: a band whose rcBuildHeightfieldLayers overflowed used to come back as a silent "0 tiles". publish prints the worker's log with the band's coordinates; the detail pass prints its own. [interaction: publish]
    struct SSNavContext : public rcContext
    {
        std::string mLog;
        bool mErrorsOnly = false;           // the main-thread detail pass: its "walk to centre failed" warnings are routine since the unset-height fix
        SSNavContext() : rcContext(true) {}
        void doLog(const rcLogCategory category, const char* msg, const int len) override
        {
            if (category == RC_LOG_PROGRESS || mLog.size() > 512) return;
            if (mErrorsOnly && category != RC_LOG_ERROR) return;
            if (!mLog.empty()) mLog += " | ";
            mLog.append(msg, (size_t)llmax(len, 0));
        }
    };

    // Every polygon walkable, one area: agent classes and door portals are later plumbing.
    // <SS:Nexii> The height detail the tile cache path leaves out: Detour reads a polygon's height off its plane unless the tile carries a detail mesh, so a hexagon merged across a hill crest floated a metre over the land. The layer the tile is built from still holds every cell's height, so it is dressed as a one-span-per-cell compact heightfield and the polygons as an rcPolyMesh, and Recast's own rcBuildPolyMeshDetail samples each polygon and adds vertices where the surface leaves the plane by more than SS_NAV_DETAIL_ERROR_M. Runs on the main thread inside the tile build; the console's publish figure is what it costs. [interaction: dtTileCacheMeshProcess::detail, renderDebug]
    struct SSNavMeshProcess : public dtTileCacheMeshProcess
    {
        SSNavContext mCtx;
        rcPolyMeshDetail* mDetail = nullptr;
        SSNavMeshProcess() { mCtx.mErrorsOnly = true; }
        ~SSNavMeshProcess() override { rcFreePolyMeshDetail(mDetail); }

        void process(dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags) override
        {
            for (int i = 0; i < params->polyCount; ++i) { polyAreas[i] = 0; polyFlags[i] = 1; }
        }

        void detail(const dtTileCacheLayer* layer, dtNavMeshCreateParams* params) override
        {
            if (!layer || !layer->header || params->polyCount == 0 || params->vertCount == 0) return;
            const int w = layer->header->width, h = layer->header->height;

            // The layer as a compact heightfield: one span per cell that carries this layer's surface.
            rcCompactHeightfield* chf = rcAllocCompactHeightfield();
            if (!chf) return;
            chf->width = w; chf->height = h;
            chf->cs = params->cs; chf->ch = params->ch;
            chf->borderSize = 0;
            chf->walkableHeight = (int)ceilf(params->walkableHeight / params->ch);
            chf->walkableClimb = (int)floorf(params->walkableClimb / params->ch);
            rcVcopy(chf->bmin, params->bmin); rcVcopy(chf->bmax, params->bmax);
            chf->cells = (rcCompactCell*)rcAlloc(sizeof(rcCompactCell) * (size_t)w * h, RC_ALLOC_PERM);
            if (!chf->cells) { rcFreeCompactHeightfield(chf); return; }
            memset(chf->cells, 0, sizeof(rcCompactCell) * (size_t)w * h);
            int n = 0;
            for (int i = 0; i < w * h; ++i) if (layer->areas[i] != DT_TILECACHE_NULL_AREA) ++n;
            if (n == 0) { rcFreeCompactHeightfield(chf); return; }
            chf->spans = (rcCompactSpan*)rcAlloc(sizeof(rcCompactSpan) * n, RC_ALLOC_PERM);
            chf->areas = (unsigned char*)rcAlloc(n, RC_ALLOC_PERM);
            if (!chf->spans || !chf->areas) { rcFreeCompactHeightfield(chf); return; }
            chf->spanCount = n;
            int si = 0;
            for (int z = 0; z < h; ++z) for (int x = 0; x < w; ++x)
            {
                const int idx = x + z * w;
                rcCompactCell& c = chf->cells[idx];
                if (layer->areas[idx] == DT_TILECACHE_NULL_AREA) { c.index = 0; c.count = 0; continue; }
                c.index = (unsigned int)si; c.count = 1;
                rcCompactSpan& sp = chf->spans[si];
                sp.y = layer->heights[idx];
                sp.reg = 0;
                sp.con = 0;
                sp.h = 255;
                for (int dir = 0; dir < 4; ++dir)
                {
                    const int nx = x + rcGetDirOffsetX(dir), nz = z + rcGetDirOffsetY(dir);
                    const bool linked = (layer->cons[idx] & (1 << dir)) && nx >= 0 && nz >= 0 && nx < w && nz < h && layer->areas[nx + nz * w] != DT_TILECACHE_NULL_AREA;
                    rcSetCon(sp, dir, linked ? 0 : RC_NOT_CONNECTED);
                }
                chf->areas[si] = layer->areas[idx];
                ++si;
            }

            // The polygons as Recast's struct: copied, because the struct frees what it points at.
            rcPolyMesh* pm = rcAllocPolyMesh();
            if (!pm) { rcFreeCompactHeightfield(chf); return; }
            pm->nverts = params->vertCount; pm->npolys = params->polyCount; pm->maxpolys = params->polyCount; pm->nvp = params->nvp;
            pm->verts = (unsigned short*)rcAlloc(sizeof(unsigned short) * 3 * pm->nverts, RC_ALLOC_PERM);
            pm->polys = (unsigned short*)rcAlloc(sizeof(unsigned short) * pm->nvp * 2 * pm->npolys, RC_ALLOC_PERM);
            pm->regs = (unsigned short*)rcAlloc(sizeof(unsigned short) * pm->npolys, RC_ALLOC_PERM);
            pm->flags = (unsigned short*)rcAlloc(sizeof(unsigned short) * pm->npolys, RC_ALLOC_PERM);
            pm->areas = (unsigned char*)rcAlloc(pm->npolys, RC_ALLOC_PERM);
            if (!pm->verts || !pm->polys || !pm->regs || !pm->flags || !pm->areas) { rcFreePolyMesh(pm); rcFreeCompactHeightfield(chf); return; }
            memcpy(pm->verts, params->verts, sizeof(unsigned short) * 3 * pm->nverts);
            memcpy(pm->polys, params->polys, sizeof(unsigned short) * pm->nvp * 2 * pm->npolys);
            memcpy(pm->flags, params->polyFlags, sizeof(unsigned short) * pm->npolys);
            memcpy(pm->areas, params->polyAreas, pm->npolys);
            for (int i = 0; i < pm->npolys; ++i) pm->regs[i] = RC_MULTIPLE_REGS;     // seed each polygon's height patch from its own centre
            rcVcopy(pm->bmin, params->bmin); rcVcopy(pm->bmax, params->bmax);
            pm->cs = params->cs; pm->ch = params->ch;
            pm->borderSize = 0;
            pm->maxEdgeError = 2.f;

            rcFreePolyMeshDetail(mDetail);
            mDetail = rcAllocPolyMeshDetail();
            if (mDetail && rcBuildPolyMeshDetail(&mCtx, *pm, *chf, SS_NAV_DETAIL_SAMPLE_M, SS_NAV_DETAIL_ERROR_M, *mDetail) && mDetail->nmeshes == params->polyCount)
            {
                params->detailMeshes = mDetail->meshes;
                params->detailVerts = mDetail->verts;
                params->detailVertsCount = mDetail->nverts;
                params->detailTris = mDetail->tris;
                params->detailTriCount = mDetail->ntris;
            }
            if (!mCtx.mLog.empty()) { LL_WARNS("SSNavMesh") << "detail mesh: " << mCtx.mLog << LL_ENDL; mCtx.mLog.clear(); }
            rcFreePolyMesh(pm);
            rcFreeCompactHeightfield(chf);
        }
    };

    // Triangle soup in Recast space (x, up, z) = (local x, local z, -local y): a proper rotation, so outward
    // winding survives and floors keep their up-facing normals.
    struct SSNavSoup
    {
        std::vector<float> mVerts;
        void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c)
        {
            const LLVector3* p[3] = {&a, &b, &c};
            for (S32 i = 0; i < 3; ++i) { mVerts.push_back(p[i]->mV[VX]); mVerts.push_back(p[i]->mV[VZ]); mVerts.push_back(-p[i]->mV[VY]); }
        }
        void quad(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d) { tri(a, b, c); tri(a, c, d); }
        S32 triCount() const { return (S32)(mVerts.size() / 9); }
    };

    // A convex xz footprint with a height range: the cut an exclusion volume makes in the walkable area.
    struct SSNavExclusion
    {
        float mVerts[8 * 3];
        int mCount = 0;
        float mMinY = 0.f, mMaxY = 0.f;
    };

    // A convex body's faces, to be filled solid column by column rather than rasterized as surfaces.
    struct SSNavConvex
    {
        SSNavSoup mFaces;
        bool mBlock = false;        // a static obstacle: filled, never walkable on top
    };

    // The main-thread snapshot a worker build consumes: geometry, terrain heights, config.
    struct SSNavBuildInput
    {
        SSNavSoup mSoup;            // geometry that may carry walkable surface
        SSNavSoup mBlockSoup;       // static obstacles: solid, never walkable
        std::vector<SSNavConvex> mConvex;
        std::vector<SSNavExclusion> mExclusions;
        F32 mMin[3] = {0, 0, 0};            // local-space column bounds (x, y, band zmin)
        F32 mMax[3] = {0, 0, 0};
        F32 mAgentHeight = 2.f, mAgentRadius = 0.5f, mAgentClimb = 0.75f, mAgentSlope = 45.f;
        S32 mTx = 0, mTyDetour = 0, mBand = 0;
        std::vector<F32> mTerrain;          // SS_NAV_TERRAIN_NODES^2 heights over the bordered column, below -900 where there is no land
        F32 mTerrainX0 = 0.f, mTerrainY0 = 0.f;
        F32 mWaterZ = -1e30f;               // the region's water surface, local z; below everything when unknown
        bool mWantSheet = false;            // the world field takes its spans from this build
    };

    // Land height at a local xy from the build's own terrain nodes; false off the grid or in the void.
    bool sheetTerrainAt(const SSNavBuildInput& in, F32 x, F32 y, F32& z)
    {
        if (in.mTerrain.empty()) return false;
        const S32 n = SS_NAV_TERRAIN_NODES;
        const F32 gx = x - in.mTerrainX0, gy = y - in.mTerrainY0;
        const S32 ix = (S32)floorf(gx), iy = (S32)floorf(gy);
        if (ix < 0 || iy < 0 || ix + 1 >= n || iy + 1 >= n) return false;
        const F32 h00 = in.mTerrain[iy * n + ix], h10 = in.mTerrain[iy * n + ix + 1];
        const F32 h01 = in.mTerrain[(iy + 1) * n + ix], h11 = in.mTerrain[(iy + 1) * n + ix + 1];
        if (h00 < -900.f || h10 < -900.f || h01 < -900.f || h11 < -900.f) return false;
        const F32 fx = gx - (F32)ix, fy = gy - (F32)iy;
        z = (h00 * (1.f - fx) + h10 * fx) * (1.f - fy) + (h01 * (1.f - fx) + h11 * fx) * fy;
        return true;
    }

    // <SS:Nexii> The span sheet: every raster span of the column's interior, unioned two cells by two into the field's 0.25 m cells, air gaps under the field's slab closed, the list clipped to the field's budget by folding its thinnest gaps, and the span the land sits in flagged as terrain (the field extends it to the world floor) and raised to the water surface where the land is under water. The band keeps this sheet for its whole life: it IS the world field's geometry, which is why nothing here is handed away or rewritten in place. Recast z runs along -local y, so sheet rows count from the far end of the raster. [interaction: SSWorldField::collectSheets]
    void extractSpanSheet(const rcHeightfield& hf, const rcConfig& cfg, const SSNavBuildInput& in, SSNavMesh::SpanSheet& out)
    {
        const S32 R = SSNavMesh::SpanSheet::RES, K = SSNavMesh::SpanSheet::SPANS;
        const F32 sheet_cell = SSNavMesh::TILE_M / (F32)R;
        const S32 per = SSNavMesh::TILE_CELLS / R;
        const S32 b = cfg.borderSize;
        out.mCount.assign((size_t)R * R, 0);
        out.mBottom.assign((size_t)R * R * K, 0.f);
        out.mTop.assign((size_t)R * R * K, 0.f);
        out.mFlags.assign((size_t)R * R * K, 0);
        struct Iv { F32 lo, hi; U8 fl; };
        std::vector<Iv> ivs, merged;
        for (S32 sy = 0; sy < R; ++sy) for (S32 sx = 0; sx < R; ++sx)
        {
            ivs.clear(); merged.clear();
            for (S32 dy = 0; dy < per; ++dy) for (S32 dx = 0; dx < per; ++dx)
            {
                const S32 hx = b + sx * per + dx;
                const S32 hz = b + SSNavMesh::TILE_CELLS - 1 - (sy * per + dy);
                if (hx < 0 || hz < 0 || hx >= hf.width || hz >= hf.height) continue;
                for (const rcSpan* sp = hf.spans[hx + hz * hf.width]; sp; sp = sp->next)
                {
                    ivs.push_back(Iv{cfg.bmin[1] + (F32)sp->smin * cfg.ch, cfg.bmin[1] + (F32)sp->smax * cfg.ch, (U8)SSRainShadowMap::SURF_MAPPED});
                }
            }
            if (ivs.empty()) continue;
            std::sort(ivs.begin(), ivs.end(), [](const Iv& a, const Iv& c) { return a.lo < c.lo; });
            for (const Iv& iv : ivs)
            {
                if (!merged.empty() && iv.lo - merged.back().hi < 0.25f) merged.back().hi = llmax(merged.back().hi, iv.hi);
                else merged.push_back(iv);
            }
            while ((S32)merged.size() > K)
            {
                size_t thinnest = 0;
                F32 best = FLT_MAX;
                for (size_t j = 0; j + 1 < merged.size(); ++j)
                {
                    const F32 gap = merged[j + 1].lo - merged[j].hi;
                    if (gap < best) { best = gap; thinnest = j; }
                }
                merged[thinnest].hi = merged[thinnest + 1].hi;
                merged.erase(merged.begin() + (thinnest + 1));
            }
            F32 tz;
            const F32 cx = in.mMin[0] + ((F32)sx + 0.5f) * sheet_cell, cy = in.mMin[1] + ((F32)sy + 0.5f) * sheet_cell;
            if (sheetTerrainAt(in, cx, cy, tz) && tz >= in.mMin[2] - 1.f && tz <= in.mMax[2] + 1.f)
            {
                for (size_t k = 0; k < merged.size(); ++k)
                {
                    if (merged[k].lo > tz + 0.3f) break;                 // everything from here up floats above the land
                    if (merged[k].hi < tz - 0.3f) continue;              // a body wholly under the land
                    merged[k].fl |= SSRainShadowMap::SURF_FALLBACK;
                    if (in.mWaterZ > merged[k].hi)
                    {
                        merged[k].hi = in.mWaterZ;
                        merged[k].fl |= SSRainShadowMap::SURF_WATER;
                        while (k + 1 < merged.size() && merged[k + 1].lo - merged[k].hi < 0.25f)
                        {
                            merged[k].hi = llmax(merged[k].hi, merged[k + 1].hi);
                            merged.erase(merged.begin() + (k + 1));
                        }
                    }
                    break;
                }
            }
            const size_t col = (size_t)sy * R + sx;
            out.mCount[col] = (U8)merged.size();
            for (size_t k = 0; k < merged.size(); ++k)
            {
                out.mBottom[col * K + k] = merged[k].lo;
                out.mTop[col * K + k] = merged[k].hi;
                out.mFlags[col * K + k] = merged[k].fl;
            }
        }
    }

    void emitBox(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavSoup& out)
    {
        LLVector3 c[8];
        S32 i = 0;
        for (S32 z = -1; z <= 1; z += 2) for (S32 y = -1; y <= 1; y += 2) for (S32 x = -1; x <= 1; x += 2)
        {
            c[i++] = r.mCenter + off + r.mAxes[0] * (r.mHalf.mV[VX] * (F32)x) + r.mAxes[1] * (r.mHalf.mV[VY] * (F32)y) + r.mAxes[2] * (r.mHalf.mV[VZ] * (F32)z);
        }
        out.quad(c[0], c[2], c[3], c[1]);
        out.quad(c[4], c[5], c[7], c[6]);
        out.quad(c[0], c[1], c[5], c[4]);
        out.quad(c[2], c[6], c[7], c[3]);
        out.quad(c[0], c[4], c[6], c[2]);
        out.quad(c[1], c[3], c[7], c[5]);
    }

    void emitCylinder(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavSoup& out)
    {
        const S32 n = llclamp((S32)(8.f + r.mRadius * 8.f), 8, 32);
        const F32 rad = r.mRadius / cosf(F_PI / (F32)n);      // circumscribed: conservative
        LLVector3 r0[32], r1[32];
        const LLVector3 c0 = r.mCenter + off - r.mAxes[2] * r.mHalfHeight;
        const LLVector3 c1 = r.mCenter + off + r.mAxes[2] * r.mHalfHeight;
        for (S32 k = 0; k < n; ++k)
        {
            const F32 a = F_TWO_PI * (F32)k / (F32)n;
            const LLVector3 o = r.mAxisU * (cosf(a) * rad) + r.mAxisV * (sinf(a) * rad);
            r0[k] = c0 + o; r1[k] = c1 + o;
        }
        for (S32 k = 0; k < n; ++k)
        {
            const S32 j = (k + 1) % n;
            out.quad(r0[k], r0[j], r1[j], r1[k]);
            out.tri(c0, r0[j], r0[k]);
            out.tri(c1, r1[k], r1[j]);
        }
    }

    void emitEllipsoid(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavSoup& out)
    {
        const F32 rmax = llmax(r.mRadii.mV[VX], llmax(r.mRadii.mV[VY], r.mRadii.mV[VZ]));
        const S32 nl = llclamp((S32)(8.f + rmax * 6.f), 8, 24), ns = llmax(4, nl / 2);
        auto pt = [&](S32 st, S32 sl)
        {
            const F32 phi = F_PI * (F32)st / (F32)ns, th = F_TWO_PI * (F32)sl / (F32)nl;
            return r.mCenter + off + r.mAxes[0] * (sinf(phi) * cosf(th) * r.mRadii.mV[VX])
                                   + r.mAxes[1] * (sinf(phi) * sinf(th) * r.mRadii.mV[VY])
                                   + r.mAxes[2] * (cosf(phi) * r.mRadii.mV[VZ]);
        };
        for (S32 st = 0; st < ns; ++st) for (S32 sl = 0; sl < nl; ++sl)
        {
            out.quad(pt(st, sl), pt(st + 1, sl), pt(st + 1, sl + 1), pt(st, sl + 1));
        }
    }

    // Convex hull (xz) of the record's AABB corners in Recast space, with its y range - the exclusion cut.
    void emitExclusion(const SSWorldFieldShapes::Record& r, const LLVector3& off, std::vector<SSNavExclusion>& out)
    {
        SSNavExclusion e;
        const LLVector3 lo = r.mBMin + off, hi = r.mBMax + off;
        const float xs[4] = {lo.mV[VX], hi.mV[VX], hi.mV[VX], lo.mV[VX]};
        const float zs[4] = {-lo.mV[VY], -lo.mV[VY], -hi.mV[VY], -hi.mV[VY]};
        // Counter-clockwise in xz as rcMarkConvexPolyArea expects.
        const int order[4] = {0, 3, 2, 1};
        for (int i = 0; i < 4; ++i) { e.mVerts[i * 3] = xs[order[i]]; e.mVerts[i * 3 + 1] = 0.f; e.mVerts[i * 3 + 2] = zs[order[i]]; }
        e.mCount = 4;
        e.mMinY = lo.mV[VZ]; e.mMaxY = hi.mV[VZ];
        out.push_back(e);
    }

    void emitShapeFaces(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavSoup& out);

    // <SS:Nexii> What the navmesh never sees: the phantom layer, and a part whose physics shape type is NONE, which the census keeps as render geometry for rain and cover. An avatar walks through both, so they are neither floor nor wall here; a mesh tree with no physics on a floating island grew a canopy of navmesh before this. A shape type the sim has not answered is NOT skipped: until the per-region fetch fix a neighbouring region never answered, and its render volume is the best floor available meanwhile. Exclusion volumes keep their cut whatever their physics. [interaction: SSWorldFieldShapes::bakePart, syncObstacles, LLViewerObjectList::fetchPhysicsFlags]
    bool navIgnores(const SSWorldFieldShapes::Record& rec)
    {
        if (rec.mNavRole == SSWorldFieldShapes::NAV_ROLE_EXCLUSION_VOLUME) return false;
        return rec.mLayer == SSWorldFieldShapes::LAYER_DECLARED_PHANTOM || rec.mNoPhysics;
    }

    void emitRecord(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavBuildInput& in)
    {
        if (r.mNavRole == SSWorldFieldShapes::NAV_ROLE_EXCLUSION_VOLUME) { emitExclusion(r, off, in.mExclusions); return; }
        const bool block = (r.mNavRole == SSWorldFieldShapes::NAV_ROLE_STATIC_OBSTACLE);
        // <SS:Nexii> Recast rasterizes surfaces, so a solid body on the ground would keep a walkable island inside it (the terrain span merges with the bottom face). Every convex record - analytic prims, mesh bounding boxes, decomposition hulls - is therefore filled solid per column instead; only tessellated soups, which may be hollow by design, stay surfaces. [interaction: rasterizeConvex]
        const bool convex = r.mClass != SSWorldFieldShapes::Record::CLASS_TRI || r.mProv == SSWorldFieldShapes::PROV_HULL || r.mProv == SSWorldFieldShapes::PROV_BBOX || r.mProv == SSWorldFieldShapes::PROV_UNFETCHED;
        if (convex)
        {
            in.mConvex.emplace_back();
            in.mConvex.back().mBlock = block;
            emitShapeFaces(r, off, in.mConvex.back().mFaces);
            return;
        }
        SSNavSoup& out = block ? in.mBlockSoup : in.mSoup;
        emitShapeFaces(r, off, out);
    }

    void emitShapeFaces(const SSWorldFieldShapes::Record& r, const LLVector3& off, SSNavSoup& out)
    {
        switch (r.mClass)
        {
            case SSWorldFieldShapes::Record::CLASS_BOX: emitBox(r, off, out); break;
            case SSWorldFieldShapes::Record::CLASS_CYLINDER: emitCylinder(r, off, out); break;
            case SSWorldFieldShapes::Record::CLASS_SPHERE: emitEllipsoid(r, off, out); break;
            case SSWorldFieldShapes::Record::CLASS_TRI:
            {
                const std::vector<LLVector3>& tri = r.tris();
                const size_t nt = tri.size() / 3;
                for (size_t k = 0; k < nt; ++k) out.tri(tri[k * 3] + off, tri[k * 3 + 1] + off, tri[k * 3 + 2] + off);
                break;
            }
        }
    }

    // Terrain over the bordered column as two triangles per 1 m cell; heights were sampled on the main thread.
    void emitTerrain(const F32* heights, F32 x0, F32 y0, SSNavSoup& out)
    {
        const S32 n = SS_NAV_TERRAIN_NODES;
        for (S32 gy = 0; gy + 1 < n; ++gy) for (S32 gx = 0; gx + 1 < n; ++gx)
        {
            const F32 h00 = heights[gy * n + gx], h10 = heights[gy * n + gx + 1], h11 = heights[(gy + 1) * n + gx + 1], h01 = heights[(gy + 1) * n + gx];
            if (h00 < -900.f || h10 < -900.f || h11 < -900.f || h01 < -900.f) continue;     // void between regions
            const F32 x = x0 + (F32)gx, y = y0 + (F32)gy;
            out.quad(LLVector3(x, y, h00), LLVector3(x + 1.f, y, h10), LLVector3(x + 1.f, y + 1.f, h11), LLVector3(x, y + 1.f, h01));
        }
    }

    // Sutherland-Hodgman against one axis plane in xz; keeps (v[axis] - value) * sign <= 0.
    int clipPolyAxis(const float* in, int nin, float* out, int axis, float value, float sign)
    {
        int nout = 0;
        for (int i = 0, j = nin - 1; i < nin; j = i++)
        {
            const float* a = in + j * 3;
            const float* b = in + i * 3;
            const float da = (a[axis] - value) * sign, db = (b[axis] - value) * sign;
            const bool ina = da <= 0.f, inb = db <= 0.f;
            if (ina != inb)
            {
                const float t = da / (da - db);
                out[nout * 3] = a[0] + (b[0] - a[0]) * t; out[nout * 3 + 1] = a[1] + (b[1] - a[1]) * t; out[nout * 3 + 2] = a[2] + (b[2] - a[2]) * t;
                ++nout;
            }
            if (inb) { out[nout * 3] = b[0]; out[nout * 3 + 1] = b[1]; out[nout * 3 + 2] = b[2]; ++nout; }
        }
        return nout;
    }

    // Solid fill of one convex body: every column it covers gets a single span from its lowest face to its highest,
    // walkable when the face on top faces up within the slope limit. Exact for convex bodies, and it is what keeps
    // the inside of a solid from ever becoming floor.
    void rasterizeConvex(rcContext& ctx, rcHeightfield& hf, const rcConfig& cfg, const SSNavConvex& cv, float walkable_thr)
    {
        const float* v = cv.mFaces.mVerts.data();
        const int ntris = cv.mFaces.triCount();
        if (ntris == 0) return;
        float bmin[3] = {v[0], v[1], v[2]}, bmax[3] = {v[0], v[1], v[2]};
        for (int i = 1; i < ntris * 3; ++i) for (int c = 0; c < 3; ++c) { bmin[c] = llmin(bmin[c], v[i * 3 + c]); bmax[c] = llmax(bmax[c], v[i * 3 + c]); }
        const float ics = 1.f / cfg.cs;
        const int x0 = llmax(0, (int)floorf((bmin[0] - cfg.bmin[0]) * ics)), x1 = llmin(hf.width - 1, (int)floorf((bmax[0] - cfg.bmin[0]) * ics));
        const int z0 = llmax(0, (int)floorf((bmin[2] - cfg.bmin[2]) * ics)), z1 = llmin(hf.height - 1, (int)floorf((bmax[2] - cfg.bmin[2]) * ics));
        if (x1 < x0 || z1 < z0) return;
        if (bmax[1] < cfg.bmin[1] || bmin[1] > cfg.bmax[1]) return;
        const int w = x1 - x0 + 1, h = z1 - z0 + 1;
        std::vector<float> lo((size_t)w * h, 1e30f), hi((size_t)w * h, -1e30f), top_ny((size_t)w * h, 0.f);

        float bufA[16 * 3], bufB[16 * 3], rowBuf[16 * 3], cellBuf[16 * 3], colA[16 * 3], colB[16 * 3];
        for (int t = 0; t < ntris; ++t)
        {
            const float* tv = v + t * 9;
            // Face normal's up component decides walkability of whatever this face tops.
            const float e1[3] = {tv[3] - tv[0], tv[4] - tv[1], tv[5] - tv[2]}, e2[3] = {tv[6] - tv[0], tv[7] - tv[1], tv[8] - tv[2]};
            const float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
            const float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
            const float up = nlen > 0.f ? ny / nlen : 0.f;
            // Pre-clip to the tile so the sweeps below stay bounded by the tile, not the face.
            float* cur = bufA; float* nxt = bufB;
            memcpy(cur, tv, 36);
            int n = 3;
            n = clipPolyAxis(cur, n, nxt, 0, cfg.bmin[0] + x0 * cfg.cs, -1.f); std::swap(cur, nxt);
            n = clipPolyAxis(cur, n, nxt, 0, cfg.bmin[0] + (x1 + 1) * cfg.cs, 1.f); std::swap(cur, nxt);
            n = clipPolyAxis(cur, n, nxt, 2, cfg.bmin[2] + z0 * cfg.cs, -1.f); std::swap(cur, nxt);
            n = clipPolyAxis(cur, n, nxt, 2, cfg.bmin[2] + (z1 + 1) * cfg.cs, 1.f); std::swap(cur, nxt);
            if (n < 3) continue;
            float tzmin = cur[2], tzmax = cur[2];
            for (int i = 1; i < n; ++i) { tzmin = llmin(tzmin, cur[i * 3 + 2]); tzmax = llmax(tzmax, cur[i * 3 + 2]); }
            const int rz0 = llmax(z0, (int)floorf((tzmin - cfg.bmin[2]) * ics)), rz1 = llmin(z1, (int)floorf((tzmax - cfg.bmin[2]) * ics));
            for (int z = rz0; z <= rz1 && n >= 3; ++z)
            {
                const float row_top = cfg.bmin[2] + (float)(z + 1) * cfg.cs;
                const int nrow = clipPolyAxis(cur, n, rowBuf, 2, row_top, 1.f);
                const int nrest = clipPolyAxis(cur, n, nxt, 2, row_top, -1.f);
                std::swap(cur, nxt); n = nrest;
                if (nrow < 3) continue;
                float rxmin = rowBuf[0], rxmax = rowBuf[0];
                for (int i = 1; i < nrow; ++i) { rxmin = llmin(rxmin, rowBuf[i * 3]); rxmax = llmax(rxmax, rowBuf[i * 3]); }
                const int cx0 = llmax(x0, (int)floorf((rxmin - cfg.bmin[0]) * ics)), cx1 = llmin(x1, (int)floorf((rxmax - cfg.bmin[0]) * ics));
                float* q = colA; float* qn = colB;
                memcpy(q, rowBuf, (size_t)nrow * 12);
                int nq = nrow;
                for (int x = cx0; x <= cx1 && nq >= 3; ++x)
                {
                    const float col_right = cfg.bmin[0] + (float)(x + 1) * cfg.cs;
                    const int ncell = clipPolyAxis(q, nq, cellBuf, 0, col_right, 1.f);
                    const int nrest2 = clipPolyAxis(q, nq, qn, 0, col_right, -1.f);
                    std::swap(q, qn); nq = nrest2;
                    if (ncell < 3) continue;
                    float ylo = cellBuf[1], yhi = cellBuf[1];
                    for (int i = 1; i < ncell; ++i) { ylo = llmin(ylo, cellBuf[i * 3 + 1]); yhi = llmax(yhi, cellBuf[i * 3 + 1]); }
                    const size_t idx = (size_t)(z - z0) * w + (x - x0);
                    lo[idx] = llmin(lo[idx], ylo);
                    if (yhi > hi[idx]) { hi[idx] = yhi; top_ny[idx] = up; }
                }
            }
        }
        const float ich = 1.f / cfg.ch;
        for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x)
        {
            const size_t idx = (size_t)(z - z0) * w + (x - x0);
            if (lo[idx] > hi[idx]) continue;
            if (hi[idx] < cfg.bmin[1] || lo[idx] > cfg.bmax[1]) continue;
            int smin = (int)floorf((lo[idx] - cfg.bmin[1]) * ich), smax = (int)ceilf((hi[idx] - cfg.bmin[1]) * ich);
            smin = llclamp(smin, 0, RC_SPAN_MAX_HEIGHT - 1);
            smax = llclamp(smax, smin + 1, RC_SPAN_MAX_HEIGHT);
            const unsigned char area = (!cv.mBlock && top_ny[idx] >= walkable_thr) ? RC_WALKABLE_AREA : RC_NULL_AREA;
            rcAddSpan(&ctx, hf, x, z, (unsigned short)smin, (unsigned short)smax, area, cfg.walkableClimb);
        }
    }

    U64 fnv(U64 h, U64 v) { h ^= v; h *= 1099511628211ull; return h; }
    U64 fnvF(U64 h, F32 f) { return fnv(h, (U64)(S64)llround(f * 100.f)); }

    // Flood the layer's walkable cells over their same-layer connections; components under min_area that touch neither the layer edge nor a layer portal are nulled.
    void cullLayerIslands(rcHeightfieldLayer& layer, const S32 min_area, std::vector<S32>& stack)
    {
        const S32 w = layer.width, h = layer.height, n = w * h;
        if (min_area <= 1 || n <= 0) return;
        std::vector<U8> seen((size_t)n, 0);
        std::vector<S32> comp;
        static const S32 DX[4] = {-1, 0, 1, 0}, DY[4] = {0, 1, 0, -1};    // rcGetDirOffsetX/Y order: dir 0 = -x, 1 = +y(z), 2 = +x, 3 = -y(z)
        for (S32 start = 0; start < n; ++start)
        {
            if (seen[start] || layer.areas[start] == RC_NULL_AREA) continue;
            comp.clear();
            stack.clear();
            stack.push_back(start);
            seen[start] = 1;
            bool keep = false;
            while (!stack.empty())
            {
                const S32 idx = stack.back(); stack.pop_back();
                comp.push_back(idx);
                const U8 con = layer.cons[idx];
                const S32 x = idx % w, y = idx / w;
                if ((con & 0xf0) || x == 0 || y == 0 || x == w - 1 || y == h - 1) keep = true;    // rcBuildHeightfieldLayers' high nibble is the inter-layer portal mask; tile borders carry no bit, the edge cell is the tell
                for (S32 dir = 0; dir < 4; ++dir)
                {
                    if (!(con & (1 << dir))) continue;
                    const S32 nx = x + DX[dir], ny = y + DY[dir];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const S32 nidx = nx + ny * w;
                    if (seen[nidx] || layer.areas[nidx] == RC_NULL_AREA) continue;
                    seen[nidx] = 1;
                    stack.push_back(nidx);
                }
            }
            if (keep || (S32)comp.size() >= min_area) continue;
            for (S32 idx : comp) layer.areas[idx] = RC_NULL_AREA;
        }
    }

    // <SS:Nexii> The land is the floor of the world: a walkable span may not top out under it. Runs after Recast's filters so
    // nothing re-promotes what this nulls; geometry buried in the land (foundations, mesh undersides, a band edge through a
    // hill) loses its floor and the terrain above stays the only walkable read. [interaction: rasterizeConvex]
    void nullUnderTerrain(rcHeightfield& hf, const rcConfig& cfg, const SSNavBuildInput& in, S32& out_nulled)
    {
        out_nulled = 0;
        for (int z = 0; z < hf.height; ++z) for (int x = 0; x < hf.width; ++x)
        {
            // Land ceiling over the cell: the terrain grid is piecewise linear between nodes, so the max over the cell's
            // corners bounds the surface the soup rasterizes; terrain spans sit at or above it by construction.
            F32 ceiling = 0.f;
            bool any = false;
            const F32 xlo = cfg.bmin[0] + (F32)x * cfg.cs;
            const F32 ylo = -(cfg.bmin[2] + (F32)(z + 1) * cfg.cs);     // raster z runs along -local y
            for (int c = 0; c < 4; ++c)
            {
                F32 tz;
                if (sheetTerrainAt(in, xlo + ((c & 1) ? cfg.cs : 0.f), ylo + ((c & 2) ? cfg.cs : 0.f), tz))
                {
                    ceiling = any ? llmax(ceiling, tz) : tz;
                    any = true;
                }
            }
            if (!any) continue;                     // no land over the cell: nothing to enforce
            ceiling -= SS_NAV_TERRAIN_SLOP_M;
            for (rcSpan* sp = hf.spans[x + z * hf.width]; sp; sp = sp->next)
            {
                if (sp->area == RC_NULL_AREA) continue;
                if (cfg.bmin[1] + (F32)sp->smax * cfg.ch < ceiling) { sp->area = RC_NULL_AREA; ++out_nulled; }
            }
        }
    }

    // The worker: one band through Recast to compressed tile cache layers; out_dropped counts layers past the per-band cap.
    void buildBand(const SSNavBuildInput& in, SSNavContext& ctx, SSNavCompressor& comp, std::vector<std::vector<U8> >& out_layers, S32& out_dropped, S32& out_under, SSNavMesh::SpanSheet* out_sheet)
    {
        out_dropped = 0;
        out_under = 0;
        rcConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.cs = SSNavMesh::CELL;
        cfg.ch = SSNavMesh::CELL;
        cfg.walkableSlopeAngle = in.mAgentSlope;
        cfg.walkableHeight = (int)ceilf(in.mAgentHeight / cfg.ch);
        cfg.walkableClimb = (int)floorf(in.mAgentClimb / cfg.ch);
        cfg.walkableRadius = (int)ceilf(in.mAgentRadius / cfg.cs);
        cfg.maxSimplificationError = SS_NAV_MAX_SIMPLIFICATION_ERROR;    // informational here; the tile cache applies its own copy (tcp.maxSimplificationError)
        const S32 min_island_cells = (S32)(SS_NAV_MIN_ISLAND_M / cfg.cs);
        const S32 min_island_area = min_island_cells * min_island_cells;
        cfg.maxVertsPerPoly = 6;
        cfg.tileSize = SSNavMesh::TILE_CELLS;
        cfg.borderSize = cfg.walkableRadius + 3;
        cfg.width = cfg.tileSize + cfg.borderSize * 2;
        cfg.height = cfg.tileSize + cfg.borderSize * 2;
        const F32 border = (F32)cfg.borderSize * cfg.cs;
        cfg.bmin[0] = in.mMin[0] - border; cfg.bmax[0] = in.mMax[0] + border;
        cfg.bmin[1] = in.mMin[2];          cfg.bmax[1] = in.mMax[2];
        cfg.bmin[2] = -in.mMax[1] - border; cfg.bmax[2] = -in.mMin[1] + border;

        const S32 ntris = in.mSoup.triCount();
        const S32 nblock = in.mBlockSoup.triCount();
        if (ntris == 0 && nblock == 0 && in.mConvex.empty()) return;
        std::vector<int> idx((size_t)llmax(ntris, nblock) * 3);
        for (S32 i = 0; i < (S32)idx.size(); ++i) idx[i] = i;
        std::vector<unsigned char> areas((size_t)llmax(ntris, nblock), 0);

        rcHeightfield* hf = rcAllocHeightfield();
        if (!hf || !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) { rcFreeHeightField(hf); return; }
        if (ntris > 0)
        {
            rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, in.mSoup.mVerts.data(), ntris * 3, idx.data(), ntris, areas.data());
            rcRasterizeTriangles(&ctx, in.mSoup.mVerts.data(), ntris * 3, idx.data(), areas.data(), ntris, *hf, cfg.walkableClimb);
        }
        if (nblock > 0)
        {
            // Static obstacles: rasterized with no area, so they block and shadow but never become floor.
            std::fill(areas.begin(), areas.end(), (unsigned char)RC_NULL_AREA);
            rcRasterizeTriangles(&ctx, in.mBlockSoup.mVerts.data(), nblock * 3, idx.data(), areas.data(), nblock, *hf, cfg.walkableClimb);
        }
        const float walkable_thr = cosf(cfg.walkableSlopeAngle * (F32)(3.14159265 / 180.0));
        for (const SSNavConvex& cv : in.mConvex) rasterizeConvex(ctx, *hf, cfg, cv, walkable_thr);
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);
        nullUnderTerrain(*hf, cfg, in, out_under);
        if (out_sheet) extractSpanSheet(*hf, cfg, in, *out_sheet);

        rcCompactHeightfield* chf = rcAllocCompactHeightfield();
        const bool compact_ok = chf && rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf);
        rcFreeHeightField(hf);
        if (!compact_ok) { rcFreeCompactHeightfield(chf); return; }
        if (cfg.walkableRadius > 0) rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);     // <SS:Nexii> radius 0 by default: a world surface, not an agent's
        for (const SSNavExclusion& e : in.mExclusions)
        {
            rcMarkConvexPolyArea(&ctx, e.mVerts, e.mCount, e.mMinY, e.mMaxY, RC_NULL_AREA, *chf);
        }

        rcHeightfieldLayerSet* lset = rcAllocHeightfieldLayerSet();
        if (lset && rcBuildHeightfieldLayers(&ctx, *chf, cfg.borderSize, (int)ceilf(SS_NAV_LAYER_MERGE_M / cfg.ch), *lset))
        {
            if (lset->nlayers > SS_NAV_MAX_LAYERS_PER_BAND) out_dropped = lset->nlayers - SS_NAV_MAX_LAYERS_PER_BAND;
            std::vector<S32> flood_stack;
            // <SS:Nexii> Over the cap, the layers with the most walkable cells publish: rcBuildHeightfieldLayers numbers layers in sweep order, so taking the first N dropped whole floors while keeping ledges. [interaction: SS_NAV_MAX_LAYERS_PER_BAND]
            std::vector<std::pair<S32, S32> > order;      // (-walkable cells, layer index)
            for (int i = 0; i < lset->nlayers; ++i)
            {
                rcHeightfieldLayer* layer = &lset->layers[i];
                cullLayerIslands(*layer, min_island_area, flood_stack);
                S32 cells = 0;
                for (S32 c = 0; c < layer->width * layer->height; ++c) if (layer->areas[c] != RC_NULL_AREA) ++cells;
                order.emplace_back(-cells, i);
            }
            std::sort(order.begin(), order.end());
            for (int k = 0; k < lset->nlayers && k < SS_NAV_MAX_LAYERS_PER_BAND; ++k)
            {
                if (order[k].first == 0) continue;               // every cell culled: nothing to publish
                const int i = order[k].second;
                rcHeightfieldLayer* layer = &lset->layers[i];
                dtTileCacheLayerHeader header;
                header.magic = DT_TILECACHE_MAGIC;
                header.version = DT_TILECACHE_VERSION;
                header.tx = in.mTx;
                header.ty = in.mTyDetour;
                header.tlayer = in.mBand * SS_NAV_MAX_LAYERS_PER_BAND + k;
                dtVcopy(header.bmin, layer->bmin);
                dtVcopy(header.bmax, layer->bmax);
                header.width = (unsigned char)layer->width;
                header.height = (unsigned char)layer->height;
                header.minx = (unsigned char)layer->minx;
                header.maxx = (unsigned char)layer->maxx;
                header.miny = (unsigned char)layer->miny;
                header.maxy = (unsigned char)layer->maxy;
                header.hmin = (unsigned short)layer->hmin;
                header.hmax = (unsigned short)layer->hmax;
                unsigned char* data = nullptr;
                int size = 0;
                if (dtStatusSucceed(dtBuildTileCacheLayer(&comp, &header, layer->heights, layer->areas, layer->cons, &data, &size)) && data)
                {
                    out_layers.emplace_back(data, data + size);
                    dtFree(data);
                }
            }
        }
        rcFreeHeightfieldLayerSet(lset);
        rcFreeCompactHeightfield(chf);
    }
}

struct SSNavMeshImpl
{
    dtTileCacheAlloc mAlloc;
    SSNavCompressor mCompressor;
    SSNavMeshProcess mProcess;
};

// ---------------------------------------------------------------------------- lifecycle

SSNavMesh::SSNavMesh() : mImpl(std::make_shared<SSNavMeshImpl>())
{
}

SSNavMesh::~SSNavMesh()
{
    teardown();
}

// Tile keys pack column and band; columns carry the band count for pruning.
U64 SSNavMesh::bandKey(S32 tx, S32 ty, S32 band)
{
    return ((U64)(U32)(tx + (1 << 20)) << 42) | ((U64)(U32)(ty + (1 << 20)) << 21) | (U64)(U32)band;
}

U64 SSNavMesh::columnKey(S32 tx, S32 ty)
{
    return bandKey(tx, ty, 0x1FFFFF);
}

// Agent space to the pinned build frame: the difference between the agent's current region origin and the one
// captured at init, so a border crossing shifts positions, never tile keys.
LLVector3 SSNavMesh::toLocal(const LLVector3& pos_agent) const
{
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp) return pos_agent;
    const LLVector3d d = regionp->getOriginGlobal() - mOriginGlobal;
    return pos_agent + LLVector3((F32)d.mdV[VX], (F32)d.mdV[VY], (F32)d.mdV[VZ]);
}

LLVector3 SSNavMesh::fromLocal(const LLVector3& pos_local) const
{
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp) return pos_local;
    const LLVector3d d = regionp->getOriginGlobal() - mOriginGlobal;
    return pos_local - LLVector3((F32)d.mdV[VX], (F32)d.mdV[VY], (F32)d.mdV[VZ]);
}

// Land height at a local-frame xy; false in the void between regions.
bool SSNavMesh::terrainZLocal(F32 x, F32 y, F32& z) const
{
    const LLVector3 pos_agent = fromLocal(LLVector3(x, y, 0.f));
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
    if (!regionp) return false;
    LLVector3 region_pos = regionp->getPosRegionFromAgent(pos_agent);
    region_pos.mV[VX] = llclamp(region_pos.mV[VX], 0.f, 255.9f);
    region_pos.mV[VY] = llclamp(region_pos.mV[VY], 0.f, 255.9f);
    z = regionp->getLand().resolveHeightRegion(region_pos.mV[VX], region_pos.mV[VY]);
    return true;
}

// Allocate the navmesh, the tile cache and the query once the agent has a region to pin the frame to.
bool SSNavMesh::ensureInit()
{
    if (mNavMesh) return true;
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp) return false;
    mOriginGlobal = regionp->getOriginGlobal();

    static LLCachedControl<F32> agent_height(gSavedSettings, "SSNavMeshAgentHeight", 0.5f);
    static LLCachedControl<F32> agent_radius(gSavedSettings, "SSNavMeshAgentRadius", 0.f);
    static LLCachedControl<F32> agent_climb(gSavedSettings, "SSNavMeshAgentClimb", 0.75f);

    dtTileCacheParams tcp;
    memset(&tcp, 0, sizeof(tcp));
    tcp.cs = CELL;
    tcp.ch = CELL;
    tcp.width = TILE_CELLS;
    tcp.height = TILE_CELLS;
    tcp.walkableHeight = agent_height;
    tcp.walkableRadius = agent_radius;
    tcp.walkableClimb = agent_climb;
    tcp.maxSimplificationError = SS_NAV_MAX_SIMPLIFICATION_ERROR;
    tcp.maxTiles = SS_NAV_MAX_TILES;
    tcp.maxObstacles = SS_NAV_MAX_OBSTACLES;
    mTileCache = dtAllocTileCache();
    if (!mTileCache || dtStatusFailed(mTileCache->init(&tcp, &mImpl->mAlloc, &mImpl->mCompressor, &mImpl->mProcess)))
    {
        LL_WARNS("SSNavMesh") << "tile cache init failed" << LL_ENDL;
        teardown();
        return false;
    }

    dtNavMeshParams np;
    memset(&np, 0, sizeof(np));
    np.tileWidth = TILE_M;
    np.tileHeight = TILE_M;
    np.maxTiles = SS_NAV_MAX_TILES;
    np.maxPolys = 1 << 16;      // per tile; DT_POLYREF64 gives 28 tile bits and 20 poly bits, so neither is squeezed
    mNavMesh = dtAllocNavMesh();
    if (!mNavMesh || dtStatusFailed(mNavMesh->init(&np)))
    {
        LL_WARNS("SSNavMesh") << "navmesh init failed" << LL_ENDL;
        teardown();
        return false;
    }
    mQuery = dtAllocNavMeshQuery();
    if (!mQuery || dtStatusFailed(mQuery->init(mNavMesh, SS_NAV_QUERY_NODES)))
    {
        LL_WARNS("SSNavMesh") << "navmesh query init failed" << LL_ENDL;
        teardown();
        return false;
    }
    mCensusStamp = 0;
    LL_INFOS("SSNavMesh") << "navmesh up: origin " << mOriginGlobal << ", " << SS_NAV_MAX_TILES << " tile slots" << LL_ENDL;
    return true;
}

// Drop everything; in-flight worker results are refused by the generation bump.
void SSNavMesh::teardown()
{
    ++mGeneration;
    if (mQuery) { dtFreeNavMeshQuery(mQuery); mQuery = nullptr; }
    if (mTileCache) { dtFreeTileCache(mTileCache); mTileCache = nullptr; }
    if (mNavMesh) { dtFreeNavMesh(mNavMesh); mNavMesh = nullptr; }
    mBands.clear();
    mSheetsHeld = 0;            // every band's sheet went with it; the field's next settle finds an empty stamp and drops its grid
    mColumns.clear();
    mWorklist.clear();
    mObstacles.clear();
    mObstacleAdds.clear();
    mObstacleRemovals.clear();
    mLayerBytes = 0;
    mCensusStamp = 0;
}

// ---------------------------------------------------------------------------- per frame

void SSNavMesh::update()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSNavMesh", false);
    static LLCachedControl<bool> census_enabled(gSavedSettings, "SSWorldFieldShapes", false);
    static LLCachedControl<U32> builds_per_frame(gSavedSettings, "SSNavMeshBuildsPerFrame", 4);
    static LLCachedControl<U32> max_in_flight(gSavedSettings, "SSNavMeshMaxInFlight", 4);
    if (!enabled || !census_enabled)
    {
        if (mNavMesh) teardown();
        return;
    }
    LL_RECORD_BLOCK_TIME(FTM_SS_NAVMESH_UPDATE);
    if (!ensureInit()) return;

    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();
    if (shapes->censusCurrent())
    {
        const U64 stamp = shapes->censusStamp();
        if (stamp != mCensusStamp)
        {
            mCensusStamp = stamp;
            schedule();
            syncObstacles();
        }
    }

    U32 launched = 0;
    while (launched < (U32)builds_per_frame && mInFlight < (S32)max_in_flight && !mWorklist.empty())
    {
        const Job job = mWorklist.back();
        mWorklist.pop_back();
        launch(job);
        ++launched;
    }

    // <SS:Nexii> One update per frame builds at most one obstacle-affected tile from its cached layer (0.26 ms in the benchmark), so movers cost a bounded slice whatever their count. [interaction: DYNAMIC]
    pumpObstacles();
    bool up_to_date = false;
    mTileCache->update(0.f, mNavMesh, &up_to_date);
}

// Bands off the census: every store-bound record nominates the columns its AABB meets with its z interval; the
// intervals of a column merge into bands across gaps smaller than SSNavMeshBandGap; a band whose geometry signature
// changed since it was published becomes a job. Columns and bands that vanished are evicted.
void SSNavMesh::schedule()
{
    static LLCachedControl<F32> nav_range(gSavedSettings, "SSNavMeshRange", 512.f);
    static LLCachedControl<F32> band_gap(gSavedSettings, "SSNavMeshBandGap", 8.f);
    // <SS:Nexii> The navmesh is a stable surface, not a bubble: columns are scheduled only while they lie wholly inside the census envelope (so every band sees all of its records), and a column that drifts out of it keeps its bands until its region leaves the world. The census reaches SSNavMeshRange while the navmesh is on. [interaction: SSWorldFieldShapes::envelopeRange]
    const F32 env = llmin(llclamp((F32)nav_range, 32.f, 1024.f), SSWorldFieldShapes::envelopeRange());
    const F32 gap = llmax((F32)band_gap, 2.f);
    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();
    const LLVector3 anchor = toLocal(shapes->censusAnchor());
    const LLVector3 bmin = shapes->censusAnchor() - LLVector3(env, env, env);
    const LLVector3 bmax = shapes->censusAnchor() + LLVector3(env, env, env);
    const LLVector3 off = toLocal(LLVector3::zero);      // agent -> local offset

    struct Interval { F32 mLo, mHi; U64 mSig; };
    std::map<U64, std::vector<Interval> > columns;
    const F32 inv = 1.f / TILE_M;
    const S32 cx0 = (S32)ceilf((anchor.mV[VX] - env) * inv), cx1 = (S32)floorf((anchor.mV[VX] + env) * inv) - 1;
    const S32 cy0 = (S32)ceilf((anchor.mV[VY] - env) * inv), cy1 = (S32)floorf((anchor.mV[VY] + env) * inv) - 1;

    S32 seen = 0, dynamic = 0, phantom = 0, tris = 0;
    shapes->forEachRecord(bmin, bmax, [&](const SSWorldFieldShapes::Record& rec)
    {
        ++seen;
        if (rec.mDynamic) { ++dynamic; return; }
        if (navIgnores(rec)) { ++phantom; return; }
        tris += (S32)(rec.tris().size() / 3);
        const LLVector3 lo = rec.mBMin + off, hi = rec.mBMax + off;
        const S32 x0 = llmax(cx0, (S32)floorf(lo.mV[VX] * inv)), x1 = llmin(cx1, (S32)floorf(hi.mV[VX] * inv));
        const S32 y0 = llmax(cy0, (S32)floorf(lo.mV[VY] * inv)), y1 = llmin(cy1, (S32)floorf(hi.mV[VY] * inv));
        if (x1 < x0 || y1 < y0) return;
        if ((S64)(x1 - x0 + 1) * (y1 - y0 + 1) > 256) return;    // a kilometre-scale surround is not navigable structure
        U64 sig = 1469598103934665603ull;
        for (U32 c = 0; c < 3; ++c) { sig = fnvF(sig, lo.mV[c]); sig = fnvF(sig, hi.mV[c]); }
        sig = fnv(sig, (U64)rec.mClass | ((U64)rec.mProv << 8) | ((U64)rec.mNavRole << 12) | ((U64)rec.tris().size() << 16));
        for (S32 y = y0; y <= y1; ++y) for (S32 x = x0; x <= x1; ++x)
        {
            columns[columnKey(x, y)].push_back(Interval{lo.mV[VZ], hi.mV[VZ], sig});
        }
    });

    // <SS:Nexii> Terrain: every column in the envelope carries its land interval and a signature taken from the 16 m surface patches it touches - each patch's min/max height and the time the sim last updated it. That changes exactly when the land changes and never otherwise; sampling heights here churned every band each census once the sample grid moved. [interaction: part cache]
    for (S32 y = cy0; y <= cy1; ++y) for (S32 x = cx0; x <= cx1; ++x)
    {
        F32 lo = FLT_MAX, hi = -FLT_MAX;
        U64 sig = 14695981039346656037ull;
        bool any = false;
        const F32 sig_step = (TILE_M + 2.f * SS_NAV_TERRAIN_MARGIN_M) * 0.5f;    // <SS:Nexii> -2, +8, +18 m: the whole window emitTerrain rasterizes, so the +x/+y margin's patch is signed too; 8 m steps stopped at +14 and left that strip's edits unseen.
        for (S32 gy = 0; gy <= 2; ++gy) for (S32 gx = 0; gx <= 2; ++gx)       // every 16 m patch the bordered column touches
        {
            const LLVector3 pos_agent = fromLocal(LLVector3((F32)x * TILE_M - SS_NAV_TERRAIN_MARGIN_M + (F32)gx * sig_step,
                                                            (F32)y * TILE_M - SS_NAV_TERRAIN_MARGIN_M + (F32)gy * sig_step, 0.f));
            LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent);
            if (!regionp) continue;
            LLVector3 region_pos = regionp->getPosRegionFromAgent(pos_agent);
            region_pos.mV[VX] = llclamp(region_pos.mV[VX], 0.f, 255.9f);
            region_pos.mV[VY] = llclamp(region_pos.mV[VY], 0.f, 255.9f);
            const LLSurfacePatch* patch = regionp->getLand().resolvePatchRegion(region_pos);
            if (!patch) continue;
            any = true;
            lo = llmin(lo, patch->getMinZ()); hi = llmax(hi, patch->getMaxZ());
            sig = fnv(sig, patch->getLastUpdateTime());
            sig = fnv(sig, regionp->getHandle());
            // <SS:Nexii> Land under water carries the water surface in its band, so the span sheet can record the surface the way the depth capture did; the level joins the signature. [interaction: extractSpanSheet]
            const F32 water = regionp->getWaterHeight();
            if (water > patch->getMinZ()) { hi = llmax(hi, water); sig = fnvF(sig, water); }
        }
        if (any) columns[columnKey(x, y)].push_back(Interval{lo, hi, sig});
    }

    for (auto& kv : mBands) kv.second.mAlive = false;
    mWorklist.clear();
    std::unordered_map<U64, S32> new_columns;
    for (auto& kv : columns)
    {
        std::vector<Interval>& iv = kv.second;
        std::sort(iv.begin(), iv.end(), [](const Interval& a, const Interval& b) { return a.mLo < b.mLo; });
        const S32 tx = (S32)((kv.first >> 42) & 0x1FFFFF) - (1 << 20);
        const S32 ty = (S32)((kv.first >> 21) & 0x1FFFFF) - (1 << 20);
        std::vector<Interval> bands;
        for (const Interval& i : iv)
        {
            if (!bands.empty() && i.mLo - bands.back().mHi < gap)
            {
                bands.back().mHi = llmax(bands.back().mHi, i.mHi);
                bands.back().mSig = fnv(bands.back().mSig, i.mSig);
            }
            else if ((S32)bands.size() < MAX_BANDS)
            {
                bands.push_back(Interval{i.mLo, i.mHi, fnv(1469598103934665603ull, i.mSig)});
            }
            else
            {
                bands.back().mHi = llmax(bands.back().mHi, i.mHi);     // over the band budget: fold into the top band
                bands.back().mSig = fnv(bands.back().mSig, i.mSig);
            }
        }
        new_columns[kv.first] = (S32)bands.size();
        // <SS:Nexii> Band identity is a persistent slot, not the ordinal: a new band takes the slot of the existing band its z-range overlaps most, so a skybox appearing or a mover settling elsewhere in the column never renumbers its neighbours' layers and never rebuilds them. Unmatched bands take the lowest free slot. [interaction: Detour tile layer index]
        bool slot_used[MAX_BANDS] = {};
        std::vector<S32> slot_of(bands.size(), -1);
        for (size_t b = 0; b < bands.size(); ++b)
        {
            const F32 lo = floorf((bands[b].mLo - 1.f) / CELL) * CELL, hi = ceilf((bands[b].mHi + 1.f) / CELL) * CELL;
            F32 best = 0.f;
            S32 best_slot = -1;
            for (S32 slot = 0; slot < MAX_BANDS; ++slot)
            {
                if (slot_used[slot]) continue;
                auto it = mBands.find(bandKey(tx, ty, slot));
                if (it == mBands.end()) continue;
                const F32 overlap = llmin(hi, it->second.mZMax) - llmax(lo, it->second.mZMin);
                if (overlap > best) { best = overlap; best_slot = slot; }
            }
            if (best_slot >= 0) { slot_of[b] = best_slot; slot_used[best_slot] = true; }
        }
        for (size_t b = 0; b < bands.size(); ++b)
        {
            if (slot_of[b] >= 0) continue;
            for (S32 slot = 0; slot < MAX_BANDS; ++slot)
            {
                if (slot_used[slot] || mBands.count(bandKey(tx, ty, slot))) continue;
                slot_of[b] = slot; slot_used[slot] = true;
                break;
            }
            if (slot_of[b] < 0)
            {
                // Every slot is held by a band this pass did not match: take the lowest slot not used this pass.
                for (S32 slot = 0; slot < MAX_BANDS; ++slot) { if (!slot_used[slot]) { slot_of[b] = slot; slot_used[slot] = true; break; } }
            }
        }
        for (size_t b = 0; b < bands.size(); ++b)
        {
            if (slot_of[b] < 0) continue;
            const U64 key = bandKey(tx, ty, slot_of[b]);
            Band& band = mBands[key];
            band.mAlive = true;
            // Band bounds snap to the global cell lattice: every tile then quantizes heights on the same grid, so a
            // corner shared by four tiles lands at one elevation instead of four. [interaction: renderDebug seams]
            const F32 zmin = floorf((bands[b].mLo - 1.f) / CELL) * CELL, zmax = ceilf((bands[b].mHi + 1.f) / CELL) * CELL;
            const U64 sig = fnvF(fnvF(bands[b].mSig, bands[b].mLo), bands[b].mHi);
            if (band.mRefs.empty()) { band.mZMin = zmin; band.mZMax = zmax; }     // pending or new: the range the match will see
            if (band.mSig == sig && !band.mRefs.empty()) continue;
            Job job;
            job.mTx = tx; job.mTy = ty; job.mBand = slot_of[b];
            job.mZMin = zmin; job.mZMax = zmax;
            job.mSig = sig;
            mWorklist.push_back(job);
        }
    }
    mColumns.swap(new_columns);

    // Loaded regions in the local frame: a band outside all of them has lost its world and goes; a band inside the
    // envelope that this schedule did not touch has genuinely vanished (bands merged, column emptied) and goes; a
    // band beyond the envelope but still inside a loaded region stays as built.
    // <SS:Nexii> Off-sim builds (a root in the region, the decor out in the void) and sim surrounds are navmesh too, so a loaded region keeps bands out to SSNavMeshVoidMargin around it; the envelope decides what gets scheduled, this only decides what a column that left the envelope may keep. [interaction: census envelope]
    static LLCachedControl<F32> void_margin_setting(gSavedSettings, "SSNavMeshVoidMargin", 256.f);
    const F32 void_margin = llclamp((F32)void_margin_setting, TILE_M, 1024.f);
    struct RegionBox { F32 x0, y0, x1, y1; };
    std::vector<RegionBox> regions;
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (!regionp) continue;
        const LLVector3d d = regionp->getOriginGlobal() - mOriginGlobal;
        const F32 w = regionp->getWidth();
        regions.push_back(RegionBox{(F32)d.mdV[VX] - void_margin, (F32)d.mdV[VY] - void_margin, (F32)d.mdV[VX] + w + void_margin, (F32)d.mdV[VY] + w + void_margin});
    }
    for (auto it = mBands.begin(); it != mBands.end();)
    {
        const S32 tx = (S32)((it->first >> 42) & 0x1FFFFF) - (1 << 20);
        const S32 ty = (S32)((it->first >> 21) & 0x1FFFFF) - (1 << 20);
        const bool in_envelope = tx >= cx0 && tx <= cx1 && ty >= cy0 && ty <= cy1;
        bool in_world = false;
        const F32 x = (F32)tx * TILE_M, y = (F32)ty * TILE_M;
        for (const RegionBox& r : regions)
        {
            if (x + TILE_M > r.x0 && x < r.x1 && y + TILE_M > r.y0 && y < r.y1) { in_world = true; break; }
        }
        const bool keep = it->second.mAlive || (in_world && !in_envelope);
        if (keep) { ++it; continue; }
        removeBand(it->first, it->second);   // <SS:Nexii> the band's sheet dies with it; the field notices through the stamp on its next settle [interaction: SSWorldField::scheduleGrid]
        it = mBands.erase(it);
    }

    mLastSeen = seen; mLastDynamic = dynamic; mLastPhantom = phantom;
    if (!mWorklist.empty())
    {
        LL_INFOS("SSNavMesh") << "schedule: census " << seen << " records (" << dynamic << " dynamic, " << phantom
                              << " phantom/no-physics skipped, " << tris << " soup tris), " << mColumns.size() << " columns, " << mBands.size()
                              << " bands, " << mWorklist.size() << " to build, " << polyCount() << " polys published, "
                              << (mLayerBytes / 1024) << " KB of layers" << LL_ENDL;
    }

    // Ground-up, nearest-first: the bands the agent stands in publish first.
    std::sort(mWorklist.begin(), mWorklist.end(), [&](const Job& a, const Job& b)
    {
        const F32 da = fabsf((F32)a.mTx * TILE_M + TILE_M * 0.5f - anchor.mV[VX]) + fabsf((F32)a.mTy * TILE_M + TILE_M * 0.5f - anchor.mV[VY]) + fabsf(a.mZMin - anchor.mV[VZ]);
        const F32 db = fabsf((F32)b.mTx * TILE_M + TILE_M * 0.5f - anchor.mV[VX]) + fabsf((F32)b.mTy * TILE_M + TILE_M * 0.5f - anchor.mV[VY]) + fabsf(b.mZMin - anchor.mV[VZ]);
        return da > db;     // back() pops first
    });
}

// <SS:Nexii> Whether band builds keep a world-field sheet: the field's master switch alone, so a sheet is either kept by every band or by none. An LLCachedControl, so it follows a live change - but only for builds launched after it; resheet() is what recovers the bands built under the old answer. [interaction: SSWorldField::update]
bool SSNavMesh::sheetsWanted()
{
    static LLCachedControl<bool> field(gSavedSettings, "SSWorldField", true);
    return field;
}

// <SS:Nexii> The off->on recovery. Zeroing the signature is what makes schedule()
// see the band as changed; mCensusStamp = 0 is what makes the next update() schedule
// at all. Bands still in flight or queued are skipped - they will build under the
// current answer anyway.
S32 SSNavMesh::resheet()
{
    if (!mNavMesh) return 0;
    S32 queued = 0;
    for (auto& kv : mBands)
    {
        if (kv.second.mSheet) continue;         // already has one
        // <SS:Nexii> NOT gated on mRefs: a band that published and produced zero
        // walkable layers has an empty mRefs and no sheet, and skipping it would leave
        // it in exactly the permanent no-sheet state this exists to break. mSig is the
        // honest test for "has published at all" - schedule() sets it on publish and it
        // is 0 until then, so a band still queued is skipped and will build under the
        // current answer anyway.
        if (kv.second.mSig == 0) continue;
        kv.second.mSig = 0;
        ++queued;
    }
    if (queued) mCensusStamp = 0;
    return queued;
}

// <SS:Nexii> The region's published sheet set, shared_ptr copies only: every live band whose column centre falls inside the region, with the agent-space corner the field needs to place its cells, plus a stamp mixing each band's key and geometry signature. The stamp is the field's change detector - an identical stamp after a settle means nothing was rebuilt and the published grid still describes the world. Main thread only. [interaction: SSWorldField::scheduleGrid]
bool SSNavMesh::collectSheets(U64 region_handle, std::vector<BandSheet>& out, U64& out_stamp) const
{
    out.clear();
    out_stamp = 0;
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp || !mNavMesh) return false;

    const LLVector3 o = toLocal(regionp->getOriginAgent());
    const F32 w = regionp->getWidth();
    for (const auto& kv : mBands)
    {
        if (!kv.second.mSheet) continue;
        const S32 tx = (S32)((kv.first >> 42) & 0x1FFFFF) - (1 << 20);
        const S32 ty = (S32)((kv.first >> 21) & 0x1FFFFF) - (1 << 20);
        const F32 x = ((F32)tx + 0.5f) * TILE_M - o.mV[VX], y = ((F32)ty + 0.5f) * TILE_M - o.mV[VY];
        if (x < 0.f || x >= w || y < 0.f || y >= w) continue;

        BandSheet bs;
        bs.mTx = tx;
        bs.mTy = ty;
        bs.mZMin = kv.second.mZMin;
        bs.mZMax = kv.second.mZMax;
        bs.mOriginAgent = fromLocal(LLVector3((F32)tx * TILE_M, (F32)ty * TILE_M, 0.f));
        bs.mSheet = kv.second.mSheet;
        out.push_back(bs);

        // <SS:Nexii> Per-band terms are SUMMED, never folded in sequence: mBands is an unordered_map and a rehash reorders it, so any order-dependent mix would move the stamp without a single band having changed and cost the world field a whole region walk. Each term is already avalanched, so the sum is a sound combiner. [interaction: SSWorldField::navSettle]
        U64 term = kv.first * 0x9E3779B97F4A7C15ull ^ kv.second.mSig * 0xC2B2AE3D27D4EB4Full;
        term ^= term >> 29;
        term *= 0xBF58476D1CE4E5B9ull;
        term ^= term >> 32;
        out_stamp += term;
    }
    // The band count joins it, so an empty set and a set whose terms happen to
    // sum to zero are not the same stamp.
    out_stamp = out_stamp * 0x9E3779B97F4A7C15ull + (U64)out.size() + 1u;
    return true;
}

void SSNavMesh::dumpAt(const LLVector3& bmin_agent, const LLVector3& bmax_agent, std::vector<std::string>& out) const
{
    if (!mNavMesh) { out.push_back("navmesh: not running (SSNavMesh off or no region yet)"); return; }
    static LLCachedControl<F32> range_setting(gSavedSettings, "SSNavMeshRange", 512.f);
    const LLVector3 lo = toLocal(bmin_agent), hi = toLocal(bmax_agent);
    const LLVector3 anchor = toLocal(SSWorldFieldShapes::getInstance()->censusAnchor());
    const S32 x0 = (S32)floorf(lo.mV[VX] / TILE_M), x1 = (S32)floorf(hi.mV[VX] / TILE_M);
    const S32 y0 = (S32)floorf(lo.mV[VY] / TILE_M), y1 = (S32)floorf(hi.mV[VY] / TILE_M);
    out.push_back(llformat("navmesh: box z %.1f..%.1f covers columns x %d..%d y %d..%d (16 m each); %d columns, %d bands, %d queued, %d building, census stamp %s",
                           lo.mV[VZ], hi.mV[VZ], x0, x1, y0, y1, columnCount(), bandCount(), pendingCount(), inFlightCount(),
                           mCensusStamp == 0 ? "reset (schedule pending)" : "set"));
    S32 shown = 0;
    for (S32 ty = y0; ty <= y1 && shown < 16; ++ty) for (S32 tx = x0; tx <= x1 && shown < 16; ++tx, ++shown)
    {
        const F32 cx = ((F32)tx + 0.5f) * TILE_M, cy = ((F32)ty + 0.5f) * TILE_M;
        const F32 d = llmax(fabsf(cx - anchor.mV[VX]), fabsf(cy - anchor.mV[VY]));
        auto cit = mColumns.find(columnKey(tx, ty));
        S32 queued = 0, building = 0;
        for (const Job& j : mWorklist) if (j.mTx == tx && j.mTy == ty) ++queued;
        for (const Job& j : mInFlightJobs) if (j.mTx == tx && j.mTy == ty) ++building;
        out.push_back(llformat("  column (%d, %d): %s, %.0f m from the anchor (range %.0f), %d queued, %d building",
                               tx, ty, cit == mColumns.end() ? "NOT SCHEDULED (outside the envelope or nothing there)" : llformat("%d bands scheduled", cit->second).c_str(),
                               d, (F32)range_setting, queued, building));
        for (S32 slot = 0; slot < MAX_BANDS; ++slot)
        {
            auto bit = mBands.find(bandKey(tx, ty, slot));
            if (bit == mBands.end()) continue;
            const Band& b = bit->second;
            const bool overlaps = hi.mV[VZ] >= b.mZMin && lo.mV[VZ] <= b.mZMax;
            out.push_back(llformat("    band slot %d: z %.1f..%.1f%s, %s, %d tiles, published %.1f s ago, %d layers dropped, %d spans nulled under the land",
                                   slot, b.mZMin, b.mZMax, overlaps ? " (covers the box)" : "", b.mAlive ? "alive" : "stale",
                                   (S32)b.mRefs.size(), LLFrameTimer::getTotalSeconds() - b.mPublishedAt, b.mLayersDropped, b.mUnderTerrain));
        }
    }
    // The land under the box centre as the build reads it, and the ceiling nullUnderTerrain enforces from the raster cell's four corners: a floor whose top sits under that ceiling has no navmesh by design.
    {
        const LLVector3 cl = toLocal(LLVector3((bmin_agent.mV[VX] + bmax_agent.mV[VX]) * 0.5f, (bmin_agent.mV[VY] + bmax_agent.mV[VY]) * 0.5f, 0.f));
        const F32 x0 = floorf(cl.mV[VX] / CELL) * CELL, y0 = floorf(cl.mV[VY] / CELL) * CELL;
        F32 zc = 0.f, zmax = -FLT_MAX;
        const bool has_c = terrainZLocal(cl.mV[VX], cl.mV[VY], zc);
        S32 corners = 0;
        for (S32 c = 0; c < 4; ++c) { F32 z; if (terrainZLocal(x0 + ((c & 1) ? CELL : 0.f), y0 + ((c & 2) ? CELL : 0.f), z)) { zmax = llmax(zmax, z); ++corners; } }
        if (has_c) out.push_back(llformat("  land at the box centre: z %.2f; nullUnderTerrain ceiling over its raster cell %.2f (max of %d corners minus the %.2f m slop): floor tops under that have no navmesh by design", zc, corners ? zmax - SS_NAV_TERRAIN_SLOP_M : -1.f, corners, SS_NAV_TERRAIN_SLOP_M));
        else out.push_back("  land at the box centre: none (void or no region)");
    }
    const LLVector3 top((bmin_agent.mV[VX] + bmax_agent.mV[VX]) * 0.5f, (bmin_agent.mV[VY] + bmax_agent.mV[VY]) * 0.5f, bmax_agent.mV[VZ] + 0.2f);
    LLVector3 nearest;
    if (nearestPoint(top, 2.f, nearest))
    {
        out.push_back(llformat("  probe: nearest navmesh point to the box top (%.1f, %.1f, %.1f) is (%.1f, %.1f, %.1f), %.2f m away",
                               top.mV[VX], top.mV[VY], top.mV[VZ], nearest.mV[VX], nearest.mV[VY], nearest.mV[VZ], (nearest - top).magVec()));
    }
    else
    {
        out.push_back(llformat("  probe: no navmesh within 2 m of the box top (%.1f, %.1f, %.1f)", top.mV[VX], top.mV[VY], top.mV[VZ]));
    }
}

void SSNavMesh::dumpLinksAt(const LLVector3& pos_agent, F32 radius, std::vector<std::string>& out) const
{
    if (!mNavMesh) return;
    const dtNavMesh* nm = mNavMesh;
    S32 shown = 0, portal = 0, unlinked = 0;
    for (int i = 0; i < nm->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = nm->getTile(i);
        if (!tile || !tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p)
        {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly.vertCount < 3) continue;
            for (int v = 0; v < (int)poly.vertCount; ++v)
            {
                if (!(poly.neis[v] & DT_EXT_LINK)) continue;
                const float* a = &tile->verts[poly.verts[v] * 3];
                const float* b = &tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3];
                const LLVector3 pa = fromLocal(LLVector3(a[0], -a[2], a[1])), pb = fromLocal(LLVector3(b[0], -b[2], b[1]));
                const LLVector3 mid = (pa + pb) * 0.5f;
                if ((mid - pos_agent).magVec() > radius) continue;
                bool linked = false;
                for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                {
                    if (tile->links[l].edge == v && tile->links[l].side != 0xff) { linked = true; break; }
                }
                ++portal;
                if (!linked) ++unlinked;
                if (shown < 40)
                {
                    ++shown;
                    out.push_back(llformat("  portal edge tile (%d, %d) band %d layer %d poly %d edge %d: %s, (%.2f, %.2f, %.2f)-(%.2f, %.2f, %.2f)",
                                           tile->header->x, -tile->header->y - 1, tile->header->layer / SS_NAV_MAX_LAYERS_PER_BAND, tile->header->layer % SS_NAV_MAX_LAYERS_PER_BAND,
                                           p, v, linked ? "linked" : "UNLINKED (red)",
                                           pa.mV[VX], pa.mV[VY], pa.mV[VZ], pb.mV[VX], pb.mV[VY], pb.mV[VZ]));
                }
            }
        }
    }
    out.push_back(llformat("  portal edges within %.0f m: %d, unlinked %d%s", radius, portal, unlinked, portal > shown ? " (first 40 listed)" : ""));
}

bool SSNavMesh::regionSettled(U64 region_handle) const
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp || !mNavMesh || mCensusStamp == 0) return false;      // a schedule is still to run
    const LLVector3 o = toLocal(regionp->getOriginAgent());
    const F32 w = regionp->getWidth();
    auto inside = [&](S32 tx, S32 ty)
    {
        const F32 x = ((F32)tx + 0.5f) * TILE_M - o.mV[VX], y = ((F32)ty + 0.5f) * TILE_M - o.mV[VY];
        return x >= 0.f && x < w && y >= 0.f && y < w;
    };
    for (const Job& j : mWorklist) if (inside(j.mTx, j.mTy)) return false;
    for (const Job& j : mInFlightJobs) if (inside(j.mTx, j.mTy)) return false;
    return true;
}

void SSNavMesh::removeBand(U64 key, Band& band)
{
    (void)key;
    mLayersDropped -= (U32)band.mLayersDropped;
    band.mLayersDropped = 0;
    if (band.mSheet) { if (mSheetsHeld) --mSheetsHeld; band.mSheet.reset(); }
    if (!mTileCache || !mNavMesh) { band.mRefs.clear(); return; }
    for (U32 ref : band.mRefs)
    {
        const dtCompressedTile* tile = mTileCache->getTileByRef(ref);
        if (tile && tile->header)
        {
            mNavMesh->removeTile(mNavMesh->getTileRefAt(tile->header->tx, tile->header->ty, tile->header->tlayer), nullptr, nullptr);
            mLayerBytes -= (size_t)tile->dataSize;
        }
        mTileCache->removeTile(ref, nullptr, nullptr);
    }
    band.mRefs.clear();
}

// Snapshot the band's geometry on the main thread, hand the Recast pipeline to the General queue, publish on return.
void SSNavMesh::launch(const Job& job)
{
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue) return;

    static LLCachedControl<F32> agent_height(gSavedSettings, "SSNavMeshAgentHeight", 0.5f);
    static LLCachedControl<F32> agent_radius(gSavedSettings, "SSNavMeshAgentRadius", 0.f);
    static LLCachedControl<F32> agent_climb(gSavedSettings, "SSNavMeshAgentClimb", 0.75f);
    static LLCachedControl<F32> agent_slope(gSavedSettings, "SSNavMeshAgentSlope", 45.f);

    std::shared_ptr<SSNavBuildInput> in = std::make_shared<SSNavBuildInput>();
    in->mTx = job.mTx;
    in->mTyDetour = -job.mTy - 1;               // Detour tile y runs along Recast z = -local y
    in->mBand = job.mBand;
    in->mAgentHeight = agent_height; in->mAgentRadius = agent_radius; in->mAgentClimb = agent_climb; in->mAgentSlope = agent_slope;
    in->mMin[0] = (F32)job.mTx * TILE_M; in->mMin[1] = (F32)job.mTy * TILE_M; in->mMin[2] = job.mZMin;
    in->mMax[0] = in->mMin[0] + TILE_M;  in->mMax[1] = in->mMin[1] + TILE_M;  in->mMax[2] = job.mZMax;

    const F32 border = (ceilf(agent_radius / CELL) + 3.f) * CELL;
    const LLVector3 off = toLocal(LLVector3::zero);
    const LLVector3 gmin_agent = LLVector3(in->mMin[0] - border, in->mMin[1] - border, in->mMin[2]) - off;
    const LLVector3 gmax_agent = LLVector3(in->mMax[0] + border, in->mMax[1] + border, in->mMax[2]) - off;
    SSWorldFieldShapes::getInstance()->forEachRecord(gmin_agent, gmax_agent, [&](const SSWorldFieldShapes::Record& rec)
    {
        if (rec.mDynamic) return;
        if (navIgnores(rec)) return;
        emitRecord(rec, off, *in);
    });

    // Terrain heights for the bordered column, void marked below -900 so the worker skips those cells.
    in->mTerrain.assign((size_t)SS_NAV_TERRAIN_NODES * SS_NAV_TERRAIN_NODES, -1000.f);
    const F32 tx0 = in->mMin[0] - SS_NAV_TERRAIN_MARGIN_M, ty0 = in->mMin[1] - SS_NAV_TERRAIN_MARGIN_M;
    in->mTerrainX0 = tx0; in->mTerrainY0 = ty0;
    bool terrain_in_band = false;
    for (S32 gy = 0; gy < SS_NAV_TERRAIN_NODES; ++gy) for (S32 gx = 0; gx < SS_NAV_TERRAIN_NODES; ++gx)
    {
        F32 z;
        if (!terrainZLocal(tx0 + (F32)gx, ty0 + (F32)gy, z)) continue;
        in->mTerrain[gy * SS_NAV_TERRAIN_NODES + gx] = z;
        if (z >= in->mMin[2] - 1.f && z <= in->mMax[2] + 1.f) terrain_in_band = true;
    }
    if (terrain_in_band) emitTerrain(in->mTerrain.data(), tx0, ty0, in->mSoup);
    in->mWantSheet = sheetsWanted();
    if (in->mWantSheet)
    {
        LLViewerRegion* column_region = LLWorld::getInstance()->getRegionFromPosAgent(fromLocal(LLVector3(in->mMin[0] + 0.5f * TILE_M, in->mMin[1] + 0.5f * TILE_M, 0.f)));
        in->mWaterZ = column_region ? column_region->getWaterHeight() : -1e30f;     // local z is agent z
    }

    const U32 generation = mGeneration;
    std::shared_ptr<SSNavMeshImpl> impl = mImpl;
    std::weak_ptr<bool> alive = mAlive;
    ++mInFlight;
    mInFlightJobs.push_back(job);
    const bool posted = main_queue->postTo(
        general_queue,
        [in, impl, job, generation]() -> std::shared_ptr<Result>
        {
            std::shared_ptr<Result> r = std::make_shared<Result>();
            r->mJob = job;
            r->mGeneration = generation;
            LLTimer t;
            if (in->mWantSheet) r->mSheet = std::make_shared<SSNavMesh::SpanSheet>();
            SSNavContext ctx;
            buildBand(*in, ctx, impl->mCompressor, r->mLayers, r->mLayersDropped, r->mUnderTerrain, r->mSheet.get());
            r->mLog = ctx.mLog;
            r->mMS = t.getElapsedTimeF32() * 1000.f;
            r->mOk = true;
            return r;
        },
        [this, alive](std::shared_ptr<Result> r)
        {
            if (alive.expired()) return;    // the singleton died with this build in flight
            publish(r);
        });
    if (!posted)
    {
        --mInFlight;
        mInFlightJobs.pop_back();
        LL_WARNS("SSNavMesh") << "General work queue refused a band build; navmesh will not fill" << LL_ENDL;
    }
}

// Main thread: swap the band's layers into the tile cache and rebuild its navmesh tiles from them.
void SSNavMesh::publish(const std::shared_ptr<Result>& result)
{
    --mInFlight;
    if (result)
    {
        for (size_t i = 0; i < mInFlightJobs.size(); ++i)
        {
            const Job& j = mInFlightJobs[i];
            if (j.mTx == result->mJob.mTx && j.mTy == result->mJob.mTy && j.mBand == result->mJob.mBand) { mInFlightJobs.erase(mInFlightJobs.begin() + i); break; }
        }
    }
    if (!result || result->mGeneration != mGeneration || !mTileCache || !mNavMesh) return;
    LL_RECORD_BLOCK_TIME(FTM_SS_NAVMESH_PUBLISH);
    const Job& job = result->mJob;
    auto it = mBands.find(bandKey(job.mTx, job.mTy, job.mBand));
    if (it == mBands.end()) return;                     // evicted while building
    Band& band = it->second;
    const S32 dropped_before = band.mLayersDropped;
    const S32 under_before = band.mUnderTerrain;
    removeBand(it->first, band);
    band.mSig = job.mSig;
    band.mZMin = job.mZMin;
    band.mZMax = job.mZMax;
    for (const std::vector<U8>& layer : result->mLayers)
    {
        unsigned char* data = (unsigned char*)dtAlloc((size_t)layer.size(), DT_ALLOC_PERM);
        if (!data) continue;
        memcpy(data, layer.data(), layer.size());
        dtCompressedTileRef ref = 0;
        if (dtStatusFailed(mTileCache->addTile(data, (int)layer.size(), DT_COMPRESSEDTILE_FREE_DATA, &ref)))
        {
            dtFree(data);
            continue;
        }
        band.mRefs.push_back(ref);
        mLayerBytes += layer.size();
    }
    LLTimer publish_timer;
    mTileCache->buildNavMeshTilesAt(job.mTx, -job.mTy - 1, mNavMesh);
    mLastPublishMS = publish_timer.getElapsedTimeF32() * 1000.f;
    band.mSheet = result->mSheet;       // <SS:Nexii> the band keeps its per-cell sheet: the world field's geometry lives here, not in a store of its own [interaction: SSWorldField::collectSheets]
    if (band.mSheet) ++mSheetsHeld;
    if (!result->mLog.empty())
    {
        LL_WARNS("SSNavMesh") << "Band " << job.mTx << "," << job.mTy << " b" << job.mBand << " (z " << job.mZMin << ".." << job.mZMax << ") Recast: " << result->mLog << LL_ENDL;
    }
    band.mLayersDropped = result->mLayersDropped;
    mLayersDropped += (U32)result->mLayersDropped;
    if (result->mLayersDropped > 0 && result->mLayersDropped != dropped_before)    // a stairwell rebuilds often; say it when the count changes, not per publish
    {
        LL_WARNS("SSNavMesh") << "Band " << job.mTx << "," << job.mTy << " b" << job.mBand << " produced " << (SS_NAV_MAX_LAYERS_PER_BAND + result->mLayersDropped)
                              << " walkable layers; " << result->mLayersDropped << " past the per-band cap of " << SS_NAV_MAX_LAYERS_PER_BAND << " have no navmesh" << LL_ENDL;
    }
    band.mUnderTerrain = result->mUnderTerrain;
    if (result->mUnderTerrain > 0 && result->mUnderTerrain != under_before)        // a buried foundation rebuilds rarely; say it when the count changes, not per publish
    {
        LL_WARNS("SSNavMesh") << "Band " << job.mTx << "," << job.mTy << " b" << job.mBand << " had " << result->mUnderTerrain
                              << " walkable spans under the land; nulled" << LL_ENDL;
    }
    band.mPublishedAt = LLFrameTimer::getTotalSeconds();
    mLastBuildMS = result->mMS;
    ++mBuildCount;
}

// Movers: every DYNAMIC record in the envelope wants a box obstacle. The desired set is diffed against the live one
// per census (a mover whose box has not changed keeps its obstacle), and the adds and removals drain through
// pumpObstacles under the tile cache's request budget rather than all at once.
void SSNavMesh::syncObstacles()
{
    if (!mTileCache) return;
    const F32 env = SSWorldFieldShapes::envelopeRange();
    SSWorldFieldShapes* shapes = SSWorldFieldShapes::getInstance();
    const LLVector3 bmin = shapes->censusAnchor() - LLVector3(env, env, env);
    const LLVector3 bmax = shapes->censusAnchor() + LLVector3(env, env, env);
    const LLVector3 off = toLocal(LLVector3::zero);

    std::vector<Obstacle> desired;
    shapes->forEachRecord(bmin, bmax, [&](const SSWorldFieldShapes::Record& rec)
    {
        if (!rec.mDynamic || navIgnores(rec)) return;
        if ((S32)desired.size() >= SS_NAV_MAX_OBSTACLES) return;
        const LLVector3 lo = rec.mBMin + off, hi = rec.mBMax + off;
        Obstacle o;
        o.mMin[0] = lo.mV[VX]; o.mMin[1] = lo.mV[VZ]; o.mMin[2] = -hi.mV[VY];
        o.mMax[0] = hi.mV[VX]; o.mMax[1] = hi.mV[VZ]; o.mMax[2] = -lo.mV[VY];
        desired.push_back(o);
    });

    // Match on the quantized box: unchanged movers keep their obstacle, everything else churns.
    auto same = [](const Obstacle& a, const Obstacle& b)
    {
        for (S32 c = 0; c < 3; ++c)
        {
            if (fabsf(a.mMin[c] - b.mMin[c]) > 0.05f || fabsf(a.mMax[c] - b.mMax[c]) > 0.05f) return false;
        }
        return true;
    };
    std::vector<bool> claimed(desired.size(), false);
    std::vector<Obstacle> kept;
    for (const Obstacle& live : mObstacles)
    {
        bool matched = false;
        for (size_t i = 0; i < desired.size(); ++i)
        {
            if (claimed[i] || !same(live, desired[i])) continue;
            claimed[i] = true;
            matched = true;
            break;
        }
        if (matched) kept.push_back(live);
        else mObstacleRemovals.push_back(live.mRef);
    }
    // Adds still queued from an earlier census are superseded by this census's view.
    mObstacleAdds.clear();
    for (size_t i = 0; i < desired.size(); ++i)
    {
        if (!claimed[i]) mObstacleAdds.push_back(desired[i]);
    }
    mObstacles.swap(kept);
}

// Hand queued obstacle removals and adds to the tile cache, at most the request budget per frame; a refused
// request (queue full) simply waits for the next frame.
void SSNavMesh::pumpObstacles()
{
    if (!mTileCache) return;
    S32 budget = SS_NAV_OBSTACLE_REQUESTS_PER_FRAME;
    while (budget > 0 && !mObstacleRemovals.empty())
    {
        if (dtStatusFailed(mTileCache->removeObstacle(mObstacleRemovals.back()))) return;
        mObstacleRemovals.pop_back();
        --budget;
    }
    while (budget > 0 && !mObstacleAdds.empty())
    {
        Obstacle o = mObstacleAdds.back();
        dtObstacleRef ref = 0;
        if (dtStatusFailed(mTileCache->addBoxObstacle(o.mMin, o.mMax, &ref))) return;
        mObstacleAdds.pop_back();
        o.mRef = ref;
        mObstacles.push_back(o);
        --budget;
    }
}

// The floater's Rebuild: everything goes, the next update re-initialises and schedules the whole envelope.
void SSNavMesh::rebuildAll()
{
    teardown();
}

// Any overlay switch on means the pipeline calls renderDebug every frame.
bool SSNavMesh::overlayEnabled()
{
    static LLCachedControl<bool> show(gSavedSettings, "SSNavMeshShow", false);
    static LLCachedControl<bool> obstacles(gSavedSettings, "SSNavMeshShowObstacles", false);
    static LLCachedControl<bool> census(gSavedSettings, "SSNavMeshShowCensus", false);
    return show || obstacles || census;
}

// Detour between the chosen endpoints; the polyline stays for the overlay until cleared or re-run.
bool SSNavMesh::runTestPath(std::string& out_status)
{
    mTestPath.clear();
    mTestValid = false;
    if (!mHasTestStart || !mHasTestEnd) return false;
    bool partial = false;
    if (!findPath(mTestStart, mTestEnd, mTestPath, partial)) return false;
    mTestValid = true;
    mTestPartial = partial;
    F32 length = 0.f;
    for (size_t i = 1; i < mTestPath.size(); ++i) length += (mTestPath[i] - mTestPath[i - 1]).magVec();
    out_status = llformat("%s path: %d points, %.1f m%s", partial ? "Partial" : "Complete", (S32)mTestPath.size(), length,
                          partial ? " - the end was not reachable, the path stops at the closest point" : "");
    return true;
}

// ---------------------------------------------------------------------------- queries

bool SSNavMesh::nearestPoint(const LLVector3& pos_agent, F32 reach, LLVector3& out_agent) const
{
    if (!mQuery) return false;
    const LLVector3 l = toLocal(pos_agent);
    const float centre[3] = {l.mV[VX], l.mV[VZ], -l.mV[VY]};
    const float ext[3] = {reach, reach * 2.f, reach};
    dtQueryFilter filter;
    dtPolyRef ref = 0;
    float pt[3];
    if (dtStatusFailed(mQuery->findNearestPoly(centre, ext, &filter, &ref, pt)) || !ref) return false;
    out_agent = fromLocal(LLVector3(pt[0], -pt[2], pt[1]));
    return true;
}

bool SSNavMesh::onNavMesh(const LLVector3& pos_agent, F32 reach) const
{
    LLVector3 p;
    return nearestPoint(pos_agent, reach, p);
}

bool SSNavMesh::findPath(const LLVector3& from_agent, const LLVector3& to_agent, std::vector<LLVector3>& out_points, bool& out_partial) const
{
    out_points.clear();
    out_partial = false;
    if (!mQuery) return false;
    const LLVector3 a = toLocal(from_agent), b = toLocal(to_agent);
    const float sp[3] = {a.mV[VX], a.mV[VZ], -a.mV[VY]};
    const float ep[3] = {b.mV[VX], b.mV[VZ], -b.mV[VY]};
    const float ext[3] = {2.f, 4.f, 2.f};
    dtQueryFilter filter;
    dtPolyRef sref = 0, eref = 0;
    float snear[3], enear[3];
    if (dtStatusFailed(mQuery->findNearestPoly(sp, ext, &filter, &sref, snear)) || !sref) return false;
    if (dtStatusFailed(mQuery->findNearestPoly(ep, ext, &filter, &eref, enear)) || !eref) return false;
    dtPolyRef polys[SS_NAV_MAX_PATH];
    int npolys = 0;
    const dtStatus st = mQuery->findPath(sref, eref, snear, enear, &filter, polys, &npolys, SS_NAV_MAX_PATH);
    if (dtStatusFailed(st) || npolys == 0) return false;
    out_partial = (st & DT_PARTIAL_RESULT) != 0;
    float target[3] = {enear[0], enear[1], enear[2]};
    if (out_partial) mQuery->closestPointOnPoly(polys[npolys - 1], enear, target, nullptr);
    float straight[SS_NAV_MAX_PATH * 3];
    unsigned char flags[SS_NAV_MAX_PATH];
    dtPolyRef straight_refs[SS_NAV_MAX_PATH];
    int nstraight = 0;
    mQuery->findStraightPath(snear, target, polys, npolys, straight, flags, straight_refs, &nstraight, SS_NAV_MAX_PATH);
    for (int i = 0; i < nstraight; ++i)
    {
        out_points.push_back(fromLocal(LLVector3(straight[i * 3], -straight[i * 3 + 2], straight[i * 3 + 1])));
    }
    return nstraight > 0;
}

U32 SSNavMesh::polyCount() const
{
    if (!mNavMesh) return 0;
    U32 n = 0;
    const dtNavMesh* nm = mNavMesh;
    for (int i = 0; i < nm->getMaxTiles(); ++i)
    {
        const dtMeshTile* tile = nm->getTile(i);
        if (tile && tile->header) n += (U32)tile->header->polyCount;
    }
    return n;
}

// ---------------------------------------------------------------------------- overlay

void SSNavMesh::renderDebug(bool force_navmesh)
{
    static LLCachedControl<bool> show(gSavedSettings, "SSNavMeshShow", false);
    static LLCachedControl<bool> show_links(gSavedSettings, "SSNavMeshShowLinks", true);
    static LLCachedControl<bool> show_flash(gSavedSettings, "SSNavMeshShowFlash", true);
    static LLCachedControl<bool> show_obstacles(gSavedSettings, "SSNavMeshShowObstacles", false);
    static LLCachedControl<bool> show_census(gSavedSettings, "SSNavMeshShowCensus", false);
    static LLCachedControl<bool> show_world(gSavedSettings, "SSNavMeshShowWorld", true);
    static LLCachedControl<bool> xray(gSavedSettings, "SSNavMeshXRay", false);
    if (!mNavMesh) return;
    const bool draw_mesh = show || force_navmesh;
    if (draw_mesh && !show_world)
    {
        // As the stock pathfinding console does: wipe the frame so only the navmesh remains.
        const LLColor4 clear = gSavedSettings.getColor4("PathfindingNavMeshClear");
        gGL.setColorMask(true, true);
        glClearColor(clear.mV[0], clear.mV[1], clear.mV[2], 0.f);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        gGL.setColorMask(true, false);
    }
    if (show_census) SSWorldFieldShapes::getInstance()->renderDebug();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const dtNavMesh* nm = mNavMesh;

    // <SS:Nexii> With the world wiped there is nothing to see through, so the mesh draws opaque and writes depth: stacked storeys then sort by depth instead of by tile order, and the fills read as solid floor. [interaction: SSNavMeshShowWorld]
    const bool opaque = draw_mesh && !show_world;
    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_TRUE, opaque ? GL_TRUE : GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // The marked spot: a magenta post with a cross at the mark, so a dump and the view line up.
    if (mHasMark)
    {
        gGL.begin(LLRender::LINES);
        gGL.color4f(1.f, 0.2f, 1.f, 0.9f);
        gGL.vertex3f(mMark.mV[VX], mMark.mV[VY], mMark.mV[VZ] - 5.f);
        gGL.vertex3f(mMark.mV[VX], mMark.mV[VY], mMark.mV[VZ] + 5.f);
        gGL.vertex3f(mMark.mV[VX] - 1.f, mMark.mV[VY], mMark.mV[VZ]);
        gGL.vertex3f(mMark.mV[VX] + 1.f, mMark.mV[VY], mMark.mV[VZ]);
        gGL.vertex3f(mMark.mV[VX], mMark.mV[VY] - 1.f, mMark.mV[VZ]);
        gGL.vertex3f(mMark.mV[VX], mMark.mV[VY] + 1.f, mMark.mV[VZ]);
        gGL.end();
    }

    // Mover obstacles: the boxes the tile cache carved out for objects the census still counts as moving.
    if (show_obstacles && !mObstacles.empty())
    {
        gGL.begin(LLRender::LINES);
        gGL.color4f(1.f, 0.55f, 0.15f, 0.8f);
        for (const Obstacle& o : mObstacles)
        {
            const LLVector3 mn = fromLocal(LLVector3(o.mMin[0], -o.mMax[2], o.mMin[1]));
            const LLVector3 mx = fromLocal(LLVector3(o.mMax[0], -o.mMin[2], o.mMax[1]));
            if (((mn + mx) * 0.5f - cam).magVec() > 256.f) continue;
            for (S32 e = 0; e < 12; ++e)
            {
                // The 12 edges of the box: pairs of corners differing in one axis.
                static const S32 edges[12][2] = {{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
                LLVector3 p[2];
                for (S32 k = 0; k < 2; ++k)
                {
                    const S32 cnr = edges[e][k];
                    p[k].set((cnr & 1) ? mx.mV[VX] : mn.mV[VX], (cnr & 2) ? mx.mV[VY] : mn.mV[VY], (cnr & 4) ? mx.mV[VZ] : mn.mV[VZ]);
                }
                gGL.vertex3fv(p[0].mV);
                gGL.vertex3fv(p[1].mV);
            }
        }
        gGL.end();
    }

    // The test path: orange when complete, yellow when it stops short, with crosses at the chosen endpoints.
    if (mHasTestStart || mHasTestEnd)
    {
        gGL.begin(LLRender::LINES);
        auto cross = [&](const LLVector3& p, F32 r, F32 g, F32 b)
        {
            gGL.color4f(r, g, b, 0.95f);
            gGL.vertex3f(p.mV[VX] - 0.5f, p.mV[VY], p.mV[VZ] + 0.1f); gGL.vertex3f(p.mV[VX] + 0.5f, p.mV[VY], p.mV[VZ] + 0.1f);
            gGL.vertex3f(p.mV[VX], p.mV[VY] - 0.5f, p.mV[VZ] + 0.1f); gGL.vertex3f(p.mV[VX], p.mV[VY] + 0.5f, p.mV[VZ] + 0.1f);
            gGL.vertex3f(p.mV[VX], p.mV[VY], p.mV[VZ]); gGL.vertex3f(p.mV[VX], p.mV[VY], p.mV[VZ] + 2.f);
        };
        if (mHasTestStart) cross(mTestStart, 0.3f, 1.f, 0.3f);
        if (mHasTestEnd) cross(mTestEnd, 1.f, 0.3f, 0.3f);
        if (mTestValid && mTestPath.size() >= 2)
        {
            if (mTestPartial) gGL.color4f(1.f, 0.9f, 0.2f, 0.95f); else gGL.color4f(1.f, 0.55f, 0.1f, 0.95f);
            for (size_t i = 1; i < mTestPath.size(); ++i)
            {
                gGL.vertex3f(mTestPath[i - 1].mV[VX], mTestPath[i - 1].mV[VY], mTestPath[i - 1].mV[VZ] + 0.15f);
                gGL.vertex3f(mTestPath[i].mV[VX], mTestPath[i].mV[VY], mTestPath[i].mV[VZ] + 0.15f);
            }
        }
        gGL.end();
    }

    if (!draw_mesh) return;

    // Two passes over the same polygons: translucent fills so the walkable surface reads as area, then the
    // edges on top so polygon and tile boundaries stay legible. Hue by band, as the world field's band view.
    auto vert = [&](const float* v, F32 lift) { return fromLocal(LLVector3(v[0], -v[2], v[1] + lift)); };
    // A band that published within the last second flashes towards white, so a rebuild is visible as it lands.
    const F64 now = LLFrameTimer::getTotalSeconds();
    auto flashOf = [&](const dtMeshTile* tile) -> F32
    {
        auto it = mBands.find(bandKey(tile->header->x, -tile->header->y - 1, tile->header->layer / SS_NAV_MAX_LAYERS_PER_BAND));
        if (it == mBands.end()) return 0.f;
        if (!show_flash) return 0.f;
        const F64 age = now - it->second.mPublishedAt;
        return (age >= 0.0 && age < 1.0) ? (F32)(1.0 - age) : 0.f;
    };
    // shade < 1 darkens (the x-ray pass, seen through geometry); alpha scales every colour of a pass.
    F32 shade = 1.f, alpha_scale = 1.f;
    auto bandColour = [&](const dtMeshTile* tile, F32 alpha)
    {
        const F32 hue = (F32)((tile->header->layer / SS_NAV_MAX_LAYERS_PER_BAND) % 8) / 8.f;
        const F32 f = flashOf(tile);
        const F32 r = 0.3f + 0.7f * hue, g = 0.9f - 0.6f * hue, b = 0.4f + 0.4f * (1.f - hue);
        gGL.color4f((r + (1.f - r) * f) * shade, (g + (1.f - g) * f) * shade, (b + (1.f - b) * f) * shade, (alpha + 0.5f * f) * alpha_scale);
    };
    // Every published tile draws: the navmesh is a whole-region surface and the overlay should read as one.
    auto drawMesh = [&]()
    {
    for (S32 pass = 0; pass < 2; ++pass)
    {
        gGL.begin(pass == 0 ? LLRender::TRIANGLES : LLRender::LINES);
        for (int i = 0; i < nm->getMaxTiles(); ++i)
        {
            const dtMeshTile* tile = nm->getTile(i);
            if (!tile || !tile->header) continue;
            bandColour(tile, opaque ? 1.f : (pass == 0 ? 0.28f : 0.18f));
            for (int p = 0; p < tile->header->polyCount; ++p)
            {
                const dtPoly& poly = tile->polys[p];
                if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly.vertCount < 3) continue;
                if (pass == 0)
                {
                    // The detail triangles when the tile carries them (the surface as Detour will answer it), else a fan over the convex polygon.
                    const dtPolyDetail* pd = tile->detailMeshes ? &tile->detailMeshes[p] : nullptr;
                    if (pd && pd->triCount > 0)
                    {
                        for (int j = 0; j < (int)pd->triCount; ++j)
                        {
                            const unsigned char* t = &tile->detailTris[(pd->triBase + j) * 4];
                            for (int k = 0; k < 3; ++k)
                            {
                                const float* dv = t[k] < poly.vertCount ? &tile->verts[poly.verts[t[k]] * 3]
                                                                         : &tile->detailVerts[(pd->vertBase + t[k] - poly.vertCount) * 3];
                                const LLVector3 pv = vert(dv, 0.04f);
                                gGL.vertex3fv(pv.mV);
                            }
                        }
                    }
                    else
                    {
                        const LLVector3 p0 = vert(&tile->verts[poly.verts[0] * 3], 0.04f);
                        for (int v = 1; v + 1 < (int)poly.vertCount; ++v)
                        {
                            const LLVector3 p1 = vert(&tile->verts[poly.verts[v] * 3], 0.04f);
                            const LLVector3 p2 = vert(&tile->verts[poly.verts[v + 1] * 3], 0.04f);
                            gGL.vertex3fv(p0.mV);
                            gGL.vertex3fv(p1.mV);
                            gGL.vertex3fv(p2.mV);
                        }
                    }
                }
                else
                {
                    for (int v = 0; v < (int)poly.vertCount; ++v)
                    {
                        if (show_links && (poly.neis[v] & DT_EXT_LINK))
                        {
                            bool linked = false;
                            for (unsigned int l = poly.firstLink; l != DT_NULL_LINK; l = tile->links[l].next)
                            {
                                if (tile->links[l].edge == v && tile->links[l].side != 0xff) { linked = true; break; }
                            }
                            if (linked) gGL.color4f(0.2f * shade, 0.95f * shade, 1.f * shade, 0.9f * alpha_scale);
                            else gGL.color4f(1.f * shade, 0.25f * shade, 0.2f * shade, 0.9f * alpha_scale);
                        }
                        else
                        {
                            // Interior edges stay faint: the fills carry the surface, the edges only hint at the polygons.
                            bandColour(tile, opaque ? 1.f : 0.18f);
                        }
                        const LLVector3 pa = vert(&tile->verts[poly.verts[v] * 3], 0.06f);
                        const LLVector3 pb = vert(&tile->verts[poly.verts[(v + 1) % poly.vertCount] * 3], 0.06f);
                        gGL.vertex3fv(pa.mV);
                        gGL.vertex3fv(pb.mV);
                    }
                }
            }
        }
        gGL.end();
    }
    };
    drawMesh();
    if (xray && !opaque)
    {
        // The parts of the navmesh behind geometry, shaded darker and fainter so they read as "through the wall".
        LLGLDepthTest depth_behind(GL_TRUE, GL_FALSE, GL_GREATER);
        shade = 0.55f;
        alpha_scale = 0.5f;
        drawMesh();
    }
}
