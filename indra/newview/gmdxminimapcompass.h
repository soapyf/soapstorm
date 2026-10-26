/**
 * @file gmdxminimapcompass.h
 * @brief GunMetal-DX mini-map compass styles for "Show Whole Region".
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

// <GMDX> The compass of the mini-map's "Show Whole Region" mode (GMDXMiniMapWholeRegion),
// which LLNetMap::draw calls after the map. The map is north-up in that mode, so the compass
// marks the map's edges; the floater's own direction letters are hidden. Three styles
// (GMDXMiniMapCompassStyle, the mini-map menu's "Compass Style"):
//  0 gunmetal frame: a brushed-metal band around the map, the eight directions and tick marks in it;
//  1 edge pills: the map fills the view, the eight directions in rounded labels at its edges / corners;
//  2 heading tape: a strip above the map scrolls with the camera's heading.
// Sizes are UI units, so they scale with the UI like the rest of the floater.

#pragma once

#include "llrect.h"
#include "stdtypes.h"

namespace GMDXMinimapCompass
{
    enum EStyle
    {
        STYLE_FRAME = 0,
        STYLE_PILLS = 1,
        STYLE_TAPE = 2,
    };

    // The selected style (GMDXMiniMapCompassStyle, clamped to a known one).
    S32 getStyle();

    // How far the map sits inside the square the compass is drawn in, on every side
    // (the frame band for the gunmetal frame, 0 for the others).
    F32 getMapInset();

    // The strip the compass takes ABOVE the map's square (the heading tape sits above the map,
    // not over it; 0 for the other styles). The mini-map's square and the floater's aspect
    // lock leave this much room on top.
    F32 getTopReserve();

    // Draws the selected style. square = the square, in view-local UI units, that the map
    // and its compass fill (the map is `square` inset by getMapInset()). heading_deg = the
    // camera's heading in degrees, 0 = north, clockwise (only the heading tape uses it; it is
// drawn in the getTopReserve() strip above `square`).
    // Untextured and text draws only; leaves texture unit 0 unbound.
    void draw(const LLRectf& square, F32 heading_deg);

    // The rotating Whole Region map's compass (GMDXMiniMapRotateCompass): STYLE_PILLS (default,
    // the map fills the view, the eight pills turn just inside its edge) or STYLE_FRAME (the
    // map shrinks inside a gunmetal frame of width getRadarRing() whose ticks and letters turn).
    // The tape is north-up only: getTopReserve() is 0 while the map rotates.
    S32 getRotatingStyle();
    F32 getRadarRing();
    // The round radar: the map's disc (centre, radius in view-local UI units); with the frame
    // its ring lies outside the radius.
    void drawRadar(F32 cx, F32 cy, F32 radius, F32 heading_deg, S32 style);
    // The square rotating map: `square` is the map (pills) or the map with its frame around it
    // (frame: the map is inset by getRadarRing()); letters slide round so heading_deg points up.
    void drawRadarSquare(const LLRectf& square, F32 heading_deg, S32 style);
}
