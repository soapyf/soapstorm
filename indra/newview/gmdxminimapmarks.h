/**
 * @file gmdxminimapmarks.h
 * @brief GunMetal-DX: named, coloured pins on the mini-map.
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

// <GMDX> Mini-map marks and zones. Marks: right-click the mini-map > Add Mark Here > a colour, then a name. A mark
// is a global position (it stays put however the map is panned, zoomed or which region it is in),
// kept for this account in gmdx_minimap_marks.json in the account's folder, written on every
// change. The mini-map draws them (GMDXMiniMapShowMarks) and its menu tracks, renames, recolours
// and removes the one under the cursor.
// Zones: outlines with no fill (rectangle, circle, polygon) in a colour, solid or dotted, with an
// optional name, drawn on the mini-map (Draw Zone), kept in the same file, global positions too.
// Avatar dot colours: see avatarColor().

#pragma once

#include "lluuid.h"
#include "v3dmath.h"
#include "v4color.h"

#include <string>
#include <vector>

struct GMDXMapMark
{
    LLUUID id;
    std::string name;
    std::string region;     // the region's name when it was made (shown in copies and the tracker)
    LLVector3d pos_global;
    S32 color = 0;          // index into GMDXMinimapMarks::color()
};

struct GMDXMapZone
{
    enum Kind { RECT, CIRCLE, POLYGON };
    LLUUID id;
    std::string name;
    Kind kind = RECT;
    std::vector<LLVector3d> points;   // RECT: two opposite corners; CIRCLE: centre, a point on the edge; POLYGON: the corners
    S32 color = 4;
    bool dotted = false;
    enum Fill { FILL_NONE, FILL_COLOR, FILL_CAUTION };
    S32 fill = FILL_NONE;             // see-through: the zone's colour, or black and yellow caution stripes
};

class GMDXMinimapMarks
{
public:
    static constexpr S32 COLORS = 6;
    static LLColor4 color(S32 index);
    static const char* colorName(S32 index);

    // This account's marks (loaded on first use after login).
    static const std::vector<GMDXMapMark>& all();
    static const GMDXMapMark* find(const LLUUID& id);

    static const GMDXMapMark& add(const LLVector3d& pos_global, const std::string& name, S32 color);
    static void rename(const LLUUID& id, const std::string& name);
    static void recolor(const LLUUID& id, S32 color);
    static void remove(const LLUUID& id);

    static const std::vector<GMDXMapZone>& zones();
    static const GMDXMapZone* findZone(const LLUUID& id);
    static void addZone(GMDXMapZone zone);
    static void renameZone(const LLUUID& id, const std::string& name);
    static void recolorZone(const LLUUID& id, S32 color);
    static void setZoneDotted(const LLUUID& id, bool dotted);
    static void setZoneFill(const LLUUID& id, S32 fill);
    static void removeZone(const LLUUID& id);

    // Avatar dot colours (right-click a dot > Dot Colour): this account's own colour for an avatar's
    // mini-map dot, over its group / contact set colour. Kept in the same file.
    static bool avatarColor(const LLUUID& avatar_id, LLColor4& color);
    static void setAvatarColor(const LLUUID& avatar_id, const LLColor4& color);
    static void clearAvatarColor(const LLUUID& avatar_id);

    // A map point (no height) on the ground: z = the terrain's height there (unchanged off the loaded regions).
    static LLVector3d onGround(LLVector3d pos_global);
    // "Region (x, y, z)" and the SLURL of a global position, as the mini-map copies them.
    static std::string coordinatesText(const LLVector3d& pos_global);
    static std::string slurl(const LLVector3d& pos_global);
};
