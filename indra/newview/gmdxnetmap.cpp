/**
 * @file gmdxnetmap.cpp
 * @brief GunMetal-DX additions to the mini-map (LLNetMap): Show Whole Region and its radar,
 * marks, zones, the coordinates under the cursor, physical objects and dot colours.
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

// <GMDX> whole file: LLNetMap members added by GunMetal-DX (declared in llnetmap.h, under <GMDX>),
// kept out of llnetmap.cpp so Firestorm's file has only the hooks that call them.
//  - Show Whole Region (GMDXMiniMapWholeRegion): the agent's region fitted to the view, north up,
//    with a compass (gmdxminimapcompass.h); the wheel zooms in and back out to the whole region,
//    zoomed in a plain drag pans. With Camera at top it turns (GMDXMiniMapRotateView): a round or
//    square radar around the agent, or the whole region about its centre.
//  - Marks and zones (gmdxminimapmarks.h), right-click > Add Mark Here / Draw Zone, and the region
//    coordinates under the cursor; Copy Coordinates / Copy SLURL.
//  - Show > Physical Objects Only (GMDXMiniMapPhysicalOnly): bullets and other moving physical
//    objects, every frame, instead of the object layer.
//  - Right-click a dot > Dot Colour: this account's own colour for an avatar's dot.

#include "llviewerprecompiledheaders.h"

#include "llnetmap.h"
#include "llfloatermap.h"

#include "gmdxminimapcompass.h"
#include "gmdxminimapimage.h"
#include "gmdxminimapmarks.h"
#include "gmdxregioncapture.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llclipboard.h"
#include "llcolorswatch.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lltracker.h"
#include "lluicolortable.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "rlvactions.h"

// Mini-map zones: outlines, no fill, 2 px in the zone's colour over a dark halo, solid or dashed;
// the name above the shape's highest point. While drawing, the shape so far follows the cursor.
namespace
{
// The round radar's disc (view coordinates) for the zone drawing; radius 0 = no radar.
LLVector2 sDiscCenter;
F32 sDiscRadius = 0.f;

// One polyline as quads, dashed with a pattern carried across its corners.
void zone_lines(const std::vector<LLVector2>& pts, bool closed, F32 width, const LLColor4& color, bool dotted)
{
    constexpr F32 DASH = 7.f, GAP = 5.f;
    if (pts.size() < 2) return;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::TRIANGLES);
    F32 phase = 0.f;   // distance into the current dash + gap
    const size_t n = pts.size(), segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i)
    {
        const LLVector2 a = pts[i], b = pts[(i + 1) % n];
        const LLVector2 d = b - a;
        const F32 len = d.length();
        if (len < 0.01f) continue;
        const LLVector2 dir = d / len;
        const LLVector2 nrm(-dir.mV[VY] * width * 0.5f, dir.mV[VX] * width * 0.5f);
        F32 t = 0.f;
        while (t < len)
        {
            F32 run;
            bool on = true;
            if (dotted)
            {
                on = phase < DASH;
                run = llmin((on ? DASH : DASH + GAP) - phase, len - t);
            }
            else
            {
                run = len - t;
            }
            F32 t0 = t, t1 = t + run;
            if (on && sDiscRadius > 0.f)   // the radar: only the part inside the disc
            {
                const LLVector2 f = a - sDiscCenter;
                const F32 r = sDiscRadius - width * 0.5f;
                const F32 bq = f * dir, cq = f * f - r * r, disc = bq * bq - cq;
                if (disc <= 0.f) on = false;
                else
                {
                    t0 = llmax(t0, -bq - sqrtf(disc));
                    t1 = llmin(t1, -bq + sqrtf(disc));
                    on = t1 > t0;
                }
            }
            if (on)
            {
                const LLVector2 p0 = a + dir * t0, p1 = a + dir * t1;
                gGL.vertex2f(p0.mV[VX] - nrm.mV[VX], p0.mV[VY] - nrm.mV[VY]);
                gGL.vertex2f(p1.mV[VX] - nrm.mV[VX], p1.mV[VY] - nrm.mV[VY]);
                gGL.vertex2f(p1.mV[VX] + nrm.mV[VX], p1.mV[VY] + nrm.mV[VY]);
                gGL.vertex2f(p0.mV[VX] - nrm.mV[VX], p0.mV[VY] - nrm.mV[VY]);
                gGL.vertex2f(p1.mV[VX] + nrm.mV[VX], p1.mV[VY] + nrm.mV[VY]);
                gGL.vertex2f(p0.mV[VX] + nrm.mV[VX], p0.mV[VY] + nrm.mV[VY]);
            }
            t += run;
            if (dotted) phase = fmodf(phase + run, DASH + GAP);
        }
    }
    gGL.end();
}

// Triangles (3 points each) covering a simple polygon: ear clipping, a fan when the outline crosses itself.
void zone_triangles(const std::vector<LLVector2>& pts, std::vector<LLVector2>& tris)
{
    tris.clear();
    const size_t n = pts.size();
    if (n < 3) return;
    F32 area = 0.f;
    for (size_t i = 0; i < n; ++i)
    {
        const LLVector2& a = pts[i];
        const LLVector2& b = pts[(i + 1) % n];
        area += a.mV[VX] * b.mV[VY] - b.mV[VX] * a.mV[VY];
    }
    const F32 sign = area >= 0.f ? 1.f : -1.f;
    auto cross = [](const LLVector2& o, const LLVector2& a, const LLVector2& b)
    { return (a.mV[VX] - o.mV[VX]) * (b.mV[VY] - o.mV[VY]) - (a.mV[VY] - o.mV[VY]) * (b.mV[VX] - o.mV[VX]); };
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    size_t guard = n * n + 8;
    while (idx.size() > 3 && guard--)
    {
        bool clipped = false;
        for (size_t k = 0; k < idx.size(); ++k)
        {
            const size_t ip = idx[(k + idx.size() - 1) % idx.size()], ic = idx[k], in = idx[(k + 1) % idx.size()];
            const LLVector2 &a = pts[ip], &b = pts[ic], &c = pts[in];
            if (cross(a, b, c) * sign <= 0.f) continue;   // reflex corner
            bool inside = false;
            for (size_t j : idx)
            {
                if (j == ip || j == ic || j == in) continue;
                const LLVector2& p = pts[j];
                if (cross(a, b, p) * sign > 0.f && cross(b, c, p) * sign > 0.f && cross(c, a, p) * sign > 0.f) { inside = true; break; }
            }
            if (inside) continue;
            tris.push_back(a); tris.push_back(b); tris.push_back(c);
            idx.erase(idx.begin() + k);
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (idx.size() == 3)
    {
        tris.push_back(pts[idx[0]]); tris.push_back(pts[idx[1]]); tris.push_back(pts[idx[2]]);
    }
    else if (idx.size() > 3)   // (a crossing outline) the rest as a fan
    {
        for (size_t k = 1; k + 1 < idx.size(); ++k)
        {
            tris.push_back(pts[idx[0]]); tris.push_back(pts[idx[k]]); tris.push_back(pts[idx[k + 1]]);
        }
    }
}

// A convex polygon cut to the side of the line n.p = c that keep_above says.
void clip_half(std::vector<LLVector2>& poly, const LLVector2& n, F32 c, bool keep_above)
{
    std::vector<LLVector2> out;
    const size_t m = poly.size();
    for (size_t i = 0; i < m; ++i)
    {
        const LLVector2& a = poly[i];
        const LLVector2& b = poly[(i + 1) % m];
        const F32 da = (a * n - c) * (keep_above ? 1.f : -1.f), db = (b * n - c) * (keep_above ? 1.f : -1.f);
        if (da >= 0.f) out.push_back(a);
        if ((da >= 0.f) != (db >= 0.f)) out.push_back(a + (b - a) * (da / (da - db)));
    }
    poly.swap(out);
}

// A convex polygon cut to the disc (a RADAR_SIDES-gon) of radius r around c.
void clip_disc(std::vector<LLVector2>& poly, const LLVector2& c, F32 r)
{
    constexpr S32 RADAR_SIDES = 96;
    const F32 apothem = r * cosf(F_PI / RADAR_SIDES);
    for (S32 k = 0; k < RADAR_SIDES && poly.size() >= 3; ++k)
    {
        const F32 a = F_TWO_PI * (k + 0.5f) / RADAR_SIDES;
        const LLVector2 n(cosf(a), sinf(a));
        clip_half(poly, n, n * c + apothem, false);
    }
    if (poly.size() < 3) poly.clear();
}

// A textured axis-aligned quad cut to the disc of radius r around the origin, inside gGL.begin(TRIANGLES).
void disc_quad(F32 x0, F32 y0, F32 x1, F32 y1, F32 u0, F32 v0, F32 u1, F32 v1, F32 r)
{
    std::vector<LLVector2> poly = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    clip_disc(poly, LLVector2(0.f, 0.f), r);
    auto emit = [&](const LLVector2& p)
    {
        gGL.texCoord2f(u0 + (u1 - u0) * (p.mV[VX] - x0) / (x1 - x0), v0 + (v1 - v0) * (p.mV[VY] - y0) / (y1 - y0));
        gGL.vertex2f(p.mV[VX], p.mV[VY]);
    };
    for (size_t k = 1; k + 1 < poly.size(); ++k)
    {
        emit(poly[0]); emit(poly[k]); emit(poly[k + 1]);
    }
}

// The fill: the zone's colour, or 45-degree black / yellow bands 7 px wide that move with the zone
// (anchor: a point of the zone in view coordinates).
void zone_fill(const std::vector<LLVector2>& outline, S32 fill, const LLColor4& color, const LLVector2& anchor)
{
    std::vector<LLVector2> tris;
    zone_triangles(outline, tris);
    if (tris.empty()) return;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    if (fill == GMDXMapZone::FILL_COLOR)
    {
        gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 0.22f);
        gGL.begin(LLRender::TRIANGLES);
        std::vector<LLVector2> tri;
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            tri.assign(tris.begin() + t, tris.begin() + t + 3);
            if (sDiscRadius > 0.f) clip_disc(tri, sDiscCenter, sDiscRadius);
            for (size_t k = 1; k + 1 < tri.size(); ++k)
            {
                gGL.vertex2f(tri[0].mV[VX], tri[0].mV[VY]);
                gGL.vertex2f(tri[k].mV[VX], tri[k].mV[VY]);
                gGL.vertex2f(tri[k + 1].mV[VX], tri[k + 1].mV[VY]);
            }
        }
        gGL.end();
        return;
    }
    constexpr F32 BAND = 7.f;
    const LLVector2 n(F_SQRT2 * 0.5f, F_SQRT2 * 0.5f);
    const F32 base = anchor * n;
    static const LLColor4 yellow(0.98f, 0.80f, 0.10f, 0.42f), black(0.04f, 0.04f, 0.04f, 0.40f);
    std::vector<LLVector2> poly;
    gGL.begin(LLRender::TRIANGLES);
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
    {
        F32 lo = F32_MAX, hi = -F32_MAX;
        for (size_t k = 0; k < 3; ++k) { const F32 s = tris[t + k] * n - base; lo = llmin(lo, s); hi = llmax(hi, s); }
        for (S32 band = (S32)floorf(lo / BAND); (F32)band * BAND < hi; ++band)
        {
            poly.assign(tris.begin() + t, tris.begin() + t + 3);
            clip_half(poly, n, base + band * BAND, true);
            clip_half(poly, n, base + (band + 1) * BAND, false);
            if (sDiscRadius > 0.f) clip_disc(poly, sDiscCenter, sDiscRadius);
            if (poly.size() < 3) continue;
            const LLColor4& c = (band & 1) ? black : yellow;
            gGL.color4fv(c.mV);
            for (size_t k = 1; k + 1 < poly.size(); ++k)
            {
                gGL.vertex2f(poly[0].mV[VX], poly[0].mV[VY]);
                gGL.vertex2f(poly[k].mV[VX], poly[k].mV[VY]);
                gGL.vertex2f(poly[k + 1].mV[VX], poly[k + 1].mV[VY]);
            }
        }
    }
    gGL.end();
}

void zone_shape(const std::vector<LLVector2>& pts, bool closed, const LLColor4& color, bool dotted)
{
    zone_lines(pts, closed, 4.f, LLColor4(0.05f, 0.06f, 0.07f, 0.55f), dotted);   // halo
    zone_lines(pts, closed, 2.f, color, dotted);
}
} // namespace

// ---- Show Whole Region ----

LLVector3d LLNetMap::mapCenterGlobal() const
{
    return mFitRegion ? mFitCenterGlobal : gAgentCamera.getCameraPositionGlobal();
}

LLVector2 LLNetMap::radarCenter() const
{
    return LLVector2((F32)(getRect().getWidth() / 2) + mFitPan.mV[VX], (F32)(getRect().getHeight() / 2) + mFitPan.mV[VY]);
}

LLVector2 LLNetMap::toRadarRim(const LLVector2& p) const
{
    const LLVector2 c = radarCenter();
    const LLVector2 d = p - c;
    if (mRadarRadius <= 0.f)   // the square radar, or the north-up Whole Region map
    {
        const F32 hx = llmax(mFitMapRect.getWidth() * 0.5f - 3.f, 1.f), hy = llmax(mFitMapRect.getHeight() * 0.5f - 3.f, 1.f);
        const F32 k = llmax(fabsf(d.mV[VX]) / hx, fabsf(d.mV[VY]) / hy);
        return k > 1.f ? c + d / k : p;
    }
    const F32 len = d.length();
    const F32 rim = llmax(mRadarRadius - 3.f, 0.f);
    return len > rim && len > 0.f ? c + d * (rim / len) : p;
}

void LLNetMap::updateWholeRegion(LLViewerRegion* region, LLRectf& fit_square)
{
    // The agent's region fitted to the view's centred square (inside the compass frame, if the
    // compass style has one); with Camera at top it turns as GMDXMiniMapRotateView says: 0 / 1 =
    // a radar, round / square, around the agent; 2 = the whole region about its centre, shrunk so
    // it fits at any angle.
    static LLCachedControl<bool> s_whole_region(gSavedSettings, "GMDXMiniMapWholeRegion", false);
    static LLCachedControl<S32> rotate_view(gSavedSettings, "GMDXMiniMapRotateView", 0);
    static LLUICachedControl<bool> rotate_map("MiniMapRotate", true);
    // (only the mini-map floater's map: the net_map in the radar / people panels stays as it is)
    const bool whole_region = s_whole_region && region && getParentByType<LLFloaterMap>();
    F32 fit_ppm = 0.f;
    const bool rotating = whole_region && rotate_map;
    const S32 rot_view = rotating ? llclamp((S32)rotate_view, 0, 2) : -1;
    const bool radar = rot_view == 0 || rot_view == 1;   // centred on the agent
    const bool rotate_frame = rotating && GMDXMinimapCompass::getRotatingStyle() == GMDXMinimapCompass::STYLE_FRAME;
    if (whole_region)
    {
        const S32 width = getRect().getWidth();
        // the heading tape's strip sits above the map: the square is fitted below it
        const S32 height = getRect().getHeight() - (S32)GMDXMinimapCompass::getTopReserve();
        // even, so it is centred on the map's centre pixel (width / 2, height / 2) exactly
        const S32 side = llmax(llmin(width, height) & ~1, 2);
        fit_square.setOriginAndSize((F32)(width / 2 - side / 2), (F32)(height / 2 - side / 2), (F32)side, (F32)side);
        mFitPan.set(0.f, (F32)(height / 2 - getRect().getHeight() / 2));   // (below the tape strip, if any)
        const F32 region_width = region->getWidth();
        const F32 inset = rotating ? (rotate_frame ? GMDXMinimapCompass::getRadarRing() : 0.f) : GMDXMinimapCompass::getMapInset();
        // (the turning whole region: its diagonal fits)
        mFitBasePixelsPerMeter = llmax((F32)side - 2.f * inset, 1.f) / (region_width * (rot_view == 2 ? F_SQRT2 : 1.f));
        mFitMapRect.setOriginAndSize(fit_square.mLeft + inset, fit_square.mBottom + inset,
                                     fit_square.getWidth() - 2.f * inset, fit_square.getHeight() - 2.f * inset);
        // Zoom (the wheel; wheel out to the whole region): a new region or turning the mode on starts whole
        if (!mFitRegion || region->getHandle() != mFitRegionHandle)
        {
            if (!mFitRegion || !radar) mFitZoom = 1.f;   // (the radar keeps its zoom across region crossings)
            mFitRegionHandle = region->getHandle();
            mFitViewCenter.set(region_width * 0.5f, region_width * 0.5f);
        }
        if (radar)
        {
            // centred on the agent; past the region's edges the disc stays dark
            const LLVector3 agent = gAgent.getPositionAgent() - region->getOriginAgent();
            mFitViewCenter.set(agent.mV[VX], agent.mV[VY]);
        }
        else if (rot_view == 2)
        {
            mFitZoom = 1.f;   // the whole region, always (no zoom or pan)
            mFitViewCenter.set(region_width * 0.5f, region_width * 0.5f);
        }
        else
        {
            clampFitView(region_width);
        }
        fit_ppm = mFitBasePixelsPerMeter * mFitZoom;
        const LLVector3d old_center = mFitCenterGlobal;
        mFitCenterGlobal = region->getOriginGlobal() + LLVector3d(mFitViewCenter.mV[VX], mFitViewCenter.mV[VY], 0.0);
        mFitCenterGlobal.mdV[VZ] = gAgentCamera.getCameraPositionGlobal().mdV[VZ];
        // (the radar's centre moves with the agent: the layers keep their timed refresh)
        if (mFitRegion && !radar && (old_center.mdV[VX] != mFitCenterGlobal.mdV[VX] || old_center.mdV[VY] != mFitCenterGlobal.mdV[VY]))
        {
            mUpdateObjectImage = true;   // the layers are centred on the view
            mUpdateParcelImage = true;
        }
    }
    if (whole_region != mFitRegion || fit_ppm != mFitPixelsPerMeter)
    {
        mFitRegion = whole_region;
        mFitPixelsPerMeter = fit_ppm;
        updateScaleDerived();
        mUpdateObjectImage = true;
        mUpdateParcelImage = true;
    }
    mRadarSquare = rot_view == 1 || rot_view == 2;
    mRadarRadius = rot_view == 0 ? mFitMapRect.getWidth() * 0.5f : 0.f;
}

void LLNetMap::drawRadarBackground()
{
    const LLVector2 c = radarCenter();
    const F32 r = mRadarRadius;
    std::vector<LLVector2> disc = { c + LLVector2(-r, -r), c + LLVector2(r, -r), c + LLVector2(r, r), c + LLVector2(-r, r) };
    clip_disc(disc, c, r);
    gGL.begin(LLRender::TRIANGLES);
    for (size_t k = 1; k + 1 < disc.size(); ++k)
    {
        gGL.vertex2f(disc[0].mV[VX], disc[0].mV[VY]);
        gGL.vertex2f(disc[k].mV[VX], disc[k].mV[VY]);
        gGL.vertex2f(disc[k + 1].mV[VX], disc[k + 1].mV[VY]);
    }
    gGL.end();
}

void LLNetMap::drawMapQuad(F32 x0, F32 y0, F32 x1, F32 y1, F32 u0, F32 v0, F32 u1, F32 v1, F32 radar_radius)
{
    gGL.begin(LLRender::TRIANGLES);
    if (radar_radius > 0.f)
    {
        disc_quad(x0, y0, x1, y1, u0, v0, u1, v1, radar_radius);
    }
    else
    {
        gGL.texCoord2f(u0, v1);
        gGL.vertex2f(x0, y1);
        gGL.texCoord2f(u0, v0);
        gGL.vertex2f(x0, y0);
        gGL.texCoord2f(u1, v0);
        gGL.vertex2f(x1, y0);

        gGL.texCoord2f(u0, v1);
        gGL.vertex2f(x0, y1);
        gGL.texCoord2f(u1, v0);
        gGL.vertex2f(x1, y0);
        gGL.texCoord2f(u1, v1);
        gGL.vertex2f(x1, y1);
    }
    gGL.end();
}

// The compass, over the map (gmdxminimapcompass.h)
void LLNetMap::drawWholeRegionCompass(const LLRectf& fit_square)
{
    if (!mFitRegion)
    {
        return;
    }
    LLLocalClipRect clip(getLocalRect());
    const LLVector3& at = LLViewerCamera::getInstance()->getAtAxis();
    F32 heading = atan2(at.mV[VX], at.mV[VY]) * RAD_TO_DEG;   // 0 = north, clockwise
    if (heading < 0.f)
    {
        heading += 360.f;
    }
    const S32 rotating_style = GMDXMinimapCompass::getRotatingStyle();
    if (mRadarSquare)
    {
        GMDXMinimapCompass::drawRadarSquare(rotating_style == GMDXMinimapCompass::STYLE_FRAME ? fit_square : mFitMapRect,
                                            heading, rotating_style);
    }
    else if (mRadarRadius > 0.f)
    {
        const LLVector2 c = radarCenter();
        GMDXMinimapCompass::drawRadar(c.mV[VX], c.mV[VY], mRadarRadius, heading, rotating_style);
    }
    else
    {
        GMDXMinimapCompass::draw(fit_square, heading);
    }
}

// Whole Region zoom: the view (region_width / zoom metres across) stays inside the region;
// at zoom 1 it is the region's middle.
void LLNetMap::clampFitView(F32 region_width)
{
    const F32 half = 0.5f * region_width / llmax(mFitZoom, 1.f);
    mFitViewCenter.mV[VX] = llclamp(mFitViewCenter.mV[VX], half, region_width - half);
    mFitViewCenter.mV[VY] = llclamp(mFitViewCenter.mV[VY], half, region_width - half);
}

// Show Whole Region, zoomed in: a plain drag pans, except in a band along the map's edges, which
// still moves the window.
bool LLNetMap::fitPanAt(S32 x, S32 y) const
{
    constexpr F32 EDGE = 16.f;
    return mFitRegion && !radarOn() && mFitZoom > 1.f && x > mFitMapRect.mLeft + EDGE && x < mFitMapRect.mRight - EDGE
        && y > mFitMapRect.mBottom + EDGE && y < mFitMapRect.mTop - EDGE;
}

// ---- the menu ----

void LLNetMap::addGMDXMenuCallbacks(LLUICtrl::CommitCallbackRegistry::ScopedRegistrar& commitRegistrar,
                                    LLUICtrl::EnableCallbackRegistry::ScopedRegistrar& enableRegistrar)
{
    // a dot's colour (Dot Colour); Show Whole Region and its compass style
    commitRegistrar.add("Minimap.Avatar.Color", [this](LLUICtrl*, const LLSD& param) { setAvatarDotColor(param.asString()); });
    enableRegistrar.add("Minimap.Avatar.HasColor", [this](LLUICtrl*, const LLSD&)
        {
            LLColor4 c;
            return GMDXMinimapMarks::avatarColor(mClosestAgentRightClick, c);
        });
    // (the turning Whole Region map keeps its own: GMDXMiniMapRotateCompass, pills or frame)
    enableRegistrar.add("Minimap.CompassStyle.Check", [](LLUICtrl*, const LLSD& param)
        {
            return (gSavedSettings.getBOOL("MiniMapRotate") ? GMDXMinimapCompass::getRotatingStyle() : GMDXMinimapCompass::getStyle())
                   == param.asInteger();
        });
    commitRegistrar.add("Minimap.CompassStyle.Set", [](LLUICtrl*, const LLSD& param)
        { gSavedSettings.setS32(gSavedSettings.getBOOL("MiniMapRotate") ? "GMDXMiniMapRotateCompass" : "GMDXMiniMapCompassStyle", param.asInteger()); });
    enableRegistrar.add("Minimap.WholeRegion.Visible", [](LLUICtrl*, const LLSD& param)
        { return gSavedSettings.getBOOL("GMDXMiniMapWholeRegion") == (param.asString() == "on"); });
    // Map Opacity (GMDXMiniMapOpacity)
    enableRegistrar.add("Minimap.Opacity.Check", [](LLUICtrl*, const LLSD& param)
        { return fabsf(gSavedSettings.getF32("GMDXMiniMapOpacity") - (F32)param.asReal()) < 0.01f; });
    commitRegistrar.add("Minimap.Opacity.Set", [](LLUICtrl*, const LLSD& param)
        { gSavedSettings.setF32("GMDXMiniMapOpacity", (F32)param.asReal()); });
    // Region Map: save the map image; export / import a map file; delete this region's map, open the folder
    commitRegistrar.add("Minimap.SaveImage", [](LLUICtrl*, const LLSD&) { gmdx_save_region_map_image(); });
    enableRegistrar.add("Minimap.RegionMap.HasCapture", [](LLUICtrl*, const LLSD&) { return GMDXRegionCapture::hasCapture(gAgent.getRegion()); });
    commitRegistrar.add("Minimap.RegionMap.Delete", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::deleteCapture(gAgent.getRegion()); });
    commitRegistrar.add("Minimap.RegionMap.OpenFolder", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::openMapsFolder(); });
    commitRegistrar.add("Minimap.RegionMap.Export", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::exportMap(gAgent.getRegion()); });
    commitRegistrar.add("Minimap.RegionMap.Import", [](LLUICtrl*, const LLSD&) { GMDXRegionCapture::importMap(); });
    // Where the menu was opened: copy it, mark it; the mark under it: track, rename, recolour, remove
    commitRegistrar.add("Minimap.CopyPosition", [this](LLUICtrl*, const LLSD& param) { copyPopupPosition(param.asString() == "slurl"); });
    enableRegistrar.add("Minimap.CanShowLocation", [](LLUICtrl*, const LLSD&) { return RlvActions::canShowLocation(); });
    commitRegistrar.add("Minimap.Mark.Add", [this](LLUICtrl*, const LLSD& param) { addMarkAtPopup(param.asInteger()); });
    commitRegistrar.add("Minimap.Mark.Track", [this](LLUICtrl*, const LLSD&)
        {
            if (const GMDXMapMark* m = GMDXMinimapMarks::find(mPopupMark))
                LLTracker::trackLocation(m->pos_global, m->name, RlvActions::canShowLocation() ? m->region : std::string());
        });
    commitRegistrar.add("Minimap.Mark.Rename", [this](LLUICtrl*, const LLSD&)
        {
            const GMDXMapMark* m = GMDXMinimapMarks::find(mPopupMark);
            if (!m) return;
            LLSD args;
            args["NAME"] = m->name;
            const LLUUID id = m->id;
            LLNotificationsUtil::add("GMDXMinimapMarkRename", args, LLSD(), [id](const LLSD& n, const LLSD& r)
                {
                    if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
                    std::string name = r["name"].asString();
                    LLStringUtil::trim(name);
                    if (!name.empty()) GMDXMinimapMarks::rename(id, name);
                });
        });
    commitRegistrar.add("Minimap.Mark.Color", [this](LLUICtrl*, const LLSD& param) { GMDXMinimapMarks::recolor(mPopupMark, param.asInteger()); });
    commitRegistrar.add("Minimap.Mark.Remove", [this](LLUICtrl*, const LLSD&) { GMDXMinimapMarks::remove(mPopupMark); });
    // Zones: draw one; the zone under the menu: line, fill, colour, rename, remove
    commitRegistrar.add("Minimap.Zone.Draw", [this](LLUICtrl*, const LLSD& param) { startZone(param.asInteger()); });
    commitRegistrar.add("Minimap.Zone.Line", [this](LLUICtrl*, const LLSD& param)
        {
            const bool dotted = param.asString() == "dotted";
            GMDXMinimapMarks::setZoneDotted(mPopupZone, dotted);
            gSavedSettings.setBOOL("GMDXMiniMapZoneDotted", dotted);   // (the next zone's too)
        });
    commitRegistrar.add("Minimap.Zone.Color", [this](LLUICtrl*, const LLSD& param)
        {
            GMDXMinimapMarks::recolorZone(mPopupZone, param.asInteger());
            gSavedSettings.setS32("GMDXMiniMapZoneColor", param.asInteger());
        });
    commitRegistrar.add("Minimap.Zone.Fill", [this](LLUICtrl*, const LLSD& param)
        {
            GMDXMinimapMarks::setZoneFill(mPopupZone, param.asInteger());
            gSavedSettings.setS32("GMDXMiniMapZoneFill", param.asInteger());   // (the next zone's too)
        });
    commitRegistrar.add("Minimap.Zone.Rename", [this](LLUICtrl*, const LLSD&)
        {
            const GMDXMapZone* z = GMDXMinimapMarks::findZone(mPopupZone);
            if (!z) return;
            LLSD args;
            args["NAME"] = z->name;
            const LLUUID id = z->id;
            LLNotificationsUtil::add("GMDXMinimapZoneName", args, LLSD(), [id](const LLSD& n, const LLSD& r)
                {
                    if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
                    std::string name = r["name"].asString();
                    LLStringUtil::trim(name);
                    GMDXMinimapMarks::renameZone(id, name);
                });
        });
    commitRegistrar.add("Minimap.Zone.Remove", [this](LLUICtrl*, const LLSD&) { GMDXMinimapMarks::removeZone(mPopupZone); });
}

// ---- zones, marks, the cursor's coordinates, physical objects, dot colours ----

std::vector<LLVector2> LLNetMap::zoneOutline(S32 kind, const std::vector<LLVector3d>& points)
{
    std::vector<LLVector2> out;
    auto view = [this](const LLVector3d& g) { const LLVector3 v = globalPosToView(g); return LLVector2(v.mV[VX], v.mV[VY]); };
    if (kind == GMDXMapZone::RECT && points.size() >= 2)
    {
        const LLVector3d& a = points[0];
        const LLVector3d& b = points[1];
        out = { view(a), view(LLVector3d(b.mdV[VX], a.mdV[VY], 0.0)), view(b), view(LLVector3d(a.mdV[VX], b.mdV[VY], 0.0)) };
    }
    else if (kind == GMDXMapZone::CIRCLE && points.size() >= 2)
    {
        const LLVector3d& c = points[0];
        const F64 r = sqrt((points[1].mdV[VX] - c.mdV[VX]) * (points[1].mdV[VX] - c.mdV[VX])
                           + (points[1].mdV[VY] - c.mdV[VY]) * (points[1].mdV[VY] - c.mdV[VY]));
        constexpr int STEPS = 64;
        for (int i = 0; i < STEPS; ++i)
        {
            const F64 a = F_TWO_PI * i / STEPS;
            out.push_back(view(LLVector3d(c.mdV[VX] + r * cos(a), c.mdV[VY] + r * sin(a), 0.0)));
        }
    }
    else
    {
        for (const LLVector3d& p : points) out.push_back(view(p));
    }
    return out;
}

void LLNetMap::drawZones()
{
    sDiscCenter = radarCenter();   // (cut to the radar's disc)
    sDiscRadius = mRadarRadius;
    static LLCachedControl<bool> show_zones(gSavedSettings, "GMDXMiniMapShowZones", true);
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    if (show_zones)
    {
        for (const GMDXMapZone& z : GMDXMinimapMarks::zones())
        {
            const std::vector<LLVector2> pts = zoneOutline(z.kind, z.points);
            const LLColor4 c = GMDXMinimapMarks::color(z.color);
            if (z.fill != GMDXMapZone::FILL_NONE && !pts.empty()) zone_fill(pts, z.fill, c, pts[0]);
            zone_shape(pts, true, c, z.dotted);
            if (!z.name.empty() && !pts.empty())
            {
                const LLVector2* top = &pts[0];
                for (const LLVector2& p : pts) if (p.mV[VY] > top->mV[VY]) top = &p;
                if (mRadarRadius > 0.f && (*top - radarCenter()).length() > mRadarRadius) continue;   // (off the radar)
                font->renderUTF8(z.name, 0, top->mV[VX], top->mV[VY] + 3.f, c, LLFontGL::HCENTER, LLFontGL::BOTTOM,
                                 LLFontGL::NORMAL, LLFontGL::DROP_SHADOW, 32);
            }
        }
    }
}

void LLNetMap::drawZonePreview()
{
    sDiscCenter = radarCenter();
    sDiscRadius = mRadarRadius;
    static LLCachedControl<bool> new_dotted(gSavedSettings, "GMDXMiniMapZoneDotted", false);
    static LLCachedControl<S32> new_color(gSavedSettings, "GMDXMiniMapZoneColor", 4);
    static LLCachedControl<S32> new_fill(gSavedSettings, "GMDXMiniMapZoneFill", 0);
    if (mDrawKind >= 0)
    {
        std::vector<LLVector3d> pts = mDrawPoints;
        if (!pts.empty() && (mDrawKind == GMDXMapZone::POLYGON || mDrawDrag)) pts.push_back(mDrawCursor);
        const LLColor4 c = GMDXMinimapMarks::color(new_color);
        if (mDrawKind == GMDXMapZone::POLYGON || pts.size() >= 2)
        {
            const std::vector<LLVector2> outline = zoneOutline(mDrawKind, pts);
            if (new_fill != GMDXMapZone::FILL_NONE && outline.size() >= 3) zone_fill(outline, new_fill, c, outline[0]);
            zone_shape(outline, mDrawKind != GMDXMapZone::POLYGON, c, new_dotted);
        }
        static const char* const hints[3] = { "Drag to draw a rectangle. Right-click to cancel.",
                                              "Drag from the centre to draw a circle. Right-click to cancel.",
                                              "Click the corners; double-click, click the first corner or right-click to finish." };
        const LLFontGL* hint_font = LLFontGL::getFontSansSerifSmall();
        const F32 cx = mFitRegion ? mFitMapRect.getCenterX() : getRect().getWidth() * 0.5f;
        const F32 ty = (mFitRegion ? mFitMapRect.mTop : (F32)getRect().getHeight()) - 6.f;
        hint_font->renderUTF8(hints[llclamp(mDrawKind, 0, 2)], 0, cx, ty, LLColor4(0.93f, 0.95f, 0.96f, 1.f),
                              LLFontGL::HCENTER, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
    sDiscRadius = 0.f;
}

LLUUID LLNetMap::zoneAt(S32 x, S32 y)
{
    static LLCachedControl<bool> show_zones(gSavedSettings, "GMDXMiniMapShowZones", true);
    if (!show_zones) return LLUUID::null;
    const LLVector2 q((F32)x, (F32)y);
    if (radarOn() && !onMap(LLVector3(q.mV[VX], q.mV[VY], 0.f))) return LLUUID::null;   // (off the radar)
    LLUUID best;
    F32 best_d = 6.f;   // px from the outline
    for (const GMDXMapZone& z : GMDXMinimapMarks::zones())
    {
        const std::vector<LLVector2> pts = zoneOutline(z.kind, z.points);
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const LLVector2 a = pts[i], b = pts[(i + 1) % pts.size()];
            const LLVector2 ab = b - a;
            const F32 len2 = ab.lengthSquared();
            const F32 t = len2 > 0.f ? llclamp(((q - a) * ab) / len2, 0.f, 1.f) : 0.f;
            const F32 d = (a + ab * t - q).length();
            if (d < best_d) { best_d = d; best = z.id; }
        }
    }
    return best;
}

void LLNetMap::startZone(S32 kind)
{
    mDrawKind = llclamp(kind, 0, 2);
    mDrawPoints.clear();
    mDrawDrag = false;
}

void LLNetMap::finishZone()
{
    GMDXMapZone z;
    z.kind = (GMDXMapZone::Kind)mDrawKind;
    z.points = mDrawPoints;
    z.color = gSavedSettings.getS32("GMDXMiniMapZoneColor");
    z.dotted = gSavedSettings.getBOOL("GMDXMiniMapZoneDotted");
    z.fill = gSavedSettings.getS32("GMDXMiniMapZoneFill");
    mDrawKind = -1;
    mDrawPoints.clear();
    if (z.points.size() < (z.kind == GMDXMapZone::POLYGON ? 3u : 2u)) return;
    LLSD args;
    args["NAME"] = llformat("Zone %d", (S32)GMDXMinimapMarks::zones().size() + 1);
    LLNotificationsUtil::add("GMDXMinimapZoneName", args, LLSD(), [z](const LLSD& n, const LLSD& r) mutable
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
            z.name = r["name"].asString();
            LLStringUtil::trim(z.name);
            GMDXMinimapMarks::addZone(z);
        });
}

// Mini-map marks: a diamond in the mark's colour with a dark outline, its name beside it.
bool LLNetMap::onMap(const LLVector3& p) const
{
    if (mRadarRadius > 0.f)
    {
        return (LLVector2(p.mV[VX], p.mV[VY]) - radarCenter()).length() <= mRadarRadius;
    }
    if (mFitRegion)
    {
        return p.mV[VX] >= mFitMapRect.mLeft && p.mV[VX] <= mFitMapRect.mRight
            && p.mV[VY] >= mFitMapRect.mBottom && p.mV[VY] <= mFitMapRect.mTop;
    }
    return p.mV[VX] >= 0.f && p.mV[VY] >= 0.f && p.mV[VX] <= (F32)getRect().getWidth() && p.mV[VY] <= (F32)getRect().getHeight();
}

void LLNetMap::drawMarks()
{
    static LLCachedControl<bool> show_marks(gSavedSettings, "GMDXMiniMapShowMarks", true);
    if (!show_marks || GMDXMinimapMarks::all().empty()) return;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    const LLColor4 outline(0.05f, 0.06f, 0.07f, 0.9f);
    for (const GMDXMapMark& m : GMDXMinimapMarks::all())
    {
        const LLVector3 p = globalPosToView(m.pos_global);
        if (!onMap(p)) continue;
        const F32 x = (F32)ll_round(p.mV[VX]), y = (F32)ll_round(p.mV[VY]);
        const LLColor4 c = GMDXMinimapMarks::color(m.color);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        for (int pass = 0; pass < 2; ++pass)   // outline, then the colour
        {
            const F32 r = pass ? 4.5f : 6.f;
            gGL.color4fv((pass ? c : outline).mV);
            gGL.begin(LLRender::TRIANGLES);
            gGL.vertex2f(x, y + r); gGL.vertex2f(x - r, y); gGL.vertex2f(x + r, y);
            gGL.vertex2f(x - r, y); gGL.vertex2f(x, y - r); gGL.vertex2f(x + r, y);
            gGL.end();
        }
        if (!m.name.empty())
        {
            font->renderUTF8(m.name, 0, x + 8.f, y, c, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                             LLFontGL::DROP_SHADOW, 24);
        }
    }
}

LLUUID LLNetMap::markAt(S32 x, S32 y)
{
    static LLCachedControl<bool> show_marks(gSavedSettings, "GMDXMiniMapShowMarks", true);
    if (!show_marks) return LLUUID::null;
    LLUUID best;
    F32 best_d2 = 8.f * 8.f;
    for (const GMDXMapMark& m : GMDXMinimapMarks::all())
    {
        const LLVector3 p = globalPosToView(m.pos_global);
        if (!onMap(p)) continue;
        const F32 dx = p.mV[VX] - (F32)x, dy = p.mV[VY] - (F32)y;
        if (dx * dx + dy * dy < best_d2) { best_d2 = dx * dx + dy * dy; best = m.id; }
    }
    return best;
}

// The region coordinates under the cursor (GMDXMiniMapCursorCoords), on a dark pill beside it;
// over a mark, its name first.
void LLNetMap::drawCursorCoordinates()
{
    static LLCachedControl<bool> show_coords(gSavedSettings, "GMDXMiniMapCursorCoords", true);
    if (!show_coords || !mCursorInside || mPanning || hasMouseCapture() || isMouseOnPopupMenu()) return;
    const LLVector3 at((F32)mCursorX, (F32)mCursorY, 0.f);
    if (!onMap(at)) return;
    const LLVector3d pos = GMDXMinimapMarks::onGround(viewPosToGlobal(mCursorX, mCursorY));
    std::string text = GMDXMinimapMarks::coordinatesText(pos);
    if (const GMDXMapMark* m = GMDXMinimapMarks::find(markAt(mCursorX, mCursorY)))
    {
        text = m->name + "  " + text;
    }
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 w = font->getWidth(text) + 10, h = (S32)font->getLineHeight() + 4;
    S32 left = mCursorX + 14, top = mCursorY - 10;
    left = llclamp(left, 2, llmax(getRect().getWidth() - w - 2, 2));
    if (left < mCursorX + 14) left = llmax(mCursorX - w - 8, 2);   // no room right of the cursor: left of it
    top = llclamp(top, h + 2, getRect().getHeight() - 2);
    gl_rect_2d(left, top, left + w, top - h, LLColor4(0.05f, 0.06f, 0.07f, 0.8f));
    font->renderUTF8(text, 0, (F32)(left + 5), (F32)(top - h / 2), LLColor4(0.93f, 0.95f, 0.96f, 1.f),
                     LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}

void LLNetMap::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mCursorInside = false;
    LLUICtrl::onMouseLeave(x, y, mask);
}

void LLNetMap::copyPopupPosition(bool slurl)
{
    const GMDXMapMark* m = GMDXMinimapMarks::find(mPopupMark);
    const LLVector3d pos = m ? m->pos_global : GMDXMinimapMarks::onGround(mPopupWorldPos);
    const std::string text = slurl ? GMDXMinimapMarks::slurl(pos) : GMDXMinimapMarks::coordinatesText(pos);
    if (text.empty()) return;
    const LLWString w = utf8str_to_wstring(text);
    LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
}

void LLNetMap::addMarkAtPopup(S32 color)
{
    const LLVector3d pos = GMDXMinimapMarks::onGround(mPopupWorldPos);
    LLSD args;
    args["NAME"] = llformat("Mark %d", (S32)GMDXMinimapMarks::all().size() + 1);
    LLNotificationsUtil::add("GMDXMinimapMarkName", args, LLSD(), [pos, color](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
            std::string name = r["name"].asString();
            LLStringUtil::trim(name);
            GMDXMinimapMarks::add(pos, name.empty() ? std::string("Mark") : name, color);
        });
}

// Only Render Physical Objects (GMDXMiniMapPhysicalOnly): every physical object
// (FLAGS_USE_PHYSICS: bullets, vehicles and other dynamic objects) as a dot with a short trail
// back along its velocity, from the live objects every frame. The static object layer holds
// only owned or larger objects (renderObjectsForMap), so bullets never reach it. Physical
// objects at rest leave the active list and are not shown. Drawn in the map's frame (centred
// on the map centre, rotated with it); only_region = Show Whole Region's region, or null.
void LLNetMap::drawPhysicalObjects(const LLVector3& map_center_agent, F32 pixels_per_meter, const LLViewerRegion* only_region)
{
    static LLUIColor physical_color = LLUIColorTable::instance().getColor("GMDXMapPhysicalColor", LLColor4(1.f, 0.6f, 0.1f, 1.f));
    constexpr F32 TRAIL_SECONDS = 0.15f;    // the trail: where the object was this long ago
    constexpr F32 MIN_HALF_SIZE = 1.5f;     // a 3 px dot at least
    constexpr F32 MAX_RADIUS_M = 8.f;       // big physical objects (vehicles) stay a marker

    const LLColor4 color = physical_color.get();
    const LLColor4 tail(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 0.f);
    const F32 view_radius = mRadarRadius > 0.f ? mRadarRadius : 0.5f * mObjectMapPixels + 2.f;    // the radar's disc, else half the view's diagonal
    const F32 view_radius_sq = view_radius * view_radius;
    auto vertex = [](F32 x, F32 y, const LLColor4& c)
    {
        gGL.color4fv(c.mV);
        gGL.vertex2f(x, y);
    };

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    for (const LLPointer<LLViewerObject>& objectp : gObjectList.getActiveObjects())
    {
        // Roots only (a linkset is one marker); a root with a parent is an attachment.
        if (objectp.isNull() || objectp->isDead() || !objectp->flagUsePhysics() || objectp->isAvatar()
            || objectp->isAttachment() || objectp->getParent())
        {
            continue;
        }
        if (only_region && objectp->getRegion() != only_region)
        {
            continue;
        }
        const LLVector3 rel = objectp->getPositionAgent() - map_center_agent;
        const F32 x = rel.mV[VX] * pixels_per_meter;
        const F32 y = rel.mV[VY] * pixels_per_meter;
        if (x * x + y * y > view_radius_sq)
        {
            continue;
        }

        const LLVector3& scale = objectp->getScale();
        const F32 half = llmax(MIN_HALF_SIZE, llmin(0.25f * (scale.mV[VX] + scale.mV[VY]), MAX_RADIUS_M) * pixels_per_meter);

        // The trail, 1.5 px wide, fading out towards where it came from.
        const LLVector3& vel = objectp->getVelocity();
        const F32 tx = -vel.mV[VX] * TRAIL_SECONDS * pixels_per_meter;
        const F32 ty = -vel.mV[VY] * TRAIL_SECONDS * pixels_per_meter;
        const F32 len = sqrtf(tx * tx + ty * ty);
        if (len > half)
        {
            const F32 nx = -ty / len * 0.75f;
            const F32 ny = tx / len * 0.75f;
            vertex(x + nx, y + ny, color); vertex(x - nx, y - ny, color); vertex(x + tx - nx, y + ty - ny, tail);
            vertex(x + nx, y + ny, color); vertex(x + tx - nx, y + ty - ny, tail); vertex(x + tx + nx, y + ty + ny, tail);
        }

        vertex(x - half, y - half, color); vertex(x + half, y - half, color); vertex(x + half, y + half, color);
        vertex(x - half, y - half, color); vertex(x + half, y + half, color); vertex(x - half, y + half, color);
    }
    gGL.end();
}

// Right-click a dot > Dot Colour: this account's own colour for it (gmdxminimapmarks.h)
void LLNetMap::destroyDotColorSwatch()
{
    // The swatch cancels its open picker when it is destroyed, which would call back into this
    // map after its members are gone: the picker is detached and closed first, without a callback.
    if (mDotColorSwatch)
    {
        mDotColorSwatch->closeFloaterColorPicker();
        mDotColorSwatch = nullptr;
    }
}

void LLNetMap::setAvatarDotColor(const std::string& which)
{
    const LLUUID id = mClosestAgentRightClick;
    if (id.isNull()) return;
    if (which == "clear")
    {
        GMDXMinimapMarks::clearAvatarColor(id);
        return;
    }
    if (which != "custom")
    {
        GMDXMinimapMarks::setAvatarColor(id, GMDXMinimapMarks::color(llclamp(atoi(which.c_str()), 0, GMDXMinimapMarks::COLORS - 1)));
        return;
    }
    if (!mDotColorSwatch)
    {
        LLColorSwatchCtrl::Params p(LLUICtrlFactory::getDefaultParams<LLColorSwatchCtrl>());
        p.name = "dot_color_swatch";
        p.rect = LLRect(0, 1, 1, 0);
        p.visible = false;
        p.can_apply_immediately = true;   // the dot follows the picker live
        mDotColorSwatch = LLUICtrlFactory::create<LLColorSwatchCtrl>(p);
        addChild(mDotColorSwatch);
        mDotColorSwatch->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&)
            { GMDXMinimapMarks::setAvatarColor(mDotColorAvatar, LLColor4(ctrl->getValue())); });
        // Cancel: the dot goes back to what it was (no colour of its own, if it had none)
        mDotColorSwatch->setOnCancelCallback([this](LLUICtrl*, const LLSD&)
            {
                if (mDotColorHad) GMDXMinimapMarks::setAvatarColor(mDotColorAvatar, mDotColorBefore);
                else GMDXMinimapMarks::clearAvatarColor(mDotColorAvatar);
            });
    }
    mDotColorAvatar = id;
    mDotColorHad = GMDXMinimapMarks::avatarColor(id, mDotColorBefore);
    const LLColor4 current = getAvatarColor(id);   // (the picker starts from the dot as it is drawn)
    mDotColorSwatch->set(current, true);
    mDotColorSwatch->setOriginal(current);
    mDotColorSwatch->showPicker(true);
}

// The menu items for where it was opened (a dot, a mark, a zone, or the map) and for the view
// (Show Whole Region hides Zoom and the re-centre items, and has its own rotation and compass
// items). Branches have no on_visible in XUI, so they are set here.
void LLNetMap::updateGMDXPopupItems(LLMenuGL* menu, S32 x, S32 y)
{
    const bool on_avatar = mClosestAgentRightClick.notNull() && mClosestAgentRightClick != gAgent.getID();
    menu->setItemVisible("Avatar Dot Colour", on_avatar);
    mPopupMark = on_avatar ? LLUUID::null : markAt(x, y);
    mPopupZone = (on_avatar || mPopupMark.notNull()) ? LLUUID::null : zoneAt(x, y);
    const bool on_mark = mPopupMark.notNull(), on_zone = mPopupZone.notNull();
    for (const char* item : { "Track Mark", "Rename Mark", "Mark Colour", "Remove Mark" }) menu->setItemVisible(item, on_mark);
    for (const char* item : { "Zone Line", "Zone Fill", "Zone Colour", "Rename Zone", "Remove Zone" }) menu->setItemVisible(item, on_zone);
    menu->setItemVisible("Add Mark Here", !on_mark && !on_zone);
    menu->setItemVisible("Draw Zone", !on_mark && !on_zone);

    const bool whole_region = gSavedSettings.getBOOL("GMDXMiniMapWholeRegion");
    menu->setItemVisible("Minimap Zoom", !whole_region);
    menu->setItemVisible("Compass Style", whole_region);
    if (LLMenuGL* compass = menu->findChildMenuByName("Compass Style", true))
    {
        compass->setItemVisible("Compass Heading Tape", !gSavedSettings.getBOOL("MiniMapRotate"));   // (north-up only)
    }
}
