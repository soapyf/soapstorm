/**
 * @file fsvatsimgui.h
 * @brief Dear ImGui host inside the viewer for the VATs editor UI (VATs spec 09 §0a, stage U2).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (c) 2026 Viewport Avatar Toolset contributors
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
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef FS_VATSIMGUI_H
#define FS_VATSIMGUI_H

#include "indra_constants.h"
#include "llcoord.h"

#include <functional>
#include <string>

// One ImGui context drawn over the viewer's UI. It exists only while at least one client is
// registered (the editor, or the "VATsImGuiTest" debug setting's demo), so with nothing open every
// hook below returns after one null check and no GL objects exist.
//
// Coordinates: ImGui works in the viewer's scaled UI units (DisplaySize = window size / UI scale,
// top-left origin), with DisplayFramebufferScale = the viewer's display scale, so it scales with
// the UI Size setting and HiDPI like LLUI does and renders at full window resolution.
namespace FSVATsImGui
{
    // Clients: each draws its ImGui windows inside the frame (between NewFrame and Render).
    // Registering the first creates the context; removing the last destroys it.
    void setClient(const std::string& name, std::function<void()> draw);
    void removeClient(const std::string& name);
    bool isOpen();

    // Display path: builds and draws one ImGui frame over the finished viewer frame, just before swap.
    void render();
    // LLViewerWindow::stopGL: frees ImGui's GL objects before the GL context goes; recreated on the next render.
    void destroyGL();

    // Input hooks, called first thing in LLViewerWindow's handlers with raw window GL coordinates.
    // Those returning bool return true when ImGui consumed the event and the viewer must not see it.
    bool mouseButton(LLCoordGL pos, MASK mask, EMouseClickType click, bool down);
    void mouseMove(LLCoordGL pos, MASK mask);
    void mouseLeave();
    bool scrollWheel(S32 clicks, bool horizontal);
    bool keyDown(KEY key, MASK mask);
    void keyUp(KEY key, MASK mask);
    bool unicodeChar(llwchar uni_char, MASK mask);
    void focusLost();
    // True while the pointer is over an ImGui window (or ImGui owns a drag): the viewer skips hover.
    bool capturesMouse();
}

#endif // FS_VATSIMGUI_H
