/**
 * @file gmdxregioncapture.h
 * @brief Region map capture and viewer sharing: maps of whole regions on the mini-map and World Map.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

#pragma once

#include "llpointer.h"
#include "stdtypes.h"
#include "v3math.h"

#include <functional>
#include <string>
#include <vector>

class LLFloater;
class LLImageRaw;
class LLSD;
class LLUUID;
class LLViewerRegion;
class LLViewerTexture;

// One orthographic render. Agent space, z up; the view looks down -Z with +Y (north) up
// and +X (east) to the right, so the image's first row (LLImageRaw is bottom-up) is the south edge.
struct LLRegionOrthoView
{
    LLVector3 eye;                       // above the ceiling
    F32 left = 0.f, right = 0.f;         // the box, relative to the eye (m, x east)
    F32 bottom = 0.f, top = 0.f;         // (m, y north)
    F32 ceiling = 0.f, floor = 0.f;      // agent-space z of the box's top (the near plane) and bottom
    F32 demand_ppm = 0.f;                // texel demand in px per metre
    F32 time = 0.f;                      // world clock
    S32 width = 0, height = 0;
    U32 frames = 1;
    U32 aa_frames = 0;
    bool shadows = true;                 // sun shadows
};

// The world through `view` rendered via Soapstorm's OpenGL pipeline as bottom-up RGB;
// with `depth`, normalized depth (1 = ceiling, 0 = floor, rows top-down).
bool glRenderRegionOrthoView(const LLRegionOrthoView& view, std::vector<U8>& rgb, std::vector<F32>* depth = nullptr);

class GMDXRegionCapture
{
public:
    // Mini-map menu: "Capture Region Map..." opens the capture window which asks first (requestCapture)
    static void openWindow();
    static void requestCapture();
    static bool canCapture();
    static bool isRunning();
    static void cancelCapture();

    // Mini-map menu, Region Map: delete this region's map (back to usual background) and open maps folder.
    static void deleteCapture(const LLViewerRegion* region);
    static void openMapsFolder();

    // Sharing: export this region's map as one .gmdxmap file, and import one someone else exported
    static void exportMap(const LLViewerRegion* region);
    static void importMap();

    // Inventory texture: upload and use
    static void uploadMap(const LLViewerRegion* region);
    static bool isMapTexture(const std::string& description);
    static void useMapTexture(const LLUUID& asset_id, const std::string& name, const std::string& description);

    // Stop on the next frame with notification reason
    static void requestStop(const std::string& why);

    // Ceiling calculation for region
    static F32 ceilingFor(const LLViewerRegion* region, bool* hills = nullptr);

    // LLFloaterReg build function
    static LLFloater* buildWindow(const LLSD& key);

    // Per-frame tick and shutdown
    static void frameTick();
    static void shutdown();

    // Query and loading
    static bool hasCapture(const LLViewerRegion* region);
    static bool getCapturedImage(const LLViewerRegion* region, LLPointer<LLImageRaw>& out);
    static bool loadCapturedImage(const LLViewerRegion* region, std::function<void(LLPointer<LLImageRaw>)> done);
    static LLViewerTexture* getMinimapTexture(const LLViewerRegion* region);
    static void forEachWorldMapTexture(const std::function<bool(U64 region_handle)>& visible,
                                       const std::function<void(U64 region_handle, LLViewerTexture* tex, F32 region_width)>& draw);

    // Object LOD override for region being captured
    static bool forceMaxLOD(const LLVector3& pos_agent, F32 radius)
    {
        return sLodOn && pos_agent.mV[VX] + radius >= sLodBox[0] && pos_agent.mV[VX] - radius <= sLodBox[2]
                      && pos_agent.mV[VY] + radius >= sLodBox[1] && pos_agent.mV[VY] - radius <= sLodBox[3];
    }
    static U32 lodEpoch() { return sLodEpoch; }

private:
    friend struct GMDXRegionCaptureState;
    static bool sLodOn;
    static F32  sLodBox[4];   // agent-space x0, y0, x1, y1
    static U32  sLodEpoch;
};

using LLRegionMapCapture = GMDXRegionCapture;
