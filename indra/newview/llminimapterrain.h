/**
 * @file llminimapterrain.h
 * @brief The mini-map's terrain background, painted by the viewer from the region's terrain.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc. (the terrain painting, LLVLComposition)
 * Copyright (C) 2026, squeedledorf (GunMetal-DX: the restored background and its changes)
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

// Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4.
// LL's mini-map terrain ("STexture"), as LL and Firestorm drew it until LL removed it in
// June 2024 (secondlife/viewer #1836, which switched the mini-map to the server's map tile):
// LLVLComposition::generateMinimapTileLand paints each terrain patch from the composition
// (height + noise picks the blend of the four detail textures, or of the PBR materials' base
// colours) and LLSurface::generateWaterTexture lays the water over it, darker with depth.
// The code is LL's, moved here so the LL files keep only small hooks. Differences:
//  - painting happens when the mini-map draws the region, not on every terrain update, and
//    within a small time budget per frame (LL painted on every patch update, minimap or not);
//  - the target images are kept, not reallocated per patch;
//  - a detail image that arrives sharper than asked for is used, not replaced by white.
// SLMiniMapUseMapTile = Slipstream's map tile instead, falling back to this until it loads.
// SLMiniMapTerrainLook = the OCOL map look (OCOL's map tile renderer, terrain part): the land at a
// 64 m texture repeat, lit by a north-west sun with cast shadows and contact shading, and the
// water coloured by depth with the shore band, baked into one image per region.

#pragma once

#include "llpointer.h"
#include "stdtypes.h"
#include <set>

class LLImageRaw;
class LLSurface;
class LLSurfacePatch;
class LLViewerTexture;
class LLVLComposition;

class LLMinimapTerrain
{
public:
    explicit LLMinimapTerrain(LLSurface* surfacep);
    ~LLMinimapTerrain();

    // A patch's heights / composition were (re)generated (LLSurfacePatch::updateGL).
    void dirtyPatch(LLSurfacePatch* patchp) { mDirtyPatches.insert(patchp); }

    // What LLNetMap::draw shows for this region, called every frame. Returns the land image,
    // and in water the image to blend over it (null when the land image is the map tile).
    LLViewerTexture* getTexture(LLViewerTexture*& water);

private:
    void createTextures();
    void paintDirtyPatches();
    bool prepareDetailImages(LLVLComposition* comp);
    void paintLand(LLVLComposition* comp, F32 x, F32 y, F32 width);
    void paintWater(F32 x, F32 y, F32 width);
    void dirtyAllPatches();
    void shadeOcol();

    LLSurface* mSurfacep;
    std::set<LLSurfacePatch*> mDirtyPatches;
    LLPointer<LLViewerTexture> mLandTexp;
    S32 mTileLogged = -1;   // last logged reason the map tile wasn't drawn (-1 = drawn / off)
    LLPointer<LLViewerTexture> mWaterTexp;
    LLPointer<LLImageRaw> mLandRawp;    // CPU image the patches are painted into
    LLPointer<LLImageRaw> mWaterRawp;
    F64 mRetryAt = 0.0;                 // detail images not ready: try again after this
    // OCOL look: the shaded image, re-shaded once the land is repainted
    LLPointer<LLImageRaw> mOcolRawp;
    LLPointer<LLViewerTexture> mOcolTexp;
    bool mOcolDirty = true;
    bool mOcolWas = false;              // SLMiniMapTerrainLook the patches were painted for
};
