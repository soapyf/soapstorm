/**
 * @file llminimapterrain.cpp
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

// Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4. Whole file: see llminimapterrain.h.

#include "llviewerprecompiledheaders.h"

#include "llminimapterrain.h"
#include "llregionmapcapture.h"

#include "llfetchedgltfmaterial.h"
#include "llframetimer.h"
#include "llimage.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvlcomposition.h"
#include "llworld.h"
#include "llappviewer.h"   // gFrameCount

extern LLColor4U MAX_WATER_COLOR;

namespace
{
    const S32 LAND_SIZE = 256;           // LLSurface::sTextureSize: 1 texel per metre
    const S32 WATER_SIZE = LAND_SIZE / 2;
    const S32 BASE_SIZE = 128;           // detail image size (llvlcomposition.cpp)
    const F32 PAINT_BUDGET = 0.001f;     // seconds per region per frame
    const F64 RETRY_DELAY = 0.5;         // seconds, while the detail images are not ready

    // OCOL look (OCOL's map tile renderer, OcolMapRenderer.cs: same constants and formulas).
    const F32 OCOL_REPEAT = 64.f;        // metres per terrain texture repeat
    const F32 OCOL_SUN_AZIMUTH = 315.f;  // north-west sun, cartographic convention
    const F32 OCOL_SUN_ALTITUDE = 35.f;
    const F32 OCOL_EXPOSURE = 1.55f;
    const F32 OCOL_WATER_SATURATION = 0.4f;
    const F32 OCOL_SHORE_GLOW = 0.05f;
    const F32 OCOL_SHORE_TINT[4] = { 0.30f, 0.46f, 0.48f, 0.35f };
    const F32 OCOL_SHADOW_STRENGTH = 0.55f;
    const S32 OCOL_MARGIN = 64;          // metres of the neighbours' land around the region (shadows)
    // ponytail: march capped at 200 m (shadows of hills > ~140 m above the receiver end early); raise if it shows
    const S32 OCOL_MAX_STEPS = 200;

    // Everything below up to paintWater is LL's (llvlcomposition.cpp / llsurface.cpp before #1836).

    void prepare_fallback_image(LLImageRaw* raw_image)
    {
        raw_image->resize(BASE_SIZE, BASE_SIZE, 4);
        raw_image->fill(LLColor4U::white);
    }

    // Check if the raw image is loaded for this texture at a discard
    // level the minimap can use, and if not then try to get it loaded.
    bool prepare_raw_image(LLPointer<LLImageRaw>& raw_image, bool emissive, LLViewerFetchedTexture* tex, bool& delete_raw_post)
    {
        if (!tex)
        {
            if (!emissive)
            {
                prepare_fallback_image(raw_image);
            }
            else
            {
                llassert(!raw_image);
                raw_image = nullptr;
            }
            return true;
        }
        if (raw_image)
        {
            // Callback already initiated
            if (raw_image->getDataSize() > 0)
            {
                // Callback finished
                delete_raw_post = true;
                return true;
            }
            else
            {
                return false;
            }
        }

        raw_image = new LLImageRaw();

        S32 ddiscard = 0;
        {
            S32 min_dim = llmin(tex->getFullWidth(), tex->getFullHeight());
            while (min_dim > BASE_SIZE && ddiscard < MAX_DISCARD_LEVEL)
            {
                ddiscard++;
                min_dim /= 2;
            }
        }

        struct PendingImage
        {
            LLImageRaw* mRawImage;
            S32 mDesiredDiscard;
            LLUUID mTextureId;
            PendingImage(LLImageRaw* raw_image, S32 ddiscard, const LLUUID& texture_id)
                : mRawImage(raw_image)
                , mDesiredDiscard(ddiscard)
                , mTextureId(texture_id)
            {
                mRawImage->ref();
            }
            ~PendingImage()
            {
                mRawImage->unref();
            }
        };
        PendingImage* pending_image = new PendingImage(raw_image, ddiscard, tex->getID());

        loaded_callback_func cb = [](bool success, LLViewerFetchedTexture * src_vi, LLImageRaw * src, LLImageRaw * src_aux, S32 discard_level, bool is_final, void* userdata) {
            PendingImage* pending = (PendingImage*)userdata;
            // Owning LLVLComposition still exists

            // Assume mRawImage only used by single LLVLComposition for now
            const bool in_use_by_composition = pending->mRawImage->getNumRefs() > 1;
            llassert(pending->mRawImage->getNumRefs());
            const bool needs_data = !pending->mRawImage->getDataSize();
            if (in_use_by_composition && needs_data)
            {
                // GMDX: <=, not ==. A texture already decoded sharper than asked for runs its
                // final callback at that level; LL then painted the whole layer white.
                if (success && src && discard_level <= pending->mDesiredDiscard)
                {
                    pending->mRawImage->resize(BASE_SIZE, BASE_SIZE, src->getComponents());
                    pending->mRawImage->copyScaled(src);
                }
                else if (is_final)
                {
                    prepare_fallback_image(pending->mRawImage);
                }
            }

            if (is_final) { delete pending; }
        };
        tex->setLoadedCallback(cb, ddiscard, true, false, pending_image, nullptr);
        tex->forceToSaveRawImage(ddiscard);

        return false;
    }

    // GMDX: LLImageRaw::composite only takes 3 or 4 components; a grey detail texture (1 or 2)
    // left LL's layer black.
    LLPointer<LLImageRaw> to_rgb(LLImageRaw* src)
    {
        const S32 comps = src->getComponents();
        if (comps >= 3)
        {
            return src;
        }
        LLPointer<LLImageRaw> rgb = new LLImageRaw(src->getWidth(), src->getHeight(), 3);
        const U8* in = src->getData();
        U8* out = rgb->getData();
        for (S32 i = 0, n = src->getWidth() * src->getHeight(); i < n; ++i)
        {
            out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = in[i * comps];
        }
        return rgb;
    }
}

LLMinimapTerrain::LLMinimapTerrain(LLSurface* surfacep)
    : mSurfacep(surfacep)
{
}

LLMinimapTerrain::~LLMinimapTerrain() = default;

LLViewerTexture* LLMinimapTerrain::getTexture(LLViewerTexture*& water)
{
    water = nullptr;

    // A captured region map (llregionmapcapture.h) wins over the tile and the painted terrain.
    static LLCachedControl<bool> use_captured(gSavedSettings, "SLRegionMapUseCaptured", true);
    if (use_captured)
    {
        if (LLViewerTexture* captured = LLRegionMapCapture::getMinimapTexture(mSurfacep->getRegion()))
        {
            return captured;
        }
    }

    static LLCachedControl<bool> use_map_tile(gSavedSettings, "SLMiniMapUseMapTile", false);
    if (use_map_tile)
    {
        // Slipstream #436: getSTexture boosts the tile and retries a failed fetch.
        LLViewerTexture* tile = mSurfacep->getSTexture();
        if (tile && tile->hasGLTexture() && !tile->isMissingAsset())
        {
            mTileLogged = -1;
            return tile;
        }
        // Why the tile isn't used (once per state change per region): missing = fetch failed
        // (retried with backoff), no data = not fetched / decoded yet.
        const S32 why = !tile ? 0 : tile->isMissingAsset() ? 1 : 2;
        if (why != mTileLogged)
        {
            mTileLogged = why;
            LLViewerFetchedTexture* ft = LLViewerTextureManager::staticCastToFetchedTexture(tile);
            LL_INFOS("SLMiniMap") << "map tile not drawn: " << (why == 0 ? "no tile" : why == 1 ? "missing asset" : "no data")
                                    << (ft ? " url=" + ft->getUrl() : std::string())
                                    << (ft ? " discard=" + std::to_string(ft->getDiscardLevel()) : std::string())
                                    << (ft ? " boost=" + std::to_string(ft->getBoostLevel()) : std::string()) << LL_ENDL;
        }
    }
    else
    {
        mTileLogged = -1;
    }

    if (mLandTexp.isNull())
    {
        createTextures();
    }
    static LLCachedControl<bool> ocol_look(gSavedSettings, "SLMiniMapTerrainLook", true);
    if ((bool)ocol_look != mOcolWas)
    {
        // the land repaints at the other texture repeat
        mOcolWas = ocol_look;
        dirtyAllPatches();
    }
    paintDirtyPatches();
    if (ocol_look)
    {
        // One region shaded per frame (a few tens of ms each), once its land is painted.
        static U32 s_shaded_frame = 0;
        if (mOcolDirty && mDirtyPatches.empty() && s_shaded_frame != gFrameCount)
        {
            s_shaded_frame = gFrameCount;
            shadeOcol();
        }
        if (mOcolTexp.notNull())
        {
            return mOcolTexp;   // water is baked in
        }
    }
    water = mWaterTexp;
    return mLandTexp;
}

void LLMinimapTerrain::dirtyAllPatches()
{
    const S32 n = mSurfacep->getPatchesPerEdge();
    for (S32 y = 0; y < n; ++y)
    {
        for (S32 x = 0; x < n; ++x)
        {
            if (LLSurfacePatch* p = mSurfacep->getPatch(x, y))
            {
                mDirtyPatches.insert(p);
            }
        }
    }
}

// OCOL's map look over the painted land (OcolMapRenderer::Composite, terrain only): lambert from
// the height slope, shadows marched toward the sun over the region and a margin of its loaded
// neighbours, contact shading (how far below its surroundings a point sits), outlines at height
// jumps, and water coloured by depth with the shore band and wet sand.
void LLMinimapTerrain::shadeOcol()
{
    mOcolDirty = false;
    const S32 N = LAND_SIZE, M = OCOL_MARGIN, W = N + 2 * M;
    const F32 mpp = 256.f * mSurfacep->getMetersPerGrid() / (F32)N;   // metres per pixel (1)

    // heights on a 1 px grid with the margin; a neighbour not loaded repeats the edge
    std::vector<F32> hm((size_t)W * W);
    const LLVector3d origin = mSurfacep->getOriginGlobal();
    LLWorld& world = LLWorld::instance();
    F32 hmin = F32_MAX, hmax = -F32_MAX;
    for (S32 r = 0; r < W; ++r)
    {
        for (S32 c = 0; c < W; ++c)
        {
            const F32 x = ((F32)(c - M) + 0.5f) * mpp, y = ((F32)(r - M) + 0.5f) * mpp;
            F32 h;
            if (c >= M && c < M + N && r >= M && r < M + N)
            {
                h = mSurfacep->resolveHeightRegion(x, y);
            }
            else
            {
                const LLVector3d pos = origin + LLVector3d(x, y, 0.0);
                LLViewerRegion* nb = world.getRegionFromPosGlobal(pos);
                h = nb ? nb->getLand().resolveHeightRegion((F32)(pos.mdV[VX] - nb->getOriginGlobal().mdV[VX]),
                                                           (F32)(pos.mdV[VY] - nb->getOriginGlobal().mdV[VY]))
                       : mSurfacep->resolveHeightRegion(llclamp(x, 0.f, 255.f * mpp), llclamp(y, 0.f, 255.f * mpp));
            }
            hm[(size_t)r * W + c] = h;
            hmin = llmin(hmin, h);
            hmax = llmax(hmax, h);
        }
    }
    auto H = [&](S32 c, S32 r) { return hm[(size_t)llclamp(r, 0, W - 1) * W + llclamp(c, 0, W - 1)]; };

    const F32 az = OCOL_SUN_AZIMUTH * DEG_TO_RAD, alt = OCOL_SUN_ALTITUDE * DEG_TO_RAD;
    const F32 sx = sinf(az) * cosf(alt), sy = cosf(az) * cosf(alt), sz = sinf(alt);
    const F32 dxs = sinf(az), dys = cosf(az), tan_alt = tanf(alt);
    const S32 steps = llmin(OCOL_MAX_STEPS, (S32)((hmax - hmin) / (tan_alt * mpp))) + 2;

    // contact shading: box blur (radius 6) of the heights, separable, over the margin grid
    const S32 R = 6;
    std::vector<F32> tmp((size_t)W * W), blur((size_t)W * W);
    for (S32 r = 0; r < W; ++r)
        for (S32 c = 0; c < W; ++c)
        {
            F32 s = 0.f;
            for (S32 k = -R; k <= R; ++k) s += H(c + k, r);
            tmp[(size_t)r * W + c] = s / (F32)(2 * R + 1);
        }
    for (S32 r = 0; r < W; ++r)
        for (S32 c = 0; c < W; ++c)
        {
            F32 s = 0.f;
            for (S32 k = -R; k <= R; ++k) s += tmp[(size_t)llclamp(r + k, 0, W - 1) * W + c];
            blur[(size_t)r * W + c] = s / (F32)(2 * R + 1);
        }

    if (mOcolRawp.isNull())
    {
        mOcolRawp = new LLImageRaw(N, N, 3);
    }
    const U8* land = mLandRawp->getData();
    U8* out = mOcolRawp->getData();
    const F32 water = mSurfacep->getWaterHeight();
    auto to_byte = [](F32 v) { return (U8)llclamp(ll_round(v * 255.f), 0, 255); };
    for (S32 j = 0; j < N; ++j)        // image rows run south to north (row 0 = south edge)
    {
        for (S32 i = 0; i < N; ++i)
        {
            const S32 c = i + M, r = j + M;
            const F32 h0 = H(c, r);

            // cast shadow: march toward the sun (x east, y north)
            F32 sh = 0.f;
            for (S32 k = 1; k < steps && sh < 1.f; ++k)
            {
                const F32 ray = h0 + (F32)k * mpp * tan_alt;
                const F32 hk = H(c + ll_round(dxs * k), r + ll_round(dys * k));
                sh = llmax(sh, llclamp((hk - ray) * 0.5f, 0.f, 1.f));
            }
            const F32 lit = 1.f - OCOL_SHADOW_STRENGTH * sh;
            const F32 ao = llclamp(1.f - (blur[(size_t)r * W + c] - h0) * 0.06f, 0.6f, 1.f);
            const F32 dzdx = (H(c + 1, r) - H(c - 1, r)) / (2.f * mpp), dzdy = (H(c, r + 1) - H(c, r - 1)) / (2.f * mpp);
            const F32 nl = sqrtf(dzdx * dzdx + dzdy * dzdy + 1.f);
            const F32 lam = llmax(0.f, (-dzdx * sx - dzdy * sy + sz) / nl);
            F32 light = (0.45f + 0.75f * lam) * lit * ao;

            F32 jump = llmax(llmax(fabsf(h0 - H(c - 1, r)), fabsf(h0 - H(c + 1, r))),
                             llmax(fabsf(h0 - H(c, r - 1)), fabsf(h0 - H(c, r + 1))));
            light *= 1.f - 0.35f * llclamp((jump - 1.f) * 0.5f, 0.f, 1.f);   // outline

            const U8* px = land + ((size_t)j * N + i) * 3;
            F32 rgb[3];
            for (S32 k = 0; k < 3; ++k)
            {
                rgb[k] = llmin(px[k] / 255.f * OCOL_EXPOSURE + 0.03f, 1.f) * light;
            }

            const F32 depth = water - h0;
            if (depth > 0.f)
            {
                const F32 relief = 0.85f + 0.15f * lam;
                const F32 td = powf(llclamp(depth / 14.f, 0.f, 1.f), 0.6f);
                const F32 tp = powf(llclamp(llmax(depth, 4.f) / 14.f, 0.f, 1.f), 0.6f);
                const F32 foam = llclamp(1.f - depth / 0.6f, 0.f, 1.f) * 0.35f;
                static const F32 shallow[3] = { 0.36f, 0.72f, 0.74f }, deep[3] = { 0.05f, 0.17f, 0.33f };
                F32 band = llclamp(1.f - depth / 4.f, 0.f, 1.f);
                band = band * band * (3.f - 2.f * band) * OCOL_SHORE_TINT[3];
                F32 w[3];
                for (S32 k = 0; k < 3; ++k)
                {
                    F32 f = (shallow[k] + (deep[k] - shallow[k]) * td) * relief;
                    f = f * (1.f - foam) + foam;
                    const F32 p = (shallow[k] + (deep[k] - shallow[k]) * tp) * relief;
                    w[k] = p + (f - p) * OCOL_SHORE_GLOW;
                    w[k] += (OCOL_SHORE_TINT[k] - w[k]) * band;
                }
                const F32 wl = 0.2126f * w[0] + 0.7152f * w[1] + 0.0722f * w[2];
                for (S32 k = 0; k < 3; ++k)
                {
                    rgb[k] = wl + (w[k] - wl) * OCOL_WATER_SATURATION;
                }
            }
            else
            {
                const F32 wet = 1.f - 0.35f * OCOL_SHORE_GLOW * llclamp(1.f + depth / 1.5f, 0.f, 1.f);
                for (F32& v : rgb) v *= wet;
            }
            U8* o = out + ((size_t)j * N + i) * 3;
            o[0] = to_byte(rgb[0]); o[1] = to_byte(rgb[1]); o[2] = to_byte(rgb[2]);
        }
    }

    if (mOcolTexp.isNull())
    {
        mOcolTexp = LLViewerTextureManager::getLocalTexture(mOcolRawp.get(), false);
        mOcolTexp->dontDiscard();
        mOcolTexp->setAddressMode(LLTexUnit::TAM_CLAMP);
    }
    mOcolTexp->setSubImage(mOcolRawp, 0, 0, N, N);
}

void LLMinimapTerrain::createTextures()
{
    // LL's LLSurface::createSTexture / createWaterTexture: grey land, deep water until painted.
    mLandRawp = new LLImageRaw(LAND_SIZE, LAND_SIZE, 3);
    mLandRawp->clear(128, 128, 128, 255);
    mLandTexp = LLViewerTextureManager::getLocalTexture(mLandRawp.get(), false);
    mLandTexp->dontDiscard();
    mLandTexp->setAddressMode(LLTexUnit::TAM_CLAMP);
    // The first setSubImage gives the texture its own writable copy (zeroed); fill it.
    mLandTexp->setSubImage(mLandRawp, 0, 0, LAND_SIZE, LAND_SIZE);

    mWaterRawp = new LLImageRaw(WATER_SIZE, WATER_SIZE, 4);
    mWaterRawp->fill(MAX_WATER_COLOR);
    mWaterTexp = LLViewerTextureManager::getLocalTexture(mWaterRawp.get(), false);
    mWaterTexp->dontDiscard();
    mWaterTexp->setAddressMode(LLTexUnit::TAM_CLAMP);
    mWaterTexp->setSubImage(mWaterRawp, 0, 0, WATER_SIZE, WATER_SIZE);
}

void LLMinimapTerrain::paintDirtyPatches()
{
    if (mDirtyPatches.empty() || LLFrameTimer::getElapsedSeconds() < mRetryAt)
    {
        return;
    }

    LLVLComposition* comp = mSurfacep->getRegion() ? mSurfacep->getRegion()->getComposition() : nullptr;
    if (!comp || !prepareDetailImages(comp))
    {
        mRetryAt = LLFrameTimer::getElapsedSeconds() + RETRY_DELAY;
        return;
    }

    // LL's LLSurfacePatch::updateGL, for the patches changed since the last paint.
    const F32 patch_size = mSurfacep->getMetersPerGrid() * (F32)mSurfacep->getGridsPerPatchEdge();
    LLTimer timer;
    for (auto it = mDirtyPatches.begin(); it != mDirtyPatches.end() && timer.getElapsedTimeF32() < PAINT_BUDGET; )
    {
        const LLVector3d origin_region = (*it)->getOriginGlobal() - mSurfacep->getOriginGlobal();
        paintLand(comp, (F32)origin_region.mdV[VX], (F32)origin_region.mdV[VY], patch_size);
        paintWater((F32)origin_region.mdV[VX], (F32)origin_region.mdV[VY], patch_size);
        it = mDirtyPatches.erase(it);
        mOcolDirty = true;
    }
}

// The first half of LL's LLVLComposition::generateMinimapTileLand: a BASE_SIZE RGB image per
// detail layer (texture, or PBR base colour x factor + emissive). True when all four are ready.
bool LLMinimapTerrain::prepareDetailImages(LLVLComposition* comp)
{
    // GMDX: boost as LL did (only while not ready); no unboost afterwards, as the VK terrain
    // renderer relies on the boost generateComposition already leaves in place.
    const bool use_textures = comp->getMaterialType() != LLTerrainMaterials::Type::PBR;
    if (use_textures)
    {
        if (!comp->makeTexturesReady(true, true)) { return false; }
    }
    else
    {
        if (!comp->makeMaterialsReady(true, true)) { return false; }
    }

    // GMDX: ask for every layer's image at once (LL returned at the first one not ready).
    bool ready = true;
    for (S32 i = 0; i < LLTerrainMaterials::ASSET_COUNT; i++)
    {
        if (comp->mRawImages[i].notNull())
        {
            continue;
        }

        // Read back a raw image for this discard level, if it exists
        LLViewerFetchedTexture* tex = nullptr;
        LLViewerFetchedTexture* tex_emissive = nullptr; // Can be null
        bool has_base_color_factor = false;
        bool has_emissive_factor = false;
        bool has_alpha = false;
        LLColor3 base_color_factor;
        LLColor3 emissive_factor;
        if (use_textures)
        {
            tex = comp->mDetailTextures[i];
        }
        else if (LLFetchedGLTFMaterial* mat = comp->mDetailRenderMaterials[i]) // GMDX: null under RenderTerrainPBRForce
        {
            tex = mat->mBaseColorTexture;
            tex_emissive = mat->mEmissiveTexture;
            base_color_factor = LLColor3(mat->mBaseColor);
            // *HACK: Treat alpha as black
            base_color_factor *= (mat->mBaseColor.mV[VW]);
            emissive_factor = mat->mEmissiveColor;
            has_base_color_factor = (base_color_factor.mV[VX] != 1.f ||
                                     base_color_factor.mV[VY] != 1.f ||
                                     base_color_factor.mV[VZ] != 1.f);
            has_emissive_factor = (emissive_factor.mV[VX] != 1.f ||
                                   emissive_factor.mV[VY] != 1.f ||
                                   emissive_factor.mV[VZ] != 1.f);
            has_alpha = mat->mAlphaMode != LLGLTFMaterial::ALPHA_MODE_OPAQUE;
        }

        // GMDX: no base colour texture = white (times the factor) straight away; LL asked the
        // white image for a loaded callback, which a generated texture may never run.

        bool delete_raw_post = false;
        bool delete_raw_post_emissive = false;
        const bool base_ready = prepare_raw_image(comp->mRawImagesBaseColor[i], false, tex, delete_raw_post);
        const bool emissive_ready = !tex_emissive || prepare_raw_image(comp->mRawImagesEmissive[i], true, tex_emissive, delete_raw_post_emissive);
        if (!base_ready || !emissive_ready)
        {
            ready = false;
            continue;
        }
        // tex_emissive can be null, and then will be ignored

        LLPointer<LLImageRaw> base = to_rgb(comp->mRawImagesBaseColor[i]);

        // In the simplest case, the minimap image is just the base color.
        // This will be replaced if we need to do any tinting/compositing.
        comp->mRawImages[i] = base;

        // *TODO: This isn't quite right for PBR:
        // 1) It does not convert the color images from SRGB to linear
        // before mixing (which will always require copying the image).
        // 2) It mixes emissive and base color before mixing terrain
        // materials, but it should be the other way around
        LLPointer<LLImageRaw> raw_emissive;
        if (tex_emissive)
        {
            raw_emissive = comp->mRawImagesEmissive[i];
            if (has_emissive_factor || raw_emissive->getComponents() != 4)
            {
                LLPointer<LLImageRaw> newraw_emissive = new LLImageRaw(BASE_SIZE, BASE_SIZE, 4);
                // Copy RGB, leave alpha alone (set to opaque by default)
                newraw_emissive->copy(to_rgb(comp->mRawImagesEmissive[i]));
                if (has_emissive_factor)
                {
                    newraw_emissive->tint(emissive_factor);
                }
                raw_emissive = newraw_emissive;
            }
        }
        // GMDX: the image's own size / components decide (LL asked the texture, whose raw level
        // can differ from the copy the callback made).
        if (has_base_color_factor ||
            raw_emissive ||
            has_alpha ||
            base->getWidth() != BASE_SIZE ||
            base->getHeight() != BASE_SIZE ||
            base->getComponents() != 3)
        {
            LLPointer<LLImageRaw> newraw = new LLImageRaw(BASE_SIZE, BASE_SIZE, 3);
            if (has_alpha)
            {
                // Approximate the water underneath terrain alpha with solid water color
                newraw->clear(
                    MAX_WATER_COLOR.mV[VX],
                    MAX_WATER_COLOR.mV[VY],
                    MAX_WATER_COLOR.mV[VZ],
                    255);
            }
            newraw->composite(base);
            if (has_base_color_factor)
            {
                newraw->tint(base_color_factor);
            }
            // Apply emissive texture
            if (raw_emissive)
            {
                newraw->addEmissive(raw_emissive);
            }

            comp->mRawImages[i] = newraw; // deletes old
        }

        if (delete_raw_post)
        {
            tex->destroyRawImage();
        }
        if (delete_raw_post_emissive)
        {
            tex_emissive->destroyRawImage();
        }

        // Remove intermediary image references
        comp->mRawImagesBaseColor[i] = nullptr;
        comp->mRawImagesEmissive[i] = nullptr;
    }
    return ready;
}

// The second half of LL's LLVLComposition::generateMinimapTileLand: blend the detail images
// by the composition value over one patch.
void LLMinimapTerrain::paintLand(LLVLComposition* comp, F32 x, F32 y, F32 width)
{
    U8* st_data[LLTerrainMaterials::ASSET_COUNT];
    S32 st_data_size[LLTerrainMaterials::ASSET_COUNT];
    for (S32 i = 0; i < LLTerrainMaterials::ASSET_COUNT; i++)
    {
        st_data[i] = comp->mRawImages[i]->getData();
        st_data_size[i] = comp->mRawImages[i]->getDataSize();
    }

    ///////////////////////////////////////
    //
    // Generate and clamp x/y bounding box.
    //
    //

    S32 x_begin, y_begin, x_end, y_end;
    x_begin = (S32)(x * comp->mScaleInv);
    y_begin = (S32)(y * comp->mScaleInv);
    x_end = ll_round( (x + width) * comp->mScaleInv );
    y_end = ll_round( (y + width) * comp->mScaleInv );

    if (x_end > comp->mWidth)
    {
        x_end = comp->mWidth;
    }
    if (y_end > comp->mWidth)
    {
        y_end = comp->mWidth;
    }

    ///////////////////////////////////////////
    //
    // Generate target texture information, stride ratios.
    //
    //

    const U32 tex_width = LAND_SIZE;
    const U32 tex_height = LAND_SIZE;
    const U32 tex_comps = 3;
    const U32 tex_stride = tex_width * tex_comps;

    const U32 st_comps = 3;
    const U32 st_width = BASE_SIZE;
    const U32 st_height = BASE_SIZE;

    const F32 tex_x_scalef = (F32)tex_width / (F32)comp->mWidth;
    const F32 tex_y_scalef = (F32)tex_height / (F32)comp->mWidth;
    const S32 tex_x_begin = (S32)((F32)x_begin * tex_x_scalef);
    const S32 tex_y_begin = (S32)((F32)y_begin * tex_y_scalef);
    const S32 tex_x_end = (S32)((F32)x_end * tex_x_scalef);
    const S32 tex_y_end = (S32)((F32)y_end * tex_y_scalef);

    const F32 tex_x_ratiof = (F32)comp->mWidth*comp->mScale / (F32)tex_width;
    const F32 tex_y_ratiof = (F32)comp->mWidth*comp->mScale / (F32)tex_height;

    U8 *rawp = mLandRawp->getData();

    F32 st_x_stride, st_y_stride;
    st_x_stride = ((F32)st_width / (F32)comp->mTexScaleX)*((F32)comp->mWidth / (F32)tex_width);
    st_y_stride = ((F32)st_height / (F32)comp->mTexScaleY)*((F32)comp->mWidth / (F32)tex_height);
    if (mOcolWas)
    {
        // OCOL look: one repeat per OCOL_REPEAT metres (the viewer's repeat averages to flat colour at map scale)
        st_x_stride = (F32)st_width / OCOL_REPEAT * (comp->mWidth * comp->mScale / (F32)tex_width);
        st_y_stride = (F32)st_height / OCOL_REPEAT * (comp->mWidth * comp->mScale / (F32)tex_height);
    }

    llassert(st_x_stride > 0.f);
    llassert(st_y_stride > 0.f);
    ////////////////////////////////
    //
    // Iterate through the target texture, striding through the
    // subtextures and interpolating appropriately.
    //
    //

    F32 sti, stj;
    S32 st_offset;
    sti = (tex_x_begin * st_x_stride) - st_width*(llfloor((tex_x_begin * st_x_stride)/st_width));
    stj = (tex_y_begin * st_y_stride) - st_height*(llfloor((tex_y_begin * st_y_stride)/st_height));

    for (S32 j = tex_y_begin; j < tex_y_end; j++)
    {
        U32 offset = j * tex_stride + tex_x_begin * tex_comps;
        sti = (tex_x_begin * st_x_stride) - st_width*((U32)(tex_x_begin * st_x_stride)/st_width);
        for (S32 i = tex_x_begin; i < tex_x_end; i++)
        {
            S32 tex0, tex1;
            F32 composition = comp->getValueScaled(i*tex_x_ratiof, j*tex_y_ratiof);

            tex0 = llfloor( composition );
            tex0 = llclamp(tex0, 0, 3);
            composition -= tex0;
            tex1 = tex0 + 1;
            tex1 = llclamp(tex1, 0, 3);

            st_offset = (lltrunc(sti) + lltrunc(stj)*st_width) * st_comps;
            for (U32 k = 0; k < tex_comps; k++)
            {
                // Linearly interpolate based on composition.
                if (st_offset < st_data_size[tex0] && st_offset < st_data_size[tex1])
                {
                    F32 a = *(st_data[tex0] + st_offset);
                    F32 b = *(st_data[tex1] + st_offset);
                    rawp[ offset ] = (U8)lltrunc( a + composition * (b - a) );
                }
                offset++;
                st_offset++;
            }

            sti += st_x_stride;
            if (sti >= st_width)
            {
                sti -= st_width;
            }
        }

        stj += st_y_stride;
        if (stj >= st_height)
        {
            stj -= st_height;
        }
    }

    if (tex_x_end > tex_x_begin && tex_y_end > tex_y_begin)
    {
        mLandTexp->setSubImage(mLandRawp, tex_x_begin, tex_y_begin, tex_x_end - tex_x_begin, tex_y_end - tex_y_begin);
    }
}

// LL's LLSurface::generateWaterTexture: water colour over the land, more opaque with depth.
void LLMinimapTerrain::paintWater(F32 x, F32 y, F32 width)
{
    const S32 tex_width = WATER_SIZE;
    const S32 tex_comps = 4;
    const S32 tex_stride = tex_width * tex_comps;
    U8 *rawp = mWaterRawp->getData();

    F32 scale = 256.f * mSurfacep->getMetersPerGrid() / (F32)tex_width;
    F32 scale_inv = 1.f / scale;

    S32 x_begin, y_begin, x_end, y_end;

    x_begin = ll_round(x * scale_inv);
    y_begin = ll_round(y * scale_inv);
    x_end = ll_round((x + width) * scale_inv);
    y_end = ll_round((y + width) * scale_inv);

    if (x_end > tex_width)
    {
        x_end = tex_width;
    }
    if (y_end > tex_width)
    {
        y_end = tex_width;
    }

    LLVector3 location;
    LLColor4U coloru;

    const F32 WATER_HEIGHT = mSurfacep->getWaterHeight();

    S32 i, j, offset;
    for (j = y_begin; j < y_end; j++)
    {
        for (i = x_begin; i < x_end; i++)
        {
            offset = j*tex_stride + i*tex_comps;
            location.mV[VX] = i*scale;
            location.mV[VY] = j*scale;

            // Sample multiple points
            const F32 height = mSurfacep->resolveHeightRegion(location);

            coloru = MAX_WATER_COLOR;
            if (height > WATER_HEIGHT)
            {
                // Above water...
                coloru.mV[3] = ABOVE_WATERLINE_ALPHA;
            }
            else
            {
                // Want non-linear curve for transparency gradient
                const F32 frac = 1.f - 2.f/(2.f - (height - WATER_HEIGHT));
                S32 alpha = 64 + ll_round((255-64)*frac);

                alpha = llmin(ll_round((F32)MAX_WATER_COLOR.mV[3]), alpha);
                alpha = llmax(64, alpha);

                coloru.mV[3] = alpha;
            }
            *(rawp + offset++) = coloru.mV[0];
            *(rawp + offset++) = coloru.mV[1];
            *(rawp + offset++) = coloru.mV[2];
            *(rawp + offset++) = coloru.mV[3];
        }
    }

    if (x_end > x_begin && y_end > y_begin)
    {
        mWaterTexp->setSubImage(mWaterRawp, x_begin, y_begin, x_end - x_begin, y_end - y_begin);
    }
}
