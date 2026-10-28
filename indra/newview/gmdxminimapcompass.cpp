/**
 * @file gmdxminimapcompass.cpp
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

// <GMDX> whole file: see gmdxminimapcompass.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxminimapcompass.h"

#include "llfontgl.h"
#include "llmath.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llviewercontrol.h"
#include "v4color.h"

namespace
{
    const LLColor4 ACCENT(0.95f, 0.36f, 0.22f, 1.f);     // north
    const LLColor4 LABEL(0.86f, 0.87f, 0.89f, 1.f);      // the other directions, the heading
    const LLColor4 PANEL(0.06f, 0.07f, 0.09f, 0.72f);    // pill fill
    const LLColor4 TAPE(0.06f, 0.07f, 0.09f, 0.75f);     // heading tape strip
    const LLColor4 TICK(0.85f, 0.87f, 0.90f, 0.60f);

    constexpr F32 FRAME_BAND = 18.f;        // gunmetal frame width
    constexpr F32 PILL_PAD = 4.f;           // pills: gap to the map edge
    constexpr F32 TAPE_HEIGHT = 20.f;
    constexpr F32 TAPE_PX_PER_DEG = 3.2f;

    // The eight directions clockwise from north: index * 45 = bearing.
    const LLWString& label(S32 i)
    {
        static const LLWString labels[8] = {
            utf8str_to_wstring("N"), utf8str_to_wstring("NE"), utf8str_to_wstring("E"), utf8str_to_wstring("SE"),
            utf8str_to_wstring("S"), utf8str_to_wstring("SW"), utf8str_to_wstring("W"), utf8str_to_wstring("NW") };
        return labels[i & 7];
    }

    const LLFontGL* labelFont(S32 i)
    {
        // cardinals larger than the intercardinals
        return (i & 1) ? LLFontGL::getFontSansSerifSmallBold() : LLFontGL::getFontSansSerifBold();
    }

    const LLColor4& labelColor(S32 i)
    {
        return i == 0 ? ACCENT : LABEL;
    }

    void vertex(F32 x, F32 y, const LLColor4& c)
    {
        gGL.color4fv(c.mV);
        gGL.vertex2f(x, y);
    }

    // Inside gGL.begin(TRIANGLES).
    void quad(F32 x0, F32 y0, F32 x1, F32 y1, const LLColor4& c)
    {
        vertex(x0, y0, c); vertex(x1, y0, c); vertex(x1, y1, c);
        vertex(x0, y0, c); vertex(x1, y1, c); vertex(x0, y1, c);
    }

    // A 1 px line just inside each edge of r. Inside gGL.begin(TRIANGLES).
    void outline(const LLRectf& r, const LLColor4& c)
    {
        quad(r.mLeft, r.mBottom, r.mRight, r.mBottom + 1.f, c);
        quad(r.mLeft, r.mTop - 1.f, r.mRight, r.mTop, c);
        quad(r.mLeft, r.mBottom + 1.f, r.mLeft + 1.f, r.mTop - 1.f, c);
        quad(r.mRight - 1.f, r.mBottom + 1.f, r.mRight, r.mTop - 1.f, c);
    }

    void text(const LLFontGL* font, const LLWString& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::VAlign valign, LLFontGL::ShadowType shadow)
    {
        font->render(s, 0, (F32)ll_round(x), (F32)ll_round(y), c, LLFontGL::HCENTER, valign, LLFontGL::NORMAL, shadow);
    }

    // ---- 0: gunmetal frame -------------------------------------------------------------

    // The brushed-metal band around the map and its edge lines (gGL.begin / end of its own).
    void frameBand(const LLRectf& sq)
    {
        const F32 b = FRAME_BAND;
        const LLRectf map(sq.mLeft + b, sq.mTop - b, sq.mRight - b, sq.mBottom + b);

        // Brushed metal: light at the top-left and bottom-right corners, dark across the
        // diagonal between them.
        const F32 span = llmax(sq.getWidth() + sq.getHeight(), 1.f);
        auto metal = [&](F32 x, F32 y)
        {
            const F32 t = ((x - sq.mLeft) + (sq.mTop - y)) / span;
            const F32 g = 0.21f + 0.21f * (0.5f + 0.5f * cosf(t * F_TWO_PI));
            return LLColor4(g, g + 0.005f, g + 0.02f, 1.f);
        };

        // The band as four trapezoids (outer edge to map edge), cut into segments so the
        // per-vertex colours follow the gradient. Corners counter-clockwise from bottom-left.
        const F32 ox[4] = { sq.mLeft, sq.mRight, sq.mRight, sq.mLeft };
        const F32 oy[4] = { sq.mBottom, sq.mBottom, sq.mTop, sq.mTop };
        const F32 ix[4] = { map.mLeft, map.mRight, map.mRight, map.mLeft };
        const F32 iy[4] = { map.mBottom, map.mBottom, map.mTop, map.mTop };
        constexpr S32 SEGMENTS = 12;

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 e = 0; e < 4; ++e)
        {
            const S32 n = (e + 1) & 3;
            for (S32 s = 0; s < SEGMENTS; ++s)
            {
                const F32 t0 = (F32)s / SEGMENTS;
                const F32 t1 = (F32)(s + 1) / SEGMENTS;
                const F32 ax = lerp(ox[e], ox[n], t0), ay = lerp(oy[e], oy[n], t0);
                const F32 bx = lerp(ox[e], ox[n], t1), by = lerp(oy[e], oy[n], t1);
                const F32 cx = lerp(ix[e], ix[n], t1), cy = lerp(iy[e], iy[n], t1);
                const F32 dx = lerp(ix[e], ix[n], t0), dy = lerp(iy[e], iy[n], t0);
                vertex(ax, ay, metal(ax, ay)); vertex(bx, by, metal(bx, by)); vertex(cx, cy, metal(cx, cy));
                vertex(ax, ay, metal(ax, ay)); vertex(cx, cy, metal(cx, cy)); vertex(dx, dy, metal(dx, dy));
            }
        }

        // A dark 1 px line along the map, a darker one around the outside.
        outline(LLRectf(map.mLeft - 1.f, map.mTop + 1.f, map.mRight + 1.f, map.mBottom - 1.f), LLColor4(0.05f, 0.05f, 0.06f, 0.95f));
        outline(sq, LLColor4(0.08f, 0.08f, 0.09f, 1.f));
        gGL.end();
    }

    void drawFrame(const LLRectf& sq)
    {
        const F32 b = FRAME_BAND;
        const LLRectf map(sq.mLeft + b, sq.mTop - b, sq.mRight - b, sq.mBottom + b);
        frameBand(sq);
        gGL.begin(LLRender::TRIANGLES);

        // Ticks every 1/16 of the side, out from the map edge; the quarter marks are left to
        // the letters.
        constexpr F32 TICK_LEN = 4.f;
        for (S32 i = 1; i < 16; ++i)
        {
            if (i % 4 == 0)
            {
                continue;
            }
            const F32 x = floorf(lerp(map.mLeft, map.mRight, i / 16.f));
            const F32 y = floorf(lerp(map.mBottom, map.mTop, i / 16.f));
            quad(x, map.mTop + 1.f, x + 1.f, map.mTop + 1.f + TICK_LEN, TICK);
            quad(x, map.mBottom - 1.f - TICK_LEN, x + 1.f, map.mBottom - 1.f, TICK);
            quad(map.mLeft - 1.f - TICK_LEN, y, map.mLeft - 1.f, y + 1.f, TICK);
            quad(map.mRight + 1.f, y, map.mRight + 1.f + TICK_LEN, y + 1.f, TICK);
        }
        gGL.end();

        // The letters in the band: cardinals at the edge centres, the others in the corners.
        const F32 h = b * 0.5f;
        const F32 cx = (sq.mLeft + sq.mRight) * 0.5f;
        const F32 cy = (sq.mBottom + sq.mTop) * 0.5f;
        const F32 px[8] = { cx, sq.mRight - h, sq.mRight - h, sq.mRight - h, cx, sq.mLeft + h, sq.mLeft + h, sq.mLeft + h };
        const F32 py[8] = { sq.mTop - h, sq.mTop - h, cy, sq.mBottom + h, sq.mBottom + h, sq.mBottom + h, cy, sq.mTop - h };
        for (S32 i = 0; i < 8; ++i)
        {
            text(labelFont(i), label(i), px[i], py[i], labelColor(i), LLFontGL::VCENTER, LLFontGL::DROP_SHADOW);
        }
    }

    // ---- 1: edge pills -----------------------------------------------------------------

    constexpr S32 ARC = 10;                 // segments per round end
    constexpr S32 PILL_POINTS = 2 * (ARC + 1);

    struct HalfCircle
    {
        F32 c[ARC + 1];
        F32 s[ARC + 1];
        HalfCircle()
        {
            for (S32 i = 0; i <= ARC; ++i)
            {
                const F32 a = -F_PI_BY_TWO + F_PI * (F32)i / ARC;
                c[i] = cosf(a);
                s[i] = sinf(a);
            }
        }
    };

    // Point k of a pill's outline, counter-clockwise: the right end bottom to top, then the
    // left end top to bottom. half_len = half the straight part.
    void pillPoint(S32 k, F32 x, F32 y, F32 half_len, F32 r, F32& px, F32& py)
    {
        static const HalfCircle arc;
        if (k <= ARC)
        {
            px = x + half_len + r * arc.c[k];
            py = y + r * arc.s[k];
        }
        else
        {
            const S32 j = k - (ARC + 1);
            px = x - half_len - r * arc.c[j];
            py = y - r * arc.s[j];
        }
    }

    // A pill with fully round ends centred at (x, y): the fill, and a 1 px outline around it.
    // Inside gGL.begin(TRIANGLES).
    void pill(F32 x, F32 y, F32 half_len, F32 r, const LLColor4& fill, const LLColor4& line)
    {
        for (S32 k = 0; k < PILL_POINTS; ++k)
        {
            const S32 n = (k + 1) % PILL_POINTS;
            F32 ax, ay, bx, by, cx, cy, dx, dy;
            pillPoint(k, x, y, half_len, r - 1.f, ax, ay);
            pillPoint(n, x, y, half_len, r - 1.f, bx, by);
            pillPoint(k, x, y, half_len, r, cx, cy);
            pillPoint(n, x, y, half_len, r, dx, dy);
            vertex(x, y, fill); vertex(ax, ay, fill); vertex(bx, by, fill);
            vertex(ax, ay, line); vertex(cx, cy, line); vertex(dx, dy, line);
            vertex(ax, ay, line); vertex(dx, dy, line); vertex(bx, by, line);
        }
    }

    void drawPills(const LLRectf& map)
    {
        const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
        const F32 r = floorf((font->getLineHeight() + 4.f) * 0.5f);
        const F32 cx = floorf((map.mLeft + map.mRight) * 0.5f);
        const F32 cy = floorf((map.mBottom + map.mTop) * 0.5f);
        const F32 top = map.mTop - PILL_PAD - r;
        const F32 bottom = map.mBottom + PILL_PAD + r;

        F32 px[8];
        F32 py[8];
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        outline(map, LLColor4(1.f, 1.f, 1.f, 0.22f));
        for (S32 i = 0; i < 8; ++i)
        {
            // "N" is a circle; the two-letter labels stretch.
            const F32 half_len = llmax(0.f, floorf(font->getWidthF32(label(i).c_str()) * 0.5f) + 3.f - r);
            const F32 half_w = half_len + r;
            const F32 right = map.mRight - PILL_PAD - half_w;
            const F32 left = map.mLeft + PILL_PAD + half_w;
            const F32 xs[8] = { cx, right, right, right, cx, left, left, left };
            const F32 ys[8] = { top, top, cy, bottom, bottom, bottom, cy, top };
            px[i] = xs[i];
            py[i] = ys[i];
            pill(px[i], py[i], half_len, r, PANEL, i == 0 ? ACCENT : LLColor4(1.f, 1.f, 1.f, 0.5f));
        }
        gGL.end();

        for (S32 i = 0; i < 8; ++i)
        {
            text(font, label(i), px[i], py[i], labelColor(i), LLFontGL::VCENTER, LLFontGL::NO_SHADOW);
        }
    }

    // ---- 2: heading tape ---------------------------------------------------------------

    // The eight direction pills turned so heading_deg points up (the rotating map). place(ux, uy,
    // half_w, r, x, y) puts one: (ux, uy) its direction on screen, half_w / r its half width / height.
    template <class Place>
    void turningPills(F32 heading_deg, Place place)
    {
        const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
        const F32 r = floorf((font->getLineHeight() + 4.f) * 0.5f);
        F32 px[8];
        F32 py[8];
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < 8; ++i)
        {
            const F32 half_len = llmax(0.f, floorf(font->getWidthF32(label(i).c_str()) * 0.5f) + 3.f - r);
            const F32 a = (i * 45.f - heading_deg) * DEG_TO_RAD;
            place(sinf(a), cosf(a), half_len + r, r, px[i], py[i]);
            px[i] = floorf(px[i]);
            py[i] = floorf(py[i]);
            pill(px[i], py[i], half_len, r, PANEL, i == 0 ? ACCENT : LLColor4(1.f, 1.f, 1.f, 0.5f));
        }
        gGL.end();
        for (S32 i = 0; i < 8; ++i)
        {
            text(font, label(i), px[i], py[i], labelColor(i), LLFontGL::VCENTER, LLFontGL::NO_SHADOW);
        }
    }

    void drawTape(const LLRectf& sq, F32 heading)
    {
        const F32 bottom = sq.mTop;   // the strip sits above the map (getTopReserve)
        const F32 cx = floorf((sq.mLeft + sq.mRight) * 0.5f);
        const F32 half_deg = sq.getWidth() * 0.5f / TAPE_PX_PER_DEG + 10.f;   // a little past the edges

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        quad(sq.mLeft, bottom, sq.mRight, bottom + TAPE_HEIGHT, TAPE);
        // Ticks every 5 degrees, longer every 15, standing on the strip's bottom.
        const S32 first = (S32)floorf((heading - half_deg) / 5.f) * 5;
        const S32 last = (S32)ceilf((heading + half_deg) / 5.f) * 5;
        for (S32 d = first; d <= last; d += 5)
        {
            const F32 x = floorf(cx + ((F32)d - heading) * TAPE_PX_PER_DEG);
            const S32 deg = ((d % 360) + 360) % 360;
            const F32 len = (deg % 15 == 0) ? 5.f : 3.f;
            quad(x, bottom + 1.f, x + 1.f, bottom + 1.f + len, TICK);
        }
        // The heading marker under the centre.
        vertex(cx - 4.5f, bottom, ACCENT);
        vertex(cx + 5.5f, bottom, ACCENT);
        vertex(cx + 0.5f, bottom - 7.f, ACCENT);
        gGL.end();

        // The directions every 45 degrees, above the ticks.
        const S32 first45 = (S32)floorf((heading - half_deg) / 45.f) * 45;
        for (S32 d = first45; (F32)d <= heading + half_deg; d += 45)
        {
            const S32 i = (((d % 360) + 360) % 360) / 45;
            const F32 x = cx + 0.5f + ((F32)d - heading) * TAPE_PX_PER_DEG;
            text(labelFont(i), label(i), x, bottom + 12.5f, labelColor(i), LLFontGL::VCENTER, LLFontGL::NO_SHADOW);
        }

        // The heading in three digits under the marker, on a dark backing so it reads over the map.
        const LLFontGL* small = LLFontGL::getFontSansSerifSmallBold();
        {
            const F32 half_w = 0.5f * (F32)small->getWidth(std::string("000")) + 4.f;
            const F32 h = (F32)small->getLineHeight() + 2.f;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);   // (the labels above bound the font)
            gGL.begin(LLRender::TRIANGLES);
            quad(cx + 0.5f - half_w, bottom - 8.f - h, cx + 0.5f + half_w, bottom - 8.f, TAPE);
            gGL.end();
        }
        static LLWString digits(3, (llwchar)'0');
        const S32 hd = ((ll_round(heading) % 360) + 360) % 360;
        digits[0] = (llwchar)('0' + hd / 100);
        digits[1] = (llwchar)('0' + (hd / 10) % 10);
        digits[2] = (llwchar)('0' + hd % 10);
        text(small, digits, cx + 0.5f, bottom - 9.f, LABEL, LLFontGL::TOP, LLFontGL::DROP_SHADOW);

    }
}

S32 GMDXMinimapCompass::getStyle()
{
    static LLCachedControl<S32> style(gSavedSettings, "GMDXMiniMapCompassStyle", STYLE_FRAME);
    return llclamp((S32)style, (S32)STYLE_FRAME, (S32)STYLE_TAPE);
}

F32 GMDXMinimapCompass::getMapInset()
{
    return getStyle() == STYLE_FRAME ? FRAME_BAND : 0.f;
}

F32 GMDXMinimapCompass::getTopReserve()
{
    // (the rotating Whole Region map has no tape)
    static LLCachedControl<bool> rotate(gSavedSettings, "MiniMapRotate", true);
    return getStyle() == STYLE_TAPE && !rotate ? TAPE_HEIGHT : 0.f;
}

S32 GMDXMinimapCompass::getRotatingStyle()
{
    static LLCachedControl<S32> style(gSavedSettings, "GMDXMiniMapRotateCompass", STYLE_PILLS);
    return style == STYLE_FRAME ? STYLE_FRAME : STYLE_PILLS;
}

void GMDXMinimapCompass::draw(const LLRectf& square, F32 heading_deg)
{
    switch (getStyle())
    {
        case STYLE_PILLS:
            drawPills(square);
            break;
        case STYLE_TAPE:
            drawTape(square, heading_deg);
            break;
        default:
            drawFrame(square);
            break;
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

F32 GMDXMinimapCompass::getRadarRing()
{
    return FRAME_BAND;
}

void GMDXMinimapCompass::drawRadar(F32 cx, F32 cy, F32 radius, F32 heading_deg, S32 style)
{
    if (style == STYLE_PILLS)   // pills just inside the disc's edge, and a faint line along it
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.pushMatrix();
        gGL.translatef(cx, cy, 0.f);
        gl_washer_2d(radius, radius - 1.f, 96, LLColor4(1.f, 1.f, 1.f, 0.22f), LLColor4(1.f, 1.f, 1.f, 0.22f));
        gGL.popMatrix();
        turningPills(heading_deg, [&](F32 ux, F32 uy, F32 half_w, F32 r, F32& x, F32& y)
            {
                const F32 d = radius - PILL_PAD - (fabsf(ux) * half_w + fabsf(uy) * r);
                x = cx + ux * d;
                y = cy + uy * d;
            });
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return;
    }
    constexpr S32 SEGMENTS = 96;
    const F32 r0 = radius, r1 = radius + FRAME_BAND;
    // Brushed metal as the frame's: light towards the top-left and bottom-right, dark between.
    auto metal = [&](F32 x, F32 y)
    {
        const F32 t = ((x - (cx - r1)) + ((cy + r1) - y)) / (4.f * r1);
        const F32 g = 0.21f + 0.21f * (0.5f + 0.5f * cosf(t * F_TWO_PI));
        return LLColor4(g, g + 0.005f, g + 0.02f, 1.f);
    };
    auto at = [&](F32 bearing_deg, F32 r, F32& x, F32& y)   // a bearing (clockwise from north), turned so the heading is up
    {
        const F32 a = (bearing_deg - heading_deg) * DEG_TO_RAD;
        x = cx + r * sinf(a);
        y = cy + r * cosf(a);
    };
    const LLColor4 inner_line(0.05f, 0.05f, 0.06f, 0.95f), outer_line(0.08f, 0.08f, 0.09f, 1.f);

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    for (S32 s = 0; s < SEGMENTS; ++s)
    {
        const F32 a0 = F_TWO_PI * s / SEGMENTS, a1 = F_TWO_PI * (s + 1) / SEGMENTS;
        const F32 c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        auto band = [&](F32 ra, F32 rb, bool shaded, const LLColor4& flat)
        {
            const F32 ax = cx + ra * c0, ay = cy + ra * s0, bx = cx + ra * c1, by = cy + ra * s1;
            const F32 dx = cx + rb * c0, dy = cy + rb * s0, ex = cx + rb * c1, ey = cy + rb * s1;
            vertex(ax, ay, shaded ? metal(ax, ay) : flat); vertex(bx, by, shaded ? metal(bx, by) : flat); vertex(ex, ey, shaded ? metal(ex, ey) : flat);
            vertex(ax, ay, shaded ? metal(ax, ay) : flat); vertex(ex, ey, shaded ? metal(ex, ey) : flat); vertex(dx, dy, shaded ? metal(dx, dy) : flat);
        };
        band(r0, r1, true, LABEL);
        band(r0 - 1.f, r0 + 1.f, false, inner_line);   // a dark line along the map, a darker one outside
        band(r1 - 1.f, r1, false, outer_line);
    }
    // ticks every 15 degrees between the labelled directions, pointing in from the band's outer edge
    for (S32 i = 0; i < 24; ++i)
    {
        if (i % 3 == 0) continue;
        F32 ox, oy, ix, iy;
        at(i * 15.f, r1 - 2.f, ox, oy);
        at(i * 15.f, r1 - 6.f, ix, iy);
        const F32 len = llmax(sqrtf((ox - ix) * (ox - ix) + (oy - iy) * (oy - iy)), 1e-3f);
        const F32 nx = -(oy - iy) / len * 0.5f, ny = (ox - ix) / len * 0.5f;
        vertex(ox - nx, oy - ny, TICK); vertex(ix - nx, iy - ny, TICK); vertex(ix + nx, iy + ny, TICK);
        vertex(ox - nx, oy - ny, TICK); vertex(ix + nx, iy + ny, TICK); vertex(ox + nx, oy + ny, TICK);
    }
    gGL.end();

    for (S32 i = 0; i < 8; ++i)
    {
        F32 x, y;
        at(i * 45.f, r0 + FRAME_BAND * 0.5f, x, y);
        text(labelFont(i), label(i), x, y, labelColor(i), LLFontGL::VCENTER, LLFontGL::DROP_SHADOW);
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

void GMDXMinimapCompass::drawRadarSquare(const LLRectf& sq, F32 heading_deg, S32 style)
{
    if (style == STYLE_PILLS)   // pills sliding round just inside the map's edges
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        outline(sq, LLColor4(1.f, 1.f, 1.f, 0.22f));
        gGL.end();
        const F32 cx = (sq.mLeft + sq.mRight) * 0.5f, cy = (sq.mBottom + sq.mTop) * 0.5f;
        turningPills(heading_deg, [&](F32 ux, F32 uy, F32 half_w, F32 r, F32& x, F32& y)
            {
                const F32 hx = sq.getWidth() * 0.5f - PILL_PAD - half_w, hy = sq.getHeight() * 0.5f - PILL_PAD - r;
                const F32 t = llmin(fabsf(ux) > 1e-4f ? hx / fabsf(ux) : F32_MAX, fabsf(uy) > 1e-4f ? hy / fabsf(uy) : F32_MAX);
                x = cx + ux * t;
                y = cy + uy * t;
            });
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return;
    }
    frameBand(sq);
    const F32 cx = (sq.mLeft + sq.mRight) * 0.5f, cy = (sq.mBottom + sq.mTop) * 0.5f;
    const F32 half = sq.getWidth() * 0.5f;
    // Where a bearing (clockwise from north, turned so the heading is up) meets the square of
    // half side h around the centre; side = true when it meets a left / right edge.
    auto at = [&](F32 bearing_deg, F32 h, F32& x, F32& y, bool& side)
    {
        const F32 a = (bearing_deg - heading_deg) * DEG_TO_RAD;
        const F32 ux = sinf(a), uy = cosf(a);
        side = fabsf(ux) > fabsf(uy);
        const F32 t = h / llmax(fabsf(ux), fabsf(uy));
        x = cx + ux * t;
        y = cy + uy * t;
    };
    constexpr F32 TICK_LEN = 4.f;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < 24; ++i)   // every 15 degrees between the labelled directions, out from the map edge
    {
        if (i % 3 == 0) continue;
        F32 x, y;
        bool side;
        at(i * 15.f, half - FRAME_BAND + 1.f, x, y, side);
        const F32 out = side ? (x > cx ? 1.f : -1.f) : (y > cy ? 1.f : -1.f);
        if (side) quad(x, y - 0.5f, x + out * TICK_LEN, y + 0.5f, TICK);
        else quad(x - 0.5f, y, x + 0.5f, y + out * TICK_LEN, TICK);
    }
    gGL.end();
    for (S32 i = 0; i < 8; ++i)
    {
        F32 x, y;
        bool side;
        at(i * 45.f, half - FRAME_BAND * 0.5f, x, y, side);
        // (kept off the corners, where two edges' letters would meet)
        x = llclamp(x, sq.mLeft + FRAME_BAND, sq.mRight - FRAME_BAND);
        y = llclamp(y, sq.mBottom + FRAME_BAND * 0.5f, sq.mTop - FRAME_BAND * 0.5f);
        text(labelFont(i), label(i), x, y, labelColor(i), LLFontGL::VCENTER, LLFontGL::DROP_SHADOW);
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}
