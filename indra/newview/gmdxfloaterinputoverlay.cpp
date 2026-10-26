/**
 * @file gmdxfloaterinputoverlay.cpp
 * @brief GunMetal-DX keyboard and mouse overlay (see gmdxfloaterinputoverlay.h).
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

// <GMDX> whole file: see gmdxfloaterinputoverlay.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxfloaterinputoverlay.h"

#include "lldraghandle.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llkeyboard.h"
#include "llmenugl.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewermenu.h" // gMenuHolder
#include "llviewerwindow.h"

namespace
{
// The keys, in key units from the cluster's top-left: x, row, width.
struct Key { const char* label; KEY key; F32 x, row, w; };
const Key KEYS[] = {
    { "1", '1', 1.f, 0.f, 1.f }, { "2", '2', 2.f, 0.f, 1.f }, { "3", '3', 3.f, 0.f, 1.f }, { "4", '4', 4.f, 0.f, 1.f }, { "5", '5', 5.f, 0.f, 1.f },
    { "Tab", KEY_TAB, 0.f, 1.f, 1.5f }, { "Q", 'Q', 1.5f, 1.f, 1.f }, { "W", 'W', 2.5f, 1.f, 1.f }, { "E", 'E', 3.5f, 1.f, 1.f }, { "R", 'R', 4.5f, 1.f, 1.f },
    { "A", 'A', 1.75f, 2.f, 1.f }, { "S", 'S', 2.75f, 2.f, 1.f }, { "D", 'D', 3.75f, 2.f, 1.f }, { "F", 'F', 4.75f, 2.f, 1.f },
    { "Shift", KEY_SHIFT, 0.f, 3.f, 2.25f }, { "Z", 'Z', 2.25f, 3.f, 1.f }, { "X", 'X', 3.25f, 3.f, 1.f }, { "C", 'C', 4.25f, 3.f, 1.f }, { "V", 'V', 5.25f, 3.f, 1.f },
    { "Ctrl", KEY_CONTROL, 0.f, 4.f, 1.5f }, { "Alt", KEY_ALT, 1.5f, 4.f, 1.25f }, { "Space", ' ', 2.75f, 4.f, 3.5f },
};
constexpr F32 MOUSE_X = 6.75f;         // the mouse, right of it
constexpr F32 MOUSE_W = 2.5f;
constexpr F32 TOTAL_W = MOUSE_X + MOUSE_W;
constexpr F32 TOTAL_H = 5.f;
constexpr F32 SCROLL_SHOW = 0.25f;     // seconds a wheel turn stays lit
constexpr F32 MOVE_FULL_PX = 14.f;     // mouse movement per frame that reaches the ring's edge

S32 sScrollDir = 0;
LLFrameTimer sScrollTimer;

LLColor4 color(const char* name, const LLColor4& fallback, F32 alpha = 1.f)
{
    LLColor4 c = LLUIColorTable::instance().getColor(name, fallback).get();
    c.mV[VALPHA] *= alpha;
    return c;
}

// A key is shown pressed only while no text field has the keyboard: what is typed in chat or
// IM never shows on the overlay (it is often on a stream).
bool typing()
{
    const LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
    return ctrl && ctrl->acceptsTextInput();
}
} // namespace

GMDXFloaterInputOverlay::GMDXFloaterInputOverlay(const LLSD& key)
:   LLFloater(key)
{
    mCommitCallbackRegistrar.add("InputOverlay.ToggleMouselook", [](LLUICtrl*, const LLSD&)
        {
            gSavedSettings.setBOOL("GMDXInputOverlayInMouselook", !gSavedSettings.getBOOL("GMDXInputOverlayInMouselook"));
        });
    mEnableCallbackRegistrar.add("InputOverlay.InMouselook", [](LLUICtrl*, const LLSD&)
        {
            return gSavedSettings.getBOOL("GMDXInputOverlayInMouselook");
        });
    mCommitCallbackRegistrar.add("InputOverlay.Close", [this](LLUICtrl*, const LLSD&) { closeFloater(); });
}

GMDXFloaterInputOverlay::~GMDXFloaterInputOverlay()
{
    if (LLView* menu = mMenuHandle.get())
    {
        menu->die();   // (it lives in gMenuHolder, not in this floater's view tree)
    }
}

bool GMDXFloaterInputOverlay::postBuild()
{
    if (LLDragHandle* handle = getDragHandle())
    {
        handle->setVisible(false);   // no title bar; the whole picture drags
    }
    if (LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
            "menu_gmdx_input_overlay.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance()))
    {
        mMenuHandle = menu->getHandle();
    }
    return true;
}

// static
void GMDXFloaterInputOverlay::noteScroll(S32 clicks)
{
    if (clicks == 0) return;
    sScrollDir = clicks > 0 ? 1 : -1;
    sScrollTimer.reset();
    sScrollTimer.start();
}

void GMDXFloaterInputOverlay::draw()
{
    const LLColor4 accent = color("EmphasisColor", LLColor4(0.4f, 0.8f, 1.f, 1.f));
    const LLColor4 text = color("LabelTextColor", LLColor4::white);
    const LLColor4 line = color("LabelTextColor", LLColor4::white, 0.35f);
    const LLColor4 fill(0.06f, 0.07f, 0.09f, 0.55f);
    const LLColor4 lit(accent.mV[VRED], accent.mV[VGREEN], accent.mV[VBLUE], 0.85f);
    const LLColor4 lit_text(0.03f, 0.04f, 0.05f, 1.f);

    const F32 w = (F32)getRect().getWidth(), h = (F32)getRect().getHeight();
    const F32 u = llmax(llmin(w / TOTAL_W, h / TOTAL_H), 4.f);   // one key unit
    const F32 ox = floorf((w - TOTAL_W * u) * 0.5f);
    const F32 top = floorf(h - (h - TOTAL_H * u) * 0.5f);
    const F32 pad = llmax(1.f, floorf(u * 0.08f));
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const bool show_labels = u >= 14.f;

    auto cap = [&](F32 x0, F32 y0, F32 x1, F32 y1, bool down, const char* label)
    {
        const S32 l = ll_round(x0 + pad), t = ll_round(y0 - pad), r = ll_round(x1 - pad), b = ll_round(y1 + pad);
        if (r <= l || t <= b) return;
        gl_rect_2d(l, t, r, b, down ? lit : fill, true);
        gl_rect_2d(l, t, r, b, down ? lit : line, false);
        if (label && show_labels)
        {
            font->renderUTF8(label, 0, (F32)(l + r) * 0.5f, (F32)(t + b) * 0.5f, down ? lit_text : text,
                             LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
    };

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    const bool keys_live = gKeyboard && !typing();
    for (const Key& k : KEYS)
    {
        const bool down = keys_live && gKeyboard->getKeyDown(k.key);
        cap(ox + k.x * u, top - k.row * u, ox + (k.x + k.w) * u, top - (k.row + 1.f) * u, down, k.label);
    }

    // The mouse: left and right buttons over the body, the wheel / middle button between them.
    const F32 mx0 = ox + MOUSE_X * u, mx1 = mx0 + MOUSE_W * u, mmid = (mx0 + mx1) * 0.5f;
    const F32 btn_top = top, btn_bottom = top - 1.6f * u;
    const F32 wheel_half = 0.28f * u;
    cap(mx0, btn_top, mmid - wheel_half, btn_bottom, gViewerWindow->getLeftMouseDown(), "L");
    cap(mmid + wheel_half, btn_top, mx1, btn_bottom, gViewerWindow->getRightMouseDown(), "R");
    const bool scrolled = sScrollDir != 0 && sScrollTimer.getStarted() && sScrollTimer.getElapsedTimeF32() < SCROLL_SHOW;
    cap(mmid - wheel_half, btn_top - 0.2f * u, mmid + wheel_half, btn_bottom + 0.2f * u,
        gViewerWindow->getMiddleMouseDown() || scrolled, nullptr);
    if (scrolled)   // the turn's direction: an arrow above or below the wheel
    {
        const F32 ay = sScrollDir > 0 ? btn_bottom + 0.05f * u : btn_top - 0.05f * u;
        const F32 tip = sScrollDir > 0 ? -0.22f * u : 0.22f * u;
        gGL.color4fv(text.mV);
        gGL.begin(LLRender::TRIANGLES);
        gGL.vertex2f(mmid - wheel_half * 0.7f, ay - tip * 0.2f);
        gGL.vertex2f(mmid + wheel_half * 0.7f, ay - tip * 0.2f);
        gGL.vertex2f(mmid, ay + tip);
        gGL.end();
    }
    cap(mx0, btn_bottom, mx1, top - 3.3f * u, false, nullptr);   // the body

    // Its movement: a dot in a ring below it, eased so a flick reads as a sweep.
    const F32 target_x = llclamp((F32)gViewerWindow->getCurrentMouseDX() / MOVE_FULL_PX, -1.f, 1.f);
    const F32 target_y = llclamp((F32)gViewerWindow->getCurrentMouseDY() / MOVE_FULL_PX, -1.f, 1.f);
    mMoveX = lerp(mMoveX, target_x, 0.35f);
    mMoveY = lerp(mMoveY, target_y, 0.35f);
    const F32 rr = 0.7f * u, rcx = mmid, rcy = top - 4.2f * u;
    gGL.color4fv(fill.mV);
    gl_circle_2d(rcx, rcy, rr, 32, true);
    gGL.color4fv(line.mV);
    gl_circle_2d(rcx, rcy, rr, 32, false);
    const F32 moved = sqrtf(mMoveX * mMoveX + mMoveY * mMoveY);
    gGL.color4fv((moved > 0.05f ? accent : text).mV);
    gl_circle_2d(rcx + mMoveX * (rr - 3.f), rcy + mMoveY * (rr - 3.f), llmax(2.f, 0.12f * u), 16, true);

    LLFloater::draw();
}

bool GMDXFloaterInputOverlay::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (childrenHandleMouseDown(x, y, mask))   // the resize edges first
    {
        return true;
    }
    mDragX = x;
    mDragY = y;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool GMDXFloaterInputOverlay::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
        handleReshape(getRect(), true);   // a user move: on screen, rect and relative position kept
        return true;
    }
    return LLFloater::handleMouseUp(x, y, mask);
}

bool GMDXFloaterInputOverlay::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        translate(x - mDragX, y - mDragY);
        return true;
    }
    return LLFloater::handleHover(x, y, mask);
}

bool GMDXFloaterInputOverlay::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    auto* menu = static_cast<LLContextMenu*>(mMenuHandle.get());
    if (!menu)
    {
        return LLFloater::handleRightMouseDown(x, y, mask);
    }
    menu->buildDrawLabels();
    menu->updateParent(LLMenuGL::sMenuContainer);
    S32 screen_x, screen_y;
    localPointToScreen(x, y, &screen_x, &screen_y);
    menu->show(screen_x, screen_y, this);
    return true;
}
