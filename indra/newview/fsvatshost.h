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

#include "llrect.h"
#include "lluuid.h"
#include "stdtypes.h"

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
    // FSVATsImGui's world layer, before ImGui's draw: other actors' bodies and props (spec 09 U5). releaseGL: the GL
    // context is going (LLViewerWindow::stopGL).
    void drawScene();
    void releaseGL();
    // True while the editor holds your avatar (logged in, editor open). audio_update_wind fades the flight wind out
    // then, as when not flying (spec 09 U5).
    bool holdsAvatar();
    // True while the editor hides every other avatar (spec 09 U5): it turns on the viewer's Render Only Friends
    // (RenderAvatarFriendsOnly) and LLVOAvatar::isBuddy answers false, so friends are hidden too; never your own.
    bool hidesOtherAvatars();
    // Build 17 (spec 09 §0f): while the editor hides the viewer's UI, the area toasts, notifications, script dialogs and
    // alerts are laid out in (LLScreenChannelBase::getChannelRect, LLScreenChannel::redrawToasts, the nearby chat toasts,
    // LLScriptFloater::show): the editor's view between its docked panels, in scaled screen coordinates. False: the
    // viewer's own places (editor closed, or Show Firestorm UI on).
    bool toastArea(LLRect& out);
    // True while the editor holds your avatar and the viewer's UI is hidden: LLToolPie shows no hover tips for objects
    // and avatars (they would draw over the editor's panels).
    bool hidesWorldTips();
    // True while the editor is open: the world's clicks and the keys are the editor's (FSVATsImGui).
    bool ownsWorld();
    // From LLAgent::setControlFlags (spec 09 U4b): the flags that may reach the avatar. While the editor holds it,
    // no movement input does (only the editor's own return autopilot), and neither does Stand Up while the
    // editor has sat it down (with a "Close the editor to stand up" tip).
    U32 filterControls(U32 flags);
    // Build 20 (spec 09 §0i, item 48), from LLVOAvatar::startMotion on your own avatar: true while Test as My Walk / Run
    // plays the editor's clip as that walk or run, so the region's walk (or run) id starts nothing locally, neither the
    // default motion nor the AO's replacement (which would ask the region for its own).
    bool takesLocomotion(const LLUUID& id);
    // True during the walk test: the movement keys are the viewer's again (FSVATsImGui).
    bool walking();
}

#endif // FS_VATSHOST_H
