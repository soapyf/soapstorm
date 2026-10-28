/**
 * @file gmdxregioncapture.h
 * @brief GunMetal-DX region maps: maps of whole regions, shared between users, on the mini-map and World Map.
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

// <GMDX> REGION MAPS, the receiving side. GunMetal-DX (another viewer) captures a region straight
// down, orthographically, as a 4096 px map; its users hand the maps to others in two ways, and this
// viewer takes both and shows them:
//  - a map file (.gmdxmap): the line "GMDXMAP1", one line of JSON (the map's sidecar fields + grid,
//    region_grid_x / region_grid_y, payload "j2c" or "png", payload_size), then the image file's
//    bytes (importMap; exportMap writes the same format back out);
//  - an inventory texture whose description starts "GMDXMAP1 <grid x> <grid y> <region width>
//    <region id>" (useMapTexture, inventory > Use as Region Map).
// Either comes from another user, so it is checked as GunMetal-DX checks it: the size, the header,
// the image's dimensions before it is decoded, this grid, and the region at those coordinates.
// Maps are stored as GunMetal-DX stores them: <user app dir>/region_maps/<grid id>/<x>_<y>.j2c (or
// .png) with a <x>_<y>.json sidecar, so both viewers share the folder's layout.
// The mini-map shows a region's map while GMDXMinimapUseCapturedMaps is on (getMinimapTexture), the
// World Map over its tiles while GMDXWorldMapUseCapturedMaps is on (forEachWorldMapTexture).

#pragma once

#include "llpointer.h"
#include "stdtypes.h"

#include <functional>
#include <string>

class LLImageRaw;
class LLUUID;
class LLViewerRegion;
class LLViewerTexture;

class GMDXRegionCapture
{
public:
    // Mini-map menu, Region Map: delete this region's map (back to the usual background), and open
    // the folder the maps are kept in.
    static void deleteCapture(const LLViewerRegion* region);
    static void openMapsFolder();
    // Sharing: export this region's map as one .gmdxmap file, and import one someone else exported
    // (checked against this grid and the region at its coordinates; replacing a map of that region
    // asks first).
    static void exportMap(const LLViewerRegion* region);
    static void importMap();
    // From an inventory texture whose description isMapTexture: fetch it and install it as that
    // region's map (the same checks as importMap).
    static bool isMapTexture(const std::string& description);
    static void useMapTexture(const LLUUID& asset_id, const std::string& name, const std::string& description);
    // Quit: the loaded textures go before the GL objects do (LLAppViewer::cleanup).
    static void shutdown();

    // hasCapture: a map file exists for this region. loadCapturedImage: decodes it on a worker thread
    // (bottom-up RGB, north up, the region's full width); done runs on the main thread (null:
    // unreadable). False, without calling done, when the region has no map.
    static bool hasCapture(const LLViewerRegion* region);
    static bool loadCapturedImage(const LLViewerRegion* region, std::function<void(LLPointer<LLImageRaw>)> done);
    // The mini-map's background for the region, null while there is none (or it is still loading,
    // or GMDXMinimapUseCapturedMaps is off).
    static LLViewerTexture* getMinimapTexture(const LLViewerRegion* region);
    // The World Map (GMDXWorldMapUseCapturedMaps): calls draw(handle, tex, width in metres) for every
    // region of this grid with a map that visible(handle) says is on screen and whose 512 px texture
    // is loaded (loading the others on a worker; the folder is looked at again every few seconds).
    // The map lays them over its own tiles.
    static void forEachWorldMapTexture(const std::function<bool(U64 region_handle)>& visible,
                                       const std::function<void(U64 region_handle, LLViewerTexture* tex, F32 region_width)>& draw);
};
