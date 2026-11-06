/**
 * @file llminimapimage.h
 * @brief Forwarding wrapper to gmdxminimapimage.h
 */

#pragma once

#include "gmdxminimapimage.h"

inline bool minimap_get_region_map_image(LLViewerRegion* regionp, std::function<void(LLPointer<LLImageRaw>)> done)
{
    return gmdx_get_region_map_image(regionp, std::move(done));
}

inline void minimap_save_region_map_image()
{
    gmdx_save_region_map_image();
}
