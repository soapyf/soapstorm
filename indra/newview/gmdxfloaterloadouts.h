/**
 * @file gmdxfloaterloadouts.h
 * @brief GunMetal-DX Loadouts window (Equipment, Loadouts, Categories) and the swap toast.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

#pragma once

#include "llfloater.h"
#include "gmdxloadouts.h"
#include "llsetkeybinddialog.h"

class LLButton;
class LLContextMenu;
class LLProgressBar;
class LLScrollListCtrl;
class LLTextBox;

class GMDXFloaterLoadouts : public LLFloater, public LLKeyBindResponderInterface
{
public:
    GMDXFloaterLoadouts(const LLSD& key);
    ~GMDXFloaterLoadouts();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    // A loadout notecard dropped here becomes a loadout (GMDXLoadouts::importNotecard).
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    void refresh() override;
    void refreshEquipment();
    void refreshLoadouts();
    void refreshCategories();

    void onWear();
    void onWeaponMenu(S32 x, S32 y);   // right-click: the weapon's parts, Remove from Equipment
    void onSaveAs();
    void onSlotClicked();
    void onNewLoadout();
    void onRenameLoadout();
    void onCopyLoadout();
    void onDeleteLoadout();
    void onEquip();
    void onAddCategory();
    void onRenameCategory();
    void onMoveCategory(S32 delta);
    void onRemoveCategory();
    void onSetKey();
    // LLKeyBindResponderInterface (the Key... dialog)
    void onCancelKeyBind() override { mKeySlot = -1; GMDXLoadouts::applyHotkeys(); }
    void onDefaultKeyBind(bool all_modes) override;
    bool onSetKeyBind(EMouseClickType click, KEY key, MASK mask, bool all_modes) override;

    std::string selectedLayer() const;
    LLUUID selectedLoadout() const;

    LLScrollListCtrl* mLayerList = nullptr;
    LLScrollListCtrl* mWeaponList = nullptr;
    LLScrollListCtrl* mLoadoutList = nullptr;
    LLScrollListCtrl* mSlotList = nullptr;
    LLScrollListCtrl* mCategoryList = nullptr;
    LLTextBox* mLoadoutStatus = nullptr;
    LLTextBox* mLoadoutName = nullptr;
    LLTextBox* mWornText = nullptr;
    LLButton* mRevertBtn = nullptr;
    LLButton* mUpdateBtn = nullptr;

    std::vector<GMDXLoadouts::Weapon> mWeapons;  // the selected layer's, for the weapon list
    LLHandle<LLContextMenu> mWeaponMenu;
    S32 mKeySlot = -1;   // the slot the Key... dialog is setting
    U64 mGeneration = 0;
};

class GMDXFloaterLoadoutToast : public LLFloater
{
public:
    GMDXFloaterLoadoutToast(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    LLTextBox* mText = nullptr;
    LLProgressBar* mProgress = nullptr;
    LLButton* mCancelBtn = nullptr;
    LLButton* mOkBtn = nullptr;
    LLButton* mPickBtn = nullptr;
};
