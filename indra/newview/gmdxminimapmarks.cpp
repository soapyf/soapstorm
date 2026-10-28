/**
 * @file gmdxminimapmarks.cpp
 * @brief GunMetal-DX: named, coloured pins on the mini-map (see gmdxminimapmarks.h).
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

// <GMDX> whole file: see gmdxminimapmarks.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxminimapmarks.h"

#include "lldir.h"
#include "llfile.h"
#include "llsdjson.h"
#include "llslurl.h"
#include "llsurface.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "rlvactions.h"
#include "rlvcommon.h"

#include <boost/json.hpp>
#include <map>

namespace
{
struct Store
{
    std::string account_dir;   // the account folder the path was made for (store() runs per avatar per frame)
    std::string path;   // the account file the marks came from ("" = not loaded)
    std::vector<GMDXMapMark> marks;
    std::vector<GMDXMapZone> zones;
    std::map<LLUUID, LLColor4> avatar_colors;
};

Store& store()
{
    static Store s;
    // (re)loaded for the account logged in now
    const std::string& account_dir = gDirUtilp->getLindenUserDir();
    if (account_dir == s.account_dir) return s;
    s.account_dir = account_dir;
    const std::string path = account_dir.empty() ? std::string()
                           : gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "gmdx_minimap_marks.json");
    if (path == s.path) return s;
    s.path = path;
    s.marks.clear();
    s.zones.clear();
    s.avatar_colors.clear();
    if (path.empty()) return s;
    llifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return s;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    LLSD data;
    try { data = LlsdFromJson(boost::json::parse(text)); }
    catch (...) { LL_WARNS("GMDXMarks") << "unreadable: " << path << LL_ENDL; return s; }
    for (const LLSD& m : llsd::inArray(data["marks"]))
    {
        GMDXMapMark mark;
        mark.id = m["id"].asUUID();
        mark.name = m["name"].asString();
        mark.region = m["region"].asString();
        mark.pos_global.setVec(m["x"].asReal(), m["y"].asReal(), m["z"].asReal());
        mark.color = llclamp((S32)m["color"].asInteger(), 0, GMDXMinimapMarks::COLORS - 1);
        if (mark.id.isNull()) mark.id.generate();
        s.marks.push_back(mark);
    }
    for (const LLSD& z : llsd::inArray(data["zones"]))
    {
        GMDXMapZone zone;
        zone.id = z["id"].asUUID();
        zone.name = z["name"].asString();
        const std::string kind = z["kind"].asString();
        zone.kind = kind == "circle" ? GMDXMapZone::CIRCLE : kind == "polygon" ? GMDXMapZone::POLYGON : GMDXMapZone::RECT;
        for (const LLSD& p : llsd::inArray(z["points"]))
        {
            zone.points.emplace_back(p[0].asReal(), p[1].asReal(), 0.0);
        }
        zone.color = llclamp((S32)z["color"].asInteger(), 0, GMDXMinimapMarks::COLORS - 1);
        zone.dotted = z["dotted"].asBoolean();
        zone.fill = llclamp((S32)z["fill"].asInteger(), (S32)GMDXMapZone::FILL_NONE, (S32)GMDXMapZone::FILL_CAUTION);
        const size_t need = zone.kind == GMDXMapZone::POLYGON ? 3 : 2;
        if (zone.points.size() < need) continue;
        if (zone.id.isNull()) zone.id.generate();
        s.zones.push_back(zone);
    }
    for (const LLSD& a : llsd::inArray(data["avatar_colors"]))
    {
        const LLUUID id = a["id"].asUUID();
        const LLSD& c = a["color"];
        if (id.notNull() && c.isArray() && c.size() >= 3)
        {
            s.avatar_colors[id] = LLColor4((F32)c[0].asReal(), (F32)c[1].asReal(), (F32)c[2].asReal(), 1.f);
        }
    }
    return s;
}

void save()
{
    Store& s = store();
    if (s.path.empty()) return;
    LLSD data = LLSD::emptyMap();
    LLSD& list = data["marks"] = LLSD::emptyArray();
    for (const GMDXMapMark& m : s.marks)
    {
        LLSD e;
        e["id"] = m.id;
        e["name"] = m.name;
        e["region"] = m.region;
        e["x"] = m.pos_global.mdV[VX];
        e["y"] = m.pos_global.mdV[VY];
        e["z"] = m.pos_global.mdV[VZ];
        e["color"] = m.color;
        list.append(e);
    }
    LLSD& zones = data["zones"] = LLSD::emptyArray();
    for (const GMDXMapZone& z : s.zones)
    {
        LLSD e;
        e["id"] = z.id;
        e["name"] = z.name;
        e["kind"] = z.kind == GMDXMapZone::CIRCLE ? "circle" : z.kind == GMDXMapZone::POLYGON ? "polygon" : "rect";
        LLSD& points = e["points"] = LLSD::emptyArray();
        for (const LLVector3d& p : z.points)
        {
            LLSD xy = LLSD::emptyArray();
            xy.append(p.mdV[VX]);
            xy.append(p.mdV[VY]);
            points.append(xy);
        }
        e["color"] = z.color;
        e["dotted"] = z.dotted;
        e["fill"] = z.fill;
        zones.append(e);
    }
    LLSD& avatars = data["avatar_colors"] = LLSD::emptyArray();
    for (const auto& [id, c] : s.avatar_colors)
    {
        LLSD e;
        e["id"] = id;
        LLSD& rgb = e["color"] = LLSD::emptyArray();
        rgb.append(c.mV[VRED]);
        rgb.append(c.mV[VGREEN]);
        rgb.append(c.mV[VBLUE]);
        avatars.append(e);
    }
    const std::string text = boost::json::serialize(LlsdToJson(data)) + "\n";
    const std::string tmp = s.path + ".tmp";
    {
        llofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return;
        f << text;
    }
    if (LLFile::rename(tmp, s.path) != 0) LLFile::remove(tmp, ENOENT);
}

GMDXMapZone* find_zone_mutable(const LLUUID& id)
{
    for (GMDXMapZone& z : store().zones)
        if (z.id == id) return &z;
    return nullptr;
}

GMDXMapMark* find_mutable(const LLUUID& id)
{
    for (GMDXMapMark& m : store().marks)
        if (m.id == id) return &m;
    return nullptr;
}
} // namespace

LLColor4 GMDXMinimapMarks::color(S32 index)
{
    static const LLColor4 colors[COLORS] = {
        LLColor4(1.f, 0.353f, 0.353f, 1.f),     // red #FF5A5A
        LLColor4(1.f, 0.710f, 0.278f, 1.f),     // orange #FFB547
        LLColor4(0.961f, 0.843f, 0.431f, 1.f),  // yellow #F5D76E
        LLColor4(0.482f, 0.847f, 0.561f, 1.f),  // green #7BD88F
        LLColor4(0.435f, 0.827f, 1.f, 1.f),     // cyan #6FD3FF
        LLColor4(0.780f, 0.608f, 1.f, 1.f),     // purple #C79BFF
    };
    return colors[llclamp(index, 0, COLORS - 1)];
}

const char* GMDXMinimapMarks::colorName(S32 index)
{
    static const char* const names[COLORS] = { "Red", "Orange", "Yellow", "Green", "Cyan", "Purple" };
    return names[llclamp(index, 0, COLORS - 1)];
}

const std::vector<GMDXMapMark>& GMDXMinimapMarks::all()
{
    return store().marks;
}

const GMDXMapMark* GMDXMinimapMarks::find(const LLUUID& id)
{
    return find_mutable(id);
}

const GMDXMapMark& GMDXMinimapMarks::add(const LLVector3d& pos_global, const std::string& name, S32 color)
{
    GMDXMapMark m;
    m.id.generate();
    m.name = name;
    m.pos_global = pos_global;
    m.color = llclamp(color, 0, COLORS - 1);
    if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global))
    {
        m.region = region->getName();
    }
    store().marks.push_back(m);
    save();
    return store().marks.back();
}

void GMDXMinimapMarks::rename(const LLUUID& id, const std::string& name)
{
    if (GMDXMapMark* m = find_mutable(id)) { m->name = name; save(); }
}

void GMDXMinimapMarks::recolor(const LLUUID& id, S32 color)
{
    if (GMDXMapMark* m = find_mutable(id)) { m->color = llclamp(color, 0, COLORS - 1); save(); }
}

void GMDXMinimapMarks::remove(const LLUUID& id)
{
    auto& marks = store().marks;
    const size_t before = marks.size();
    marks.erase(std::remove_if(marks.begin(), marks.end(), [&id](const GMDXMapMark& m) { return m.id == id; }), marks.end());
    if (marks.size() != before) save();
}

const std::vector<GMDXMapZone>& GMDXMinimapMarks::zones()
{
    return store().zones;
}

const GMDXMapZone* GMDXMinimapMarks::findZone(const LLUUID& id)
{
    return find_zone_mutable(id);
}

void GMDXMinimapMarks::addZone(GMDXMapZone zone)
{
    if (zone.id.isNull()) zone.id.generate();
    zone.color = llclamp(zone.color, 0, COLORS - 1);
    store().zones.push_back(std::move(zone));
    save();
}

void GMDXMinimapMarks::renameZone(const LLUUID& id, const std::string& name)
{
    if (GMDXMapZone* z = find_zone_mutable(id)) { z->name = name; save(); }
}

void GMDXMinimapMarks::recolorZone(const LLUUID& id, S32 color)
{
    if (GMDXMapZone* z = find_zone_mutable(id)) { z->color = llclamp(color, 0, COLORS - 1); save(); }
}

void GMDXMinimapMarks::setZoneDotted(const LLUUID& id, bool dotted)
{
    if (GMDXMapZone* z = find_zone_mutable(id)) { z->dotted = dotted; save(); }
}

void GMDXMinimapMarks::setZoneFill(const LLUUID& id, S32 fill)
{
    if (GMDXMapZone* z = find_zone_mutable(id))
    {
        z->fill = llclamp(fill, (S32)GMDXMapZone::FILL_NONE, (S32)GMDXMapZone::FILL_CAUTION);
        save();
    }
}

void GMDXMinimapMarks::removeZone(const LLUUID& id)
{
    auto& zones = store().zones;
    const size_t before = zones.size();
    zones.erase(std::remove_if(zones.begin(), zones.end(), [&id](const GMDXMapZone& z) { return z.id == id; }), zones.end());
    if (zones.size() != before) save();
}

bool GMDXMinimapMarks::avatarColor(const LLUUID& avatar_id, LLColor4& color)
{
    const Store& s = store();
    const auto it = s.avatar_colors.find(avatar_id);
    if (it == s.avatar_colors.end()) return false;
    color = it->second;
    return true;
}

void GMDXMinimapMarks::setAvatarColor(const LLUUID& avatar_id, const LLColor4& color)
{
    if (avatar_id.isNull()) return;
    store().avatar_colors[avatar_id] = LLColor4(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 1.f);
    save();
}

void GMDXMinimapMarks::clearAvatarColor(const LLUUID& avatar_id)
{
    if (store().avatar_colors.erase(avatar_id)) save();
}

LLVector3d GMDXMinimapMarks::onGround(LLVector3d pos_global)
{
    if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global))
    {
        const LLVector3 p = region->getPosRegionFromGlobal(pos_global);
        pos_global.mdV[VZ] = region->getOriginGlobal().mdV[VZ] + region->getLand().resolveHeightRegion(p);
    }
    return pos_global;
}

std::string GMDXMinimapMarks::coordinatesText(const LLVector3d& pos_global)
{
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global);
    // [RLVa] the region's name, and where off the loaded regions, are hidden while the location is
    const bool show = RlvActions::canShowLocation();
    if (!region) return show ? llformat("%.0f, %.0f", pos_global.mdV[VX], pos_global.mdV[VY]) : RlvStrings::getString(RlvStringKeys::Hidden::Region);
    const LLVector3 p = region->getPosRegionFromGlobal(pos_global);
    const std::string name = show ? region->getName() : RlvStrings::getString(RlvStringKeys::Hidden::Region);
    return llformat("%s (%d, %d, %d)", name.c_str(), ll_round(p.mV[VX]), ll_round(p.mV[VY]), ll_round(p.mV[VZ]));
}

std::string GMDXMinimapMarks::slurl(const LLVector3d& pos_global)
{
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global);
    if (!region || !RlvActions::canShowLocation()) return std::string();   // [RLVa] no SLURL while the location is hidden
    const LLVector3 p = region->getPosRegionFromGlobal(pos_global);
    return LLSLURL(region->getName(), LLVector3((F32)ll_round(p.mV[VX]), (F32)ll_round(p.mV[VY]), (F32)ll_round(p.mV[VZ])))
        .getSLURLString();
}
