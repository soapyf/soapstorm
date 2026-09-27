/**
 * @file fsvatshost.h
 * @brief The VATs editor in the viewer: vats_ui over the world through a viewer Host (VATs spec 09, stage U3).
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

#ifndef FS_VATSHOST_H
#define FS_VATSHOST_H

// The VATs editor (Avatar > VATs Editor, setting "VATsEditor"): Viewport Avatar Toolset's own editor
// UI (indra/libvats/ui, vats::App) drawn by FSVATsImGui over the world, with a viewer implementation
// of vats::ui::Host (paths under app_settings/vats and user_settings/vats, the worn avatar driven
// through VATsClipMotion, LLViewerCamera as the view, the viewer's file pickers, audio engine and
// direct upload).
namespace FSVATsEditor
{
    // Once a frame from FSVATsImGui::render, before the ImGui frame: opens or closes the editor to match
    // the setting, and closes it (autosaving, no prompt) when the viewer quits.
    void update(bool want_open);
    // True while the editor is open: the world's clicks and the keys are the editor's (FSVATsImGui).
    bool ownsWorld();
}

#endif // FS_VATSHOST_H
