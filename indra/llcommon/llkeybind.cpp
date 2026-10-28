/**
 * @file llkeybind.cpp
 * @brief Information about key combinations.
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

#include "linden_common.h"

#include "llkeybind.h"

#include "llsd.h"
#include "llsdutil.h"
#include <algorithm>

LLKeyData::LLKeyData()
    :
    mMouse(CLICK_NONE),
    mKey(KEY_NONE),
    mMask(MASK_NONE),
    mIgnoreMasks(false),
    mControllerActionType(255),
    mControllerAction(0)
{
}

LLKeyData::LLKeyData(EMouseClickType mouse, KEY key, MASK mask)
    :
    mMouse(mouse),
    mKey(key),
    mMask(mask),
    mIgnoreMasks(false),
    mControllerActionType(255),
    mControllerAction(0)
{
}

LLKeyData::LLKeyData(EMouseClickType mouse, KEY key, bool ignore_mask)
    :
    mMouse(mouse),
    mKey(key),
    mMask(MASK_NONE),
    mIgnoreMasks(ignore_mask),
    mControllerActionType(255),
    mControllerAction(0)
{
}

LLKeyData::LLKeyData(EMouseClickType mouse, KEY key, MASK mask, bool ignore_mask)
    :
    mMouse(mouse),
    mKey(key),
    mMask(mask),
    mIgnoreMasks(ignore_mask),
    mControllerActionType(255),
    mControllerAction(0)
{
}

LLKeyData::LLKeyData(U8 actionType, U8 action)
    :
    mMouse(CLICK_NONE),
    mKey(KEY_NONE),
    mMask(MASK_NONE),
    mIgnoreMasks(true),
    mControllerActionType(actionType),
    mControllerAction(action)
{

}

// OCOL: these names and indices must match LLGameControl::actionFromString()/
// stringFromAction() (llwindow can't be used from llcommon). 0 = axis, 1 = button.
static const char* CONTROLLER_AXIS_NAMES[] = {
    "axis_left", "axis_right", "axis_forward", "axis_backward",
    "axis_turn_left", "axis_turn_right", "axis_look_up", "axis_look_down",
    "axis_up", "axis_down", "axis_roll_left", "axis_roll_right" };
static const char* CONTROLLER_BUTTON_NAMES[] = {
    "button_south", "button_east", "button_west", "button_north",
    "button_back", "button_guide", "button_start", "button_leftstick", "button_rightstick",
    "button_leftshoulder", "button_rightshoulder",
    "button_dpad_up", "button_dpad_down", "button_dpad_left", "button_dpad_right",
    "button_misc1", "button_paddle1", "button_paddle2", "button_paddle3", "button_paddle4",
    "button_touchpad",
    "button_misc2", "button_misc3", "button_misc4", "button_misc5", "button_misc6", // SDL MISC2..6
    "", "", "", "", // 26-29 unused
    "button_lefttrigger", "button_righttrigger" };

static std::string controllerStringFromKeyData(const U8 actionType, const U8 action)
{
    if (actionType == 0 && action < LL_ARRAY_SIZE(CONTROLLER_AXIS_NAMES))
    {
        return CONTROLLER_AXIS_NAMES[action];
    }
    if (actionType == 1 && action < LL_ARRAY_SIZE(CONTROLLER_BUTTON_NAMES) && CONTROLLER_BUTTON_NAMES[action][0])
    {
        return CONTROLLER_BUTTON_NAMES[action];
    }
    return std::string();
}

static bool controllerActionFromString(const std::string& string, U8& actionType, U8& action)
{
    actionType = 255;
    for (U8 i = 0; i < LL_ARRAY_SIZE(CONTROLLER_AXIS_NAMES); ++i)
    {
        if (string == CONTROLLER_AXIS_NAMES[i])
        {
            actionType = 0;
            action = i;
            return true;
        }
    }
    for (U8 i = 0; i < LL_ARRAY_SIZE(CONTROLLER_BUTTON_NAMES); ++i)
    {
        if (CONTROLLER_BUTTON_NAMES[i][0] && string == CONTROLLER_BUTTON_NAMES[i])
        {
            actionType = 1;
            action = i;
            return true;
        }
    }
    return false;
}

LLKeyData::LLKeyData(const LLSD &key_data)
{
    if (key_data.has("mouse"))
    {
        mMouse = (EMouseClickType)key_data["mouse"].asInteger();
    }
    if (key_data.has("key"))
    {
        mKey = key_data["key"].asInteger();
    }
    if (key_data.has("ignore_accelerators"))
    {
        mIgnoreMasks = key_data["ignore_accelerators"];
    }
    if (key_data.has("mask"))
    {
        mMask = key_data["mask"].asInteger();
    }
    if (key_data.has("controller"))
    {
        U8 actionType = 255;
        U8 action = 0;
        controllerActionFromString(key_data["controller"],actionType, action);
        mControllerActionType = actionType;
        mControllerAction = action;
    }
}

LLSD LLKeyData::asLLSD() const
{
    LLSD data;
    data["mouse"] = (LLSD::Integer)mMouse;
    data["key"] = (LLSD::Integer)mKey;
    data["mask"] = (LLSD::Integer)mMask;
    if (mIgnoreMasks)
    {
        data["ignore_accelerators"] = (LLSD::Boolean)mIgnoreMasks;
    }
    if (isController())
    {
        data["controller"] = (LLSD::String)controllerStringFromKeyData(mControllerActionType, mControllerAction);
    }
    return data;
}

bool LLKeyData::isEmpty() const
{
    return mMouse == CLICK_NONE && mKey == KEY_NONE && !isController();
}

bool LLKeyData::isController() const
{
    return mControllerActionType != 255;
}

void LLKeyData::reset()
{
    mMouse = CLICK_NONE;
    mKey = KEY_NONE;
    mMask = MASK_NONE;
    mIgnoreMasks = false;
    mControllerActionType = 255;
    mControllerAction = 0;
}

LLKeyData& LLKeyData::operator=(const LLKeyData& rhs)
{
    mMouse = rhs.mMouse;
    mKey = rhs.mKey;
    mMask = rhs.mMask;
    mIgnoreMasks = rhs.mIgnoreMasks;
    mControllerActionType = rhs.mControllerActionType;
    mControllerAction = rhs.mControllerAction;
    return *this;
}

bool LLKeyData::operator==(const LLKeyData& rhs) const
{
    if (mMouse != rhs.mMouse) return false;
    if (mKey != rhs.mKey) return false;
    if (mMask != rhs.mMask) return false;
    if (mIgnoreMasks != rhs.mIgnoreMasks) return false;
    if (mControllerActionType != rhs.mControllerActionType) return false;
    if (mControllerAction != rhs.mControllerAction) return false;
    return true;
}

bool LLKeyData::operator!=(const LLKeyData& rhs) const
{
    if (mMouse != rhs.mMouse) return true;
    if (mKey != rhs.mKey) return true;
    if (mMask != rhs.mMask) return true;
    if (mIgnoreMasks != rhs.mIgnoreMasks) return true;
    if (mControllerActionType != rhs.mControllerActionType) return true;
    if (mControllerAction != rhs.mControllerAction) return true;
    return false;
}

bool LLKeyData::canHandle(const LLKeyData& data) const
{
    if (data.mKey == mKey
        && data.mMouse == mMouse
        && ((mIgnoreMasks && (data.mMask & mMask) == mMask) || data.mMask == mMask)
        && (mControllerActionType == data.mControllerActionType && mControllerAction == data.mControllerAction)
    )
    {
        return true;
    }
    return false;
}

bool LLKeyData::canHandle(EMouseClickType mouse, KEY key, MASK mask) const
{
    if (mouse == mMouse
        && key == mKey
        && ((mIgnoreMasks && (mask & mMask) == mMask) || mask == mMask))
    {
        return true;
    }
    return false;
}

// LLKeyBind

LLKeyBind::LLKeyBind(const LLSD &key_bind)
{
    if (key_bind.isArray())
    {
        for (LLSD::array_const_iterator data = key_bind.beginArray(), endLists = key_bind.endArray();
            data != endLists;
            data++
            )
        {
            mData.push_back(LLKeyData(*data));
        }
    }
}

bool LLKeyBind::operator==(const LLKeyBind& rhs) const
{
    auto size = mData.size();
    if (size != rhs.mData.size()) return false;

    for (size_t i = 0; i < size; i++)
    {
        if (mData[i] != rhs.mData[i]) return false;
    }

    return true;
}

bool LLKeyBind::operator!=(const LLKeyBind& rhs) const
{
    auto size = mData.size();
    if (size != rhs.mData.size()) return true;

    for (U32 i = 0; i < size; i++)
    {
        if (mData[i] != rhs.mData[i]) return true;
    }

    return false;
}

bool LLKeyBind::isEmpty() const
{
    for (const LLKeyData& key_data : mData)
    {
        if (!key_data.isEmpty()) return false;
    }
    return true;
}

LLKeyBind::data_vector_t::const_iterator LLKeyBind::endNonEmpty() const
{
    // search backwards for last non-empty entry, then turn back into forwards
    // iterator (.base() call)
    return std::find_if_not(mData.rbegin(), mData.rend(),
                            [](const auto& kdata){ return kdata.empty(); }).base();
}

LLSD LLKeyBind::asLLSD() const
{
    LLSD data;
    for (const LLKeyData& key_data : mData)
    {
        // append intermediate entries even if empty to not affect visual
        // representation
        data.append(key_data.asLLSD());
    }
    return data;
}

bool LLKeyBind::canHandle(EMouseClickType mouse, KEY key, MASK mask) const
{
    if (mouse == CLICK_NONE && key == KEY_NONE)
    {
        // assume placeholder
        return false;
    }

    for (const LLKeyData& key_data : mData)
    {
        if (key_data.canHandle(mouse, key, mask))
        {
            return true;
        }
    }
    return false;
}

bool LLKeyBind::canHandleKey(KEY key, MASK mask) const
{
    return canHandle(CLICK_NONE, key, mask);
}

bool LLKeyBind::canHandleMouse(EMouseClickType mouse, MASK mask) const
{
    return canHandle(mouse, KEY_NONE, mask);
}

bool LLKeyBind::hasKeyData(EMouseClickType mouse, KEY key, MASK mask, bool ignore) const
{
    if (mouse != CLICK_NONE || key != KEY_NONE)
    {
        for (const LLKeyData& key_data : mData)
        {
            if (key_data.mKey == key
                && key_data.mMask == mask
                && key_data.mMouse == mouse
                && key_data.mIgnoreMasks == ignore)
            {
                return true;
            }
        }
    }
    return false;
}

bool LLKeyBind::hasKeyData(const LLKeyData& data) const
{
    if (data.isController())
    {
        // OCOL: controller bindings only match the same controller input
        for (const LLKeyData& key_data : mData)
        {
            if (key_data.mControllerActionType == data.mControllerActionType
                && key_data.mControllerAction == data.mControllerAction)
            {
                return true;
            }
        }
        return false;
    }
    return hasKeyData(data.mMouse, data.mKey, data.mMask, data.mIgnoreMasks);
}

bool LLKeyBind::hasKeyData(U32 index) const
{
    return mData.size() > index;
}

S32 LLKeyBind::findKeyData(EMouseClickType mouse, KEY key, MASK mask, bool ignore) const
{
    if (mouse != CLICK_NONE || key != KEY_NONE)
    {
        for (S32 i = 0; i < mData.size(); ++i)
        {
            if (mData[i].mKey == key
                && mData[i].mMask == mask
                && mData[i].mMouse == mouse
                && mData[i].mIgnoreMasks == ignore)
            {
                return i;
            }
        }
    }
    return -1;
}

S32 LLKeyBind::findKeyData(const LLKeyData& data) const
{
    if (data.isController())
    {
        // OCOL: controller bindings only match the same controller input
        for (S32 i = 0; i < (S32)mData.size(); ++i)
        {
            if (mData[i].mControllerActionType == data.mControllerActionType
                && mData[i].mControllerAction == data.mControllerAction)
            {
                return i;
            }
        }
        return -1;
    }
    return findKeyData(data.mMouse, data.mKey, data.mMask, data.mIgnoreMasks);
}

LLKeyData LLKeyBind::getKeyData(U32 index) const
{
    if (mData.size() > index)
    {
        return mData[index];
    }
    return LLKeyData();
}

bool LLKeyBind::addKeyData(EMouseClickType mouse, KEY key, MASK mask, bool ignore)
{
    if (!hasKeyData(mouse, key, mask, ignore))
    {
        mData.push_back(LLKeyData(mouse, key, mask, ignore));
        return true;
    }
    return false;
}

bool LLKeyBind::addKeyData(const LLKeyData& data)
{
    if (!hasKeyData(data))
    {
        mData.push_back(data);
        return true;
    }
    return false;
}

void LLKeyBind::replaceKeyData(EMouseClickType mouse, KEY key, MASK mask, bool ignore, U32 index)
{
    replaceKeyData(LLKeyData(mouse, key, mask, ignore), index);
}

void LLKeyBind::replaceKeyData(const LLKeyData& data, U32 index)
{
    if (!data.isEmpty())
    {
        // if both click and key are none (isEmpty()), we are inserting a placeholder, we don't want to reset anything
        // otherwise reset identical key
        for (LLKeyData& key_data : mData)
        {
            if (key_data.mKey == data.mKey
                && key_data.mMouse == data.mMouse
                && key_data.mIgnoreMasks == data.mIgnoreMasks
                && key_data.mMask == data.mMask
                && key_data.mControllerActionType == data.mControllerActionType
                && key_data.mControllerAction == data.mControllerAction)
            {
                // Replacing only fully equal combinations even in case 'ignore' is set
                // Reason: Simplicity and user might decide to do a 'move' command as W and Shift+Ctrl+W, and 'run' as Shift+W
                key_data.reset();
                break;
            }
        }
    }
    if (mData.size() <= index)
    {
        mData.resize(index + 1);
    }
    mData[index] = data;
}

void LLKeyBind::resetKeyData(S32 index)
{
    if (mData.size() > index)
    {
        mData[index].reset();
    }
}

void LLKeyBind::trimEmpty()
{
    mData.erase(endNonEmpty(), mData.end());
}

size_t LLKeyBind::getDataCount()
{
    return mData.size();
}
