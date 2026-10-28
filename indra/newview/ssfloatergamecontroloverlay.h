/**
 * @file ssfloatergamecontroloverlay.h
 * @brief The controller picture on its own, showing what is pressed.
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

#ifndef SS_FLOATERGAMECONTROLOVERLAY_H
#define SS_FLOATERGAMECONTROLOVERLAY_H

#include "llfloater.h"

class SSGameControlDiagram;

// Just the controller in use, lighting up as it is pressed: for showing inputs on stream
// or in a tutorial, or checking a pad. No background, title or close button; drag it by
// the picture, resize it from its edges, and right-click it for Controller options, Show
// in mouselook (GameControlOverlayInMouselook, see LLAgent's mouselook skip list) and
// Close. It remembers its place and whether it was open.
class SSFloaterGameControlOverlay : public LLFloater
{
public:
    SSFloaterGameControlOverlay(const LLSD& key);
    ~SSFloaterGameControlOverlay();

    bool postBuild() override;
    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;

private:
    SSGameControlDiagram* mDiagram { nullptr };
    LLHandle<LLView> mMenuHandle;
    S32 mDragX { 0 };   // where the drag grabbed the floater, in its own coordinates
    S32 mDragY { 0 };
};

#endif // SS_FLOATERGAMECONTROLOVERLAY_H
