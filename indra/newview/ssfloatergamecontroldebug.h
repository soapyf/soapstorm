/**
 * @file ssfloatergamecontroldebug.h
 * @brief Develop > Game Controller Debug: live input, calibration, hardware remap
 *        and the data sent to the region.
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

#ifndef SS_FLOATERGAMECONTROLDEBUG_H
#define SS_FLOATERGAMECONTROLDEBUG_H

#include "llfloater.h"
#include "llgamecontrol.h"

#include <map>
#include <vector>

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLPanel;
class LLScrollListCell;
class LLScrollListCtrl;
class LLScrollListItem;
class LLSpinCtrl;
class LLTextBox;

// The diagnostics that used to be three tabs of the Game Controls preferences. They
// are not settings a player tunes: raw and adjusted axis values with calibration, the
// physical-to-canonical remap for a controller SDL maps wrongly, and the last
// GameControlData packet. Everything here is a per-device option or a readout, so an
// edit is applied to the device and saved at once rather than waiting for a
// Preferences OK -- with the one consequence that Cancel in Preferences, if it is
// open at the same time, restores the device options it captured on opening.
class SSFloaterGameControlDebug : public LLFloater
{
public:
    SSFloaterGameControlDebug(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    // A controller was connected or removed.
    static void updateDeviceList();
    // A GameControlData packet went out.
    static void updateDataOutput();

private:
    struct DeviceOptions
    {
        std::string name;
        LLGameControl::Options options;
    };

    void reloadDevices();
    void saveDevice(const std::string& guid);

    // Hardware remap tab
    void populateDeviceTitle();
    void populateDeviceSettings(const std::string& guid);
    void populateAxisChannelsRows();
    void populateAxisChannelsCells();
    void populateButtonChannelsRows();
    void populateButtonChannelsCells();
    void onAxisChannelsSelect();
    void onButtonChannelsSelect();
    void onResetDeviceToDefaults();
    LLGameControl::Options* selectedDeviceOptions();

    // Live input tab
    void updateDeviceStateList();
    void populateAxisStateRows();
    void populateAxisStateOptionCells();
    void populateButtonStateRows();
    void populateDeviceStateValues();
    void onAxisStateSelect();
    void onCommitNumericValue();
    void onResetDeviceOptionsToDefaults();
    void onAutoCalibrate();
    void updateAutoCalibration();
    S32 axisStateColumn(const std::string& name) const;

    // Server data tab
    void populateDataOutputRows();
    void populateDataOutputSemanticRows(LLGameControl::AgentControlMode mode);
    void populateDataOutputValues();

    // In-cell selectors on the remap tables
    bool initCombobox(LLScrollListItem* item, LLScrollListCtrl* grid);
    void onCommitOutput(LLUICtrl* ctrl);
    void clearSelectionState();

    std::map<std::string, DeviceOptions> mDeviceOptions;  // keyed by device GUID
    std::string mSelectedDeviceGUID;        // remap tab
    std::string mStateSelectedDeviceGUID;   // live input tab

    LLTextBox* mNoDeviceMessage { nullptr };
    LLTextBox* mDevicePrompt { nullptr };
    LLComboBox* mDeviceList { nullptr };
    LLCheckBoxCtrl* mCheckShowAllDevices { nullptr };
    LLPanel* mPanelDeviceSettings { nullptr };
    LLButton* mRestoreDeviceDefaults { nullptr };
    LLScrollListCtrl* mAxisChannels { nullptr };
    LLScrollListCtrl* mButtonChannels { nullptr };

    LLPanel* mTabDeviceState { nullptr };
    LLTextBox* mStateNoDeviceMessage { nullptr };
    LLTextBox* mStateDevicePrompt { nullptr };
    LLTextBox* mStateRemapNote { nullptr };
    LLComboBox* mStateDeviceList { nullptr };
    LLButton* mRestoreDeviceOptionsDefaults { nullptr };
    LLButton* mAutoCalibrate { nullptr };
    LLPanel* mPanelDeviceState { nullptr };
    LLScrollListCtrl* mAxisState { nullptr };
    LLScrollListCtrl* mButtonState { nullptr };

    LLPanel* mTabDataOutput { nullptr };
    LLScrollListCtrl* mDataOutput { nullptr };
    S32 mDataOutputSemanticMode { -1 };

    LLSpinCtrl* mNumericValueEditor { nullptr };
    LLComboBox* mAxisOutputSelector { nullptr };
    LLComboBox* mAxisOutputGlyphSelector { nullptr };
    LLComboBox* mButtonInputSelector { nullptr };
    LLComboBox* mButtonInputGlyphSelector { nullptr };

    LLScrollListCtrl* mSelectedGrid { nullptr };
    LLScrollListItem* mSelectedItem { nullptr };
    LLScrollListCell* mSelectedCell { nullptr };
    LLComboBox* mSelectedCombobox { nullptr };
    LLSD mSelectedCellValue;
    S32 mSelectedRow { -1 };
    S32 mSelectedCol { -1 };
    LLRect mSelectedOpenRect;

    static constexpr S32 CALIBRATION_SAMPLE_COUNT = 5;
    bool mCalibrating { false };
    S32 mCalibrationSamplesCollected { 0 };
    std::vector<std::vector<S16>> mCalibrationSamples;
};

#endif // SS_FLOATERGAMECONTROLDEBUG_H
