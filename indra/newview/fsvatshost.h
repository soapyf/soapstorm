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
    // The editor's own body under the pointer (View > Body's mesh body in your avatar's place: drawn by the editor, so
    // no world object the viewer's picks find): its nearest point there, global. LLToolCamera's alt-cam focus uses it.
    bool bodyUnderPointer(S32 x, S32 y, LLVector3d& out);
    // Build 32: true while View > Body shows a mesh body in your avatar's place (the editor holds your avatar):
    // LLVOAvatar::isVisible hides your own avatar and, with it, its attachments, on this screen only.
    bool hidesYourAvatar();
    // VATs spec 08 RS-7: true while the editor draws your avatar resized (joint scales on your screen only).
    // LLVOAvatar then takes no new body size from the scaled joints, so none is sent to the region
    // (bodySizeChanged -> sendAgentSetAppearance); the size is measured again once the scales are put back.
    bool resizesYourAvatar();
    // Around LLVOAvatar writing your avatar's joint scales (shape params and avatar physics, which add to the scale
    // they find; a mesh's lock-scale overrides, which keep the scale they find): while the editor resizes your avatar
    // its joints hold their own scales for the change, and the editor takes them again after it.
    struct AvatarScaleChange
    {
        explicit AvatarScaleChange(bool self);
        ~AvatarScaleChange();
        AvatarScaleChange(const AvatarScaleChange&) = delete;
        AvatarScaleChange& operator=(const AvatarScaleChange&) = delete;
        bool mSelf;
    };
    // True while the editor hides every other avatar (spec 09 U5): it turns on the viewer's Render Only Friends
    // (RenderAvatarFriendsOnly) and LLVOAvatar::isBuddy answers false, so friends are hidden too; never your own.
    bool hidesOtherAvatars();
    // Build 17 (spec 09 §0f): while the editor hides the viewer's UI, the area toasts, notifications, script dialogs and
    // alerts are laid out in (LLScreenChannelBase::getChannelRect, LLScreenChannel::redrawToasts, the nearby chat toasts,
    // LLScriptFloater::show): the editor's view between its docked panels, in scaled screen coordinates. False: the
    // viewer's own places (editor closed, or Show SoapStorm UI on).
    bool toastArea(LLRect& out);
    // True while the editor holds your avatar and the viewer's UI is hidden: LLToolPie shows no hover tips for objects
    // and avatars (they would draw over the editor's panels).
    bool hidesWorldTips();
    // True while the editor is open: the world's clicks and the keys are the editor's (FSVATsImGui).
    bool ownsWorld();
    // Spec 09 §0m, Viewer > Edit Objects in World: true while the viewer's Build window edits objects over the open editor;
    // the world's clicks, hover and keys are then the viewer's again (FSVATsImGui), the editor still holds your avatar.
    // toggleEditObjects: Ctrl+B while the editor is open.
    bool editingObjects();
    void toggleEditObjects();
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
