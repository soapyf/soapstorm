/**
 * @file llsetkeybinddialog.h
 * @brief LLSetKeyBindDialog class definition
 *
 * $LicenseInfo:firstyear=2019&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2019, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */


#ifndef LL_LLSETKEYBINDDIALOG_H
#define LL_LLSETKEYBINDDIALOG_H

#include "llmodaldialog.h"

class LLCheckBoxCtrl;
class LLTextBase;

// Filters for LLSetKeyBindDialog
static const U32 ALLOW_MOUSE = 1;
static const U32 ALLOW_MASK_MOUSE = 2;
static const U32 ALLOW_KEYS = 4; //keyboard
static const U32 ALLOW_MASK_KEYS = 8;
static const U32 ALLOW_MASKS = 16;
static const U32 ALLOW_CONTROLLER = 32; // OCOL: game controller buttons, sticks and triggers
// <SS:GameControl> keys a menu shortcut uses. For a controller button that types a key: the typed
// key goes through the same handling as the real one, so the menu gets it exactly when it would anyway.
static const U32 ALLOW_MENU_KEYS = 64;
static const U32 DEFAULT_KEY_FILTER = ALLOW_MOUSE | ALLOW_MASK_MOUSE | ALLOW_KEYS | ALLOW_MASKS | ALLOW_MASK_KEYS | ALLOW_CONTROLLER;


class LLKeyBindResponderInterface
{
public:
    virtual ~LLKeyBindResponderInterface() {};

    virtual void onCancelKeyBind() = 0;
    virtual void onDefaultKeyBind(bool all_modes) = 0;
    // returns true if parent failed to set key due to key being in use
    virtual bool onSetKeyBind(EMouseClickType click, KEY key, MASK mask, bool all_modes) = 0;
    // OCOL: a game controller input was chosen (see LLGameControl::ActionType)
    virtual bool onSetControllerBind(U8 action_type, U8 action, bool all_modes) { return false; }
};

class LLSetKeyBindDialog : public LLModalDialog
{
public:
    LLSetKeyBindDialog(const LLSD& key);
    ~LLSetKeyBindDialog();

    /*virtual*/ bool postBuild();
    /*virtual*/ void onOpen(const LLSD& data);
    /*virtual*/ void onClose(bool app_quiting);
    /*virtual*/ void draw();

    void setParent(LLKeyBindResponderInterface* parent, LLView* frustum_origin, U32 key_mask = DEFAULT_KEY_FILTER);

    // Wrapper around recordAndHandleKey
    // It does not record, it handles, but handleKey function is already in use
    static bool recordKey(KEY key, MASK mask, bool down);

    bool handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down);
    static void onCancel(void* user_data);
    static void onBlank(void* user_data);
    static void onDefault(void* user_data);
    static void onClickTimeout(void* user_data, MASK mask);

    static bool isRecording() { return sRecordKeys; }
    // <SS:GameControl> A key press also arrives as a typed character, after the key itself
    // was recorded and the dialog closed; that character must not reach whatever has focus
    // next (in Preferences, the search box). True while recording and for the frame a
    // recorded key-down came in.
    static bool consumesTypedChar();

    class Updater;

private:
    bool recordAndHandleKey(KEY key, MASK mask, bool down);
    void recordController();
    void setKeyBind(EMouseClickType click, KEY key, MASK mask, bool all_modes);
    LLKeyBindResponderInterface *pParent;
    LLCheckBoxCtrl *pCheckBox;
    LLTextBase *pDescription;

    U32 mKeyFilterMask;
    Updater *pUpdater;
    KEY mLastMaskKey;
    U32 mLastControllerButtons; // OCOL
    U32 mLastControllerAxes; // OCOL: bit per half-axis past the threshold

    static bool sRecordKeys; // for convinience and not to check instance each time
    static U32 sRecordedKeyFrame; // <SS:GameControl/> frame of the last recorded key-down

    // drawFrustum
private:
    void drawFrustum();

    LLHandle <LLView>   mFrustumOrigin;
    F32                 mContextConeOpacity;
    F32                 mContextConeInAlpha;
    F32                 mContextConeOutAlpha;
    F32                 mContextConeFadeTime;
};


#endif  // LL_LLSETKEYBINDDIALOG_H
