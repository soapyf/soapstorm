/**
 * @file llpanelpreferencegamecontrol.h
 * @brief LLPanelPreferenceGameControl class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#pragma once

#include "llfloaterpreference.h"
#include "llgamecontrol.h"
#include "llsetkeybinddialog.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"

#include <array>

class ALCurveEditorCtrl;
class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLPanel;
class LLSliderCtrl;
class LLTextBox;
class SSGameControlDiagram;
class SSGameControlStickView;

// <SS:GameControl> Preferences > Game Controls, rebuilt around a picture of the
// controller: pick a mode, pick an input on the picture (or press it), and edit what
// it does in the inspector below. The same bindings are still available as the old
// tables ("List view"). Hardware remap, raw input, calibration and the server packet
// moved to Develop > Game Controller Debug (SSFloaterGameControlDebug).
class LLPanelPreferenceGameControl : public LLPanelPreference, public LLKeyBindResponderInterface
{
    LOG_CLASS(LLPanelPreferenceGameControl);

public:
    // A controller was connected or removed.
    static void updateDeviceList();

    LLPanelPreferenceGameControl();
    ~LLPanelPreferenceGameControl();

    void onOpen(const LLSD& key) override;
    void draw() override;
    void apply() override;
    void cancel(const std::vector<std::string> settings_to_skip = {}) override;
    void saveSettings() override;

    LLSD getSettingsAsLLSD();

    void onCancelKeyBind() override;
    void onDefaultKeyBind(bool all_modes) override;
    bool onSetKeyBind(EMouseClickType click, KEY key, MASK mask, bool all_modes) override;

    // True while controller input should configure rather than play: a list-view
    // selector is waiting for an input, or the controller picture is on screen.
    static bool isWaitingForInputChannel();
    // Main loop hook while waiting: refresh the controller state and hand it on.
    static void applyGameControlInput();

protected:
    bool postBuild() override;

private:
    // ---- modes ----
    std::string currentEditMode();
    void selectMode(S32 mode);
    void onActionModeChanged();
    void onModeEnabledToggled(bool enabled);
    void updateActionModeEnabledUI();
    void updateCurrentModeIndicator();
    LLComboBox* actionSelectorForMode(bool axis, const std::string& mode) const;

    // ---- controller picture and inspector ----
    void refreshDiagram();          // callouts, conflicts and the inspector for the current mode
    void refreshInspector();
    void refreshTuning();
    void updateLiveInput();         // per frame: highlights, stick view, select-by-press
    void selectElement(S32 element);
    void updateControllerName();
    std::string buttonInputValue(S32 element) const;
    std::string buttonActionFor(const std::string& input_value, S32* count = nullptr);
    std::string axisActionFor(const std::string& input_value, S32* count = nullptr);
    std::string actionLabel(const std::string& action, bool axis);
    void fillButtonActionCombo(LLComboBox* combo, const std::string& input_value);
    void fillAxisActionCombo(LLComboBox* combo, const std::string& input_value);
    void fillBehaviorCombo(LLComboBox* combo, const std::string& action);
    void assignButtonAction(const std::string& input_value, const std::string& action);
    void assignAxisAction(const std::string& input_value, const std::string& action);
    void onButtonActionCommit(LLComboBox* combo, const std::string& input_value);
    void onBehaviorCommit(LLComboBox* combo, const std::string& input_value);
    void onAxisInvertCommit(LLCheckBoxCtrl* check, const std::string& input_value);
    void onTuningCommit();
    void onDeadZoneCommit();
    LLGameControl::Stick selectedStick() const;
    // The dead zone lives on the controller (per physical axis); this reads and
    // writes it for the stick's two canonical axes on the controller in use.
    F32 deadZoneFraction(LLGameControl::Stick stick) const;
    void setDeadZoneFraction(LLGameControl::Stick stick, F32 fraction);

    // ---- list view (the original tables) ----
    void setListMode(bool list);
    void populateActionMappings();
    void onGridSelect(LLUICtrl* ctrl);
    bool initCombobox(LLScrollListItem* item, LLScrollListCtrl* grid);
    void onCommitInputChannel(LLUICtrl* ctrl);
    void clearSelectionState();
    void removeDuplicateActionInput(const std::string& mode, const std::string& input_type,
        const std::string& keep_action, const std::string& input_value, const LLComboBox* input_selector);
    void openKeyDialog(const std::string& action);

    // ---- presets and resets ----
    void populatePresets();
    void onPresetSelected();
    void onPresetSave();
    void onPresetDelete();
    void onResetActionsToDefaults();
    void resetModeToDefaults(const std::string& mode);
    void onResetAll();

    void rememberOriginalSettings();

    // modes and this-mode options
    LLComboBox* mActionMode { nullptr };  // hidden; holds the edited AgentControlMode
    std::array<LLButton*, 5> mModeButtons {};
    LLCheckBoxCtrl* mCheckActionModeEnabled { nullptr };
    LLCheckBoxCtrl* mCheckFlycamAllowRoll { nullptr };
    LLSliderCtrl* mSliderFlycamSpeedFactor { nullptr };
    LLTextBox* mFlycamSlowLabel { nullptr };
    LLTextBox* mFlycamFastLabel { nullptr };
    LLCheckBoxCtrl* mCheckFlycamCrosshair { nullptr };
    LLTextBox* mCurrentModeIndicator { nullptr };
    LLGameControl::AgentControlMode mLastDrawnControlMode { LLGameControl::CONTROL_MODE_NONE };

    // top and bottom bars
    LLTextBox* mControllerName { nullptr };
    LLComboBox* mPresetList { nullptr };
    LLComboBox* mStyleList { nullptr };
    LLButton* mRestoreActionsDefaults { nullptr };
    LLButton* mViewToggle { nullptr };
    LLTextBox* mConflictText { nullptr };
    LLCheckBoxCtrl* mCheckGameControlToServer { nullptr };

    // picture and inspector
    LLView* mBodyStack { nullptr };
    SSGameControlDiagram* mDiagram { nullptr };
    LLTextBox* mInspectorTitle { nullptr };
    LLPanel* mInspectorButton { nullptr };
    LLPanel* mInspectorStick { nullptr };
    LLPanel* mInspectorTrigger { nullptr };
    LLPanel* mInspectorTuning { nullptr };
    LLComboBox* mButtonAction { nullptr };
    LLComboBox* mButtonBehavior { nullptr };
    LLTextBox* mButtonNote { nullptr };
    LLComboBox* mStickXAction { nullptr };
    LLComboBox* mStickYAction { nullptr };
    LLCheckBoxCtrl* mStickXInvert { nullptr };
    LLCheckBoxCtrl* mStickYInvert { nullptr };
    LLComboBox* mStickClickAction { nullptr };
    LLComboBox* mStickClickBehavior { nullptr };
    LLComboBox* mTriggerAxisAction { nullptr };
    LLCheckBoxCtrl* mTriggerAxisInvert { nullptr };
    LLComboBox* mTriggerButtonAction { nullptr };
    LLComboBox* mTriggerBehavior { nullptr };
    LLSliderCtrl* mTuneSensitivity { nullptr };
    LLSliderCtrl* mTuneCurve { nullptr };
    LLSliderCtrl* mTuneDeadZone { nullptr };
    LLSliderCtrl* mTuneSmoothing { nullptr };
    LLCheckBoxCtrl* mTuneRescale { nullptr };
    LLSliderCtrl* mTuneOuter { nullptr };
    LLSliderCtrl* mTuneBoost { nullptr };
    SSGameControlStickView* mStickView { nullptr };
    ALCurveEditorCtrl* mCurveView { nullptr };
    U32 mPrevLiveButtons { 0 };
    std::array<F32, 4> mPrevStickMagnitude {};  // for select-by-flick: LX, LY, RX, RY

    // list view
    LLPanel* mListView { nullptr };
    bool mListMode { false };
    LLScrollListCtrl* mActionMappingsAxes { nullptr };
    LLScrollListCtrl* mActionMappingsButtons { nullptr };
    LLComboBox* mAxisInputSelector { nullptr };
    LLComboBox* mButtonInputSelector { nullptr };
    LLComboBox* mAxisInputGlyphSelector { nullptr };
    LLComboBox* mButtonInputGlyphSelector { nullptr };
    LLComboBox* mAnalogActionSelector { nullptr };
    LLComboBox* mBinaryActionSelector { nullptr };
    LLComboBox* mMouselookAnalogActionSelector { nullptr };
    LLComboBox* mMouselookBinaryActionSelector { nullptr };
    LLComboBox* mFlycamAnalogActionSelector { nullptr };
    LLComboBox* mFlycamBinaryActionSelector { nullptr };
    LLComboBox* mCaptiveBinaryActionSelector { nullptr };
    LLComboBox* mCursorAnalogActionSelector { nullptr };
    LLComboBox* mCursorBinaryActionSelector { nullptr };

    LLSD mOrigSettings;  // captured for Cancel
    std::string mEditingKeyAction;
    std::string mPendingKeyInput;  // inspector: the button a new keyboard key goes on
};
