/**
 * @file ssfloatergamecontroldebug.cpp
 * @brief See ssfloatergamecontroldebug.h. The table code is the Device Mappings,
 *        Device Options and Server Data tabs of LLPanelPreferenceGameControl, moved
 *        here intact apart from saving each edit as it is made.
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

#include "ssfloatergamecontroldebug.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llspinctrl.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "ssgamecontrolui.h"

using namespace SSGameControlUI;

SSFloaterGameControlDebug::SSFloaterGameControlDebug(const LLSD& key)
:   LLFloater(key)
{
}

// static
void SSFloaterGameControlDebug::updateDeviceList()
{
    if (SSFloaterGameControlDebug* floater = LLFloaterReg::findTypedInstance<SSFloaterGameControlDebug>("game_control_debug"))
    {
        floater->reloadDevices();
    }
}

// static
// Called on every outgoing GameControlData, so only refresh while the tab is showing.
void SSFloaterGameControlDebug::updateDataOutput()
{
    SSFloaterGameControlDebug* floater = LLFloaterReg::findTypedInstance<SSFloaterGameControlDebug>("game_control_debug");
    if (floater && floater->mTabDataOutput && floater->mTabDataOutput->isInVisibleChain())
    {
        floater->populateDataOutputValues();
    }
}

bool SSFloaterGameControlDebug::postBuild()
{
    getChild<LLTabContainer>("game_control_debug_tabs")->setCommitCallback([this](LLUICtrl*, const LLSD&) { clearSelectionState(); });

    // Hardware remap tab
    mNoDeviceMessage = getChild<LLTextBox>("nodevice_message");
    mDevicePrompt = getChild<LLTextBox>("device_prompt");
    mDeviceList = getChild<LLComboBox>("device_list");
    mCheckShowAllDevices = getChild<LLCheckBoxCtrl>("show_all_known_devices");
    mPanelDeviceSettings = getChild<LLPanel>("device_settings");
    mCheckShowAllDevices->setCommitCallback([this](LLUICtrl*, const LLSD&) { populateDeviceTitle(); });
    mDeviceList->setCommitCallback([this](LLUICtrl*, const LLSD& value) { populateDeviceSettings(value); });
    mRestoreDeviceDefaults = getChild<LLButton>("restore_device_defaults");
    mRestoreDeviceDefaults->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetDeviceToDefaults(); });
    mAxisChannels = getChild<LLScrollListCtrl>("axis_channels");
    mAxisChannels->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAxisChannelsSelect(); });
    mButtonChannels = getChild<LLScrollListCtrl>("button_channels");
    mButtonChannels->setCommitCallback([this](LLUICtrl*, const LLSD&) { onButtonChannelsSelect(); });

    // Live input tab
    mTabDeviceState = getChild<LLPanel>("tab_device_options");
    mStateNoDeviceMessage = getChild<LLTextBox>("state_nodevice_message");
    mStateDevicePrompt = getChild<LLTextBox>("state_device_prompt");
    mStateRemapNote = findChild<LLTextBox>("state_remap_note"); // <GMDX> optional: not in the XUI
    mStateDeviceList = getChild<LLComboBox>("options_device_list");
    mPanelDeviceState = getChild<LLPanel>("device_state_settings");
    mStateDeviceList->setCommitCallback([this](LLUICtrl*, const LLSD& value)
        {
            mStateSelectedDeviceGUID = value.asString();
            clearSelectionState();
            mCalibrating = false;  // a different device invalidates the pass in progress
            populateAxisStateOptionCells();
        });
    mRestoreDeviceOptionsDefaults = getChild<LLButton>("restore_device_options_defaults");
    mRestoreDeviceOptionsDefaults->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetDeviceOptionsToDefaults(); });
    mAutoCalibrate = getChild<LLButton>("auto_calibrate");
    mAutoCalibrate->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAutoCalibrate(); });
    mAxisState = getChild<LLScrollListCtrl>("axis_state");
    mAxisState->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAxisStateSelect(); });
    mButtonState = getChild<LLScrollListCtrl>("button_state");

    // Server data tab
    mTabDataOutput = getChild<LLPanel>("tab_server_data");
    mDataOutput = getChild<LLScrollListCtrl>("data_output");

    mNumericValueEditor = getChild<LLSpinCtrl>("numeric_value_editor");
    mNumericValueEditor->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitNumericValue(); });

    mAxisOutputSelector = getChild<LLComboBox>("axis_output_selector");
    mAxisOutputSelector->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onCommitOutput(ctrl); });
    mAxisOutputGlyphSelector = getChild<LLComboBox>("axis_output_glyph_selector");
    mAxisOutputGlyphSelector->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onCommitOutput(ctrl); });
    buildInputGlyphSelector(mAxisOutputSelector, mAxisOutputGlyphSelector);
    mButtonInputSelector = getChild<LLComboBox>("button_input_selector");
    mButtonInputSelector->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onCommitOutput(ctrl); });
    mButtonInputGlyphSelector = getChild<LLComboBox>("button_input_glyph_selector");
    mButtonInputGlyphSelector->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onCommitOutput(ctrl); });
    buildInputGlyphSelector(mButtonInputSelector, mButtonInputGlyphSelector);

    populateAxisChannelsRows();
    populateButtonChannelsRows();
    populateAxisStateRows();
    populateButtonStateRows();
    populateDataOutputRows();

    // LLScrollListCtrl with draw_heading="true" starts 17 px too high; each list
    // fills a wrapper panel, so its corrected top is the parent's height - 1.
    for (LLScrollListCtrl* grid : { mAxisChannels, mButtonChannels, mAxisState, mButtonState, mDataOutput })
    {
        LLRect rect = grid->getRect();
        rect.mTop = grid->getParent()->getRect().getHeight() - 1;
        grid->setRect(rect);
        grid->updateLayout();
    }

    clearSelectionState();
    return true;
}

void SSFloaterGameControlDebug::onOpen(const LLSD& key)
{
    clearSelectionState();
    reloadDevices();
}

void SSFloaterGameControlDebug::onClose(bool app_quitting)
{
    clearSelectionState();
    mCalibrating = false;
}

void SSFloaterGameControlDebug::draw()
{
    if (mTabDeviceState && mTabDeviceState->isInVisibleChain())
    {
        populateDeviceStateValues();
        if (mCalibrating)
        {
            updateAutoCalibration();
        }
    }
    // a selector left open over a cell that has since scrolled away is closed
    if (mSelectedCombobox && mSelectedGrid && mSelectedRow >= 0 && mSelectedCol >= 0
        && cellRect(mSelectedGrid, mSelectedCombobox->getParent(), mSelectedRow, mSelectedCol) != mSelectedOpenRect)
    {
        clearSelectionState();
    }
    LLFloater::draw();
}

// Saved device options, plus any connected device that has none yet.
void SSFloaterGameControlDebug::reloadDevices()
{
    mDeviceOptions.clear();
    for (const auto& [guid, config] : LLGameControl::getDeviceOptions())
    {
        DeviceOptions entry;
        if (!entry.options.loadFromString(entry.name, config))
        {
            LL_WARNS("SDL3") << "Discarding invalid saved device options for '" << guid << "'; using defaults" << LL_ENDL;
        }
        if (entry.name.empty())
        {
            entry.name = guid;
        }
        mDeviceOptions.emplace(guid, entry);
    }
    for (const auto& device : LLGameControl::getDevices())
    {
        if (mDeviceOptions.find(device.getGUID()) == mDeviceOptions.end())
        {
            mDeviceOptions[device.getGUID()] = { device.getName(), device.getOptions() };
        }
    }

    mCheckShowAllDevices->setValue(false);
    populateDeviceTitle();
    updateDeviceStateList();
}

// Apply to the live device and write it to settings now.
void SSFloaterGameControlDebug::saveDevice(const std::string& guid)
{
    auto it = mDeviceOptions.find(guid);
    if (it == mDeviceOptions.end())
    {
        return;
    }
    LLGameControl::setDeviceOptions(guid, it->second.options);
    LLGameControl::setDeviceConfig(guid, it->second.options.saveToString(it->second.name, true));
    LLGameControl::saveToSettings();
}

LLGameControl::Options* SSFloaterGameControlDebug::selectedDeviceOptions()
{
    auto it = mDeviceOptions.find(mSelectedDeviceGUID);
    return it == mDeviceOptions.end() ? nullptr : &it->second.options;
}

// ---- Hardware remap tab ---------------------------------------------------

void SSFloaterGameControlDebug::populateDeviceTitle()
{
    mSelectedDeviceGUID.clear();

    bool show_all = mCheckShowAllDevices->getValue().asBoolean();
    size_t count = show_all ? mDeviceOptions.size() : LLGameControl::getDevices().size();

    mNoDeviceMessage->setVisible(!count);
    mDevicePrompt->setVisible(count);
    mDeviceList->setVisible(count);
    mPanelDeviceSettings->setVisible(count);
    mRestoreDeviceDefaults->setVisible(count);
    if (!count)
    {
        return;
    }

    auto item = [](const std::string& guid, const std::string& name)
    {
        return LLSD().with("value", guid).with("columns", LLSD().with("label", guid + ", " + name));
    };
    mDeviceList->clear();
    mDeviceList->clearRows();
    if (show_all)
    {
        for (const auto& [guid, device] : mDeviceOptions)
        {
            mDeviceList->addElement(item(guid, device.name));
        }
    }
    else
    {
        for (const LLGameControl::Device& device : LLGameControl::getDevices())
        {
            mDeviceList->addElement(item(device.getGUID(), device.getName()));
        }
    }
    mDeviceList->selectNthItem(0);
    populateDeviceSettings(mDeviceList->getValue());
}

void SSFloaterGameControlDebug::populateDeviceSettings(const std::string& guid)
{
    mSelectedDeviceGUID = guid;
    populateAxisChannelsCells();
    populateButtonChannelsCells();
}

// One row per physical axis: its fixed input (glyph + text), and the canonical axis
// it maps to (glyph + text), edited in place.
void SSFloaterGameControlDebug::populateAxisChannelsRows()
{
    mAxisChannels->clearRows();
    std::vector<LLScrollListItem*> items = mAxisOutputSelector->getAllData();
    S32 input_glyph_col = mAxisChannels->getColumn("input")->mIndex;
    S32 input_desc_col  = mAxisChannels->getColumn("input_description")->mIndex;

    LLScrollListItem::Params row_params;
    LLScrollListCell::Params cell_params;
    for (S32 i = 0; i < (S32)mAxisChannels->getNumColumns(); ++i)
    {
        const std::string& name = mAxisChannels->getColumn(i)->mName;
        cell_params.font = (name == "input" || name == "output") ? promptFontForCell() : LLFontGL::getFontSansSerif();
        cell_params.column = name;
        row_params.columns.add(cell_params);
    }
    for (size_t i = 0; i < LLGameControl::NUM_AXES; ++i)
    {
        LLScrollListItem* row = mAxisChannels->addRow(row_params);
        std::string glyph = promptFontGlyph(items[i]->getValue().asString());
        row->getColumn(input_desc_col)->setValue(items[i]->getColumn(0)->getValue());
        row->getColumn(input_glyph_col)->setValue(glyph.empty() ? LLSD() : LLSD(glyph));
    }
}

void SSFloaterGameControlDebug::populateAxisChannelsCells()
{
    const LLGameControl::Options* options = selectedDeviceOptions();
    if (!options)
    {
        return;
    }
    std::vector<LLScrollListItem*> rows = mAxisChannels->getAllData();
    const auto& axis_map = options->getAxisMap();
    S32 output_desc_col  = mAxisChannels->getColumn("output_description")->mIndex;
    S32 output_glyph_col = mAxisChannels->getColumn("output")->mIndex;
    for (size_t i = 0; i < rows.size() && i < axis_map.size(); ++i)
    {
        std::string output_name = LLGameControl::axisOutputName(axis_map[i]);
        std::string glyph = promptFontGlyph(output_name);
        rows[i]->getColumn(output_desc_col)->setValue(inputLabel(mAxisOutputSelector, output_name));
        rows[i]->getColumn(output_glyph_col)->setValue(glyph.empty() ? LLSD() : LLSD(glyph));
    }
}

void SSFloaterGameControlDebug::populateButtonChannelsRows()
{
    mButtonChannels->clearRows();
    std::vector<LLScrollListItem*> items = mButtonInputSelector->getAllData();
    S32 input_glyph_col = mButtonChannels->getColumn("input")->mIndex;
    S32 input_desc_col  = mButtonChannels->getColumn("input_description")->mIndex;

    LLScrollListItem::Params row_params;
    LLScrollListCell::Params cell_params;
    for (S32 i = 0; i < (S32)mButtonChannels->getNumColumns(); ++i)
    {
        const std::string& name = mButtonChannels->getColumn(i)->mName;
        cell_params.font = (name == "input" || name == "output") ? promptFontForCell() : LLFontGL::getFontSansSerif();
        cell_params.column = name;
        row_params.columns.add(cell_params);
    }
    for (size_t i = 0; i < LLGameControl::NUM_BUTTONS; ++i)
    {
        LLScrollListItem* row = mButtonChannels->addRow(row_params);
        std::string glyph = promptFontGlyph(items[i]->getValue().asString());
        row->getColumn(input_desc_col)->setValue(items[i]->getColumn(0)->getValue());
        row->getColumn(input_glyph_col)->setValue(glyph.empty() ? LLSD() : LLSD(glyph));
    }
}

void SSFloaterGameControlDebug::populateButtonChannelsCells()
{
    const LLGameControl::Options* options = selectedDeviceOptions();
    if (!options)
    {
        return;
    }
    std::vector<LLScrollListItem*> rows = mButtonChannels->getAllData();
    const auto& button_map = options->getButtonMap();
    std::vector<LLScrollListItem*> items = mButtonInputSelector->getAllData();
    S32 output_desc_col  = mButtonChannels->getColumn("output_description")->mIndex;
    S32 output_glyph_col = mButtonChannels->getColumn("output")->mIndex;
    for (size_t i = 0; i < rows.size() && i < button_map.size(); ++i)
    {
        U8 output = button_map[i];
        rows[i]->getColumn(output_desc_col)->setValue(selectorLabelAt(mButtonInputSelector, output));
        std::string value = (output < items.size()) ? items[output]->getValue().asString() : LLStringUtil::null;
        std::string glyph = promptFontGlyph(value);
        rows[i]->getColumn(output_glyph_col)->setValue(glyph.empty() ? LLSD() : LLSD(glyph));
    }
}

void SSFloaterGameControlDebug::onAxisChannelsSelect()
{
    clearSelectionState();
    mButtonChannels->deselectAllItems(true);
    if (LLScrollListItem* row = mAxisChannels->getFirstSelected())
    {
        initCombobox(row, mAxisChannels);
    }
}

void SSFloaterGameControlDebug::onButtonChannelsSelect()
{
    clearSelectionState();
    mAxisChannels->deselectAllItems(true);
    if (LLScrollListItem* row = mButtonChannels->getFirstSelected())
    {
        initCombobox(row, mButtonChannels);
    }
}

void SSFloaterGameControlDebug::onResetDeviceToDefaults()
{
    clearSelectionState();
    LLGameControl::Options* options = selectedDeviceOptions();
    if (!options)
    {
        return;
    }
    options->resetToDefaults();
    saveDevice(mSelectedDeviceGUID);
    populateAxisChannelsCells();
    populateButtonChannelsCells();
    populateAxisStateOptionCells();
}

// Only the Output columns (glyph + text) are editable; the Input columns are the
// fixed physical axis or button.
bool SSFloaterGameControlDebug::initCombobox(LLScrollListItem* item, LLScrollListCtrl* grid)
{
    const bool axes = (grid == mAxisChannels);
    S32 sel = item->getSelectedCell();
    S32 desc_col  = grid->getColumn("output_description")->mIndex;
    S32 glyph_col = grid->getColumn("output")->mIndex;
    LLComboBox* combobox = nullptr;
    if (sel == glyph_col)
    {
        combobox = axes ? mAxisOutputGlyphSelector : mButtonInputGlyphSelector;
    }
    else if (sel == desc_col)
    {
        combobox = axes ? mAxisOutputSelector : mButtonInputSelector;
    }
    if (!combobox)
    {
        return false;
    }
    LLScrollListText* cell = dynamic_cast<LLScrollListText*>(item->getColumn(sel));
    if (!cell)
    {
        return false;
    }

    S32 row_index = grid->getItemIndex(item);
    fitInRect(combobox, grid, row_index, sel);
    mSelectedRow = row_index;
    mSelectedCol = sel;
    mSelectedOpenRect = cellRect(grid, combobox->getParent(), row_index, sel);

    // pre-select the item whose label matches the cell
    std::string cell_value = cell->getValue();
    std::string value;
    for (const LLScrollListItem* combo_item : combobox->getAllData())
    {
        if (combo_item->getColumn(0)->getValue().asString() == cell_value)
        {
            value = combo_item->getValue().asString();
            break;
        }
    }
    if (value.empty())
    {
        value = combobox->getAllData().back()->getValue().asString(); // "None"
    }
    combobox->setValue(value);
    combobox->setVisible(true);
    combobox->showList();

    mSelectedGrid = grid;
    mSelectedItem = item;
    mSelectedCell = cell;
    mSelectedCombobox = combobox;
    // blank the cell under the translucent selector; clearSelectionState() restores it
    mSelectedCellValue = cell->getValue();
    cell->setValue(LLSD());
    return true;
}

void SSFloaterGameControlDebug::onCommitOutput(LLUICtrl* ctrl)
{
    LLComboBox* combobox = dynamic_cast<LLComboBox*>(ctrl);
    LLGameControl::Options* options = selectedDeviceOptions();
    if (!mSelectedGrid || !mSelectedItem || !mSelectedCell || !combobox || !options)
    {
        return;
    }

    if (mSelectedGrid == mAxisChannels)
    {
        S32 axis = mAxisChannels->getItemIndex(mSelectedItem);
        options->getAxisMap()[axis] = LLGameControl::axisOutputFromName(combobox->getValue().asString());
    }
    else if (mSelectedGrid == mButtonChannels)
    {
        S32 button = mButtonChannels->getItemIndex(mSelectedItem);
        S32 output = combobox->getCurrentIndex();
        if (output >= 0 && output < (S32)LLGameControl::NUM_BUTTONS)
        {
            options->getButtonMap()[button] = (U8)output;
        }
    }
    saveDevice(mSelectedDeviceGUID);

    mSelectedCellValue = mSelectedCell->getValue();
    mSelectedGrid->deselectAllItems();
    clearSelectionState();
    populateAxisChannelsCells();
    populateButtonChannelsCells();
}

void SSFloaterGameControlDebug::clearSelectionState()
{
    if (mSelectedCell)
    {
        mSelectedCell->setValue(mSelectedCellValue);
    }
    mSelectedCellValue = LLSD();
    if (mSelectedGrid)
    {
        mSelectedGrid->deselectAllItems(true);
    }
    mSelectedGrid = nullptr;
    mSelectedItem = nullptr;
    mSelectedCell = nullptr;
    mSelectedCombobox = nullptr;
    mSelectedRow = -1;
    mSelectedCol = -1;
    for (LLUICtrl* ctrl : { (LLUICtrl*)mNumericValueEditor, (LLUICtrl*)mAxisOutputSelector, (LLUICtrl*)mAxisOutputGlyphSelector,
                            (LLUICtrl*)mButtonInputSelector, (LLUICtrl*)mButtonInputGlyphSelector })
    {
        if (ctrl)
        {
            ctrl->setVisible(false);
        }
    }
}

// ---- Live input tab -------------------------------------------------------

// Only connected devices: the tab shows live input.
void SSFloaterGameControlDebug::updateDeviceStateList()
{
    mStateDeviceList->clear();
    mStateDeviceList->clearRows();
    const std::list<LLGameControl::Device>& devices = LLGameControl::getDevices();
    bool has_device = !devices.empty();
    mStateNoDeviceMessage->setVisible(!has_device);
    mStateDevicePrompt->setVisible(has_device);
    if (mStateRemapNote) mStateRemapNote->setVisible(has_device);
    mStateDeviceList->setVisible(has_device);
    mRestoreDeviceOptionsDefaults->setVisible(has_device);
    mPanelDeviceState->setVisible(has_device);
    if (!has_device)
    {
        mStateSelectedDeviceGUID.clear();
        return;
    }
    for (const LLGameControl::Device& device : devices)
    {
        mStateDeviceList->addElement(LLSD().with("value", device.getGUID())
            .with("columns", LLSD().with("label", device.getGUID() + ", " + device.getName())));
    }
    if (mStateSelectedDeviceGUID.empty() || !mStateDeviceList->setSelectedByValue(mStateSelectedDeviceGUID, true))
    {
        mStateDeviceList->selectNthItem(0);
    }
    mStateSelectedDeviceGUID = mStateDeviceList->getValue().asString();
    populateAxisStateOptionCells();
}

S32 SSFloaterGameControlDebug::axisStateColumn(const std::string& name) const
{
    LLScrollListColumn* column = mAxisState->getColumn(name);
    return column ? column->mIndex : -1;
}

// Rows are physical axes. Columns are addressed by name, so the XML owns their order.
void SSFloaterGameControlDebug::populateAxisStateRows()
{
    mAxisState->clearRows();
    std::vector<LLScrollListItem*> items = mAxisOutputSelector->getAllData();

    LLScrollListItem::Params row_params;
    LLScrollListCell::Params cell_params;
    for (S32 i = 0; i < (S32)mAxisState->getNumColumns(); ++i)
    {
        const std::string& name = mAxisState->getColumn(i)->mName;
        cell_params.font = (name == "input_glyph") ? promptFontForCell() : LLFontGL::getFontSansSerif();
        cell_params.column = name;
        row_params.columns.add(cell_params);
    }
    row_params.columns(axisStateColumn("raw_value")).font_halign = "right";
    row_params.columns(axisStateColumn("invert")).type = "checkbox";
    row_params.columns(axisStateColumn("invert")).font_halign = "right";
    row_params.columns(axisStateColumn("offset")).font_halign = "right";
    row_params.columns(axisStateColumn("dead_zone")).font_halign = "right";
    row_params.columns(axisStateColumn("fixed_value")).font_halign = "right";

    S32 input_glyph_col = axisStateColumn("input_glyph");
    S32 input_col = axisStateColumn("input");
    for (size_t i = 0; i < LLGameControl::NUM_AXES; ++i)
    {
        LLScrollListItem* row = mAxisState->addRow(row_params);
        std::string glyph = promptFontGlyph(items[i]->getValue().asString());
        row->getColumn(input_glyph_col)->setValue(glyph.empty() ? LLSD() : LLSD(glyph));
        row->getColumn(input_col)->setValue(items[i]->getColumn(0)->getValue());
    }
    populateAxisStateOptionCells();
}

void SSFloaterGameControlDebug::populateAxisStateOptionCells()
{
    auto it = mDeviceOptions.find(mStateSelectedDeviceGUID);
    if (it == mDeviceOptions.end())
    {
        return;
    }
    const auto& all_axis_options = it->second.options.getAxisOptions();
    std::vector<LLScrollListItem*> rows = mAxisState->getAllData();
    S32 invert_col = axisStateColumn("invert");
    S32 offset_col = axisStateColumn("offset");
    S32 dead_zone_col = axisStateColumn("dead_zone");
    for (size_t i = 0; i < rows.size() && i < all_axis_options.size(); ++i)
    {
        const LLGameControl::Options::AxisOptions& axis_options = all_axis_options[i];
        rows[i]->getColumn(invert_col)->setValue(axis_options.mMultiplier == -1);
        setNumericLabel(rows[i]->getColumn(offset_col), axis_options.mOffset);
        setNumericLabel(rows[i]->getColumn(dead_zone_col), axis_options.mDeadZone);
    }
}

void SSFloaterGameControlDebug::populateButtonStateRows()
{
    mButtonState->clearRows();
    std::vector<LLScrollListItem*> items = mButtonInputSelector->getAllData();
    S32 input_glyph_col = mButtonState->getColumn("input_glyph")->mIndex;
    S32 input_col = mButtonState->getColumn("input")->mIndex;

    LLScrollListItem::Params row_params;
    LLScrollListCell::Params cell_params;
    for (S32 i = 0; i < (S32)mButtonState->getNumColumns(); ++i)
    {
        const std::string& name = mButtonState->getColumn(i)->mName;
        cell_params.font = (name == "input_glyph") ? promptFontForCell() : LLFontGL::getFontSansSerif();
        cell_params.column = name;
        row_params.columns.add(cell_params);
    }
    row_params.columns(mButtonState->getColumn("value")->mIndex).font_halign = "right";
    for (size_t i = 0; i < LLGameControl::NUM_BUTTONS; ++i)
    {
        LLScrollListItem* row = mButtonState->addRow(row_params);
        std::string glyph = promptFontGlyph(items[i]->getValue().asString());
        row->getColumn(input_glyph_col)->setValue(glyph.empty() ? LLSD() : LLSD(glyph));
        row->getColumn(input_col)->setValue(items[i]->getColumn(0)->getValue());
    }
}

// Rows are physical axes and buttons, so read the pre-map physical state: the
// canonical State::mAxes/mButtons would land on the wrong rows after a remap.
void SSFloaterGameControlDebug::populateDeviceStateValues()
{
    if (mStateSelectedDeviceGUID.empty())
    {
        return;
    }
    const LLGameControl::State* state = nullptr;
    for (const LLGameControl::Device& device : LLGameControl::getDevices())
    {
        if (device.getGUID() == mStateSelectedDeviceGUID)
        {
            state = &device.getState();
            break;
        }
    }
    if (!state)
    {
        return;
    }
    std::vector<LLScrollListItem*> axis_rows = mAxisState->getAllData();
    S32 raw_col = axisStateColumn("raw_value");
    S32 fixed_col = axisStateColumn("fixed_value");
    for (size_t i = 0; i < axis_rows.size() && i < state->mPhysicalRawAxes.size(); ++i)
    {
        axis_rows[i]->getColumn(raw_col)->setValue(llformat("%d ", (S32)state->mPhysicalRawAxes[i]));
        axis_rows[i]->getColumn(fixed_col)->setValue(llformat("%d ", (S32)state->mPhysicalFixedAxes[i]));
    }
    std::vector<LLScrollListItem*> button_rows = mButtonState->getAllData();
    S32 value_col = mButtonState->getColumn("value")->mIndex;
    for (size_t i = 0; i < button_rows.size(); ++i)
    {
        button_rows[i]->getColumn(value_col)->setValue(llformat("%d ", (S32)((state->mPhysicalButtons >> i) & 0x1)));
    }
}

// Invert applies at once; Offset and Dead Zone open a spinner over the cell.
void SSFloaterGameControlDebug::onAxisStateSelect()
{
    clearSelectionState();
    mButtonState->deselectAllItems(true);
    auto it = mDeviceOptions.find(mStateSelectedDeviceGUID);
    LLScrollListItem* row = mAxisState->getFirstSelected();
    if (it == mDeviceOptions.end() || !row)
    {
        return;
    }
    LLGameControl::Options& options = it->second.options;
    S32 row_index = mAxisState->getItemIndex(row);

    S32 invert_col = axisStateColumn("invert");
    S32 multiplier = row->getColumn(invert_col)->getValue().asBoolean() ? -1 : 1;
    if (multiplier != options.getAxisOptions()[row_index].mMultiplier)
    {
        options.getAxisOptions()[row_index].mMultiplier = multiplier;
        if (row_index == LLGameControl::AXIS_LEFT_TRIGGER || row_index == LLGameControl::AXIS_RIGHT_TRIGGER)
        {
            // the two triggers act as one bidirectional axis: keep their inversion together
            S32 other = row_index == LLGameControl::AXIS_LEFT_TRIGGER ? LLGameControl::AXIS_RIGHT_TRIGGER : LLGameControl::AXIS_LEFT_TRIGGER;
            options.getAxisOptions()[other].mMultiplier = multiplier;
            if (LLScrollListItem* other_row = mAxisState->getItemByIndex(other))
            {
                other_row->getColumn(invert_col)->setValue(multiplier == -1);
            }
        }
        saveDevice(mStateSelectedDeviceGUID);
    }

    S32 offset_col = axisStateColumn("offset");
    S32 dead_zone_col = axisStateColumn("dead_zone");
    S32 column = row->getSelectedCell();
    if (column == offset_col || column == dead_zone_col)
    {
        fitInRect(mNumericValueEditor, mAxisState, row_index, column);
        if (column == dead_zone_col)
        {
            mNumericValueEditor->setMinValue(0);
            mNumericValueEditor->setMaxValue(LLGameControl::MAX_AXIS_DEAD_ZONE);
            mNumericValueEditor->setValue(options.getAxisOptions()[row_index].mDeadZone);
        }
        else
        {
            mNumericValueEditor->setMinValue(-LLGameControl::MAX_AXIS_OFFSET);
            mNumericValueEditor->setMaxValue(LLGameControl::MAX_AXIS_OFFSET);
            mNumericValueEditor->setValue(options.getAxisOptions()[row_index].mOffset);
        }
        mNumericValueEditor->setVisible(true);
    }
}

void SSFloaterGameControlDebug::onCommitNumericValue()
{
    auto it = mDeviceOptions.find(mStateSelectedDeviceGUID);
    LLScrollListItem* row = mAxisState->getFirstSelected();
    if (it == mDeviceOptions.end() || !row)
    {
        return;
    }
    LLGameControl::Options& options = it->second.options;
    S32 value = mNumericValueEditor->getValue().asInteger();
    S32 row_index = mAxisState->getItemIndex(row);
    S32 offset_col = axisStateColumn("offset");
    S32 dead_zone_col = axisStateColumn("dead_zone");
    S32 column = row->getSelectedCell();
    if (column == dead_zone_col)
    {
        value = std::clamp<S32>(value, 0, LLGameControl::MAX_AXIS_DEAD_ZONE);
        options.getAxisOptions()[row_index].mDeadZone = (U16)value;
    }
    else if (column == offset_col)
    {
        value = std::clamp<S32>(value, -LLGameControl::MAX_AXIS_OFFSET, LLGameControl::MAX_AXIS_OFFSET);
        options.getAxisOptions()[row_index].mOffset = (S16)value;
    }
    else
    {
        return;
    }
    setNumericLabel(row->getColumn(column), value);
    saveDevice(mStateSelectedDeviceGUID);
}

// Resets Invert / Offset / Dead Zone for every axis; leaves the remaps alone.
void SSFloaterGameControlDebug::onResetDeviceOptionsToDefaults()
{
    clearSelectionState();
    auto it = mDeviceOptions.find(mStateSelectedDeviceGUID);
    if (it == mDeviceOptions.end())
    {
        return;
    }
    for (LLGameControl::Options::AxisOptions& axis_options : it->second.options.getAxisOptions())
    {
        axis_options.resetToDefaults();
    }
    saveDevice(mStateSelectedDeviceGUID);
    populateAxisStateOptionCells();
}

void SSFloaterGameControlDebug::onAutoCalibrate()
{
    clearSelectionState();
    if (mStateSelectedDeviceGUID.empty())
    {
        return;
    }
    mCalibrationSamples.assign(LLGameControl::NUM_AXES, {});
    mCalibrationSamplesCollected = 0;
    mCalibrating = true;
}

// Samples the raw axes for CALIBRATION_SAMPLE_COUNT frames; an axis that never moved
// is at rest, so its reading becomes its offset and twice that its dead zone.
void SSFloaterGameControlDebug::updateAutoCalibration()
{
    const LLGameControl::State* state = nullptr;
    for (const LLGameControl::Device& device : LLGameControl::getDevices())
    {
        if (device.getGUID() == mStateSelectedDeviceGUID)
        {
            state = &device.getState();
            break;
        }
    }
    if (!state)
    {
        mCalibrating = false;
        return;
    }
    for (size_t i = 0; i < mCalibrationSamples.size() && i < state->mPhysicalRawAxes.size(); ++i)
    {
        mCalibrationSamples[i].push_back(state->mPhysicalRawAxes[i]);
    }
    if (++mCalibrationSamplesCollected < CALIBRATION_SAMPLE_COUNT)
    {
        return;
    }
    mCalibrating = false;

    auto it = mDeviceOptions.find(mStateSelectedDeviceGUID);
    if (it != mDeviceOptions.end())
    {
        auto& all_axis_options = it->second.options.getAxisOptions();
        for (size_t i = 0; i < mCalibrationSamples.size() && i < all_axis_options.size(); ++i)
        {
            const std::vector<S16>& samples = mCalibrationSamples[i];
            bool steady = !samples.empty();
            for (size_t s = 1; s < samples.size() && steady; ++s)
            {
                steady = (samples[s] == samples[0]);
            }
            if (!steady)
            {
                continue; // being moved: leave it alone
            }
            S32 offset = std::clamp<S32>(-(S32)samples[0], -(S32)LLGameControl::MAX_AXIS_OFFSET, (S32)LLGameControl::MAX_AXIS_OFFSET);
            S32 dead_zone = std::clamp(2 * std::abs(offset), (S32)LLGameControl::DEFAULT_AXIS_DEAD_ZONE, (S32)LLGameControl::MAX_AXIS_DEAD_ZONE);
            all_axis_options[i].mOffset = (S16)offset;
            all_axis_options[i].mDeadZone = (U16)dead_zone;
        }
        saveDevice(mStateSelectedDeviceGUID);
        populateAxisStateOptionCells();
    }
    mCalibrationSamples.clear();
    mCalibrationSamplesCollected = 0;
}

// ---- Server data tab ------------------------------------------------------

// Canonical axes, a blank row, canonical buttons, then the mode-dependent block.
void SSFloaterGameControlDebug::populateDataOutputRows()
{
    mDataOutput->clearRows();
    mDataOutputSemanticMode = -1;

    LLScrollListItem::Params row_params;
    LLScrollListCell::Params cell_params;
    cell_params.font = LLFontGL::getFontSansSerif();
    for (S32 i = 0; i < (S32)mDataOutput->getNumColumns(); ++i)
    {
        cell_params.column = mDataOutput->getColumn(i)->mName;
        row_params.columns.add(cell_params);
    }
    row_params.columns(2).font_halign = "right";

    for (size_t i = 0; i < LLGameControl::NUM_AXES; ++i)
    {
        LLScrollListItem* row = mDataOutput->addRow(row_params);
        row->getColumn(0)->setValue(llformat("%d", (S32)i));
        row->getColumn(1)->setValue(LLGameControl::axisOutputName((U8)i));
    }
    mDataOutput->addRow(row_params);
    for (size_t i = 0; i < LLGameControl::NUM_BUTTONS; ++i)
    {
        LLScrollListItem* row = mDataOutput->addRow(row_params);
        row->getColumn(0)->setValue(llformat("%d", (S32)i));
        std::string name = LLGameControl::InputChannel(LLGameControl::InputChannel::TYPE_BUTTON, (U8)i).getRemoteName();
        LLStringUtil::trim(name);
        row->getColumn(1)->setValue(name.empty() ? llformat("BUTTON_%d", (S32)i) : name);
    }
    populateDataOutputSemanticRows((LLGameControl::AgentControlMode)LLGameControl::getServerState().mActionMode);
}

// The semantic block's rows and labels depend on the mode, so it is rebuilt on a
// mode change rather than having stale rows reused.
void SSFloaterGameControlDebug::populateDataOutputSemanticRows(LLGameControl::AgentControlMode mode)
{
    static const S32 CANONICAL_ROW_COUNT = (S32)LLGameControl::NUM_AXES + 1 + (S32)LLGameControl::NUM_BUTTONS;
    while (mDataOutput->getItemCount() > CANONICAL_ROW_COUNT)
    {
        mDataOutput->deleteSingleItem(mDataOutput->getItemCount() - 1);
    }

    LLScrollListItem::Params row_params;
    LLScrollListCell::Params cell_params;
    cell_params.font = LLFontGL::getFontSansSerif();
    for (S32 i = 0; i < (S32)mDataOutput->getNumColumns(); ++i)
    {
        cell_params.column = mDataOutput->getColumn(i)->mName;
        row_params.columns.add(cell_params);
    }
    row_params.columns(2).font_halign = "right";

    mDataOutput->addRow(row_params);
    for (U8 i = 0; i < LLGameControl::numSemanticAxesForMode(mode); ++i)
    {
        LLScrollListItem* row = mDataOutput->addRow(row_params);
        row->getColumn(0)->setValue(llformat("%d", (S32)i));
        row->getColumn(1)->setValue(LLGameControl::semanticAxisName(mode, i));
    }
    mDataOutput->addRow(row_params);
    for (U8 i = 0; i < LLGameControl::numSemanticButtonsForMode(mode); ++i)
    {
        LLScrollListItem* row = mDataOutput->addRow(row_params);
        row->getColumn(0)->setValue(llformat("%d", (S32)i));
        row->getColumn(1)->setValue(LLGameControl::semanticButtonName(mode, i));
    }
    mDataOutputSemanticMode = (S32)mode;
}

void SSFloaterGameControlDebug::populateDataOutputValues()
{
    const LLGameControl::ServerState& state = LLGameControl::getServerState();
    LLGameControl::AgentControlMode mode = (LLGameControl::AgentControlMode)state.mActionMode;
    if ((S32)mode != mDataOutputSemanticMode)
    {
        populateDataOutputSemanticRows(mode);
    }
    std::vector<LLScrollListItem*> rows = mDataOutput->getAllData();
    size_t row = 0;
    for (size_t i = 0; i < LLGameControl::NUM_AXES && i < state.mAxes.size() && row < rows.size(); ++i, ++row)
    {
        rows[row]->getColumn(2)->setValue(llformat("%d ", (S32)state.mAxes[i]));
    }
    ++row;
    for (size_t i = 0; i < LLGameControl::NUM_BUTTONS && row < rows.size(); ++i, ++row)
    {
        rows[row]->getColumn(2)->setValue(llformat("%d ", (S32)((state.mButtons >> i) & 0x1)));
    }
    ++row;
    for (size_t i = 0; i < LLGameControl::numSemanticAxesForMode(mode) && i < state.mSemanticAxes.size() && row < rows.size(); ++i, ++row)
    {
        rows[row]->getColumn(2)->setValue(llformat("%d ", (S32)state.mSemanticAxes[i]));
    }
    ++row;
    for (size_t i = 0; i < LLGameControl::numSemanticButtonsForMode(mode) && row < rows.size(); ++i, ++row)
    {
        rows[row]->getColumn(2)->setValue(llformat("%d ", (S32)((state.mSemanticButtons >> i) & 0x1)));
    }
}
