/**
 * @file gmdxfloaterinputoverlay.h
 * @brief GunMetal-DX keyboard and mouse overlay.
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

// <GMDX> The keyboard and mouse twin of the controller overlay (SSFloaterGameControlOverlay): the
// keys combat uses (1-5, Tab Q W E R, A S D F, Shift Z X C V, Ctrl Alt Space) and the mouse (left,
// middle, right, the wheel's last turn, and its movement as a dot in a ring), lighting up as they
// are pressed. Drawn, no artwork. No background, title or close button: drag it by the picture,
// resize it from its edges, right-click for Show in mouselook (GMDXInputOverlayInMouselook, see
// LLAgent's mouselook skip list) and Close. It remembers its place and whether it was open.
// Avatar > Movement > Keyboard & Mouse Overlay.

#pragma once

#include "llfloater.h"

class GMDXFloaterInputOverlay : public LLFloater
{
public:
    GMDXFloaterInputOverlay(const LLSD& key);
    ~GMDXFloaterInputOverlay();

    bool postBuild() override;
    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;

    // LLViewerWindow::handleScrollWheel: the wheel turned (clicks > 0 = towards the user).
    static void noteScroll(S32 clicks);

private:
    LLHandle<LLView> mMenuHandle;
    S32 mDragX { 0 };
    S32 mDragY { 0 };
    F32 mMoveX { 0.f };   // the mouse's movement, eased (-1..1 of the ring)
    F32 mMoveY { 0.f };
};
