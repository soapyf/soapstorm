/**
 * @file ssgamecontroldiagram.cpp
 * @brief See ssgamecontroldiagram.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SkoomaStorm Viewer Source Code
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "ssgamecontroldiagram.h"

#include "llfontgl.h"
#include "llgamecontrol.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "lluiimage.h"
#include "llwindow.h"

static LLDefaultChildRegistry::Register<SSGameControlDiagram> r_diagram("game_control_diagram");
static LLDefaultChildRegistry::Register<SSGameControlStickView> r_stick_view("game_control_stick_view");

namespace
{
    // TheHoodieGuy02's vector gamepads (CC0), repainted for the skin, plus a Steam
    // Controller drawn for the viewer in the same style; one per controller family, with
    // each element's hotspot in art pixels from the top-left. Generated with the images
    // by soapstorm-build/controller-art/make_vg.py.
    struct Body
    {
        const char* image;
        F32 width, height;
        struct { F32 x, y, radius; } anchors[SSGameControlDiagram::NUM_ELEMENTS];
    };

    // Indexed by LLGameControl::FaceStyle: Xbox, Switch (Joy-Cons in the grip, the same
    // layout as the Pro Controller), PlayStation, PS5, which shares the DualShock 4, and
    // the Steam Controller (2026).
    const Body BODIES[] = {
    { "GameControl_Body_xbox", 640.f, 482.f, { { 483.6f, 180.5f, 22.4f }, { 528.3f, 136.2f, 22.4f }, { 438.9f, 136.2f, 22.4f }, { 483.6f, 93.7f, 22.4f }, { 275.1f, 135.4f, 16.8f }, { 318.5f, 55.9f, 25.8f }, { 367.1f, 136.2f, 16.8f }, { 159.0f, 136.2f, 36.0f }, { 398.4f, 260.0f, 36.0f }, { 172.0f, 38.0f, 15.0f }, { 468.0f, 38.0f, 15.0f }, { 236.8f, 223.2f, 16.0f }, { 236.8f, 287.4f, 16.0f }, { 204.7f, 255.3f, 16.0f }, { 268.9f, 255.3f, 16.0f }, { 126.0f, 14.0f, 13.0f }, { 514.0f, 14.0f, 13.0f } } },
    { "GameControl_Body_switch", 640.f, 395.f, { { 466.1f, 153.0f, 13.0f }, { 491.0f, 128.0f, 13.0f }, { 441.3f, 128.3f, 13.0f }, { 466.2f, 103.2f, 13.0f }, { 208.7f, 61.7f, 10.0f }, { 448.0f, 293.0f, 12.0f }, { 431.3f, 61.7f, 10.0f }, { 174.0f, 128.2f, 29.0f }, { 466.2f, 231.2f, 29.0f }, { 153.8f, 40.0f, 16.0f }, { 486.2f, 40.0f, 16.0f }, { 174.0f, 206.1f, 12.0f }, { 173.9f, 256.0f, 12.0f }, { 149.0f, 231.0f, 12.0f }, { 198.9f, 231.1f, 12.0f }, { 161.3f, 18.0f, 13.0f }, { 479.5f, 18.0f, 13.0f } } },
    { "GameControl_Body_ps4", 640.f, 409.f, { { 517.2f, 206.2f, 20.7f }, { 564.5f, 159.6f, 21.4f }, { 471.6f, 159.6f, 21.4f }, { 517.2f, 112.5f, 20.7f }, { 195.0f, 92.6f, 24.8f }, { 318.1f, 253.3f, 15.6f }, { 449.4f, 92.6f, 24.8f }, { 219.4f, 246.8f, 36.0f }, { 420.2f, 246.8f, 36.0f }, { 130.0f, 36.0f, 12.0f }, { 510.0f, 36.0f, 12.0f }, { 123.0f, 123.3f, 27.6f }, { 122.2f, 196.5f, 28.1f }, { 87.6f, 159.3f, 27.2f }, { 159.1f, 160.0f, 27.6f }, { 130.0f, 13.0f, 11.0f }, { 510.0f, 13.0f, 11.0f } } },
    { "GameControl_Body_ps4", 640.f, 409.f, { { 517.2f, 206.2f, 20.7f }, { 564.5f, 159.6f, 21.4f }, { 471.6f, 159.6f, 21.4f }, { 517.2f, 112.5f, 20.7f }, { 195.0f, 92.6f, 24.8f }, { 318.1f, 253.3f, 15.6f }, { 449.4f, 92.6f, 24.8f }, { 219.4f, 246.8f, 36.0f }, { 420.2f, 246.8f, 36.0f }, { 130.0f, 36.0f, 12.0f }, { 510.0f, 36.0f, 12.0f }, { 123.0f, 123.3f, 27.6f }, { 122.2f, 196.5f, 28.1f }, { 87.6f, 159.3f, 27.2f }, { 159.1f, 160.0f, 27.6f }, { 130.0f, 13.0f, 11.0f }, { 510.0f, 13.0f, 11.0f } } },
    { "GameControl_Body_steam", 640.f, 473.f, { { 509.8f, 167.0f, 20.1f }, { 546.5f, 130.6f, 20.1f }, { 473.4f, 130.6f, 20.1f }, { 509.8f, 94.0f, 20.1f }, { 208.1f, 86.6f, 20.8f }, { 319.8f, 129.1f, 22.1f }, { 431.4f, 86.6f, 20.8f }, { 231.1f, 176.4f, 36.0f }, { 408.4f, 176.4f, 36.0f }, { 115.0f, 37.4f, 15.0f }, { 524.6f, 37.4f, 15.0f }, { 128.4f, 99.6f, 15.5f }, { 128.4f, 161.6f, 15.5f }, { 97.4f, 130.6f, 15.5f }, { 159.4f, 130.6f, 15.5f }, { 124.9f, 18.5f, 13.0f }, { 514.6f, 18.5f, 13.0f } } },
    };

    // Which column an element's callout sits in, indexed by SSGameControlDiagram::Element.
    const bool CALLOUT_LEFT[SSGameControlDiagram::NUM_ELEMENTS] = {
        false, false, false, false,  // face buttons
        true, false, false,          // back, guide, start
        true, false,                 // sticks
        true, false,                 // shoulders
        true, true, true, true,      // d-pad
        true, false,                 // triggers
    };

    // Kenney input prompt icons (CC0, via ControllerImage), named per element.
    const char* ICON_NAMES[SSGameControlDiagram::NUM_ELEMENTS] = {
        "s", "e", "w", "n", "back", "guide", "start", "leftstick", "rightstick",
        "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright",
        "lefttrigger", "righttrigger",
    };

    constexpr S32 CALLOUT_GAP = 12;    // between the callout columns and the art

    const LLFontGL* glyphFont()
    {
        const LLFontGL* font = LLFontGL::getFont(LLFontDescriptor("SecondLife-game-control", "Medium", 0));
        return font ? font : LLFontGL::getFontSansSerif();
    }

    const Body& bodyFor(U8 face_style)
    {
        return BODIES[llmin<size_t>(face_style, LL_ARRAY_SIZE(BODIES) - 1)];
    }

    // Icon family for a face style; Kenney has no Xbox or PlayStation guide icon.
    const char* iconFamily(U8 face_style)
    {
        switch (face_style)
        {
            case LLGameControl::FACE_BAYX: return "nintendo";
            case LLGameControl::FACE_SONY:
            case LLGameControl::FACE_PS5:  return "sony";
            default:                       return "xbox";
        }
    }

    LLColor4 color(const std::string& name, const LLColor4& fallback, F32 alpha = 1.f)
    {
        LLColor4 c = LLUIColorTable::instance().getColor(name, fallback);
        c.mV[VALPHA] *= alpha;
        return c;
    }
}

SSGameControlDiagram::Params::Params()
:   callout_width("callout_width", 165),
    show_callouts("show_callouts", true)
{
}

SSGameControlDiagram::SSGameControlDiagram(const Params& p)
:   LLUICtrl(p),
    mCalloutWidth(p.callout_width),
    mShowCallouts(p.show_callouts)
{
}

// static
U8 SSGameControlDiagram::buttonForElement(S32 element)
{
    switch (element)
    {
        case EL_LEFT_TRIGGER:  return LLGameControl::BUTTON_LEFT_TRIGGER;
        case EL_RIGHT_TRIGGER: return LLGameControl::BUTTON_RIGHT_TRIGGER;
        default:               return (element >= 0 && element < EL_LEFT_TRIGGER) ? (U8)element : (U8)LLGameControl::NUM_BUTTONS;
    }
}

// static
S32 SSGameControlDiagram::elementForButton(U8 button)
{
    if (button == LLGameControl::BUTTON_LEFT_TRIGGER)  return EL_LEFT_TRIGGER;
    if (button == LLGameControl::BUTTON_RIGHT_TRIGGER) return EL_RIGHT_TRIGGER;
    return button < EL_LEFT_TRIGGER ? (S32)button : EL_NONE;
}

void SSGameControlDiagram::setCallout(S32 element, const Callout& callout)
{
    if (element >= 0 && element < NUM_ELEMENTS)
    {
        mCallouts[element] = callout;
    }
}

// The art, as large as the space between the callout columns allows, centred.
LLRect SSGameControlDiagram::artRect() const
{
    const LLRect local = getLocalRect();
    F32 avail_w = (F32)(local.getWidth() - (mShowCallouts ? 2 * (mCalloutWidth + CALLOUT_GAP) : 8));
    F32 avail_h = (F32)(local.getHeight() - 8);
    const Body& body = bodyFor(mFaceStyle);
    F32 scale = llmax(0.1f, llmin(avail_w / body.width, avail_h / body.height));
    S32 w = ll_round(body.width * scale);
    S32 h = ll_round(body.height * scale);
    S32 left = (local.getWidth() - w) / 2;
    S32 bottom = (local.getHeight() - h) / 2;
    return LLRect(left, bottom + h, left + w, bottom);
}

LLVector2 SSGameControlDiagram::anchor(S32 element) const
{
    const LLRect art = artRect();
    const Body& body = bodyFor(mFaceStyle);
    const F32 scale = (F32)art.getWidth() / body.width;
    const auto& a = body.anchors[element];
    return LLVector2((F32)art.mLeft + a.x * scale, (F32)art.mTop - a.y * scale);
}

F32 SSGameControlDiagram::hotspotRadius(S32 element) const
{
    const Body& body = bodyFor(mFaceStyle);
    return body.anchors[element].radius * (F32)artRect().getWidth() / body.width;
}

// One column per side, each callout at the height of its input where there is room,
// pushed apart where there isn't, so the leader lines stay as close to level as they
// can without the labels overlapping.
void SSGameControlDiagram::layoutCallouts()
{
    const LLRect local = getLocalRect();
    if (local == mLaidOutFor && mFaceStyle == mLaidOutStyle)
    {
        return;
    }
    mLaidOutFor = local;
    mLaidOutStyle = mFaceStyle;

    // rows as tall as the font wants, squeezed if the fuller column wouldn't fit
    S32 left_count = 0;
    for (bool l : CALLOUT_LEFT)
    {
        left_count += l ? 1 : 0;
    }
    const S32 most_rows = llmax(left_count, NUM_ELEMENTS - left_count);
    const S32 row_h = llmin(llmax(18, (S32)LLFontGL::getFontSansSerif()->getLineHeight() + 6),
                            local.getHeight() / most_rows);
    for (bool left : { true, false })
    {
        std::vector<S32> column;
        for (S32 el = 0; el < NUM_ELEMENTS; ++el)
        {
            if (CALLOUT_LEFT[el] == left)
            {
                column.push_back(el);
            }
        }
        // top of the art first
        std::sort(column.begin(), column.end(), [this](S32 a, S32 b) { return anchor(a).mV[VY] > anchor(b).mV[VY]; });

        std::vector<F32> centers;
        F32 limit = (F32)local.getHeight() - row_h / 2.f;
        for (S32 el : column)
        {
            F32 y = llmin(anchor(el).mV[VY], limit);
            centers.push_back(y);
            limit = y - row_h;
        }
        // ran off the bottom: push up from the bottom only as far as needed, so the
        // top rows stay inside while the column still fits
        F32 floor = row_h / 2.f;
        for (size_t i = centers.size(); i-- > 0; )
        {
            centers[i] = llmax(centers[i], floor);
            floor = centers[i] + row_h;
        }

        for (size_t i = 0; i < column.size(); ++i)
        {
            S32 top = ll_round(centers[i] + row_h / 2.f);
            S32 x0 = left ? 0 : local.getWidth() - mCalloutWidth;
            mCalloutRects[column[i]] = LLRect(x0, top, x0 + mCalloutWidth, top - row_h);
        }
    }
}

S32 SSGameControlDiagram::hitTest(S32 x, S32 y) const
{
    if (!mShowCallouts)
    {
        return EL_NONE; // the overlay only shows; a click on it moves its floater
    }
    for (S32 el = 0; el < NUM_ELEMENTS; ++el)
    {
        if (mCalloutRects[el].pointInRect(x, y))
        {
            return el;
        }
    }
    // nearest hotspot that contains the point, so neighbouring D-pad arrows resolve
    S32 best = EL_NONE;
    F32 best_dist = F32_MAX;
    for (S32 el = 0; el < NUM_ELEMENTS; ++el)
    {
        LLVector2 d = anchor(el) - LLVector2((F32)x, (F32)y);
        F32 dist = d.length();
        if (dist <= hotspotRadius(el) && dist < best_dist)
        {
            best = el;
            best_dist = dist;
        }
    }
    return best;
}

bool SSGameControlDiagram::handleMouseDown(S32 x, S32 y, MASK mask)
{
    S32 hit = hitTest(x, y);
    if (hit != EL_NONE)
    {
        mSelected = hit;
        onCommit();
        return true;
    }
    return LLUICtrl::handleMouseDown(x, y, mask);
}

bool SSGameControlDiagram::handleHover(S32 x, S32 y, MASK mask)
{
    mHovered = hitTest(x, y);
    if (mHovered != EL_NONE)
    {
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    return LLUICtrl::handleHover(x, y, mask);
}

void SSGameControlDiagram::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHovered = EL_NONE;
    LLUICtrl::onMouseLeave(x, y, mask);
}

void SSGameControlDiagram::drawRing(const LLVector2& center, F32 radius, const LLColor4& ring_color) const
{
    gGL.color4fv(ring_color.mV);
    for (F32 r = radius; r < radius + 2.5f; r += 1.f)
    {
        gl_circle_2d(center.mV[VX], center.mV[VY], r, 32, false);
    }
}

void SSGameControlDiagram::draw()
{
    if (mShowCallouts)
    {
        layoutCallouts();
    }

    const LLRect art = artRect();

    const LLColor4 text_color    = color("LabelTextColor", LLColor4::white);
    const LLColor4 unbound_color = color("LabelDisabledColor", LLColor4::grey);
    const LLColor4 accent        = color("EmphasisColor", LLColor4(0.4f, 0.8f, 1.f, 1.f));
    const LLColor4 warn_color    = color("AlertCautionTextColor", LLColor4::yellow);
    const LLColor4 line_color    = color("LabelTextColor", LLColor4::white, 0.35f);
    const LLColor4 select_fill   = color("EmphasisColor", LLColor4(0.4f, 0.8f, 1.f, 1.f), 0.25f);
    const LLColor4 hover_fill    = color("LabelTextColor", LLColor4::white, 0.08f);

    // the controller
    if (LLUIImagePtr body = LLUI::getUIImage(bodyFor(mFaceStyle).image))
    {
        body->draw(art.mLeft, art.mBottom, art.getWidth(), art.getHeight());
    }

    // live input: pressed buttons fill in, sticks show where they are, triggers fill with their pull
    for (S32 el = 0; el < NUM_ELEMENTS; ++el)
    {
        const LLVector2 c = anchor(el);
        F32 strength = 0.f;
        if (isTrigger(el))
        {
            strength = mLiveAxes[el == EL_LEFT_TRIGGER ? 4 : 5];
        }
        else if (!isStick(el))
        {
            strength = (mLiveButtons & (1U << buttonForElement(el))) ? 1.f : 0.f;
        }
        if (strength > 0.02f)
        {
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            LLColor4 tint = accent;
            tint.mV[VALPHA] *= 0.35f + 0.55f * strength;
            gGL.color4fv(tint.mV);
            gl_circle_2d(c.mV[VX], c.mV[VY], hotspotRadius(el) * 0.8f, 24, true);
        }
        if (isStick(el))
        {
            const F32 sx = mLiveAxes[el == EL_LEFT_STICK ? 0 : 2];
            const F32 sy = mLiveAxes[el == EL_LEFT_STICK ? 1 : 3];
            const bool clicked = (mLiveButtons & (1U << buttonForElement(el))) != 0;
            const F32 travel = hotspotRadius(el) * 0.5f;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            LLColor4 dot = accent;
            dot.mV[VALPHA] = (clicked || sx * sx + sy * sy > 0.0025f) ? 0.9f : 0.45f;
            gGL.color4fv(dot.mV);
            gl_circle_2d(c.mV[VX] + sx * travel, c.mV[VY] + sy * travel, hotspotRadius(el) * (clicked ? 0.4f : 0.25f), 20, true);
        }
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // hover and selection rings on the art
    if (mHovered != EL_NONE && mHovered != mSelected)
    {
        drawRing(anchor(mHovered), hotspotRadius(mHovered), line_color);
    }
    if (mSelected != EL_NONE)
    {
        drawRing(anchor(mSelected), hotspotRadius(mSelected), accent);
    }

    if (!mShowCallouts)
    {
        LLUICtrl::draw();
        return;
    }

    // callouts and their leader lines
    const LLFontGL* font = LLFontGL::getFontSansSerif();
    const LLFontGL* gfont = glyphFont();
    const std::string family = iconFamily(mFaceStyle);
    for (S32 el = 0; el < NUM_ELEMENTS; ++el)
    {
        const LLRect& rect = mCalloutRects[el];
        const Callout& callout = mCallouts[el];
        const bool left = CALLOUT_LEFT[el];
        const bool selected = (el == mSelected);

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        if (selected)
        {
            gl_rect_2d(rect, select_fill, true);
        }
        else if (el == mHovered)
        {
            gl_rect_2d(rect, hover_fill, true);
        }

        const LLVector2 c = anchor(el);
        const S32 line_x = left ? rect.mRight : rect.mLeft;
        const S32 line_y = rect.getCenterY();
        const F32 r = hotspotRadius(el);
        // stop the line at the hotspot's edge rather than drawing over the button
        LLVector2 dir = LLVector2((F32)line_x, (F32)line_y) - c;
        F32 len = dir.length();
        LLVector2 end = len > r ? c + dir * (r / len) : c;
        LLColor4 lc = selected ? accent : line_color;
        gl_line_2d(line_x, line_y, ll_round(end.mV[VX]), ll_round(end.mV[VY]), lc);

        const bool bound = !callout.mText.empty();
        LLColor4 tc = callout.mWarn ? warn_color : (bound ? text_color : unbound_color);
        if (selected)
        {
            tc = text_color;
        }
        const std::string text = bound ? callout.mText : std::string("\xE2\x80\x94"); // em dash
        const S32 pad = 6;
        const S32 icon_size = rect.getHeight() - 2;
        const F32 y = (F32)rect.getCenterY();

        // the input's prompt icon for this controller family; the font glyph where the
        // icon set has none (the Xbox and PlayStation guide buttons)
        LLUIImagePtr icon;
        if (el != EL_GUIDE || mFaceStyle == LLGameControl::FACE_BAYX)
        {
            icon = LLUI::getUIImage(std::string("GameControl_Icon_") + family + "_" + ICON_NAMES[el]);
        }
        S32 icon_w = icon ? icon_size + 4 : (callout.mGlyph.empty() ? 0 : gfont->getWidth(callout.mGlyph) + 4);
        const S32 icon_x = left ? rect.mRight - pad - icon_w + 4 : rect.mLeft + pad;
        if (icon)
        {
            icon->draw(icon_x, rect.getCenterY() - icon_size / 2, icon_size, icon_size, tc);
        }
        else if (icon_w)
        {
            gfont->renderUTF8(callout.mGlyph, 0, (F32)icon_x, y, tc,
                              LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }

        // [text][icon] or [icon][text], the icon against the leader line
        const S32 max_text = rect.getWidth() - 2 * pad - icon_w;
        if (left)
        {
            font->renderUTF8(text, 0, (F32)(rect.mRight - pad - icon_w), y, tc,
                             LLFontGL::RIGHT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                             S32_MAX, max_text, nullptr, true);
        }
        else
        {
            font->renderUTF8(text, 0, (F32)(rect.mLeft + pad + icon_w), y, tc,
                             LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                             S32_MAX, max_text, nullptr, true);
        }
    }

    LLUICtrl::draw();
}

// ---------------------------------------------------------------------------

SSGameControlStickView::SSGameControlStickView(const Params& p)
:   LLUICtrl(p)
{
}

void SSGameControlStickView::draw()
{
    const LLRect local = getLocalRect();
    const LLColor4 frame  = color("LabelTextColor", LLColor4::white, 0.45f);
    const LLColor4 dead   = color("LabelTextColor", LLColor4::white, 0.12f);
    const LLColor4 accent = color("EmphasisColor", LLColor4(0.4f, 0.8f, 1.f, 1.f));
    const LLColor4 idle   = color("LabelDisabledColor", LLColor4::grey);

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    if (mTriggers)
    {
        // two vertical bars, pull filling upward, dead zone shaded at the bottom
        const S32 bar_w = llmax(8, local.getWidth() / 5);
        const S32 gap = bar_w;
        const S32 total = 2 * bar_w + gap;
        const S32 x0 = (local.getWidth() - total) / 2;
        const S32 top = local.getHeight() - 2;
        const S32 bottom = 2;
        const S32 h = top - bottom;
        for (S32 i = 0; i < 2; ++i)
        {
            const S32 l = x0 + i * (bar_w + gap);
            const F32 pull = llclamp(i == 0 ? mX : mY, 0.f, 1.f);
            gl_rect_2d(l, bottom + ll_round(h * mDeadZone), l + bar_w, bottom, dead, true);
            gl_rect_2d(l, bottom + ll_round(h * pull), l + bar_w, bottom, pull > mDeadZone ? accent : idle, true);
            gl_rect_2d(l, top, l + bar_w, bottom, frame, false);
        }
        return;
    }

    const F32 cx = (F32)local.getWidth() / 2.f;
    const F32 cy = (F32)local.getHeight() / 2.f;
    const F32 radius = llmin(cx, cy) - 3.f;

    gGL.color4fv(dead.mV);
    gl_circle_2d(cx, cy, radius * llclamp(mDeadZone, 0.f, 1.f), 32, true);
    if (mOuterZone < 0.999f)
    {
        // <SS:GameControl> the ring past which the stick counts as fully pushed
        gGL.color4fv(frame.mV);
        gl_circle_2d(cx, cy, radius * llclamp(mOuterZone, 0.f, 1.f), 40, false);
    }
    gGL.color4fv(frame.mV);
    gl_circle_2d(cx, cy, radius, 48, false);
    gl_line_2d(ll_round(cx - radius), ll_round(cy), ll_round(cx + radius), ll_round(cy), dead);
    gl_line_2d(ll_round(cx), ll_round(cy - radius), ll_round(cx), ll_round(cy + radius), dead);

    const F32 x = llclamp(mX, -1.f, 1.f);
    const F32 y = llclamp(mY, -1.f, 1.f);
    const bool outside = x * x + y * y > mDeadZone * mDeadZone;
    gGL.color4fv((outside ? accent : idle).mV);
    gl_circle_2d(cx + x * radius, cy + y * radius, 4.f, 16, true);
}
