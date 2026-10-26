/**
 * @file ssfloatergamecontroloverlay.cpp
 * @brief See ssfloatergamecontroloverlay.h.
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

#include "ssfloatergamecontroloverlay.h"

#include "lldraghandle.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llmenugl.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewermenu.h" // gMenuHolder
#include "ssgamecontroldiagram.h"
#include "ssgamecontrolui.h"

SSFloaterGameControlOverlay::SSFloaterGameControlOverlay(const LLSD& key)
:   LLFloater(key)
{
    // registered before postBuild: the context menu resolves these as it is built
    mCommitCallbackRegistrar.add("GameControlOverlay.Options", [](LLUICtrl*, const LLSD&)
        {
            LLFloaterReg::showInstance("preferences", LLSD().with("tab", "game_control"));
        });
    mCommitCallbackRegistrar.add("GameControlOverlay.ToggleMouselook", [](LLUICtrl*, const LLSD&)
        {
            gSavedSettings.setBOOL("GameControlOverlayInMouselook", !gSavedSettings.getBOOL("GameControlOverlayInMouselook"));
        });
    mEnableCallbackRegistrar.add("GameControlOverlay.InMouselook", [](LLUICtrl*, const LLSD&)
        {
            return gSavedSettings.getBOOL("GameControlOverlayInMouselook");
        });
    mCommitCallbackRegistrar.add("GameControlOverlay.Close", [this](LLUICtrl*, const LLSD&) { closeFloater(); });
}

SSFloaterGameControlOverlay::~SSFloaterGameControlOverlay()
{
    // the menu lives in gMenuHolder, not in this floater's view tree
    if (LLView* menu = mMenuHandle.get())
    {
        menu->die();
    }
}

bool SSFloaterGameControlOverlay::postBuild()
{
    mDiagram = getChild<SSGameControlDiagram>("overlay_diagram");
    if (LLDragHandle* handle = getDragHandle())
    {
        handle->setVisible(false); // no title bar; the whole picture drags
    }
    if (LLContextMenu* menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>(
            "menu_game_control_overlay.xml", gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance()))
    {
        mMenuHandle = menu->getHandle();
    }
    return true;
}

void SSFloaterGameControlOverlay::draw()
{
    U32 buttons = 0;
    std::array<F32, 6> axes {};
    SSGameControlUI::readLiveInput(buttons, axes);
    mDiagram->setLiveInput(buttons, axes);
    mDiagram->setFaceStyle(SSGameControlUI::faceStyle());
    LLFloater::draw();
}

bool SSFloaterGameControlOverlay::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // the resize edges first
    if (childrenHandleMouseDown(x, y, mask))
    {
        return true;
    }
    mDragX = x;
    mDragY = y;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool SSFloaterGameControlOverlay::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
        // <GMDX> a user move, as a title-bar drag ends one: keeps it on screen, remembers the rect
        // AND the floater's relative position, which the view reapplies when the screen's snap
        // area changes (entering mouselook hides the toolbars; translate() alone left the
        // position it opened at, so the overlay jumped back to its corner)
        handleReshape(getRect(), true);
        // </GMDX>
        return true;
    }
    return LLFloater::handleMouseUp(x, y, mask);
}

bool SSFloaterGameControlOverlay::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        translate(x - mDragX, y - mDragY);
        return true;
    }
    return LLFloater::handleHover(x, y, mask);
}

bool SSFloaterGameControlOverlay::handleRightMouseDown(S32 x, S32 y, MASK mask)
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
    menu->show(screen_x, screen_y, this); // LLContextMenu::show, not showPopup (see ALFloaterScopes)
    return true;
}
