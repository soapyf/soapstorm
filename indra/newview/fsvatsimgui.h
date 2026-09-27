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

// One ImGui context drawn with the world, under the viewer's UI. It exists only while at least one client is
// registered (the editor, or the "VATsImGuiTest" debug setting's demo), so with nothing open every
// hook below returns after one null check and no GL objects exist.
//
// Coordinates: ImGui covers the world view (the window less the viewer's menu, navigation and
// toolbar strips) in the viewer's scaled UI units, top-left origin, with DisplayFramebufferScale =
// the viewer's display scale, so it scales with the UI Size setting and HiDPI like LLUI does,
// renders at full window resolution and matches LLViewerCamera's projection (spec 09 U3).
//
// Layers (spec 09 U4b): the whole frame, the world layer (bones, gizmos, markers, ghosts) and the editor's
// panels over it, is drawn just before LLUI (renderWorld), so every viewer window (floaters, chat, toasts,
// menus, dialogs, toolbars) is drawn over the panels.
//
// Input matches that order: LLUI first. The pointer reaches ImGui only where the viewer's hover reached the
// world (no LLUI view under it; the panels sit on the world view), or during an ImGui drag. A press no viewer
// window takes comes back through worldClick: over an ImGui window or with an ImGui popup open it is ImGui's;
// on the world it is the editor's while the editor is open, unless Alt is held (the viewer's camera: Alt-cam,
// Ctrl+Alt orbit, Ctrl+Alt+Shift pan). The wheel is ImGui's over its windows, else the viewer's zoom. Keys go to
// an ImGui text field first, then to LLUI while one of its controls has keyboard focus (the chat bar, a
// floater); the viewer's Alt camera keys (Alt with the arrows, Page Up/Down, A, D, W, S, E, C) stay the
// viewer's; every other key is the editor's. Without the editor (the "VATsImGuiTest" demo) ImGui gets only
// what its windows take.
namespace FSVATsImGui
{
    // Clients: each draws its ImGui windows inside the frame (between NewFrame and Render).
    // Registering the first creates the context; removing the last destroys it.
    // before (optional) runs outside the frame, just before NewFrame (fonts, camera sync). Neither may
    // add or remove clients; do that before render() (FSVATsEditor::update runs there).
    void setClient(const std::string& name, std::function<void()> draw, std::function<void()> before = {});
    void removeClient(const std::string& name);
    bool isOpen();

    // Display path: renderWorld, just before LLUI draws (render_ui_2d, and display_startup for the login screen),
    // builds this frame's ImGui frame and draws it; render, just before swap, draws it only on a frame that had
    // no such pass (a snapshot), and updates the cursor.
    void renderWorld();
    void render();
    // LLViewerWindow::stopGL: frees ImGui's GL objects before the GL context goes; recreated on the next render.
    void destroyGL();

    // Input hooks, called first thing in LLViewerWindow's handlers with raw window GL coordinates.
    // Those returning bool return true when ImGui consumed the event and the viewer must not see it.
    bool mouseButton(LLCoordGL pos, MASK mask, EMouseClickType click, bool down);
    // From handleAnyMouseClick where a click LLUI did not take would reach the world (the current tool): true
    // when the editor takes it instead.
    bool worldClick(LLCoordGL pos, MASK mask, EMouseClickType click, bool down);
    // From updateUI where the hover would reach the world (the current tool): true when the editor owns it
    // (no object hover, no touch cursor). Also records that the pointer is on the world this frame.
    bool worldHover(MASK mask);
    void mouseMove(LLCoordGL pos, MASK mask);
    void mouseLeave();
    bool scrollWheel(S32 clicks, bool horizontal);
    bool keyDown(KEY key, MASK mask);
    void keyUp(KEY key, MASK mask);
    bool unicodeChar(llwchar uni_char, MASK mask);
    void focusLost();
    // True while ImGui owns a drag: the viewer skips hover.
    bool capturesMouse();
    // True while the viewer's hover reached the world this frame (no LLUI view under the pointer).
    bool pointerOnWorld();
}

#endif // FS_VATSIMGUI_H
