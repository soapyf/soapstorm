/**
 * @file sslocalcontentlimits.h
 * @brief Build-tool scale and position limits for viewer-local content (Atmo Magic landscape objects).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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

#ifndef SS_LOCALCONTENTLIMITS_H
#define SS_LOCALCONTENTLIMITS_H

#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworld.h"

// <SS:Nexii> A local landscape object never reaches a simulator, so the sim's prim limits (64 m, inside the region) are not its limits: scenery is mountains and coastlines, drawn in the void around the region. These apply ONLY to objects flagged ssIsLocalContent(); every stock object keeps the stock clamps. The area is 2048 m square centred on the object's own region, so a locked record re-anchors to the same spot relative to whichever region it lands in. doc/atmo_landscape/design_synthesis.md.
constexpr F32 SS_LOCAL_CONTENT_MAX_SCALE_M = 2048.f;
constexpr F32 SS_LOCAL_CONTENT_REACH_M = 1024.f;        // half the placement area's side

// The scale ceiling the build tools apply to this object: the local-content ceiling, or the stock one passed in.
inline F32 ssMaxPrimScale(const LLViewerObject* obj, F32 stock_max)
{
    return (obj && obj->ssIsLocalContent()) ? SS_LOCAL_CONTENT_MAX_SCALE_M : stock_max;
}

// Region-local X/Y bounds of the placement area for local content anchored to this region.
inline void ssLocalContentRegionBounds(const LLViewerRegion* regionp, F32& lo, F32& hi)
{
    const F32 centre = regionp ? regionp->getWidth() * 0.5f : 128.f;
    lo = centre - SS_LOCAL_CONTENT_REACH_M;
    hi = centre + SS_LOCAL_CONTENT_REACH_M;
}

// Clamp a region-local position's X/Y into the placement area; Z is left to the caller's height clamps.
inline LLVector3 ssClampLocalContentRegionPos(const LLViewerRegion* regionp, LLVector3 pos_region)
{
    F32 lo, hi;
    ssLocalContentRegionBounds(regionp, lo, hi);
    pos_region.mV[VX] = llclamp(pos_region.mV[VX], lo, hi);
    pos_region.mV[VY] = llclamp(pos_region.mV[VY], lo, hi);
    return pos_region;
}

// The move clip every manipulator applies to a root: local content is clamped into its placement area, everything else is clipped to the visible regions as stock does.
inline LLVector3d ssClipLocalContentMove(const LLViewerObject* obj, const LLVector3d& start_global, const LLVector3d& end_global)
{
    if (obj && obj->ssIsLocalContent())
    {
        const LLViewerRegion* home = obj->getRegion();
        if (!home) return end_global;
        return home->getPosGlobalFromRegion(ssClampLocalContentRegionPos(home, home->getPosRegionFromGlobal(end_global)));
    }
    return LLWorld::getInstance()->clipToVisibleRegions(start_global, end_global);
}

#endif // SS_LOCALCONTENTLIMITS_H
