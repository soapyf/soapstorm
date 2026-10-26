/**
 * @file ssgamecontrolui.h
 * @brief Helpers shared by the Game Controls preferences and the Game Controller
 *        Debug floater: controller glyphs, selector lookups and in-cell editors.
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

#ifndef SS_GAMECONTROLUI_H
#define SS_GAMECONTROLUI_H

#include "stdtypes.h"

#include <array>
#include <string>

class LLComboBox;
class LLFontGL;
#include "llrect.h"
class LLScrollListCell;
class LLScrollListCtrl;
class LLUICtrl;
class LLView;

namespace SSGameControlUI
{
    // Selector items use this label for the unmapped entry; cells show it blank.
    extern const std::string NONE_LABEL;

    // SecondLife-game-control glyph(s) for a canonical input value such as
    // "BUTTON_SOUTH" or "AXIS_TRIGGERS"; empty if it has none.
    std::string promptFontGlyph(const std::string& input_value);
    // The font those glyphs are drawn in, sized for a table cell.
    const LLFontGL* promptFontForCell();

    // Label of the selector item with this value, or empty.
    std::string inputLabel(const LLComboBox* selector, const std::string& value);
    // Label of the selector item at this index (= canonical index), or empty.
    std::string selectorLabelAt(const LLComboBox* selector, S32 index);
    // Fill glyph_selector with text_selector's values, labelled as glyphs.
    void buildInputGlyphSelector(const LLComboBox* text_selector, LLComboBox* glyph_selector);

    // Place an editor over a table cell, and where that cell is relative to a view.
    void fitInRect(LLUICtrl* ctrl, LLScrollListCtrl* grid, S32 row_index, S32 col_index);
    LLRect cellRect(LLScrollListCtrl* grid, LLView* relative_to, S32 row_index, S32 col_index);
    void setNumericLabel(LLScrollListCell* cell, S32 value);

    // The controller art and icons to show: the GameControlButtonStyle setting, or the
    // detected controller's (LLGameControl::getFaceStyle()) when that is Auto (-1).
    U8 faceStyle();
    // What the controller is doing now, for the picture: canonical buttons (triggers-as-
    // buttons included) and sticks in [-1, 1] with +y up, triggers in [0, 1].
    void readLiveInput(U32& buttons, std::array<F32, 6>& axes);
}

#endif // SS_GAMECONTROLUI_H
