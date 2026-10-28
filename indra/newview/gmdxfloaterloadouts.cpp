/**
 * @file gmdxfloaterloadouts.cpp
 * @brief Loadouts window (Equipment, Loadouts, Categories) and the swap toast.
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

#include "llviewerprecompiledheaders.h"

#include "gmdxfloaterloadouts.h"

#include "llbutton.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llprogressbar.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "gmdxweaponsorter.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llui.h"
#include "llmenugl.h"
#include "llviewermenu.h"

namespace
{
    const F64 kToastSeconds = 3.0;  // a clean swap's toast stays this long

    // Asks for a name; on OK calls back with what was typed.
    void ask_name(const std::string& title, const std::string& name, std::function<void(const std::string&)> cb)
    {
        LLSD args;
        args["TITLE"] = title;
        args["NAME"] = name;
        LLNotificationsUtil::add("GMDXLoadoutName", args, LLSD(), [cb](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                cb(response["message"].asString());
            }
            return false;
        });
    }

    // "[Ash] - Ebony Spear v1.0.4" -> "Ebony Spear v1.0.4" (the tag dropped; menus put the version apart).
    std::string short_name(const std::string& name)
    {
        std::string title, version;
        GMDXWeaponSorter::splitName(name, title, version);
        return version.empty() ? title : title + " " + version;
    }

    LLSD text_row(const LLSD& value, std::initializer_list<std::pair<const char*, std::string>> cells, bool bold = false, bool enabled = true)
    {
        LLSD row;
        row["value"] = value;
        row["enabled"] = enabled;
        S32 i = 0;
        for (const auto& cell : cells)
        {
            row["columns"][i]["column"] = cell.first;
            row["columns"][i]["value"] = cell.second;
            if (bold)
            {
                row["columns"][i]["font"]["style"] = "BOLD";
            }
            ++i;
        }
        return row;
    }
}

//-----------------------------------------------------------------------------
// GMDXFloaterLoadouts
//-----------------------------------------------------------------------------

GMDXFloaterLoadouts::GMDXFloaterLoadouts(const LLSD& key)
:   LLFloater(key)
{
}

bool GMDXFloaterLoadouts::postBuild()
{
    mLayerList = getChild<LLScrollListCtrl>("layer_list");
    mWeaponList = getChild<LLScrollListCtrl>("weapon_list");
    mLoadoutList = getChild<LLScrollListCtrl>("loadout_list");
    mSlotList = getChild<LLScrollListCtrl>("slot_list");
    mCategoryList = getChild<LLScrollListCtrl>("category_list");
    mLoadoutStatus = getChild<LLTextBox>("loadout_status");
    mLoadoutName = getChild<LLTextBox>("loadout_name");
    mWornText = getChild<LLTextBox>("worn_text");
    mRevertBtn = getChild<LLButton>("revert_btn");
    mUpdateBtn = getChild<LLButton>("update_btn");

    GMDXLoadouts& lo = GMDXLoadouts::instance();

    // Equipment
    mLayerList->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshEquipment(); });
    mWeaponList->setDoubleClickCallback([this] { onWear(); });
    mWeaponList->setRightMouseDownCallback([this](LLUICtrl*, S32 x, S32 y, MASK) { onWeaponMenu(x, y); });
    getChild<LLButton>("wear_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onWear(); });
    mRevertBtn->setCommitCallback([&lo](LLUICtrl*, const LLSD&) { lo.equip(lo.wornLoadout()); });
    mUpdateBtn->setCommitCallback([&lo](LLUICtrl*, const LLSD&) { lo.saveWornInto(lo.wornLoadout()); });
    getChild<LLButton>("save_new_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSaveAs(); });

    // Loadouts
    mLoadoutList->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshLoadouts(); });
    mLoadoutList->setDoubleClickCallback([this] { onEquip(); });
    mSlotList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSlotClicked(); });
    getChild<LLButton>("new_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onNewLoadout(); });
    getChild<LLButton>("rename_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRenameLoadout(); });
    getChild<LLButton>("copy_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCopyLoadout(); });
    getChild<LLButton>("delete_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDeleteLoadout(); });
    getChild<LLButton>("share_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        GMDXLoadouts::instance().shareLoadout(selectedLoadout());
    });
    LLButton* equip_btn = getChild<LLButton>("equip_btn");
    equip_btn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEquip(); });
    getChild<LLPanel>("loadouts_panel")->setDefaultBtn(equip_btn);

    // Categories
    getChild<LLButton>("add_cat_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddCategory(); });
    getChild<LLButton>("rename_cat_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRenameCategory(); });
    getChild<LLButton>("up_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMoveCategory(-1); });
    getChild<LLButton>("down_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMoveCategory(1); });
    getChild<LLButton>("remove_cat_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemoveCategory(); });
    getChild<LLButton>("key_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetKey(); });
    LLLineEditor* watch = getChild<LLLineEditor>("watch_edit");
    watch->setText(gSavedPerAccountSettings.getString("GMDXLoadoutWatchTags"));
    watch->setCommitOnFocusLost(true);
    watch->setCommitCallback([](LLUICtrl* ctrl, const LLSD&)
    {
        gSavedPerAccountSettings.setString("GMDXLoadoutWatchTags", ctrl->getValue().asString());
    });
    getChild<LLButton>("scan_btn")->setCommitCallback([watch](LLUICtrl*, const LLSD&)
    {
        gSavedPerAccountSettings.setString("GMDXLoadoutWatchTags", watch->getText());
        GMDXWeaponSorter::instance().scanInventory();
    });
    return true;
}

void GMDXFloaterLoadouts::onOpen(const LLSD& key)
{
    GMDXLoadouts::instance().ensureFolders();
    if (key.has("tab"))
    {
        getChild<LLTabContainer>("tabs")->selectTabByName(key["tab"].asString());
    }
    refresh();
}

void GMDXFloaterLoadouts::draw()
{
    const U64 generation = GMDXLoadouts::instance().generation();
    if (generation != mGeneration)
    {
        refresh();
    }
    LLFloater::draw();
}

void GMDXFloaterLoadouts::refresh()
{
    mGeneration = GMDXLoadouts::instance().generation();
    refreshEquipment();
    refreshLoadouts();
    refreshCategories();
}

bool GMDXFloaterLoadouts::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type,
                                            void* cargo_data, EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type != DAD_NOTECARD)
    {
        return LLFloater::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
    }
    const LLInventoryObject* obj = static_cast<const LLInventoryObject*>(cargo_data);
    if (!obj || !gInventory.getItem(obj->getUUID()))
    {
        *accept = ACCEPT_NO;   // (a notecard in an object: copy it to inventory first)
        return true;
    }
    *accept = ACCEPT_YES_SINGLE;
    tooltip_msg = getString("drop_notecard");
    if (drop)
    {
        GMDXLoadouts::instance().importNotecard(obj->getUUID());
    }
    return true;
}

std::string GMDXFloaterLoadouts::selectedLayer() const
{
    LLScrollListItem* item = mLayerList->getFirstSelected();
    return item ? item->getValue().asString() : std::string();
}

LLUUID GMDXFloaterLoadouts::selectedLoadout() const
{
    LLScrollListItem* item = mLoadoutList->getFirstSelected();
    return item ? item->getValue().asUUID() : LLUUID::null;
}

void GMDXFloaterLoadouts::refreshEquipment()
{
    GMDXLoadouts& lo = GMDXLoadouts::instance();
    const std::vector<std::string> cats = lo.categories();
    const GMDXLoadouts::slots_t worn = lo.worn();
    const std::vector<std::string> changed = lo.changedCategories();

    std::string layer = selectedLayer();
    if (std::find(cats.begin(), cats.end(), layer) == cats.end())
    {
        layer = cats.empty() ? std::string() : cats.front();
    }
    const S32 layer_scroll = mLayerList->getScrollPos();
    mLayerList->deleteAllItems();
    for (const std::string& cat : cats)
    {
        auto it = worn.find(cat);
        const bool is_changed = std::find(changed.begin(), changed.end(), cat) != changed.end();
        mLayerList->addElement(text_row(cat, { { "layer", is_changed ? cat + " \xE2\x80\xA2" : cat },
                                               { "worn", it != worn.end() ? short_name(it->second.name) : getString("empty") } }));
    }
    mLayerList->selectByValue(layer);
    mLayerList->setScrollPos(layer_scroll);

    // The layer's weapons under their group headings.
    const LLSD selected_weapon = mWeaponList->getSelectedValue();
    const S32 weapon_scroll = mWeaponList->getScrollPos();
    mWeaponList->deleteAllItems();
    mWeapons = layer.empty() ? std::vector<GMDXLoadouts::Weapon>() : lo.weapons(layer);
    if (!layer.empty())
    {
        auto worn_it = worn.find(layer);
        const LLUUID worn_id = worn_it != worn.end() ? worn_it->second.id : LLUUID::null;
        LLStringUtil::format_map_t args;
        args["[CAT]"] = layer;
        mWeaponList->addElement(text_row("none", { { "weapon", getString("nothing_on", args) } }, worn_id.isNull()));
        // By name (tag and version aside), under the groups the user made (not the sorter's "Other").
        auto title_of = [](const GMDXLoadouts::Weapon& w)
        {
            std::string title, version;
            GMDXWeaponSorter::splitName(w.name, title, version);
            return title;
        };
        std::stable_sort(mWeapons.begin(), mWeapons.end(), [&title_of](const GMDXLoadouts::Weapon& a, const GMDXLoadouts::Weapon& b)
        {
            const std::string ga = GMDXLoadouts::shownGroup(a), gb = GMDXLoadouts::shownGroup(b);
            if (ga != gb) return LLStringUtil::compareDict(ga, gb) < 0;
            return LLStringUtil::compareDict(title_of(a), title_of(b)) < 0;
        });
        std::string group = "\n";
        for (const GMDXLoadouts::Weapon& weapon : mWeapons)
        {
            const std::string shown = GMDXLoadouts::shownGroup(weapon);
            if (shown != group)
            {
                group = shown;
                if (!group.empty())
                {
                    mWeaponList->addElement(text_row(LLSD(), { { "weapon", group } }, false, false));
                }
            }
            const bool on = weapon.id == worn_id;
            const std::string indent = shown.empty() ? "" : "    ";
            std::string title, version;
            GMDXWeaponSorter::splitName(weapon.name, title, version);
            LLSD row = text_row(weapon.id, { { "weapon", indent + title + (on ? " " + getString("worn_suffix") : "") },
                                             { "version", version } }, on);
            row["columns"][1]["color"] = LLUIColorTable::instance().getColor("MenuItemDisabledColor").get().getValue();
            mWeaponList->addElement(row);
        }
        if (mWeapons.empty())
        {
            mWeaponList->addElement(text_row(LLSD(), { { "weapon", getString("layer_empty", args) } }, false, false));
        }
        mWeaponList->selectByValue(selected_weapon);
        mWeaponList->setScrollPos(weapon_scroll);
    }

    LLViewerInventoryCategory* worn_loadout = gInventory.getCategory(lo.wornLoadout());
    LLStringUtil::format_map_t args;
    if (worn_loadout)
    {
        args["[NAME]"] = worn_loadout->getName();
        args["[COUNT]"] = llformat("%d", S32(changed.size()));
    }
    mLoadoutStatus->setText(!worn_loadout ? getString("no_loadout")
                            : changed.empty() ? getString("loadout_worn", args) : getString("loadout_changed", args));
    mRevertBtn->setVisible(worn_loadout && !changed.empty());
    mUpdateBtn->setVisible(worn_loadout && !changed.empty());
}

void GMDXFloaterLoadouts::refreshLoadouts()
{
    GMDXLoadouts& lo = GMDXLoadouts::instance();
    const std::vector<GMDXLoadouts::Loadout> list = lo.loadouts();
    const LLUUID worn_id = lo.wornLoadout();

    LLUUID selected = selectedLoadout();
    if (selected.isNull() || std::none_of(list.begin(), list.end(), [&](const auto& l) { return l.id == selected; }))
    {
        selected = worn_id.notNull() ? worn_id : (list.empty() ? LLUUID::null : list.front().id);
    }
    const S32 scroll = mLoadoutList->getScrollPos();
    mLoadoutList->deleteAllItems();
    const GMDXLoadouts::Loadout* sel = nullptr;
    for (const GMDXLoadouts::Loadout& loadout : list)
    {
        const bool on = loadout.id == worn_id;
        mLoadoutList->addElement(text_row(loadout.id, { { "name", on ? loadout.name + " " + getString("worn_suffix") : loadout.name } }, on));
        if (loadout.id == selected)
        {
            sel = &loadout;
        }
    }
    mLoadoutList->selectByValue(selected);
    mLoadoutList->setScrollPos(scroll);

    mSlotList->deleteAllItems();
    mLoadoutName->setText(sel ? sel->name : std::string());
    getChild<LLButton>("equip_btn")->setEnabled(sel != nullptr);
    for (const char* name : { "rename_btn", "copy_btn", "delete_btn" })
    {
        getChild<LLButton>(name)->setEnabled(sel != nullptr);
    }
    if (!sel)
    {
        mWornText->setText(LLStringUtil::null);
        return;
    }
    for (const std::string& cat : lo.categories())
    {
        auto it = sel->slots.find(cat);
        mSlotList->addElement(text_row(cat, { { "layer", cat }, { "item", it != sel->slots.end() ? short_name(it->second.name) : getString("empty") } }));
    }
    for (const std::string& name : sel->missing)
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = name;
        mSlotList->addElement(text_row(LLSD(), { { "layer", "" }, { "item", getString("missing", args) } }, false, false));
    }
    mWornText->setText(sel->id == worn_id && lo.changedCategories().empty() ? getString("worn_now") : LLStringUtil::null);
}

void GMDXFloaterLoadouts::refreshCategories()
{
    GMDXLoadouts& lo = GMDXLoadouts::instance();
    const LLSD selected = mCategoryList->getSelectedValue();
    mCategoryList->deleteAllItems();
    S32 slot = 1;
    for (const std::string& cat : lo.categories())
    {
        const std::string key = slot <= GMDXLoadouts::HOTKEY_SLOTS ? GMDXLoadouts::hotkeyLabel(slot - 1) : std::string();
        mCategoryList->addElement(text_row(cat, { { "slot", llformat("%d", slot) }, { "name", cat },
                                                  { "key", key.empty() ? getString("no_key") : key },
                                                  { "folder", "Equipment/" + cat },
                                                  { "items", llformat("%d", S32(lo.weapons(cat).size())) } }));
        ++slot;
    }
    mCategoryList->selectByValue(selected);
}

GMDXFloaterLoadouts::~GMDXFloaterLoadouts()
{
    if (LLContextMenu* menu = mWeaponMenu.get())
    {
        gMenuHolder->removeChild(menu);
        delete menu;
    }
}

void GMDXFloaterLoadouts::onSetKey()
{
    // The slot is the category's place in the list of categories (the table may be sorted otherwise).
    const std::vector<std::string> cats = GMDXLoadouts::instance().categories();
    const std::string cat = mCategoryList->getSelectedValue().asString();
    const auto at = std::find(cats.begin(), cats.end(), cat);
    const S32 slot = at == cats.end() ? -1 : S32(at - cats.begin());
    if (slot < 0 || slot >= GMDXLoadouts::HOTKEY_SLOTS)
    {
        LLNotificationsUtil::add("GMDXHotkeySlots");
        return;
    }
    if (!LLFloaterReg::isRegistered("keybind_dialog"))   // (Preferences registers it when first built)
    {
        LLFloaterReg::add("keybind_dialog", "floater_select_key.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<LLSetKeyBindDialog>);
    }
    LLSetKeyBindDialog* dialog = LLFloaterReg::getTypedInstance<LLSetKeyBindDialog>("keybind_dialog", LLSD());
    if (dialog)
    {
        mKeySlot = slot;
        GMDXLoadouts::applyHotkeys(true);   // our own keys free while picking (else "reserved by a menu")
        dialog->setParent(this, mCategoryList, ALLOW_KEYS | ALLOW_MASK_KEYS);
        dialog->getChildView("apply_all")->setVisible(false);   // (movement modes: not ours)
        LLFloaterReg::showTypedInstance<LLSetKeyBindDialog>("keybind_dialog", LLSD(), true);
    }
}

void GMDXFloaterLoadouts::onDefaultKeyBind(bool)
{
    if (mKeySlot >= 0)
    {
        GMDXLoadouts::setHotkey(mKeySlot, KEY(KEY('1') + mKeySlot), MASK_ALT);
        mKeySlot = -1;
        GMDXLoadouts::applyHotkeys();
        refreshCategories();
    }
}

bool GMDXFloaterLoadouts::onSetKeyBind(EMouseClickType click, KEY key, MASK mask, bool)
{
    const S32 slot = mKeySlot;
    mKeySlot = -1;
    GMDXLoadouts::applyHotkeys();   // (back on the items whatever happens; setHotkey applies again)
    if (slot < 0 || click != CLICK_NONE)
    {
        return false;   // (mouse buttons aren't offered)
    }
    if (key == ';' || key == '|')   // the setting's separators
    {
        LLSD args;
        args["KEY"] = LLKeyboard::stringFromAccelerator(mask, key);
        LLNotificationsUtil::add("GMDXHotkeyBad", args);
        return true;
    }
    // Blank (KEY_NONE) unbinds the slot.
    if (!GMDXLoadouts::setHotkey(slot, key, mask))
    {
        LLSD args;
        args["KEY"] = LLKeyboard::stringFromAccelerator(mask, key);
        LLNotificationsUtil::add("GMDXHotkeyInUse", args);
        return true;
    }
    refreshCategories();
    return false;
}

//-----------------------------------------------------------------------------
// Equipment tab
//-----------------------------------------------------------------------------

void GMDXFloaterLoadouts::onWear()
{
    const std::string layer = selectedLayer();
    LLScrollListItem* item = mWeaponList->getFirstSelected();
    if (layer.empty() || !item)
    {
        return;
    }
    if (item->getValue().asString() == "none")
    {
        GMDXLoadouts::instance().swap(layer, nullptr);
        return;
    }
    const LLUUID id = item->getValue().asUUID();
    for (const GMDXLoadouts::Weapon& weapon : mWeapons)
    {
        if (weapon.id == id)
        {
            GMDXLoadouts::instance().swap(layer, &weapon);
            return;
        }
    }
}

void GMDXFloaterLoadouts::onWeaponMenu(S32 x, S32 y)
{
    LLScrollListItem* hit = mWeaponList->hitItem(x, y);
    if (!hit || hit->getValue().asUUID().isNull())
    {
        return;
    }
    mWeaponList->selectByValue(hit->getValue());
    const LLUUID id = hit->getValue().asUUID();
    auto it = std::find_if(mWeapons.begin(), mWeapons.end(), [&id](const GMDXLoadouts::Weapon& w) { return w.id == id; });
    if (it == mWeapons.end())
    {
        return;
    }
    const GMDXLoadouts::Weapon weapon = *it;

    if (LLContextMenu* old = mWeaponMenu.get())
    {
        gMenuHolder->removeChild(old);
        delete old;
    }
    LLContextMenu::Params mp;
    mp.name("gmdx_weapon_parts_menu");
    mp.visible(false);
    LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(mp);

    // Parts: ticked ones go on with the weapon; unticked ones wait in its Not worn folder.
    const std::vector<GMDXLoadouts::Part> parts = GMDXLoadouts::instance().parts(weapon.id);
    if (parts.size() > 1)
    {
        LLMenuItemCallGL::Params hp;
        hp.name("parts_label");
        hp.label(getString("parts_heading"));
        hp.enabled(false);
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(hp));
        const size_t worn_count = std::count_if(parts.begin(), parts.end(), [](const GMDXLoadouts::Part& p) { return p.worn; });
        for (const GMDXLoadouts::Part& part : parts)
        {
            std::string title, version;
            GMDXWeaponSorter::splitName(part.name, title, version);
            LLMenuItemCheckGL::Params p;
            p.name("part");
            p.label(title);
            p.enabled(!part.worn || worn_count > 1);   // the last part worn stays
            const bool checked = part.worn;
            const LLUUID folder = weapon.id, part_id = part.id;
            p.on_check.function([checked](LLUICtrl*, const LLSD&) { return checked; });
            p.on_click.function([folder, part_id, checked](LLUICtrl*, const LLSD&)
            {
                GMDXLoadouts::instance().setPartWorn(folder, part_id, !checked);
            });
            menu->addChild(LLUICtrlFactory::create<LLMenuItemCheckGL>(p));
        }
        menu->addSeparator();
    }
    LLMenuItemCallGL::Params rp;
    rp.name("remove");
    rp.label(getString("remove_weapon"));
    rp.on_click.function([weapon](LLUICtrl*, const LLSD&)
    {
        LLSD args;
        args["NAME"] = weapon.name;
        LLNotificationsUtil::add("GMDXRemoveWeapon", args, LLSD(), [weapon](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0)
            {
                GMDXLoadouts::instance().removeWeapon(weapon);
            }
            return false;
        });
    });
    menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(rp));

    mWeaponMenu = menu->getHandle();
    gMenuHolder->addChild(menu);
    S32 sx, sy;
    mWeaponList->localPointToOtherView(x, y, &sx, &sy, gMenuHolder);
    menu->show(sx, sy);
    LLMenuGL::showPopup(gMenuHolder, menu, sx, sy);
}

void GMDXFloaterLoadouts::onSaveAs()
{
    ask_name(getString("save_as_title"), "", [](const std::string& name)
    {
        GMDXLoadouts::instance().createLoadout(name, GMDXLoadouts::instance().worn(), true);
    });
}

//-----------------------------------------------------------------------------
// Loadouts tab
//-----------------------------------------------------------------------------

void GMDXFloaterLoadouts::onSlotClicked()
{
    LLScrollListItem* item = mSlotList->getFirstSelected();
    const LLUUID loadout_id = selectedLoadout();
    if (!item || loadout_id.isNull())
    {
        return;
    }
    const std::string cat = item->getValue().asString();
    mSlotList->deselectAllItems(true);  // so the same row can be clicked again
    if (cat.empty())
    {
        return;
    }
    GMDXLoadouts::Loadout loadout;
    GMDXLoadouts::instance().findLoadout(loadout_id, loadout);
    auto it = loadout.slots.find(cat);
    S32 x = 0, y = 0;
    LLUI::getInstance()->getMousePositionScreen(&x, &y);
    GMDXLoadouts::instance().showChooser(cat, it != loadout.slots.end() ? it->second.id : LLUUID::null, x, y,
        [loadout_id, cat](const GMDXLoadouts::Weapon* weapon)
        {
            GMDXLoadouts::instance().setLoadoutSlot(loadout_id, cat, weapon);
        }, false);
}

void GMDXFloaterLoadouts::onNewLoadout()
{
    onSaveAs();
}

void GMDXFloaterLoadouts::onRenameLoadout()
{
    const LLUUID id = selectedLoadout();
    LLViewerInventoryCategory* cat = gInventory.getCategory(id);
    if (!cat)
    {
        return;
    }
    ask_name(getString("rename_title"), cat->getName(), [id](const std::string& name)
    {
        GMDXLoadouts::instance().renameLoadout(id, name);
    });
}

void GMDXFloaterLoadouts::onCopyLoadout()
{
    GMDXLoadouts::Loadout loadout;
    if (!GMDXLoadouts::instance().findLoadout(selectedLoadout(), loadout))
    {
        return;
    }
    ask_name(getString("copy_title"), loadout.name + " copy", [slots = loadout.slots](const std::string& name)
    {
        GMDXLoadouts::instance().createLoadout(name, slots, false);
    });
}

void GMDXFloaterLoadouts::onDeleteLoadout()
{
    const LLUUID id = selectedLoadout();
    LLViewerInventoryCategory* cat = gInventory.getCategory(id);
    if (!cat)
    {
        return;
    }
    LLSD args;
    args["NAME"] = cat->getName();
    LLNotificationsUtil::add("GMDXLoadoutDelete", args, LLSD(), [id](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
        {
            GMDXLoadouts::instance().deleteLoadout(id);
        }
        return false;
    });
}

void GMDXFloaterLoadouts::onEquip()
{
    const LLUUID id = selectedLoadout();
    if (id.isNull())
    {
        return;
    }
    GMDXLoadouts::instance().equip(id);
    if (gSavedSettings.getBOOL("GMDXLoadoutCloseOnEquip"))
    {
        closeFloater();
    }
}

//-----------------------------------------------------------------------------
// Categories tab
//-----------------------------------------------------------------------------

void GMDXFloaterLoadouts::onAddCategory()
{
    ask_name(getString("add_category_title"), "", [](const std::string& name)
    {
        GMDXLoadouts::instance().addCategory(name);
    });
}

void GMDXFloaterLoadouts::onRenameCategory()
{
    const std::string from = mCategoryList->getSelectedValue().asString();
    if (from.empty())
    {
        return;
    }
    ask_name(getString("rename_category_title"), from, [from](const std::string& name)
    {
        GMDXLoadouts::instance().renameCategory(from, name);
    });
}

void GMDXFloaterLoadouts::onMoveCategory(S32 delta)
{
    const std::string cat = mCategoryList->getSelectedValue().asString();
    std::vector<std::string> cats = GMDXLoadouts::instance().categories();
    auto it = std::find(cats.begin(), cats.end(), cat);
    if (it == cats.end())
    {
        return;
    }
    const S32 from = S32(it - cats.begin());
    const S32 to = from + delta;
    if (to < 0 || to >= S32(cats.size()))
    {
        return;
    }
    std::swap(cats[from], cats[to]);
    GMDXLoadouts::instance().setCategories(cats);
}

void GMDXFloaterLoadouts::onRemoveCategory()
{
    const std::string cat = mCategoryList->getSelectedValue().asString();
    std::vector<std::string> cats = GMDXLoadouts::instance().categories();
    auto it = std::find(cats.begin(), cats.end(), cat);
    if (it != cats.end())
    {
        cats.erase(it);
        GMDXLoadouts::instance().setCategories(cats);  // the folder and its items stay
    }
}

//-----------------------------------------------------------------------------
// GMDXFloaterLoadoutToast
//-----------------------------------------------------------------------------

GMDXFloaterLoadoutToast::GMDXFloaterLoadoutToast(const LLSD& key)
:   LLFloater(key)
{
}

bool GMDXFloaterLoadoutToast::postBuild()
{
    mText = getChild<LLTextBox>("toast_text");
    mProgress = getChild<LLProgressBar>("toast_progress");
    mCancelBtn = getChild<LLButton>("cancel_btn");
    mOkBtn = getChild<LLButton>("ok_btn");
    mPickBtn = getChild<LLButton>("pick_btn");
    mCancelBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { GMDXLoadouts::instance().cancelQueued(); });
    mOkBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    mPickBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        closeFloater();
        LLFloaterReg::showInstance("gmdx_loadouts", LLSD().with("tab", "loadouts_panel"));
    });
    return true;
}

void GMDXFloaterLoadoutToast::onOpen(const LLSD& key)
{
    // Top right, under the menu bar.
    if (LLView* parent = getParent())
    {
        const LLRect& area = parent->getLocalRect();
        setOrigin(area.getWidth() - getRect().getWidth() - 16, area.getHeight() - getRect().getHeight() - 16);
    }
}

void GMDXFloaterLoadoutToast::draw()
{
    const GMDXLoadouts::Progress& p = GMDXLoadouts::instance().progress();
    // One line when it only reports a finished swap; room for the bar or buttons otherwise.
    const S32 height = (p.busy || (p.finished && !p.missing.empty())) ? 74 : 44;
    if (getRect().getHeight() != height)
    {
        const S32 top = getRect().mTop;
        reshape(getRect().getWidth(), height);
        setOrigin(getRect().mLeft, top - height);
    }
    LLStringUtil::format_map_t args;
    args["[NAME]"] = p.label;
    args["[CAT]"] = p.cat;
    args["[NEXT]"] = p.queued;
    if (p.busy)
    {
        args["[DONE]"] = llformat("%d", p.done);
        args["[TOTAL]"] = llformat("%d", p.total);
        args["[STEP]"] = p.step;
        mText->setText(getString(p.queued.empty() ? "equipping" : "equipping_next", args)
                       + (p.step.empty() ? "" : "\n" + p.step));
        mProgress->setVisible(true);
        mProgress->setValue(p.total > 0 ? 100.f * p.done / p.total : 0.f);
        mCancelBtn->setVisible(!p.queued.empty());
        mOkBtn->setVisible(false);
        mPickBtn->setVisible(false);
    }
    else if (p.finished && !p.missing.empty())
    {
        std::string names;
        for (const std::string& name : p.missing)
        {
            names += (names.empty() ? "" : ", ") + name;
        }
        args["[MISSING]"] = names;
        mText->setText(getString("missing", args));
        mProgress->setVisible(false);
        mCancelBtn->setVisible(false);
        mOkBtn->setVisible(true);
        mPickBtn->setVisible(true);
    }
    else
    {
        std::string text = getString(p.cat.empty() ? "equipped" : p.takeOff ? "taken_off" : "swapped", args);
        LLViewerInventoryCategory* worn = gInventory.getCategory(GMDXLoadouts::instance().wornLoadout());
        if (!p.cat.empty() && worn && !GMDXLoadouts::instance().changedCategories().empty())
        {
            args["[LOADOUT]"] = worn->getName();
            text += " " + getString("loadout_changed", args);
        }
        mText->setText(text);
        mProgress->setVisible(false);
        mCancelBtn->setVisible(false);
        mOkBtn->setVisible(false);
        mPickBtn->setVisible(false);
        if (LLTimer::getTotalSeconds() - p.finishedAt > kToastSeconds)
        {
            closeFloater();
            return;
        }
    }
    LLFloater::draw();
}
