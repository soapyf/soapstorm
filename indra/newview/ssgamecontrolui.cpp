/**
 * @file ssgamecontrolui.cpp
 * @brief See ssgamecontrolui.h. Lifted out of llpanelpreferencegamecontrol.cpp so
 *        the debug floater and the preferences panel share one copy.
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

#include "ssgamecontrolui.h"

#include "llcombobox.h"
#include "llgamecontrol.h"
#include "llviewercontrol.h"
#include "llfontgl.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"

namespace SSGameControlUI
{
const std::string NONE_LABEL("None");

// SecondLife-game-control glyph(s) for a canonical controller input value.
// Codepoints are from the SecondLife-game-control package's promptfont.h,
// preferring the generic vendor-neutral "gamepad"/"dpad"/"analog" sets and
// falling back to the Xbox set for shoulders/triggers (no generic equivalents
// exist). Rendered with the "SecondLife-game-control" family declared in
// fonts.xml.  Inputs with no entry (e.g. BUTTON_21..31, *_NONE) return empty
// and keep their text label.
std::string promptFontGlyph(const std::string& input_value)
{
    static const std::map<std::string, LLWString> sGlyphs = {
        // Face buttons
        { "BUTTON_SOUTH", { 0x21A7 } },  // PF_GAMEPAD_A (bottom)
        { "BUTTON_EAST",  { 0x21A6 } },  // PF_GAMEPAD_B (right)
        { "BUTTON_WEST",  { 0x21A4 } },  // PF_GAMEPAD_X (left)
        { "BUTTON_NORTH", { 0x21A5 } },  // PF_GAMEPAD_Y (top)
        // D-pad
        { "BUTTON_DPAD_UP",    { 0x219F } },  // PF_DPAD_UP
        { "BUTTON_DPAD_DOWN",  { 0x21A1 } },  // PF_DPAD_DOWN
        { "BUTTON_DPAD_LEFT",  { 0x219E } },  // PF_DPAD_LEFT
        { "BUTTON_DPAD_RIGHT", { 0x21A0 } },  // PF_DPAD_RIGHT
        // Shoulders / stick clicks
        { "BUTTON_LEFT_SHOULDER",  { 0x2198 } },  // PF_XBOX_LEFT_SHOULDER
        { "BUTTON_RIGHT_SHOULDER", { 0x2199 } },  // PF_XBOX_RIGHT_SHOULDER
        { "BUTTON_LEFT_STICK",     { 0x21BA } },  // PF_ANALOG_L_CLICK (L3)
        { "BUTTON_RIGHT_STICK",    { 0x21BB } },  // PF_ANALOG_R_CLICK (R3)
        // Menu cluster
        { "BUTTON_SELECT", { 0x21F7 } },  // PF_GAMEPAD_SELECT (menu left)
        { "BUTTON_HOME",   { 0x21F9 } },  // PF_GAMEPAD_HOME   (menu middle)
        { "BUTTON_START",  { 0x21F8 } },  // PF_GAMEPAD_START  (menu right)
        // Misc / paddles / touchpad
        { "BUTTON_MISC1",    { 0x2212 } },  // PF_GAMEPAD_M1 (Misc1)
        { "BUTTON_PADDLE1",  { 0x2277 } },  // PF_GAMEPAD_R4
        { "BUTTON_PADDLE2",  { 0x2276 } },  // PF_GAMEPAD_L4
        { "BUTTON_PADDLE3",  { 0x2279 } },  // PF_GAMEPAD_R5
        { "BUTTON_PADDLE4",  { 0x2278 } },  // PF_GAMEPAD_L5
        { "BUTTON_TOUCHPAD", { 0x21E7 } },  // PF_SONY_TOUCHPAD
        { "BUTTON_LEFT_TRIGGER",  { 0x2196 } },  // OCOL: trigger as button (PF_XBOX_LEFT_TRIGGER)
        { "BUTTON_RIGHT_TRIGGER", { 0x2197 } },  // OCOL: trigger as button (PF_XBOX_RIGHT_TRIGGER)
        // Axes
        { "AXIS_LEFTX",  { 0x21C4 } },  // PF_ANALOG_L_LEFT_RIGHT
        { "AXIS_LEFTY",  { 0x21C5 } },  // PF_ANALOG_L_UP_DOWN
        { "AXIS_RIGHTX", { 0x21C6 } },  // PF_ANALOG_R_LEFT_RIGHT
        { "AXIS_RIGHTY", { 0x21F5 } },  // PF_ANALOG_R_UP_DOWN
        { "AXIS_LEFT_TRIGGER",  { 0x2196 } },  // PF_XBOX_LEFT_TRIGGER
        { "AXIS_RIGHT_TRIGGER", { 0x2197 } },  // PF_XBOX_RIGHT_TRIGGER
        // Combined trigger pair: show both trigger glyphs side by side.
        { "AXIS_TRIGGERS", { 0x2196, 0x2197 } },
    };
    auto it = sGlyphs.find(input_value);
    if (it == sGlyphs.end())
    {
        return LLStringUtil::null;
    }
    return wstring_to_utf8str(it->second);
}

// The SecondLife-game-control glyphs live on real Unicode arrow codepoints,
// so they must be drawn with the dedicated SecondLife-game-control family
// rather than the default text font. Sized larger than the SansSerif rows
// so the icons read clearly.
const LLFontGL* promptFontForCell()
{
    const LLFontGL* font = LLFontGL::getFont(LLFontDescriptor("SecondLife-game-control", "Huge", 0));
    return font ? font : LLFontGL::getFontSansSerif();
}

std::string inputLabel(const LLComboBox* selector, const std::string& value)
{
    for (const LLScrollListItem* item : selector->getAllData())
    {
        if (item->getValue().asString() == value)
        {
            return item->getColumn(0)->getValue().asString();
        }
    }
    return LLStringUtil::null;
}

std::string selectorLabelAt(const LLComboBox* selector, S32 index)
{
    std::vector<LLScrollListItem*> items = selector->getAllData();
    if (index < 0 || index >= (S32)items.size())
    {
        return LLStringUtil::null;
    }
    return items[index]->getColumn(0)->getValue().asString();
}

// Each item keeps the canonical value but shows its SecondLife-game-control glyph;
// inputs with no glyph (None, spare buttons) keep their text label.
void buildInputGlyphSelector(const LLComboBox* text_selector, LLComboBox* glyph_selector)
{
    glyph_selector->clearRows();
    for (const LLScrollListItem* item : text_selector->getAllData())
    {
        std::string value = item->getValue().asString();
        std::string glyph = promptFontGlyph(value);

        LLSD element;
        element["value"] = value;
        LLSD& column = element["columns"][0];
        column["column"] = "input";
        if (glyph.empty())
        {
            column["value"] = item->getColumn(0)->getValue().asString();
            column["font"]["name"] = "SansSerif";
            column["font"]["size"] = "Small";
        }
        else
        {
            column["value"] = glyph;
            column["font"]["name"] = "SecondLife-game-control";
            column["font"]["size"] = "Huge";
        }
        glyph_selector->addElement(element);
    }
}

LLRect cellRect(LLScrollListCtrl* grid, LLView* relative_to, S32 row_index, S32 col_index)
{
    LLRect rect(grid->getCellRect(row_index, col_index));
    LLView* parent = grid->getParent();
    while (parent && parent != relative_to)
    {
        rect.translate(parent->getRect().mLeft, parent->getRect().mBottom);
        parent = parent->getParent();
    }
    return rect;
}

void fitInRect(LLUICtrl* ctrl, LLScrollListCtrl* grid, S32 row_index, S32 col_index)
{
    LLRect rect = cellRect(grid, ctrl->getParent(), row_index, col_index);
    ctrl->setRect(rect);
    rect.translate(-rect.mLeft, -rect.mBottom);
    for (LLView* child : *ctrl->getChildList())
    {
        LLRect child_rect(child->getRect());
        child_rect.intersectWith(rect);
        if (child_rect.mRight < rect.mRight &&
            child_rect.mRight > (rect.mLeft + rect.mRight) / 2)
        {
            child_rect.mRight = rect.mRight;
        }
        child->setRect(child_rect);
    }
}

void setNumericLabel(LLScrollListCell* cell, S32 value)
{
    cell->setValue(llformat("%d ", value));
}
} // namespace SSGameControlUI

namespace SSGameControlUI
{
    namespace
    {
        F32 liveAxis(const std::vector<U16>& half_axes, U8 axis)
        {
            if (half_axes.size() <= (size_t)axis * 2 + 1)
            {
                return 0.f;
            }
            return ((F32)half_axes[axis * 2] - (F32)half_axes[axis * 2 + 1]) / 32767.f;
        }
    }

    U8 faceStyle()
    {
        static LLCachedControl<S32> style(gSavedSettings, "GameControlButtonStyle", -1);
        return style() < 0 ? (U8)LLGameControl::getFaceStyle() : (U8)style();
    }

    void readLiveInput(U32& buttons, std::array<F32, 6>& axes)
    {
        U32 previous = 0;
        LLGameControl::getButtonStates(buttons, previous);
        const std::vector<U16>& state = LLGameControl::getState().mAxes;
        axes[0] = liveAxis(state, LLGameControl::AXIS_LEFTX);
        axes[1] = -liveAxis(state, LLGameControl::AXIS_LEFTY);   // SDL's forward is negative
        axes[2] = liveAxis(state, LLGameControl::AXIS_RIGHTX);
        axes[3] = -liveAxis(state, LLGameControl::AXIS_RIGHTY);
        axes[4] = llclamp(liveAxis(state, LLGameControl::AXIS_LEFT_TRIGGER), 0.f, 1.f);
        axes[5] = llclamp(liveAxis(state, LLGameControl::AXIS_RIGHT_TRIGGER), 0.f, 1.f);
    }
}
