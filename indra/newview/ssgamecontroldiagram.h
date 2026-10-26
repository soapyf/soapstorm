/**
 * @file ssgamecontroldiagram.h
 * @brief The game controller preferences' picture of the controller, and the
 *        stick position view that sits beside it.
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

#ifndef SS_GAMECONTROLDIAGRAM_H
#define SS_GAMECONTROLDIAGRAM_H

#include "lluictrl.h"

#include <array>
#include <string>

// A picture of a controller with a callout for every input, each naming what that
// input does in the mode being edited. Clicking an input or its callout selects it
// (commit fires); the owner fills the callouts and feeds live input in, so this class
// knows nothing about bindings. The art (skins/default/textures/gamecontrol) is
// TheHoodieGuy02's vector gamepad for the controller family in use -- Xbox, Switch or
// PlayStation -- with Kenney input prompt icons for that family beside the callouts.
class SSGameControlDiagram : public LLUICtrl
{
public:
    // Order matches LLGameControl::Button for the first fifteen, so an element and
    // its canonical button share an index. The sticks stand for both their axes and
    // their click; the triggers for their axis and their trigger-as-button.
    enum Element : S32
    {
        EL_NONE = -1,
        EL_SOUTH = 0,
        EL_EAST,
        EL_WEST,
        EL_NORTH,
        EL_BACK,
        EL_GUIDE,
        EL_START,
        EL_LEFT_STICK,
        EL_RIGHT_STICK,
        EL_LEFT_SHOULDER,
        EL_RIGHT_SHOULDER,
        EL_DPAD_UP,
        EL_DPAD_DOWN,
        EL_DPAD_LEFT,
        EL_DPAD_RIGHT,
        EL_LEFT_TRIGGER,
        EL_RIGHT_TRIGGER,
        NUM_ELEMENTS
    };

    struct Callout
    {
        std::string mGlyph;  // SecondLife-game-control glyph for the input, may be empty
        std::string mText;   // what it does; empty draws as unbound
        bool mWarn = false;  // drawn in the warning colour (e.g. a binding conflict)
    };

    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<S32> callout_width;
        Optional<bool> show_callouts; // <SS:GameControl/> false: just the controller and its live input (the overlay)
        Params();
    };

    SSGameControlDiagram(const Params& p);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

    void setCallout(S32 element, const Callout& callout);
    S32 getSelected() const { return mSelected; }
    void setSelected(S32 element) { mSelected = element; }
    void setFaceStyle(U8 face_style) { mFaceStyle = face_style; }

    // Live input to draw: canonical buttons (LLGameControl::Button bits, triggers-as-
    // buttons included) and the six canonical axes, sticks in [-1, 1] with +y up,
    // triggers in [0, 1].
    void setLiveInput(U32 buttons, const std::array<F32, 6>& axes) { mLiveButtons = buttons; mLiveAxes = axes; }

    static bool isStick(S32 element) { return element == EL_LEFT_STICK || element == EL_RIGHT_STICK; }
    static bool isTrigger(S32 element) { return element == EL_LEFT_TRIGGER || element == EL_RIGHT_TRIGGER; }
    // The canonical button an element presses: the stick click for a stick, the
    // trigger-as-button for a trigger.
    static U8 buttonForElement(S32 element);
    static S32 elementForButton(U8 button);

private:
    LLRect artRect() const;
    LLVector2 anchor(S32 element) const;   // screen position of an element on the art
    F32 hotspotRadius(S32 element) const;
    void layoutCallouts();
    S32 hitTest(S32 x, S32 y) const;
    void drawRing(const LLVector2& center, F32 radius, const LLColor4& color) const;

    std::array<Callout, NUM_ELEMENTS> mCallouts;
    std::array<LLRect, NUM_ELEMENTS> mCalloutRects;
    LLRect mLaidOutFor;          // the rect and body the callouts were last laid out for
    U8 mLaidOutStyle { 0 };
    S32 mCalloutWidth;
    bool mShowCallouts;
    S32 mSelected { EL_NONE };
    S32 mHovered { EL_NONE };
    U8 mFaceStyle { 0 };
    U32 mLiveButtons { 0 };
    std::array<F32, 6> mLiveAxes {};
};

// A stick's position inside its travel, with its dead zone shaded, or a trigger's pull
// as a bar. Fed each frame by the owner; draws the raw position so the dead zone can be
// seen swallowing small movements.
class SSGameControlStickView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    SSGameControlStickView(const Params& p);

    void draw() override;

    void setDeadZone(F32 fraction) { mDeadZone = fraction; }
    void setOuterZone(F32 fraction) { mOuterZone = fraction; } // <SS:GameControl/> travel that counts as full
    void setPosition(F32 x, F32 y) { mX = x; mY = y; }   // [-1, 1], +y up
    void setTriggers(bool triggers) { mTriggers = triggers; }  // x, y are then the left and right pull, [0, 1]

private:
    F32 mDeadZone { 0.f };
    F32 mOuterZone { 1.f };
    F32 mX { 0.f };
    F32 mY { 0.f };
    bool mTriggers { false };
};

#endif // SS_GAMECONTROLDIAGRAM_H
