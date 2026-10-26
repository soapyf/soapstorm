/**
 * @file llpanelpreferencegamecontrol.cpp
 * @brief Preferences > Game Controls.
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

#include "llviewerprecompiledheaders.h"

#include "llpanelpreferencegamecontrol.h"

#include "alcurveeditorctrl.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llgesturemgr.h"
#include "llkeyboard.h"
#include "llnotificationsutil.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "ssfloatergamecontroldebug.h"
#include "ssgamecontroldiagram.h"
#include "ssgamecontrolui.h"

using namespace SSGameControlUI;

namespace
{
    // Only one game control panel exists at a time.
    LLPanelPreferenceGameControl* sGameControlPanel { nullptr };

    // List view: the cell being edited and the selector open over it.
    LLScrollListCtrl* sSelectedGrid { nullptr };
    LLScrollListItem* sSelectedItem { nullptr };
    LLScrollListCell* sSelectedCell { nullptr };
    LLComboBox* sSelectedCombobox { nullptr };
    S32 sSelectedRow { -1 };
    S32 sSelectedCol { -1 };
    LLRect sSelectedOpenRect;
    // The selector's drop-down is translucent, so the cell's own text is blanked while
    // it is open and restored from here when it closes.
    LLSD sSelectedCellValue;

    const std::string MODE_INPUT_TYPE_AXES("Axes");
    const std::string MODE_INPUT_TYPE_BUTTONS("Buttons");
    const std::string MODE_INPUT_TYPE_AXES_INVERT("AxesInvert");
    const std::string MODE_BUTTON_TOGGLE("ButtonToggle");
    const std::string BUTTON_NONE("BUTTON_NONE");
    const std::string AXIS_NONE("AXIS_NONE");
    const std::string AXIS_TRIGGERS("AXIS_TRIGGERS");
    const std::string ADD_KEY_ACTION("add_key");          // list view: the "Add keyboard key..." row
    const std::string ACTION_NOTHING("__nothing__");      // inspector: unbind
    const std::string ACTION_NEW_KEY("__key__");          // inspector: bind a new keyboard key here
    const std::string PRESETS_SETTING("GameControlPresets");
    const std::string BUILTIN_PRESET_PREFIX("__builtin_");

    // Ready-made layouts offered at the top of the Presets list. Bindings start blank; these
    // are starting points. Each is the blank layout (which keeps the stick inverts) plus
    // its bindings, with some modes taken whole from the Linden Lab layout.
    struct Bind { const char* mode; const char* kind; const char* action; const char* input; };

    LLSD buildLayout(std::initializer_list<const char*> ll_modes, std::initializer_list<Bind> binds)
    {
        LLSD layout = LLGameControl::getDefaultModeMappings();
        const LLSD ll = LLGameControl::getSuggestedModeMappings();
        for (const char* mode : ll_modes)
        {
            layout[mode] = ll[mode];
        }
        for (const Bind& b : binds)
        {
            layout[b.mode][b.kind][b.action] = b.input;
        }
        return layout;
    }

    // Mouselook's look stick turns faster held at its edge (1.8x after 0.1 s, over 0.3 s),
    // so aiming stays fine and big turns still get made.
    LLSD withLookBoost(LLSD layout)
    {
        layout["Mouselook"]["Tuning"]["Right"]["TurnBoost"] = 1.8;
        return layout;
    }

    // Sticks as the Linden Lab layout has them, and the buttons every layout keeps so you
    // can always reach mouselook, the mouse cursor and voice.
    #define GC_STICKS(mode) \
        { mode, "Axes", "Strafe left/right", "AXIS_LEFTX" }, { mode, "Axes", "Advance forward/back", "AXIS_LEFTY" }, \
        { mode, "Axes", "Turn left/right", "AXIS_RIGHTX" }, { mode, "Axes", "Look up/down", "AXIS_RIGHTY" }
    #define GC_SYSTEM(mode) \
        { mode, "Buttons", "Toggle mouselook", "BUTTON_START" }, { mode, "Buttons", "Toggle mouse cursor", "BUTTON_SELECT" }, \
        { mode, "Buttons", "Toggle speak", "BUTTON_HOME" }

    const std::vector<std::pair<std::string, LLSD>>& builtInPresets()
    {
        static const std::vector<std::pair<std::string, LLSD>> presets = {
            { "Linden Lab default", LLGameControl::getSuggestedModeMappings() },
            // First-person shooter: A jumps, the right stick clicks to crouch, and in
            // mouselook the triggers are the mouse buttons (right trigger fires).
            { "Shooter", withLookBoost(buildLayout({ "Cursor", "FlyCam" }, {
                GC_STICKS("Avatar"), GC_SYSTEM("Avatar"),
                { "Avatar", "Buttons", "Jump", "BUTTON_SOUTH" },
                { "Avatar", "Buttons", "Crouch", "BUTTON_RIGHT_STICK" },
                GC_STICKS("Mouselook"), GC_SYSTEM("Mouselook"),
                { "Mouselook", "Buttons", "Jump", "BUTTON_SOUTH" },
                { "Mouselook", "Buttons", "Crouch", "BUTTON_RIGHT_STICK" },
                { "Mouselook", "Buttons", "Mouse click left", "BUTTON_RIGHT_TRIGGER" },
                { "Mouselook", "Buttons", "Mouse click right", "BUTTON_LEFT_TRIGGER" },
                GC_STICKS("Captive"), GC_SYSTEM("Captive"),
                { "Captive", "Buttons", "Toggle sit", "BUTTON_SOUTH" },
            })) },
            // Getting about: triggers fly up and down, the face buttons jump, crouch, sit
            // and touch, the d-pad zooms.
            { "Explorer", buildLayout({ "Mouselook", "Cursor", "FlyCam" }, {
                GC_STICKS("Avatar"), GC_SYSTEM("Avatar"),
                { "Avatar", "Axes", "Fly up/down", "AXIS_TRIGGERS" },
                { "Avatar", "Buttons", "Jump", "BUTTON_SOUTH" },
                { "Avatar", "Buttons", "Crouch", "BUTTON_EAST" },
                { "Avatar", "Buttons", "Toggle sit", "BUTTON_WEST" },
                { "Avatar", "Buttons", "Zoom +", "BUTTON_DPAD_UP" },
                { "Avatar", "Buttons", "Zoom -", "BUTTON_DPAD_DOWN" },
                { "Avatar", "Buttons", "Toggle flycam", "BUTTON_RIGHT_STICK" },
                GC_STICKS("Captive"), GC_SYSTEM("Captive"),
                { "Captive", "Buttons", "Toggle sit", "BUTTON_WEST" },
                { "Captive", "Buttons", "Zoom +", "BUTTON_DPAD_UP" },
                { "Captive", "Buttons", "Zoom -", "BUTTON_DPAD_DOWN" },
            }) },
            // Pictures and machinima: Y flies the camera on its own; there the bumpers
            // zoom, A orbits, the d-pad rolls and B levels it again.
            { "Photographer", buildLayout({ "Mouselook", "Cursor" }, {
                GC_STICKS("Avatar"), GC_SYSTEM("Avatar"),
                { "Avatar", "Axes", "Fly up/down", "AXIS_TRIGGERS" },
                { "Avatar", "Buttons", "Jump", "BUTTON_SOUTH" },
                { "Avatar", "Buttons", "Toggle flycam", "BUTTON_NORTH" },
                { "FlyCam", "Axes", "Truck left/right", "AXIS_LEFTX" },
                { "FlyCam", "Axes", "Dolly forward/back", "AXIS_LEFTY" },
                { "FlyCam", "Axes", "Pan left/right", "AXIS_RIGHTX" },
                { "FlyCam", "Axes", "Tilt up/down", "AXIS_RIGHTY" },
                { "FlyCam", "Axes", "Boom up/down", "AXIS_TRIGGERS" },
                { "FlyCam", "Buttons", "Zoom out", "BUTTON_LEFT_SHOULDER" },
                { "FlyCam", "Buttons", "Zoom in", "BUTTON_RIGHT_SHOULDER" },
                { "FlyCam", "Buttons", "Orbit", "BUTTON_SOUTH" },
                { "FlyCam", "Buttons", "Unroll", "BUTTON_EAST" },
                { "FlyCam", "Buttons", "Roll CCW", "BUTTON_DPAD_LEFT" },
                { "FlyCam", "Buttons", "Roll CW", "BUTTON_DPAD_RIGHT" },
                { "FlyCam", "Buttons", "Toggle flycam", "BUTTON_NORTH" },
                { "FlyCam", "Buttons", "Toggle mouse cursor", "BUTTON_SELECT" },
                GC_STICKS("Captive"), GC_SYSTEM("Captive"),
                { "Captive", "Buttons", "Toggle sit", "BUTTON_WEST" },
                { "Captive", "Buttons", "Toggle flycam", "BUTTON_NORTH" },
            }) },
        };
        return presets;
    }
    #undef GC_STICKS
    #undef GC_SYSTEM

    // The built-in layout a Presets list value names, or null.
    const LLSD* builtInPreset(const std::string& value)
    {
        if (!LLStringUtil::startsWith(value, BUILTIN_PRESET_PREFIX))
        {
            return nullptr;
        }
        U32 index = 0;
        if (!LLStringUtil::convertToU32(value.substr(BUILTIN_PRESET_PREFIX.size()), index))
        {
            return nullptr;
        }
        return index < builtInPresets().size() ? &builtInPresets()[index].second : nullptr;
    }

    const char* MODE_NAMES[] = { "Avatar", "Mouselook", "FlyCam", "Captive", "Cursor" };

    LLSD blankIfNone(const LLSD& label)
    {
        return label.asString() == NONE_LABEL ? LLSD() : label;
    }

    bool isKeyAction(const std::string& action)
    {
        return LLStringUtil::startsWith(action, "key:");
    }

    // One-shot commands fire on the press itself, so holding or toggling them means nothing.
    bool isOneShotAction(const std::string& action)
    {
        return LLStringUtil::startsWith(action, "Toggle ") || action == "ESC" || action == "Unroll"
            || action == "Sit down" || action == "Stand up";
    }

    // "Strafe left/right" -> "Strafe": a stick's callout holds two of these.
    // Callout text for a button action: "Toggle mouselook" and "Toggle mouse cursor" both
    // cut to "Toggle mouse..." in a callout, so the Toggle goes (the inspector keeps it).
    std::string calloutLabel(std::string label)
    {
        if (LLStringUtil::startsWith(label, "Toggle ") && label.size() > 7)
        {
            label.erase(0, 7);
            label[0] = (char)toupper((unsigned char)label[0]);
        }
        return label;
    }

    std::string shortAxisLabel(const std::string& action)
    {
        if (action == "Mouse left/right") return "Cursor \xE2\x86\x94";
        if (action == "Mouse up/down")    return "Cursor \xE2\x86\x95";
        size_t space = action.find(' ');
        return space == std::string::npos ? action : action.substr(0, space);
    }

    std::string stickAxisInput(S32 element, bool horizontal)
    {
        bool left = (element == SSGameControlDiagram::EL_LEFT_STICK);
        return horizontal ? (left ? "AXIS_LEFTX" : "AXIS_RIGHTX") : (left ? "AXIS_LEFTY" : "AXIS_RIGHTY");
    }

    // The controller whose dead zones the inspector edits and whose raw position the
    // stick view shows: the first one connected.
    const LLGameControl::Device* primaryDevice()
    {
        const auto& devices = LLGameControl::getDevices();
        return devices.empty() ? nullptr : &devices.front();
    }

    F32 signedAxis(const std::vector<U16>& half_axes, U8 axis)
    {
        if (half_axes.size() <= (size_t)axis * 2 + 1)
        {
            return 0.f;
        }
        return ((F32)half_axes[axis * 2] - (F32)half_axes[axis * 2 + 1]) / 32767.f;
    }
}

static LLPanelInjector<LLPanelPreferenceGameControl> t_pref_game_control("panel_preference_game_control");

// static
void LLPanelPreferenceGameControl::updateDeviceList()
{
    if (sGameControlPanel)
    {
        sGameControlPanel->updateControllerName();
        sGameControlPanel->refreshDiagram();
    }
    SSFloaterGameControlDebug::updateDeviceList();
}

LLPanelPreferenceGameControl::LLPanelPreferenceGameControl()
{
    sGameControlPanel = this;
}

LLPanelPreferenceGameControl::~LLPanelPreferenceGameControl()
{
    sGameControlPanel = nullptr;
}

// The whole GameControl setting. Connected devices are folded in with their names
// even at default options, so "Show all known devices" can find them after unplugging.
LLSD LLPanelPreferenceGameControl::getSettingsAsLLSD()
{
    for (const LLGameControl::Device& device : LLGameControl::getDevices())
    {
        LLGameControl::setDeviceConfig(device.getGUID(), device.saveOptionsToString(true));
    }
    LLSD result = LLSD::emptyMap();
    result["GameControl"] = LLGameControl::getGameControlSettings();
    return result;
}

void LLPanelPreferenceGameControl::saveSettings()
{
    LLPanelPreference::saveSettings();
    if (mOrigSettings.isEmpty())
    {
        rememberOriginalSettings();
    }
    LLSD settings = getSettingsAsLLSD();
    for (auto it = settings.beginMap(); it != settings.endMap(); ++it)
    {
        if (LLControlVariable* control = gSavedSettings.getControl(it->first))
        {
            control->set(it->second);
            mSavedValues[control] = control->getValue();
        }
    }
}

bool LLPanelPreferenceGameControl::postBuild()
{
    // ---- modes ----
    mActionMode = getChild<LLComboBox>("action_mode");
    for (S32 i = 0; i < (S32)mModeButtons.size(); ++i)
    {
        mModeButtons[i] = getChild<LLButton>(llformat("mode_btn_%d", i));
        mModeButtons[i]->setCommitCallback([this, i](LLUICtrl*, const LLSD&) { selectMode(i); });
    }
    mCheckActionModeEnabled = getChild<LLCheckBoxCtrl>("action_mode_enabled");
    mCheckActionModeEnabled->setCommitCallback([this](LLUICtrl*, const LLSD&) { onModeEnabledToggled(mCheckActionModeEnabled->getValue()); });
    mCheckFlycamAllowRoll = getChild<LLCheckBoxCtrl>("flycam_allow_roll");
    mCheckFlycamAllowRoll->setCommitCallback([this](LLUICtrl*, const LLSD&) { LLGameControl::setFlycamRollAllowed(mCheckFlycamAllowRoll->getValue()); });
    mSliderFlycamSpeedFactor = getChild<LLSliderCtrl>("flycam_speed_factor");
    mSliderFlycamSpeedFactor->setCommitCallback([this](LLUICtrl*, const LLSD&) { LLGameControl::setSpeedFactor("FlyCam", (F32)mSliderFlycamSpeedFactor->getValue().asReal()); });
    mFlycamSlowLabel = getChild<LLTextBox>("flycam_speed_slow_label");
    mFlycamFastLabel = getChild<LLTextBox>("flycam_speed_fast_label");
    mCheckFlycamCrosshair = getChild<LLCheckBoxCtrl>("flycam_crosshair");
    mCheckFlycamCrosshair->setCommitCallback([this](LLUICtrl*, const LLSD&) { LLGameControl::setFlycamCrosshairEnabled(mCheckFlycamCrosshair->getValue()); });
    mCurrentModeIndicator = getChild<LLTextBox>("current_mode_indicator");

    // ---- top and bottom bars ----
    mControllerName = getChild<LLTextBox>("controller_name");
    mPresetList = getChild<LLComboBox>("preset_list");
    mPresetList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetSelected(); });
    getChild<LLButton>("preset_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetSave(); });
    getChild<LLButton>("preset_delete")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPresetDelete(); });
    mRestoreActionsDefaults = getChild<LLButton>("restore_actions_defaults");
    mRestoreActionsDefaults->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetActionsToDefaults(); });
    getChild<LLButton>("reset_all")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResetAll(); });
    getChild<LLButton>("open_debug")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("game_control_debug"); });
    getChild<LLButton>("open_gyro")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("game_control_gyro"); });
    getChild<LLButton>("pop_out")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::toggleInstance("game_control_overlay"); });
    mStyleList = getChild<LLComboBox>("style_list");
    mStyleList->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            gSavedSettings.setS32("GameControlButtonStyle", mStyleList->getValue().asInteger());
            refreshDiagram();
        });
    mViewToggle = getChild<LLButton>("view_toggle");
    mViewToggle->setCommitCallback([this](LLUICtrl*, const LLSD&) { setListMode(!mListMode); });
    mConflictText = getChild<LLTextBox>("conflict_text");
    mCheckGameControlToServer = getChild<LLCheckBoxCtrl>("game_control_to_server");
    mCheckGameControlToServer->setCommitCallback([this](LLUICtrl*, const LLSD&) { LLGameControl::setSendToServer(mCheckGameControlToServer->getValue()); });

    // ---- picture and inspector ----
    mBodyStack = getChild<LLView>("body_stack");
    mDiagram = getChild<SSGameControlDiagram>("controller_diagram");
    mDiagram->setCommitCallback([this](LLUICtrl*, const LLSD&) { selectElement(mDiagram->getSelected()); });
    mInspectorTitle = getChild<LLTextBox>("inspector_title");
    mInspectorButton = getChild<LLPanel>("inspector_button");
    mInspectorStick = getChild<LLPanel>("inspector_stick");
    mInspectorTrigger = getChild<LLPanel>("inspector_trigger");
    mInspectorTuning = getChild<LLPanel>("inspector_tuning");

    auto selectedButtonInput = [this]() { return buttonInputValue(mDiagram->getSelected()); };
    mButtonAction = getChild<LLComboBox>("button_action");
    mButtonAction->setCommitCallback([this, selectedButtonInput](LLUICtrl*, const LLSD&) { onButtonActionCommit(mButtonAction, selectedButtonInput()); });
    mButtonBehavior = getChild<LLComboBox>("button_behavior");
    mButtonBehavior->setCommitCallback([this, selectedButtonInput](LLUICtrl*, const LLSD&) { onBehaviorCommit(mButtonBehavior, selectedButtonInput()); });
    mButtonNote = getChild<LLTextBox>("button_note");

    mStickXAction = getChild<LLComboBox>("stick_x_action");
    mStickXAction->setCommitCallback([this](LLUICtrl*, const LLSD&)
        { assignAxisAction(stickAxisInput(mDiagram->getSelected(), true), mStickXAction->getValue().asString()); refreshDiagram(); });
    mStickYAction = getChild<LLComboBox>("stick_y_action");
    mStickYAction->setCommitCallback([this](LLUICtrl*, const LLSD&)
        { assignAxisAction(stickAxisInput(mDiagram->getSelected(), false), mStickYAction->getValue().asString()); refreshDiagram(); });
    mStickXInvert = getChild<LLCheckBoxCtrl>("stick_x_invert");
    mStickXInvert->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAxisInvertCommit(mStickXInvert, stickAxisInput(mDiagram->getSelected(), true)); });
    mStickYInvert = getChild<LLCheckBoxCtrl>("stick_y_invert");
    mStickYInvert->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAxisInvertCommit(mStickYInvert, stickAxisInput(mDiagram->getSelected(), false)); });
    mStickClickAction = getChild<LLComboBox>("stick_click_action");
    mStickClickAction->setCommitCallback([this, selectedButtonInput](LLUICtrl*, const LLSD&) { onButtonActionCommit(mStickClickAction, selectedButtonInput()); });
    mStickClickBehavior = getChild<LLComboBox>("stick_click_behavior");
    mStickClickBehavior->setCommitCallback([this, selectedButtonInput](LLUICtrl*, const LLSD&) { onBehaviorCommit(mStickClickBehavior, selectedButtonInput()); });

    mTriggerAxisAction = getChild<LLComboBox>("trigger_axis_action");
    mTriggerAxisAction->setCommitCallback([this](LLUICtrl*, const LLSD&) { assignAxisAction(AXIS_TRIGGERS, mTriggerAxisAction->getValue().asString()); refreshDiagram(); });
    mTriggerAxisInvert = getChild<LLCheckBoxCtrl>("trigger_axis_invert");
    mTriggerAxisInvert->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAxisInvertCommit(mTriggerAxisInvert, AXIS_TRIGGERS); });
    mTriggerButtonAction = getChild<LLComboBox>("trigger_button_action");
    mTriggerButtonAction->setCommitCallback([this, selectedButtonInput](LLUICtrl*, const LLSD&) { onButtonActionCommit(mTriggerButtonAction, selectedButtonInput()); });
    mTriggerBehavior = getChild<LLComboBox>("trigger_behavior");
    mTriggerBehavior->setCommitCallback([this, selectedButtonInput](LLUICtrl*, const LLSD&) { onBehaviorCommit(mTriggerBehavior, selectedButtonInput()); });

    mTuneSensitivity = getChild<LLSliderCtrl>("tune_sensitivity");
    mTuneCurve = getChild<LLSliderCtrl>("tune_curve");
    mTuneSmoothing = getChild<LLSliderCtrl>("tune_smoothing");
    mTuneRescale = getChild<LLCheckBoxCtrl>("tune_rescale");
    mTuneOuter = getChild<LLSliderCtrl>("tune_outer");
    mTuneBoost = getChild<LLSliderCtrl>("tune_boost");
    for (LLUICtrl* ctrl : { (LLUICtrl*)mTuneSensitivity, (LLUICtrl*)mTuneCurve, (LLUICtrl*)mTuneSmoothing, (LLUICtrl*)mTuneRescale,
                            (LLUICtrl*)mTuneOuter, (LLUICtrl*)mTuneBoost })
    {
        ctrl->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTuningCommit(); });
    }
    mTuneDeadZone = getChild<LLSliderCtrl>("tune_dead_zone");
    mTuneDeadZone->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDeadZoneCommit(); });
    mStickView = getChild<SSGameControlStickView>("stick_view");
    mCurveView = getChild<ALCurveEditorCtrl>("curve_view");
    mCurveView->setHandles({});

    // ---- list view ----
    mListView = getChild<LLPanel>("list_view");
    mActionMappingsAxes = getChild<LLScrollListCtrl>("action_mappings_axes");
    mActionMappingsAxes->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onGridSelect(ctrl); });
    mActionMappingsButtons = getChild<LLScrollListCtrl>("action_mappings_buttons");
    mActionMappingsButtons->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onGridSelect(ctrl); });

    mAnalogActionSelector = getChild<LLComboBox>("analog_action_selector");
    mBinaryActionSelector = getChild<LLComboBox>("binary_action_selector");
    mMouselookAnalogActionSelector = getChild<LLComboBox>("mouselook_analog_action_selector");
    mMouselookBinaryActionSelector = getChild<LLComboBox>("mouselook_binary_action_selector");
    mFlycamAnalogActionSelector = getChild<LLComboBox>("flycam_analog_action_selector");
    mFlycamBinaryActionSelector = getChild<LLComboBox>("flycam_binary_action_selector");
    mCaptiveBinaryActionSelector = getChild<LLComboBox>("sit_binary_action_selector");
    mCursorAnalogActionSelector = getChild<LLComboBox>("cursor_analog_action_selector");
    mCursorBinaryActionSelector = getChild<LLComboBox>("cursor_binary_action_selector");

    mAxisInputSelector = getChild<LLComboBox>("axis_input_selector");
    mAxisInputSelector->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onCommitInputChannel(ctrl); });
    mAxisInputGlyphSelector = getChild<LLComboBox>("axis_input_glyph_selector");
    mAxisInputGlyphSelector->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onCommitInputChannel(ctrl); });
    buildInputGlyphSelector(mAxisInputSelector, mAxisInputGlyphSelector);
    mButtonInputSelector = getChild<LLComboBox>("button_input_selector");
    mButtonInputSelector->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onCommitInputChannel(ctrl); });
    mButtonInputGlyphSelector = getChild<LLComboBox>("button_input_glyph_selector");
    mButtonInputGlyphSelector->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) { onCommitInputChannel(ctrl); });
    buildInputGlyphSelector(mButtonInputSelector, mButtonInputGlyphSelector);

    // LLScrollListCtrl with draw_heading="true" starts 17 px too high; each list fills
    // a wrapper panel, so its corrected top is the parent's height - 1.
    for (LLScrollListCtrl* grid : { mActionMappingsAxes, mActionMappingsButtons })
    {
        LLRect rect = grid->getRect();
        rect.mTop = grid->getParent()->getRect().getHeight() - 1;
        grid->setRect(rect);
        grid->updateLayout();
    }

    mActionMode->selectFirstItem();
    clearSelectionState();
    return true;
}

void LLPanelPreferenceGameControl::onOpen(const LLSD& key)
{
    mCheckGameControlToServer->setValue(LLGameControl::sendToServer());
    mStyleList->setValue(std::to_string(gSavedSettings.getS32("GameControlButtonStyle")));
    clearSelectionState();
    updateControllerName();
    populatePresets();
    populateActionMappings();
    updateActionModeEnabledUI();
    // captured again on the first saveSettings() after opening
    mOrigSettings = LLSD::emptyMap();
}

void LLPanelPreferenceGameControl::draw()
{
    updateCurrentModeIndicator();
    if (mDiagram && mDiagram->isInVisibleChain())
    {
        updateLiveInput();
    }
    // a list-view selector left open over a cell that has since moved is closed
    if (sSelectedCombobox && sSelectedGrid && sSelectedRow >= 0 && sSelectedCol >= 0
        && cellRect(sSelectedGrid, sSelectedCombobox->getParent(), sSelectedRow, sSelectedCol) != sSelectedOpenRect)
    {
        clearSelectionState();
    }
    LLPanelPreference::draw();
}

void LLPanelPreferenceGameControl::apply()
{
    LLPanelPreference::apply();
    clearSelectionState();
    // cancel() also runs from onClose() after OK, so drop the snapshot it would restore
    mOrigSettings = LLSD::emptyMap();
}

void LLPanelPreferenceGameControl::cancel(const std::vector<std::string> settings_to_skip)
{
    LLPanelPreference::cancel(settings_to_skip);
    clearSelectionState();
    if (mOrigSettings.isEmpty())
    {
        return;
    }
    // Only this page's edits are undone. The Gyro Aim and Trackpads window and the
    // debug window write to the same GameControl map and save straight away.
    LLSD restore = mOrigSettings;
    const LLSD& live = LLGameControl::getGameControlSettings();
    for (const char* key : { "Gyro", "Trackpads", "Devices" })
    {
        if (live.has(key))
        {
            restore[key] = live[key];
        }
        else
        {
            restore.erase(key);
        }
    }
    LLGameControl::applySettingsFromLLSD(restore);
    LLGameControl::saveToSettings();
}

void LLPanelPreferenceGameControl::rememberOriginalSettings()
{
    mOrigSettings = LLGameControl::getSettingsAsLLSD();
}

// static
bool LLPanelPreferenceGameControl::isWaitingForInputChannel()
{
    if (sSelectedCell)
    {
        return true;
    }
    // While the controller picture is showing, the controller is being configured, so
    // it selects inputs on the picture rather than moving the avatar.
    return sGameControlPanel && sGameControlPanel->mDiagram && sGameControlPanel->mDiagram->isInVisibleChain();
}

// static
// Main loop, while waiting. Refreshes the controller state without the send-to-server
// bookkeeping, and if a list-view selector is open and the user presses a button or
// tilts an axis of the kind it wants, picks that input as if chosen from the list.
// The picture reads the refreshed state itself in draw().
void LLPanelPreferenceGameControl::applyGameControlInput()
{
    if (!sGameControlPanel)
    {
        return;
    }
    LLGameControl::computeFinalState();

    if (!sSelectedCombobox)
    {
        return;
    }
    bool wants_axis = (sSelectedGrid == sGameControlPanel->mActionMappingsAxes);
    bool wants_button = (sSelectedGrid == sGameControlPanel->mActionMappingsButtons);
    LLGameControl::InputChannel channel = LLGameControl::getActiveInputChannel();
    if ((wants_axis && !channel.isAxis()) || (wants_button && !channel.isButton()) || (!wants_axis && !wants_button))
    {
        return;
    }
    std::string value = channel.getRemoteName();
    if (!sSelectedCombobox->setSelectedByValue(value, true))
    {
        // the axis list has no single triggers, only the pair
        if ((value != "AXIS_LEFT_TRIGGER" && value != "AXIS_RIGHT_TRIGGER")
            || !sSelectedCombobox->setSelectedByValue(AXIS_TRIGGERS, true))
        {
            return;
        }
    }
    sGameControlPanel->onCommitInputChannel(sSelectedCombobox);
}

// ===========================================================================
// Modes
// ===========================================================================

std::string LLPanelPreferenceGameControl::currentEditMode()
{
    S32 ordinal = mActionMode->getValue().asInteger();
    return LLGameControl::getModeName((LLGameControl::AgentControlMode)ordinal);
}

void LLPanelPreferenceGameControl::selectMode(S32 mode)
{
    mActionMode->setValue(mode);
    onActionModeChanged();
}

void LLPanelPreferenceGameControl::onActionModeChanged()
{
    clearSelectionState();
    populateActionMappings();
    updateActionModeEnabledUI();
}

void LLPanelPreferenceGameControl::onModeEnabledToggled(bool enabled)
{
    clearSelectionState();
    LLGameControl::setModeEnabled(currentEditMode(), enabled);
    updateActionModeEnabledUI();
}

// Syncs everything that depends on the edited mode: its tab, its enable flag (a
// disabled mode's bindings are locked), its FlyCam-only options, and the picture.
void LLPanelPreferenceGameControl::updateActionModeEnabledUI()
{
    const std::string mode = currentEditMode();
    const S32 ordinal = mActionMode->getValue().asInteger();
    for (S32 i = 0; i < (S32)mModeButtons.size(); ++i)
    {
        mModeButtons[i]->setToggleState(i == ordinal);
    }

    const bool enabled = LLGameControl::isModeEnabled(mode);
    mCheckActionModeEnabled->set(enabled);
    mActionMappingsAxes->setEnabled(enabled);
    mActionMappingsButtons->setEnabled(enabled);
    mRestoreActionsDefaults->setEnabled(enabled);

    const bool is_flycam = (mode == "FlyCam");
    for (LLView* view : { (LLView*)mCheckFlycamAllowRoll, (LLView*)mSliderFlycamSpeedFactor, (LLView*)mFlycamSlowLabel,
                          (LLView*)mFlycamFastLabel, (LLView*)mCheckFlycamCrosshair })
    {
        view->setVisible(is_flycam);
    }
    if (is_flycam)
    {
        mCheckFlycamAllowRoll->set(LLGameControl::isFlycamRollAllowed());
        mCheckFlycamAllowRoll->setEnabled(enabled);
        mSliderFlycamSpeedFactor->setValue(LLGameControl::getSpeedFactor("FlyCam"));
        mSliderFlycamSpeedFactor->setEnabled(enabled);
        mCheckFlycamCrosshair->set(LLGameControl::isFlycamCrosshairEnabled());
        mCheckFlycamCrosshair->setEnabled(enabled);
    }
    refreshDiagram();
}

// The live AgentControlMode, which need not be the one being edited.
void LLPanelPreferenceGameControl::updateCurrentModeIndicator()
{
    LLGameControl::AgentControlMode mode = LLGameControl::getAgentControlMode();
    if (mode == mLastDrawnControlMode || !mCurrentModeIndicator)
    {
        return;
    }
    mLastDrawnControlMode = mode;
    static const std::map<LLGameControl::AgentControlMode, std::string> names = {
        { LLGameControl::CONTROL_MODE_AVATAR,    "Moving" },
        { LLGameControl::CONTROL_MODE_MOUSELOOK, "Mouselook" },
        { LLGameControl::CONTROL_MODE_FLYCAM,    "Flycam" },
        { LLGameControl::CONTROL_MODE_CAPTIVE,   "Sitting" },
        { LLGameControl::CONTROL_MODE_CURSOR,    "Cursor" },
    };
    auto it = names.find(mode);
    mCurrentModeIndicator->setText(std::string("Now: ") + (it != names.end() ? it->second : "no mode"));
}

// Which selector supplies a mode's actions. Avatar and Captive share axis actions;
// every mode has its own button actions (Captive swaps Stand for Jump, Cursor adds
// the mouse clicks).
LLComboBox* LLPanelPreferenceGameControl::actionSelectorForMode(bool axis, const std::string& mode) const
{
    bool flycam = (mode == "FlyCam");
    bool mouselook = (mode == "Mouselook");
    bool cursor = (mode == "Cursor");
    if (axis)
    {
        return flycam ? mFlycamAnalogActionSelector
            : mouselook ? mMouselookAnalogActionSelector
            : cursor ? mCursorAnalogActionSelector
            : mAnalogActionSelector;
    }
    if (mouselook) return mMouselookBinaryActionSelector;
    if (flycam) return mFlycamBinaryActionSelector;
    if (cursor) return mCursorBinaryActionSelector;
    return (mode == "Captive") ? mCaptiveBinaryActionSelector : mBinaryActionSelector;
}

// ===========================================================================
// Controller picture and inspector
// ===========================================================================

void LLPanelPreferenceGameControl::updateControllerName()
{
    const auto& devices = LLGameControl::getDevices();
    if (devices.empty())
    {
        mControllerName->setText(std::string("No controller connected"));
        return;
    }
    std::string name = devices.front().getName();
    if (devices.size() > 1)
    {
        name += llformat(" (+%d more)", (S32)devices.size() - 1);
    }
    mControllerName->setText(name);
}

std::string LLPanelPreferenceGameControl::buttonInputValue(S32 element) const
{
    U8 button = SSGameControlDiagram::buttonForElement(element);
    std::vector<LLScrollListItem*> items = mButtonInputSelector->getAllData();
    return button < items.size() ? items[button]->getValue().asString() : LLStringUtil::null;
}

// The action this input drives in the edited mode, and how many it drives (more than
// one is a conflict: the backend's "last write wins" picks one silently).
std::string LLPanelPreferenceGameControl::buttonActionFor(const std::string& input_value, S32* count)
{
    std::string found;
    S32 n = 0;
    if (!input_value.empty())
    {
        LLSD mapping = LLGameControl::getModeMapping(currentEditMode(), MODE_INPUT_TYPE_BUTTONS);
        for (auto it = mapping.beginMap(); it != mapping.endMap(); ++it)
        {
            if (it->second.asString() == input_value)
            {
                if (found.empty())
                {
                    found = it->first;
                }
                ++n;
            }
        }
    }
    if (count)
    {
        *count = n;
    }
    return found;
}

std::string LLPanelPreferenceGameControl::axisActionFor(const std::string& input_value, S32* count)
{
    std::string found;
    S32 n = 0;
    LLSD mapping = LLGameControl::getModeMapping(currentEditMode(), MODE_INPUT_TYPE_AXES);
    for (auto it = mapping.beginMap(); it != mapping.endMap(); ++it)
    {
        if (it->second.asString() == input_value)
        {
            if (found.empty())
            {
                found = it->first;
            }
            ++n;
        }
    }
    if (count)
    {
        *count = n;
    }
    return found;
}

std::string LLPanelPreferenceGameControl::actionLabel(const std::string& action, bool axis)
{
    KEY key = KEY_NONE;
    MASK mask = MASK_NONE;
    if (LLViewerInput::keyFromControllerKeyAction(action, key, mask))
    {
        return "Key: " + LLKeyboard::stringFromAccelerator(mask, key);
    }
    std::string label = inputLabel(actionSelectorForMode(axis, currentEditMode()), action);
    return label.empty() ? action : label;
}

// This mode's button actions, its keyboard keys, then "Keyboard key..." and "Nothing".
void LLPanelPreferenceGameControl::fillButtonActionCombo(LLComboBox* combo, const std::string& input_value)
{
    const std::string mode = currentEditMode();
    combo->removeall();
    for (LLScrollListItem* item : actionSelectorForMode(false, mode)->getAllData())
    {
        std::string action = item->getValue().asString();
        if (action != NONE_LABEL)
        {
            combo->add(item->getColumn(0)->getValue().asString(), LLSD(action));
        }
    }
    LLSD buttons = LLGameControl::getModeMapping(mode, MODE_INPUT_TYPE_BUTTONS);
    for (auto it = buttons.beginMap(); it != buttons.endMap(); ++it)
    {
        if (isKeyAction(it->first))
        {
            combo->add(actionLabel(it->first, false), LLSD(it->first));
        }
    }
    combo->add(std::string("Keyboard key..."), LLSD(ACTION_NEW_KEY));
    combo->add(std::string("Nothing"), LLSD(ACTION_NOTHING));

    std::string current = buttonActionFor(input_value);
    combo->setValue(current.empty() ? ACTION_NOTHING : current);
}

void LLPanelPreferenceGameControl::fillAxisActionCombo(LLComboBox* combo, const std::string& input_value)
{
    combo->removeall();
    for (LLScrollListItem* item : actionSelectorForMode(true, currentEditMode())->getAllData())
    {
        std::string action = item->getValue().asString();
        if (action != NONE_LABEL)
        {
            combo->add(item->getColumn(0)->getValue().asString(), LLSD(action));
        }
    }
    combo->add(std::string("Nothing"), LLSD(ACTION_NOTHING));
    std::string current = axisActionFor(input_value);
    combo->setValue(current.empty() ? ACTION_NOTHING : current);
}

void LLPanelPreferenceGameControl::fillBehaviorCombo(LLComboBox* combo, const std::string& action)
{
    const bool can_toggle = !action.empty() && !isKeyAction(action) && !isOneShotAction(action)
        && LLGameControl::isModeEnabled(currentEditMode());
    combo->setEnabled(can_toggle);
    combo->setValue(can_toggle && LLGameControl::getButtonToggle(currentEditMode(), action) ? "toggle" : "hold");
}

// Put 'action' on this button. Whatever the button did before moves to where
// 'action' used to be, so a swap never leaves an action silently unbound.
void LLPanelPreferenceGameControl::assignButtonAction(const std::string& input_value, const std::string& action)
{
    if (input_value.empty())
    {
        return;
    }
    const std::string mode = currentEditMode();
    LLSD mapping = LLGameControl::getModeMapping(mode, MODE_INPUT_TYPE_BUTTONS);
    std::vector<std::string> previous;
    for (auto it = mapping.beginMap(); it != mapping.endMap(); ++it)
    {
        if (it->second.asString() == input_value && it->first != action)
        {
            previous.push_back(it->first);
        }
    }
    std::string vacated = BUTTON_NONE;
    if (action != ACTION_NOTHING)
    {
        if (mapping.has(action))
        {
            vacated = mapping[action].asString();
        }
        LLGameControl::updateModeMapping(mode, MODE_INPUT_TYPE_BUTTONS, action, input_value);
    }
    for (const std::string& other : previous)
    {
        LLGameControl::updateModeMapping(mode, MODE_INPUT_TYPE_BUTTONS, other, vacated == input_value ? BUTTON_NONE : vacated);
    }
    populateActionMappings();
}

void LLPanelPreferenceGameControl::assignAxisAction(const std::string& input_value, const std::string& action)
{
    const std::string mode = currentEditMode();
    LLSD mapping = LLGameControl::getModeMapping(mode, MODE_INPUT_TYPE_AXES);
    std::vector<std::string> previous;
    for (auto it = mapping.beginMap(); it != mapping.endMap(); ++it)
    {
        if (it->second.asString() == input_value && it->first != action)
        {
            previous.push_back(it->first);
        }
    }
    std::string vacated = AXIS_NONE;
    if (action != ACTION_NOTHING)
    {
        if (mapping.has(action))
        {
            vacated = mapping[action].asString();
        }
        LLGameControl::updateModeMapping(mode, MODE_INPUT_TYPE_AXES, action, input_value);
    }
    for (const std::string& other : previous)
    {
        LLGameControl::updateModeMapping(mode, MODE_INPUT_TYPE_AXES, other, vacated == input_value ? AXIS_NONE : vacated);
    }
    populateActionMappings();
}

void LLPanelPreferenceGameControl::onButtonActionCommit(LLComboBox* combo, const std::string& input_value)
{
    std::string action = combo->getValue().asString();
    if (action == ACTION_NEW_KEY)
    {
        mPendingKeyInput = input_value;
        openKeyDialog(ADD_KEY_ACTION);
        refreshInspector(); // puts the combo back until a key is chosen
        return;
    }
    assignButtonAction(input_value, action);
    refreshDiagram();
}

void LLPanelPreferenceGameControl::onBehaviorCommit(LLComboBox* combo, const std::string& input_value)
{
    std::string action = buttonActionFor(input_value);
    if (!action.empty())
    {
        LLGameControl::setButtonToggle(currentEditMode(), action, combo->getValue().asString() == "toggle");
    }
    refreshDiagram();
}

void LLPanelPreferenceGameControl::onAxisInvertCommit(LLCheckBoxCtrl* check, const std::string& input_value)
{
    std::string action = axisActionFor(input_value);
    if (!action.empty())
    {
        LLGameControl::setAxisInvert(currentEditMode(), action, check->get());
        populateActionMappings();
    }
}

LLGameControl::Stick LLPanelPreferenceGameControl::selectedStick() const
{
    switch (mDiagram->getSelected())
    {
        case SSGameControlDiagram::EL_LEFT_STICK:  return LLGameControl::STICK_LEFT;
        case SSGameControlDiagram::EL_RIGHT_STICK: return LLGameControl::STICK_RIGHT;
        default:                                   return LLGameControl::STICK_TRIGGERS;
    }
}

void LLPanelPreferenceGameControl::onTuningCommit()
{
    LLGameControl::StickTuning tuning = LLGameControl::getStickTuning(currentEditMode(), selectedStick());
    tuning.mSensitivity = mTuneSensitivity->getValueF32();
    tuning.mCurve = mTuneCurve->getValueF32();
    tuning.mSmoothing = mTuneSmoothing->getValueF32();
    tuning.mRescaleDeadZone = mTuneRescale->get();
    tuning.mOuterDeadZone = mTuneOuter->getValueF32() / 100.f;
    tuning.mBoostGain = mTuneBoost->getValueF32();
    LLGameControl::setStickTuning(currentEditMode(), selectedStick(), tuning);
    mStickView->setOuterZone(selectedStick() == LLGameControl::STICK_TRIGGERS ? 1.f : tuning.mOuterDeadZone);
    const F32 curve = tuning.mCurve;
    mCurveView->setCurve([curve](F32 x) { return powf(llclamp(x, 0.f, 1.f), curve); });
}

F32 LLPanelPreferenceGameControl::deadZoneFraction(LLGameControl::Stick stick) const
{
    const LLGameControl::Device* device = primaryDevice();
    if (!device)
    {
        return 0.f;
    }
    const LLGameControl::Options& options = device->getOptions();
    const auto& axis_options = options.getAxisOptions();
    U16 dead_zone = 0;
    for (U8 phys = 0; phys < axis_options.size() && phys < LLGameControl::NUM_AXES; ++phys)
    {
        if (LLGameControl::stickForAxis(options.mapAxis(phys)) == stick && options.mapAxis(phys) < LLGameControl::NUM_AXES)
        {
            dead_zone = std::max(dead_zone, axis_options[phys].mDeadZone);
        }
    }
    return (F32)dead_zone / 32767.f;
}

// Applied live and recorded in the device's saved config; OK keeps it, Cancel restores it.
void LLPanelPreferenceGameControl::setDeadZoneFraction(LLGameControl::Stick stick, F32 fraction)
{
    const LLGameControl::Device* device = primaryDevice();
    if (!device)
    {
        return;
    }
    LLGameControl::Options options = device->getOptions();
    U16 dead_zone = (U16)llclamp(ll_round(fraction * 32767.f), 0, (S32)LLGameControl::MAX_AXIS_DEAD_ZONE);
    auto& axis_options = options.getAxisOptions();
    for (U8 phys = 0; phys < axis_options.size() && phys < LLGameControl::NUM_AXES; ++phys)
    {
        U8 out = options.mapAxis(phys);
        if (out < LLGameControl::NUM_AXES && LLGameControl::stickForAxis(out) == stick)
        {
            axis_options[phys].mDeadZone = dead_zone;
        }
    }
    LLGameControl::setDeviceOptions(device->getGUID(), options);
    LLGameControl::setDeviceConfig(device->getGUID(), options.saveToString(device->getName(), true));
}

void LLPanelPreferenceGameControl::onDeadZoneCommit()
{
    setDeadZoneFraction(selectedStick(), mTuneDeadZone->getValueF32() / 100.f);
    mStickView->setDeadZone(deadZoneFraction(selectedStick()));
}

void LLPanelPreferenceGameControl::selectElement(S32 element)
{
    mDiagram->setSelected(element);
    refreshInspector();
}

// Callouts for every input, the conflict line, then the inspector.
void LLPanelPreferenceGameControl::refreshDiagram()
{
    if (!mDiagram)
    {
        return;
    }
    const std::string mode = currentEditMode();
    const U32 reserved = gViewerInput.getBoundControllerButtons() | LLGestureMgr::instance().getControllerBoundButtons();
    std::string conflict;

    for (S32 el = 0; el < SSGameControlDiagram::NUM_ELEMENTS; ++el)
    {
        SSGameControlDiagram::Callout callout;
        const std::string input = buttonInputValue(el);
        if (SSGameControlDiagram::isStick(el))
        {
            std::string x = axisActionFor(stickAxisInput(el, true));
            std::string y = axisActionFor(stickAxisInput(el, false));
            if (!x.empty() && !y.empty())
            {
                callout.mText = shortAxisLabel(x) + " \xC2\xB7 " + shortAxisLabel(y);
            }
            else
            {
                callout.mText = x.empty() ? (y.empty() ? LLStringUtil::null : shortAxisLabel(y)) : shortAxisLabel(x);
            }
            // the stick's click, which the stick callout would otherwise hide
            const std::string click = buttonActionFor(input);
            if (!click.empty())
            {
                const std::string click_label = calloutLabel(actionLabel(click, false));
                callout.mText = callout.mText.empty() ? "Click: " + click_label : callout.mText + " / " + click_label;
            }
            callout.mGlyph = promptFontGlyph(stickAxisInput(el, true));
        }
        else
        {
            S32 count = 0;
            std::string action = buttonActionFor(input, &count);
            if (reserved & (1U << SSGameControlDiagram::buttonForElement(el)))
            {
                callout.mText = "Viewer action or gesture";
            }
            else if (!action.empty())
            {
                callout.mText = calloutLabel(actionLabel(action, false));
                if (LLGameControl::getButtonToggle(mode, action) && !isOneShotAction(action) && !isKeyAction(action))
                {
                    callout.mText += " (toggle)";
                }
            }
            else if (SSGameControlDiagram::isTrigger(el))
            {
                std::string axis = axisActionFor(AXIS_TRIGGERS);
                if (!axis.empty())
                {
                    callout.mText = shortAxisLabel(axis) + (el == SSGameControlDiagram::EL_LEFT_TRIGGER ? " \xE2\x88\x92" : " +");
                }
            }
            if (count > 1)
            {
                callout.mWarn = true;
                callout.mText = llformat("%d actions", count);
                if (conflict.empty())
                {
                    conflict = llformat("%s does %d things in this mode. Pick one for it, or move the others elsewhere.",
                                        LLGameControl::buttonDisplayName(SSGameControlDiagram::buttonForElement(el)).c_str(), count);
                }
            }
            callout.mGlyph = promptFontGlyph(input);
        }
        mDiagram->setCallout(el, callout);
    }

    if (conflict.empty())
    {
        for (const char* axis : { "AXIS_LEFTX", "AXIS_LEFTY", "AXIS_RIGHTX", "AXIS_RIGHTY", "AXIS_TRIGGERS" })
        {
            S32 count = 0;
            axisActionFor(axis, &count);
            if (count > 1)
            {
                conflict = llformat("%s drives %d actions in this mode.", inputLabel(mAxisInputSelector, axis).c_str(), count);
                break;
            }
        }
    }
    mConflictText->setText(conflict);
    mDiagram->setFaceStyle(faceStyle());
    refreshInspector();
}

void LLPanelPreferenceGameControl::refreshInspector()
{
    const S32 el = mDiagram->getSelected();
    mInspectorButton->setVisible(false);
    mInspectorStick->setVisible(false);
    mInspectorTrigger->setVisible(false);
    mInspectorTuning->setVisible(false);
    if (el == SSGameControlDiagram::EL_NONE)
    {
        mInspectorTitle->setText(std::string("Select an input on the controller, or press it."));
        return;
    }

    const std::string mode = currentEditMode();
    const bool enabled = LLGameControl::isModeEnabled(mode);
    const std::string input = buttonInputValue(el);
    const std::string action = buttonActionFor(input);

    if (SSGameControlDiagram::isStick(el))
    {
        mInspectorTitle->setText(std::string(el == SSGameControlDiagram::EL_LEFT_STICK ? "Left stick" : "Right stick"));
        mInspectorStick->setVisible(true);
        mInspectorTuning->setVisible(true);
        for (bool horizontal : { true, false })
        {
            LLComboBox* combo = horizontal ? mStickXAction : mStickYAction;
            LLCheckBoxCtrl* invert = horizontal ? mStickXInvert : mStickYInvert;
            const std::string axis_input = stickAxisInput(el, horizontal);
            fillAxisActionCombo(combo, axis_input);
            std::string axis_action = axisActionFor(axis_input);
            invert->set(!axis_action.empty() && LLGameControl::getAxisInvert(mode, axis_action));
            combo->setEnabled(enabled);
            invert->setEnabled(enabled && !axis_action.empty());
        }
        fillButtonActionCombo(mStickClickAction, input);
        mStickClickAction->setEnabled(enabled);
        fillBehaviorCombo(mStickClickBehavior, action);
    }
    else if (SSGameControlDiagram::isTrigger(el))
    {
        mInspectorTitle->setText(std::string(el == SSGameControlDiagram::EL_LEFT_TRIGGER ? "Left trigger" : "Right trigger"));
        mInspectorTrigger->setVisible(true);
        mInspectorTuning->setVisible(true);
        fillAxisActionCombo(mTriggerAxisAction, AXIS_TRIGGERS);
        std::string axis_action = axisActionFor(AXIS_TRIGGERS);
        mTriggerAxisInvert->set(!axis_action.empty() && LLGameControl::getAxisInvert(mode, axis_action));
        mTriggerAxisAction->setEnabled(enabled);
        mTriggerAxisInvert->setEnabled(enabled && !axis_action.empty());
        fillButtonActionCombo(mTriggerButtonAction, input);
        mTriggerButtonAction->setEnabled(enabled);
        fillBehaviorCombo(mTriggerBehavior, action);
    }
    else
    {
        mInspectorTitle->setText(LLGameControl::buttonDisplayName(SSGameControlDiagram::buttonForElement(el)));
        mInspectorButton->setVisible(true);
        fillButtonActionCombo(mButtonAction, input);
        mButtonAction->setEnabled(enabled);
        fillBehaviorCombo(mButtonBehavior, action);

        std::string note;
        const U32 reserved = gViewerInput.getBoundControllerButtons() | LLGestureMgr::instance().getControllerBoundButtons();
        if (reserved & (1U << SSGameControlDiagram::buttonForElement(el)))
        {
            note = "This button also runs a viewer action (Preferences > Controls) or a gesture, "
                   "so it does that instead of the action above.";
        }
        else if (isKeyAction(action))
        {
            note = "Presses that key while the button is held, as if typed on the keyboard.";
        }
        else if (isOneShotAction(action))
        {
            note = "Happens once per press.";
        }
        else if (!enabled)
        {
            note = "The controller is switched off in this mode.";
        }
        mButtonNote->setText(note);
    }

    if (mInspectorTuning->getVisible())
    {
        refreshTuning();
    }
}

void LLPanelPreferenceGameControl::refreshTuning()
{
    const std::string mode = currentEditMode();
    const bool enabled = LLGameControl::isModeEnabled(mode);
    const LLGameControl::Stick stick = selectedStick();
    LLGameControl::StickTuning tuning = LLGameControl::getStickTuning(mode, stick);
    mTuneSensitivity->setValue(tuning.mSensitivity);
    mTuneCurve->setValue(tuning.mCurve);
    mTuneSmoothing->setValue(tuning.mSmoothing);
    mTuneRescale->set(tuning.mRescaleDeadZone);
    mTuneOuter->setValue((F32)ll_round(tuning.mOuterDeadZone * 100.f));
    mTuneBoost->setValue(tuning.mBoostGain);
    // the outer zone and the turn boost are stick-only (a stick is shaped as a vector)
    const bool is_stick = stick != LLGameControl::STICK_TRIGGERS;
    mTuneOuter->setVisible(is_stick);
    mTuneBoost->setVisible(is_stick);
    mStickView->setOuterZone(is_stick ? tuning.mOuterDeadZone : 1.f);
    mTuneDeadZone->setValue((F32)ll_round(deadZoneFraction(stick) * 100.f));
    for (LLUICtrl* ctrl : { (LLUICtrl*)mTuneSensitivity, (LLUICtrl*)mTuneCurve, (LLUICtrl*)mTuneSmoothing, (LLUICtrl*)mTuneRescale,
                            (LLUICtrl*)mTuneOuter, (LLUICtrl*)mTuneBoost })
    {
        ctrl->setEnabled(enabled);
    }
    mTuneDeadZone->setEnabled(primaryDevice() != nullptr);

    const F32 curve = tuning.mCurve;
    mCurveView->setCurve([curve](F32 x) { return powf(llclamp(x, 0.f, 1.f), curve); });
    mStickView->setTriggers(stick == LLGameControl::STICK_TRIGGERS);
    mStickView->setDeadZone(deadZoneFraction(stick));
}

// Per frame while the picture shows: what is pressed and where the sticks are, and
// selecting an input by pressing it (or flicking a stick).
void LLPanelPreferenceGameControl::updateLiveInput()
{
    U32 buttons = 0;
    std::array<F32, 6> live {};
    readLiveInput(buttons, live);
    mDiagram->setLiveInput(buttons, live);

    // a fresh press selects that input
    U32 pressed = buttons & ~mPrevLiveButtons;
    mPrevLiveButtons = buttons;
    for (U8 b = 0; pressed && b < LLGameControl::NUM_BUTTONS; ++b)
    {
        if (pressed & (1U << b))
        {
            S32 el = SSGameControlDiagram::elementForButton(b);
            if (el != SSGameControlDiagram::EL_NONE)
            {
                if (el != mDiagram->getSelected())
                {
                    selectElement(el);
                }
                break;
            }
        }
    }
    // so does a flick of a stick
    for (S32 stick = 0; stick < 2; ++stick)
    {
        F32 magnitude = llmax(std::abs(live[stick * 2]), std::abs(live[stick * 2 + 1]));
        F32& was = mPrevStickMagnitude[stick];
        if (magnitude > 0.7f && was < 0.3f)
        {
            S32 el = stick == 0 ? SSGameControlDiagram::EL_LEFT_STICK : SSGameControlDiagram::EL_RIGHT_STICK;
            if (el != mDiagram->getSelected())
            {
                selectElement(el);
            }
        }
        was = magnitude;
    }

    // the stick view shows the raw position, so the dead zone can be seen at work
    if (mInspectorTuning->getVisible())
    {
        const LLGameControl::Device* device = primaryDevice();
        const std::vector<U16> no_axes;
        const std::vector<U16>& raw = device ? device->getState().mRawAxes : no_axes;
        switch (selectedStick())
        {
            case LLGameControl::STICK_LEFT:
                mStickView->setPosition(signedAxis(raw, LLGameControl::AXIS_LEFTX), -signedAxis(raw, LLGameControl::AXIS_LEFTY));
                break;
            case LLGameControl::STICK_RIGHT:
                mStickView->setPosition(signedAxis(raw, LLGameControl::AXIS_RIGHTX), -signedAxis(raw, LLGameControl::AXIS_RIGHTY));
                break;
            default:
                mStickView->setPosition(llclamp(signedAxis(raw, LLGameControl::AXIS_LEFT_TRIGGER), 0.f, 1.f),
                                        llclamp(signedAxis(raw, LLGameControl::AXIS_RIGHT_TRIGGER), 0.f, 1.f));
                break;
        }
    }
}

// ===========================================================================
// List view: the original tables
// ===========================================================================

void LLPanelPreferenceGameControl::setListMode(bool list)
{
    clearSelectionState();
    mListMode = list;
    mListView->setVisible(list);
    mBodyStack->setVisible(!list);
    mViewToggle->setLabel(list ? std::string("Diagram view") : std::string("List view"));
    if (!list)
    {
        refreshDiagram();
    }
}

// Axis actions in the top table, button actions (and keyboard keys) in the bottom one.
// Each row's value is {input_type, action}.
void LLPanelPreferenceGameControl::populateActionMappings()
{
    clearSelectionState(); // the selection points into the rows freed below
    mActionMappingsAxes->clearRows();
    mActionMappingsButtons->clearRows();
    const std::string mode = currentEditMode();

    auto addBlock = [&](LLScrollListCtrl* grid, const std::string& input_type,
                        const LLComboBox* action_selector, const LLComboBox* input_selector)
    {
        LLSD mapping = LLGameControl::getModeMapping(mode, input_type);
        S32 invert_col = (input_type == MODE_INPUT_TYPE_AXES) ? grid->getColumn("invert")->mIndex : -1;
        std::string none_value = input_selector->getAllData().back()->getValue().asString();
        for (LLScrollListItem* item : action_selector->getAllData())
        {
            std::string action = item->getValue().asString();
            if (action == NONE_LABEL)
            {
                continue;
            }
            std::string input_value = mapping.has(action) ? mapping[action].asString() : LLStringUtil::null;
            std::string glyph = promptFontGlyph(input_value);

            LLScrollListItem::Params row_params;
            for (S32 c = 0; c < grid->getNumColumns(); ++c)
            {
                LLScrollListCell::Params cell_params;
                cell_params.column = grid->getColumn(c)->mName;
                cell_params.font = (c == 1 && !glyph.empty()) ? promptFontForCell() : LLFontGL::getFontSansSerif();
                if (c == invert_col)
                {
                    cell_params.type = "checkbox";
                    cell_params.font_halign = "right";
                }
                row_params.columns.add(cell_params);
            }
            LLSD row_value;
            row_value["input_type"] = input_type;
            row_value["action"] = action;
            row_params.value = row_value;

            LLScrollListItem* row = grid->addRow(row_params);
            row->getColumn(0)->setValue(item->getColumn(0)->getValue());
            row->getColumn(1)->setValue(glyph.empty() ? LLSD() : LLSD(glyph));
            row->getColumn(2)->setValue(blankIfNone(inputLabel(input_selector, input_value)));
            if (invert_col >= 0)
            {
                bool mapped = !input_value.empty() && input_value != none_value;
                row->getColumn(invert_col)->setValue(LLGameControl::getAxisInvert(mode, action));
                row->getColumn(invert_col)->setEnabled(mapped);
            }
        }
    };
    addBlock(mActionMappingsAxes, MODE_INPUT_TYPE_AXES, actionSelectorForMode(true, mode), mAxisInputSelector);
    addBlock(mActionMappingsButtons, MODE_INPUT_TYPE_BUTTONS, actionSelectorForMode(false, mode), mButtonInputSelector);

    // OCOL: this mode's keyboard keys, then a row to add another
    auto addKeyRow = [&](const std::string& action, const std::string& label, const std::string& input_value)
    {
        std::string glyph = promptFontGlyph(input_value);
        LLScrollListItem::Params row_params;
        for (S32 c = 0; c < mActionMappingsButtons->getNumColumns(); ++c)
        {
            LLScrollListCell::Params cell_params;
            cell_params.column = mActionMappingsButtons->getColumn(c)->mName;
            cell_params.font = (c == 1 && !glyph.empty()) ? promptFontForCell() : LLFontGL::getFontSansSerif();
            row_params.columns.add(cell_params);
        }
        LLSD row_value;
        row_value["input_type"] = MODE_INPUT_TYPE_BUTTONS;
        row_value["action"] = action;
        row_params.value = row_value;
        LLScrollListItem* row = mActionMappingsButtons->addRow(row_params);
        row->getColumn(0)->setValue(label);
        row->getColumn(1)->setValue(glyph.empty() ? LLSD() : LLSD(glyph));
        row->getColumn(2)->setValue(blankIfNone(inputLabel(mButtonInputSelector, input_value)));
    };
    LLSD buttons = LLGameControl::getModeMapping(mode, MODE_INPUT_TYPE_BUTTONS);
    for (auto it = buttons.beginMap(); it != buttons.endMap(); ++it)
    {
        if (isKeyAction(it->first))
        {
            addKeyRow(it->first, actionLabel(it->first, false), it->second.asString());
        }
    }
    addKeyRow(ADD_KEY_ACTION, "Add keyboard key...", LLStringUtil::null);
}

// A click on the editable Input columns of a row opens the matching selector.
void LLPanelPreferenceGameControl::onGridSelect(LLUICtrl* ctrl)
{
    clearSelectionState();
    LLScrollListCtrl* table = dynamic_cast<LLScrollListCtrl*>(ctrl);
    if (!table || !table->getEnabled())
    {
        return;
    }
    LLScrollListCtrl* sibling = (table == mActionMappingsAxes) ? mActionMappingsButtons : mActionMappingsAxes;
    sibling->deselectAllItems(true);

    LLScrollListItem* item = table->getFirstSelected();
    if (!item)
    {
        return;
    }
    if (table == mActionMappingsAxes)
    {
        // clicking the Invert checkbox selects the row but doesn't store it
        const std::string mode = currentEditMode();
        std::string action = item->getValue()["action"].asString();
        LLSD mapping = LLGameControl::getModeMapping(mode, MODE_INPUT_TYPE_AXES);
        std::string input_value = mapping.has(action) ? mapping[action].asString() : LLStringUtil::null;
        if (!input_value.empty() && input_value != AXIS_NONE)
        {
            S32 invert_col = mActionMappingsAxes->getColumn("invert")->mIndex;
            LLGameControl::setAxisInvert(mode, action, item->getColumn(invert_col)->getValue().asBoolean());
        }
    }
    else
    {
        // OCOL: the Action cell of a keyboard key row picks the key
        std::string action = item->getValue()["action"].asString();
        if (action == ADD_KEY_ACTION || (isKeyAction(action) && item->getSelectedCell() == 0))
        {
            table->deselectAllItems();
            mPendingKeyInput.clear();
            openKeyDialog(action);
            return;
        }
    }
    if (!initCombobox(item, table))
    {
        table->deselectAllItems();
    }
}

// The icon column (1) opens the glyph selector, the text column (2) the text one;
// both commit through onCommitInputChannel().
bool LLPanelPreferenceGameControl::initCombobox(LLScrollListItem* item, LLScrollListCtrl* grid)
{
    LLSD row_value = item->getValue();
    if (!row_value.isMap())
    {
        return false;
    }
    const bool axes = (grid == mActionMappingsAxes);
    const S32 sel = item->getSelectedCell();
    LLComboBox* combobox = nullptr;
    if (sel == 1)
    {
        combobox = axes ? mAxisInputGlyphSelector : mButtonInputGlyphSelector;
    }
    else if (sel == 2)
    {
        combobox = axes ? mAxisInputSelector : mButtonInputSelector;
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
    sSelectedRow = row_index;
    sSelectedCol = sel;
    sSelectedOpenRect = cellRect(grid, combobox->getParent(), row_index, sel);

    LLSD mapping = LLGameControl::getModeMapping(currentEditMode(), row_value["input_type"].asString());
    std::string action = row_value["action"].asString();
    std::string value = mapping.has(action) ? mapping[action].asString() : LLStringUtil::null;
    if (value.empty())
    {
        value = combobox->getAllData().back()->getValue().asString(); // "None"
    }
    combobox->setValue(value);
    combobox->setVisible(true);
    combobox->showList();

    sSelectedGrid = grid;
    sSelectedItem = item;
    sSelectedCell = cell;
    sSelectedCombobox = combobox;
    sSelectedCellValue = cell->getValue();
    cell->setValue(LLSD());
    return true;
}

void LLPanelPreferenceGameControl::onCommitInputChannel(LLUICtrl* ctrl)
{
    LLComboBox* combobox = dynamic_cast<LLComboBox*>(ctrl);
    if (!sSelectedGrid || !sSelectedItem || !sSelectedCell || !combobox)
    {
        return;
    }
    LLSD row_value = sSelectedItem->getValue();
    const std::string mode = currentEditMode();
    const std::string input_type = row_value["input_type"].asString();
    const std::string action = row_value["action"].asString();
    const LLComboBox* input_selector = (input_type == MODE_INPUT_TYPE_AXES) ? mAxisInputSelector : mButtonInputSelector;
    const std::string input_value = combobox->getValue().asString();
    const std::string input_label = combobox->getSelectedItemLabel();
    if (input_label != NONE_LABEL)
    {
        // an input drives at most one action within a block
        removeDuplicateActionInput(mode, input_type, action, input_value, input_selector);
    }
    LLGameControl::updateModeMapping(mode, input_type, action, input_value);
    sSelectedCell->setValue(blankIfNone(input_label));

    sSelectedCellValue = sSelectedCell->getValue();
    sSelectedGrid->deselectAllItems();
    clearSelectionState();
    populateActionMappings();
    refreshDiagram();
}

void LLPanelPreferenceGameControl::removeDuplicateActionInput(const std::string& mode,
    const std::string& input_type, const std::string& keep_action, const std::string& input_value,
    const LLComboBox* input_selector)
{
    std::string none_value = input_selector->getAllData().back()->getValue().asString();
    LLSD mapping = LLGameControl::getModeMapping(mode, input_type);
    for (auto it = mapping.beginMap(); it != mapping.endMap(); ++it)
    {
        if (it->first != keep_action && it->second.asString() == input_value)
        {
            LLGameControl::updateModeMapping(mode, input_type, it->first, none_value);
        }
    }
}

void LLPanelPreferenceGameControl::clearSelectionState()
{
    if (sSelectedCell)
    {
        sSelectedCell->setValue(sSelectedCellValue);
    }
    sSelectedCellValue = LLSD();
    if (sSelectedGrid)
    {
        sSelectedGrid->deselectAllItems(true);
    }
    sSelectedGrid = nullptr;
    sSelectedItem = nullptr;
    sSelectedCell = nullptr;
    sSelectedCombobox = nullptr;
    sSelectedRow = -1;
    sSelectedCol = -1;
    for (LLUICtrl* ctrl : { (LLUICtrl*)mAxisInputSelector, (LLUICtrl*)mButtonInputSelector,
                            (LLUICtrl*)mAxisInputGlyphSelector, (LLUICtrl*)mButtonInputGlyphSelector })
    {
        if (ctrl)
        {
            ctrl->setVisible(false);
        }
    }
}

// OCOL: the key dialog for a keyboard key row ("add_key" adds a new one)
void LLPanelPreferenceGameControl::openKeyDialog(const std::string& action)
{
    LLSetKeyBindDialog* dialog = LLFloaterReg::getTypedInstance<LLSetKeyBindDialog>("keybind_dialog", LLSD());
    if (!dialog)
    {
        return;
    }
    mEditingKeyAction = action;
    dialog->setParent(this, mListMode ? (LLView*)mActionMappingsButtons : (LLView*)mButtonAction,
                      ALLOW_KEYS | ALLOW_MASK_KEYS | ALLOW_MASKS | ALLOW_MENU_KEYS);
    if (LLFloater* root_floater = gFloaterView->getParentFloater(this))
    {
        root_floater->addDependentFloater(dialog);
    }
    dialog->openFloater();
    dialog->setFocus(true);
}

void LLPanelPreferenceGameControl::onCancelKeyBind()
{
    mEditingKeyAction.clear();
    mPendingKeyInput.clear();
}

void LLPanelPreferenceGameControl::onDefaultKeyBind(bool all_modes)
{
    // keyboard keys have no default; "Default" removes the key like "Set Empty"
    onSetKeyBind(CLICK_NONE, KEY_NONE, MASK_NONE, all_modes);
}

bool LLPanelPreferenceGameControl::onSetKeyBind(EMouseClickType click, KEY key, MASK mask, bool all_modes)
{
    std::string editing = mEditingKeyAction;
    std::string pending_input = mPendingKeyInput;
    mEditingKeyAction.clear();
    mPendingKeyInput.clear();
    if (editing.empty())
    {
        return true;
    }

    const std::string mode = currentEditMode();
    LLSD buttons = LLGameControl::getModeMapping(mode, MODE_INPUT_TYPE_BUTTONS);

    // the button a key row keeps, or the one the inspector is adding it to
    std::string input_value = pending_input.empty() ? BUTTON_NONE : pending_input;
    if (editing != ADD_KEY_ACTION && buttons.has(editing))
    {
        input_value = buttons[editing].asString();
        buttons.erase(editing);
    }
    if (key != KEY_NONE)
    {
        std::string action = LLViewerInput::controllerKeyAction(key, mask);
        if (!buttons.has(action) || editing != ADD_KEY_ACTION || !pending_input.empty())
        {
            if (!pending_input.empty())
            {
                // added from the inspector: the key takes this button over
                for (auto it = buttons.beginMap(); it != buttons.endMap(); ++it)
                {
                    if (it->second.asString() == pending_input)
                    {
                        it->second = BUTTON_NONE;
                    }
                }
            }
            buttons[action] = input_value;
        }
    }
    LLGameControl::setModeMapping(mode, MODE_INPUT_TYPE_BUTTONS, buttons);
    populateActionMappings();
    refreshDiagram();
    return true;
}

// ===========================================================================
// Presets and resets
// ===========================================================================

// Named copies of every mode's bindings, toggles and tuning (the ModeMappings block),
// kept in their own setting so they survive a reset and apply to any controller.
void LLPanelPreferenceGameControl::populatePresets()
{
    mPresetList->removeall();
    mPresetList->add(std::string("Presets..."), LLSD(std::string()));
    // bindings start blank; the ready-made layouts come first, then the user's own
    for (size_t i = 0; i < builtInPresets().size(); ++i)
    {
        mPresetList->add(builtInPresets()[i].first, LLSD(BUILTIN_PRESET_PREFIX + std::to_string(i)));
    }
    mPresetList->addSeparator();
    LLSD presets = gSavedSettings.getLLSD(PRESETS_SETTING);
    for (auto it = presets.beginMap(); it != presets.endMap(); ++it)
    {
        mPresetList->add(it->first, LLSD(it->first));
    }
    mPresetList->selectFirstItem();
}

void LLPanelPreferenceGameControl::onPresetSelected()
{
    std::string name = mPresetList->getValue().asString();
    LLSD presets = gSavedSettings.getLLSD(PRESETS_SETTING);
    const LLSD* built_in = builtInPreset(name);
    if (!built_in && (name.empty() || !presets.has(name)))
    {
        return;
    }
    LLSD settings = LLGameControl::getGameControlSettings();
    settings["ModeMappings"] = built_in ? *built_in : presets[name];
    LLGameControl::setGameControlSettings(settings);
    populateActionMappings();
    updateActionModeEnabledUI();
}

void LLPanelPreferenceGameControl::onPresetSave()
{
    LLHandle<LLPanel> handle = getHandle();
    LLNotificationsUtil::add("GameControlSavePreset", LLSD(), LLSD(),
        [handle](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
            {
                return false;
            }
            std::string name = response["message"].asString();
            LLStringUtil::trim(name);
            LLPanelPreferenceGameControl* panel = static_cast<LLPanelPreferenceGameControl*>(handle.get());
            if (name.empty() || !panel || LLStringUtil::startsWith(name, BUILTIN_PRESET_PREFIX))
            {
                return false;
            }
            LLSD presets = gSavedSettings.getLLSD(PRESETS_SETTING);
            if (!presets.isMap())
            {
                presets = LLSD::emptyMap();
            }
            presets[name] = LLGameControl::getGameControlSettings()["ModeMappings"];
            gSavedSettings.setLLSD(PRESETS_SETTING, presets);
            panel->populatePresets();
            panel->mPresetList->setValue(name);
            return false;
        });
}

void LLPanelPreferenceGameControl::onPresetDelete()
{
    std::string name = mPresetList->getValue().asString();
    LLSD presets = gSavedSettings.getLLSD(PRESETS_SETTING);
    if (name.empty() || !presets.has(name))
    {
        return;
    }
    presets.erase(name);
    gSavedSettings.setLLSD(PRESETS_SETTING, presets);
    populatePresets();
}

void LLPanelPreferenceGameControl::resetModeToDefaults(const std::string& mode)
{
    LLSD defaults = LLGameControl::getDefaultModeMappings();
    LLGameControl::setModeMapping(mode, MODE_INPUT_TYPE_AXES, defaults[mode][MODE_INPUT_TYPE_AXES]);
    LLGameControl::setModeMapping(mode, MODE_INPUT_TYPE_BUTTONS, defaults[mode][MODE_INPUT_TYPE_BUTTONS]);
    LLGameControl::setModeMapping(mode, MODE_INPUT_TYPE_AXES_INVERT, defaults[mode][MODE_INPUT_TYPE_AXES_INVERT]);
    LLGameControl::setModeMapping(mode, MODE_BUTTON_TOGGLE, LLSD::emptyMap());
    LLGameControl::setStickTuning(mode, LLGameControl::StickTuning()); // OCOL; also clears the per-stick tuning
    if (mode == "FlyCam")
    {
        // these live outside the mappings restored above
        LLGameControl::setFlycamRollAllowed(false);
        LLGameControl::setSpeedFactor("FlyCam", 1.0f);
        LLGameControl::setFlycamCrosshairEnabled(false);
        LLGameControl::setFlycamHideCursorEnabled(true);
    }
    else if (mode == "Avatar" || mode == "Mouselook")
    {
        // Linden Lab's turn speed for these modes; Sensitivity above does that job here
        LLGameControl::setSpeedFactor(mode, 1.0f);
    }
}

void LLPanelPreferenceGameControl::onResetActionsToDefaults()
{
    clearSelectionState();
    resetModeToDefaults(currentEditMode());
    populateActionMappings();
    updateActionModeEnabledUI();
}

void LLPanelPreferenceGameControl::onResetAll()
{
    LLHandle<LLPanel> handle = getHandle();
    LLNotificationsUtil::add("GameControlResetAll", LLSD(), LLSD(),
        [handle](const LLSD& notification, const LLSD& response)
        {
            LLPanelPreferenceGameControl* panel = static_cast<LLPanelPreferenceGameControl*>(handle.get());
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0 || !panel)
            {
                return false;
            }
            panel->clearSelectionState();
            for (const char* mode : MODE_NAMES)
            {
                panel->resetModeToDefaults(mode);
            }
            panel->populateActionMappings();
            panel->updateActionModeEnabledUI();
            return false;
        });
}
