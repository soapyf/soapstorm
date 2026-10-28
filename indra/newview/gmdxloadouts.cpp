/**
 * @file gmdxloadouts.cpp
 * @brief GunMetal-DX loadouts: equipment categories, saved loadouts, swaps and the quick-swap menu.
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

#include "gmdxloadouts.h"

#include "gmdxweaponsorter.h"
#include "llagent.h"
#include "llassetstorage.h"
#include "llfilesystem.h"
#include "llnotecard.h"
#include "llviewerassetupload.h"
#include "llviewerregion.h"
#include "llagentcamera.h"
#include "llappearancemgr.h"
#include "llcallbacklist.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryfunctions.h"
#include "llinventoryobserver.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "lltoolcomp.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewermenu.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "rlvcommon.h"
#include "rlvhandler.h"

#include <algorithm>

namespace
{
    const char* const kFolderNames[GMDXLoadouts::FOLDER_COUNT]  = {"Equipment", "Loadouts", "Outdated"};
    const char* const kLegacyNames[GMDXLoadouts::FOLDER_COUNT]  = {"GunMetal-DX Equipment", "GunMetal-DX Loadouts",
                                                                   "GunMetal-DX Outdated"};
    const char* const kFolderSettings[GMDXLoadouts::FOLDER_COUNT] = {"GMDXFolderEquipment", "GMDXFolderLoadouts",
                                                                    "GMDXFolderOutdated"};
    const char* const kNotWorn = "Not worn";
    const F64 kSwapTimeout = 20.0;  // seconds before an attachment that never arrived counts as failed

    class LoadoutsObserver : public LLInventoryObserver
    {
    public:
        void changed(U32 mask) override
        {
            if (mask & (LABEL | ADD | REMOVE | STRUCTURE | INTERNAL))
            {
                GMDXLoadouts::instance().bump();
            }
        }
    };

    // A weapon menu item: the name, its version dim at the right edge (where shortcuts go).
    class WeaponMenuItem : public LLMenuItemCheckGL
    {
    public:
        struct Params : public LLInitParam::Block<Params, LLMenuItemCheckGL::Params>
        {
            Optional<std::string> version;
            Params() : version("version") {}
        };
        U32 getNominalWidth() const override
        {
            return LLMenuItemCheckGL::getNominalWidth() + (mVersion.empty() ? 0 : kGap + getFont()->getWidth(mVersion));
        }
        void draw() override
        {
            LLMenuItemCheckGL::draw();
            if (mVersion.empty()) return;
            const bool lit = getEnabled() && getHighlight();
            getFont()->render(utf8str_to_wstring(mVersion), 0, (F32)(getRect().getWidth() - kRightPad), 2.f,
                              lit ? mHighlightForeground.get() : mDisabledColor.get(),
                              LLFontGL::RIGHT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
        WeaponMenuItem(const Params& p) : LLMenuItemCheckGL(p), mVersion(p.version) {}   // (LLUICtrlFactory::create)
    private:
        static const S32 kGap = 24, kRightPad = 10;
        std::string mVersion;
    };

    bool is_object(const LLViewerInventoryItem* item)
    {
        return item && item->getType() == LLAssetType::AT_OBJECT && !item->getIsBrokenLink();
    }

    std::string clean_name(std::string name)
    {
        name.erase(std::remove(name.begin(), name.end(), '|'), name.end());
        LLStringUtil::trim(name);
        return name;
    }

    // Attachment item ids worn right now.
    std::vector<LLUUID> worn_attachment_ids()
    {
        std::vector<LLUUID> ids;
        if (!isAgentAvatarValid())
        {
            return ids;
        }
        for (const auto& kvp : gAgentAvatarp->mAttachmentPoints)
        {
            for (const LLPointer<LLViewerObject>& obj : kvp.second->mAttachedObjects)
            {
                if (obj.notNull() && !obj->isDead())
                {
                    ids.push_back(obj->getAttachmentItemID());
                }
            }
        }
        return ids;
    }
}

GMDXLoadouts::GMDXLoadouts()
{
    mObserver = new LoadoutsObserver();
    gInventory.addObserver(mObserver);
}

GMDXLoadouts::~GMDXLoadouts()
{
    // At shutdown the inventory deletes the observers it still holds, this one included.
    if (gInventory.containsObserver(mObserver))
    {
        gInventory.removeObserver(mObserver);
        delete mObserver;
    }
}

U64 GMDXLoadouts::generation() const
{
    // Inventory changes bump mGeneration; attachments coming and going change the fingerprint.
    U32 worn = 0;
    for (const LLUUID& id : worn_attachment_ids())
    {
        worn += id.getCRC32();
    }
    return (U64(mGeneration) << 32) | worn;
}

//-----------------------------------------------------------------------------
// Folders and categories
//-----------------------------------------------------------------------------

LLUUID GMDXLoadouts::findFolder(const LLUUID& parent, const std::string& name) const
{
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(parent, cats, items);
    if (cats)
    {
        for (const auto& cat : *cats)
        {
            if (cat->getName() == name)
            {
                return cat->getUUID();
            }
        }
    }
    return LLUUID::null;
}

const char* GMDXLoadouts::folderName(EFolder f)
{
    return kFolderNames[f];
}

LLUUID GMDXLoadouts::folder(EFolder f) const
{
    if (!gInventory.isInventoryUsable())
    {
        return LLUUID::null;
    }
    const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    LLUUID id(gSavedPerAccountSettings.getString(kFolderSettings[f]));
    if (id.notNull() && gInventory.getCategory(id) && !gInventory.isObjectDescendentOf(id, trash))
    {
        return id;
    }
    // Not saved yet (or gone): the folder under its first name (renamed now) or its own name.
    const LLUUID root = gInventory.getRootFolderID();
    id = findFolder(root, kLegacyNames[f]);
    if (id.notNull())
    {
        rename_category(&gInventory, id, kFolderNames[f]);
    }
    else
    {
        // A folder of that plain name is only taken over when it looks like this one's (the
        // saved id is a local setting: a new computer asks again). A user's own "Equipment" stays
        // theirs; a new one is made beside it.
        id = findFolder(root, kFolderNames[f]);
        if (id.notNull() && !looksLike(f, id))
        {
            id.setNull();
        }
    }
    const std::string saved = id.notNull() ? id.asString() : std::string();
    if (gSavedPerAccountSettings.getString(kFolderSettings[f]) != saved)
    {
        gSavedPerAccountSettings.setString(kFolderSettings[f], saved);
    }
    return id;
}

bool GMDXLoadouts::looksLike(EFolder f, const LLUUID& id) const
{
    LLInventoryModel::cat_array_t* subs = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(id, subs, items);
    if (f == EQUIPMENT)
    {
        // Holds at least one of the category folders.
        const std::vector<std::string> cats = categories();
        if (subs)
        {
            for (const auto& sub : *subs)
            {
                if (std::find(cats.begin(), cats.end(), sub->getName()) != cats.end()) return true;
            }
        }
        return false;
    }
    if (f == LOADOUTS)
    {
        // Folders of links only.
        if (!subs || subs->empty() || (items && !items->empty())) return false;
        for (const auto& sub : *subs)
        {
            LLInventoryModel::cat_array_t* c2 = nullptr;
            LLInventoryModel::item_array_t* i2 = nullptr;
            gInventory.getDirectDescendentsOf(sub->getUUID(), c2, i2);
            if (c2 && !c2->empty()) return false;
            if (i2)
            {
                for (const auto& item : *i2)
                {
                    if (!item->getIsLinkType()) return false;
                }
            }
        }
        return true;
    }
    // OUTDATED: empty, or only things carrying a watched tag.
    std::string tags = gSavedPerAccountSettings.getString("GMDXLoadoutWatchTags");
    LLStringUtil::toLower(tags);
    LLInventoryModel::cat_array_t all_cats;
    LLInventoryModel::item_array_t all_items;
    gInventory.collectDescendents(id, all_cats, all_items, false);
    for (const auto& item : all_items)
    {
        std::string name = item->getName();
        LLStringUtil::toLower(name);
        bool tagged = false;
        size_t start = 0;
        while (start <= tags.size())
        {
            size_t end = tags.find(',', start);
            if (end == std::string::npos) end = tags.size();
            std::string tag = tags.substr(start, end - start);
            LLStringUtil::trim(tag);
            if (!tag.empty() && name.find(tag) != std::string::npos) tagged = true;
            start = end + 1;
        }
        if (!tagged) return false;
    }
    return true;
}

void GMDXLoadouts::withFolder(EFolder f, std::function<void(const LLUUID&)> cb)
{
    const LLUUID id = folder(f);
    if (id.notNull())
    {
        cb(id);
        return;
    }
    mFolderWaiters[f].push_back(cb);
    if (mFolderWaiters[f].size() > 1)
    {
        return;   // already being created: wait for that one
    }
    gInventory.createNewCategory(gInventory.getRootFolderID(), LLFolderType::FT_NONE, kFolderNames[f], [f](const LLUUID& created)
    {
        GMDXLoadouts& self = GMDXLoadouts::instance();
        if (created.notNull())
        {
            gSavedPerAccountSettings.setString(kFolderSettings[f], created.asString());
        }
        auto waiters = std::move(self.mFolderWaiters[f]);
        self.mFolderWaiters[f].clear();
        for (auto& waiter : waiters)
        {
            waiter(created);
        }
        self.bump();
    });
}

int GMDXLoadouts::folderKind(const LLUUID& id)
{
    // Looked up again only when the inventory changes: inventory views ask per folder per draw.
    static LLUUID ids[FOLDER_COUNT];
    static U32 looked_up_at = 0;
    if (id.isNull() || !gInventory.isInventoryUsable() || !gSavedPerAccountSettings.controlExists(kFolderSettings[0]))
    {
        return 0;
    }
    if (looked_up_at != GMDXLoadouts::instance().mGeneration)
    {
        looked_up_at = GMDXLoadouts::instance().mGeneration;
        for (int f = 0; f < FOLDER_COUNT; ++f)
        {
            ids[f] = GMDXLoadouts::instance().folder(EFolder(f));
        }
    }
    for (int f = 0; f < FOLDER_COUNT; ++f)
    {
        if (ids[f] == id)
        {
            return f + 1;
        }
    }
    return 0;
}

void GMDXLoadouts::ensureFolders()
{
    if (!gInventory.isInventoryUsable() || mPendingCreates > 0)
    {
        return;
    }
    auto created = [](const LLUUID& id)
    {
        GMDXLoadouts& self = GMDXLoadouts::instance();
        if (id.isNull())
        {
            self.mCreateFailed = true;  // no retry loop: the next window open tries again
        }
        if (--self.mPendingCreates == 0 && !self.mCreateFailed)
        {
            doOnIdleOneTime([] { GMDXLoadouts::instance().ensureFolders(); });  // the category folders, once Equipment exists
        }
        self.bump();
    };
    mCreateFailed = false;
    if (loadoutsFolder().isNull())
    {
        ++mPendingCreates;
        withFolder(LOADOUTS, created);
    }
    const LLUUID equip = equipmentFolder();
    if (equip.isNull())
    {
        ++mPendingCreates;
        withFolder(EQUIPMENT, created);
        return;
    }
    for (const std::string& cat : categories())
    {
        if (findFolder(equip, cat).isNull())
        {
            ++mPendingCreates;
            gInventory.createNewCategory(equip, LLFolderType::FT_NONE, cat, created);
        }
    }
    if (!mFetched)
    {
        // Normally the login fetch already has these; links in the loadouts need their targets.
        mFetched = true;
        LLInventoryModelBackgroundFetch::instance().start(equip);
        if (LLUUID loadouts = loadoutsFolder(); loadouts.notNull())
        {
            LLInventoryModelBackgroundFetch::instance().start(loadouts);
        }
    }
}

std::vector<std::string> GMDXLoadouts::categories() const
{
    std::vector<std::string> cats;
    std::string list = gSavedPerAccountSettings.getString("GMDXLoadoutCategories");
    size_t start = 0;
    while (start <= list.size())
    {
        size_t end = list.find('|', start);
        if (end == std::string::npos)
        {
            end = list.size();
        }
        std::string name = clean_name(list.substr(start, end - start));
        if (!name.empty() && std::find(cats.begin(), cats.end(), name) == cats.end())
        {
            cats.push_back(name);
        }
        start = end + 1;
    }
    return cats;
}

void GMDXLoadouts::setCategories(const std::vector<std::string>& cats)
{
    std::string list;
    for (const std::string& cat : cats)
    {
        list += (list.empty() ? "" : "|") + cat;
    }
    gSavedPerAccountSettings.setString("GMDXLoadoutCategories", list);
    bump();
}

void GMDXLoadouts::addCategory(const std::string& name)
{
    std::string cat = clean_name(name);
    std::vector<std::string> cats = categories();
    if (cat.empty() || std::find(cats.begin(), cats.end(), cat) != cats.end())
    {
        return;
    }
    cats.push_back(cat);
    setCategories(cats);
    ensureFolders();
}

void GMDXLoadouts::renameCategory(const std::string& from, const std::string& to)
{
    std::string name = clean_name(to);
    std::vector<std::string> cats = categories();
    auto it = std::find(cats.begin(), cats.end(), from);
    if (name.empty() || it == cats.end() || std::find(cats.begin(), cats.end(), name) != cats.end())
    {
        return;
    }
    if (findFolder(equipmentFolder(), name).notNull())
    {
        LLSD args;
        args["NAME"] = name;
        LLNotificationsUtil::add("GMDXLoadoutFolderExists", args);  // two folders of one name would hide one
        return;
    }
    *it = name;
    if (LLUUID folder = findFolder(equipmentFolder(), from); folder.notNull())
    {
        rename_category(&gInventory, folder, name);
    }
    setCategories(cats);
}

//-----------------------------------------------------------------------------
// Reading equipment and loadouts
//-----------------------------------------------------------------------------

void GMDXLoadouts::buildIndex(Index& idx)
{
    const LLUUID equip = equipmentFolder();
    if (equip.isNull())
    {
        return;
    }
    for (const std::string& cat : categories())
    {
        std::vector<Weapon>& list = idx.weapons[cat];
        const LLUUID cat_folder = findFolder(equip, cat);
        if (cat_folder.isNull())
        {
            continue;
        }
        LLInventoryModel::cat_array_t* groups = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(cat_folder, groups, items);
        auto add_object = [&list](const LLViewerInventoryItem* item, const std::string& group)
        {
            if (is_object(item))
            {
                list.push_back({ item->getName(), group, item->getUUID(), { item->getLinkedUUID() } });
            }
        };
        if (items)
        {
            for (const auto& item : *items)
            {
                add_object(item, "");
            }
        }
        if (!groups)
        {
            continue;
        }
        for (const auto& group : *groups)
        {
            LLInventoryModel::cat_array_t* weapon_folders = nullptr;
            LLInventoryModel::item_array_t* group_items = nullptr;
            gInventory.getDirectDescendentsOf(group->getUUID(), weapon_folders, group_items);
            if (group_items)
            {
                for (const auto& item : *group_items)
                {
                    add_object(item, group->getName());
                }
            }
            if (!weapon_folders)
            {
                continue;
            }
            for (const auto& folder : *weapon_folders)
            {
                // Groups are one level deep; anything below a weapon folder is its parts
                LLInventoryModel::cat_array_t sub_cats;
                LLInventoryModel::item_array_t parts;
                gInventory.collectDescendents(folder->getUUID(), sub_cats, parts, false);
                Weapon weapon{ folder->getName(), group->getName(), folder->getUUID(), {} };
                const LLUUID not_worn = findFolder(folder->getUUID(), kNotWorn);
                for (const auto& part : parts)
                {
                    if (!is_object(part))
                    {
                        continue;
                    }
                    if (not_worn.notNull() && gInventory.isObjectDescendentOf(part->getUUID(), not_worn))
                    {
                        idx.spare.insert(part->getLinkedUUID());
                    }
                    else
                    {
                        weapon.parts.push_back(part->getLinkedUUID());
                    }
                }
                if (!weapon.parts.empty())
                {
                    list.push_back(weapon);
                }
            }
        }
        std::sort(list.begin(), list.end(), [](const Weapon& a, const Weapon& b)
        {
            if (a.group != b.group)
            {
                return LLStringUtil::compareDict(a.group, b.group) < 0;
            }
            return LLStringUtil::compareDict(a.name, b.name) < 0;
        });
        for (const Weapon& weapon : list)
        {
            for (const LLUUID& part : weapon.parts)
            {
                idx.owner.emplace(part, std::make_pair(cat, weapon.id));  // multimap: parts can be shared
            }
        }
    }
}

std::vector<GMDXLoadouts::Weapon> GMDXLoadouts::weapons(const std::string& cat)
{
    Index idx;
    buildIndex(idx);
    return idx.weapons[cat];
}

GMDXLoadouts::slots_t GMDXLoadouts::resolve(const Index& idx, const std::set<LLUUID>& ids) const
{
    // Per category, the weapon with the most of its parts among ids, a complete one first. A part two
    // weapons share (one holster for two guns) then never decides which gun it is.
    slots_t slots;
    for (const auto& kvp : idx.weapons)
    {
        const Weapon* best = nullptr;
        size_t best_count = 0;
        bool best_full = false;
        for (const Weapon& weapon : kvp.second)
        {
            size_t count = 0;
            for (const LLUUID& part : weapon.parts)
            {
                count += ids.count(part);
            }
            const bool full = count == weapon.parts.size();
            if (count > 0 && (!best || (full && !best_full) || (full == best_full && count > best_count)))
            {
                best = &weapon;
                best_count = count;
                best_full = full;
            }
        }
        if (best)
        {
            slots[kvp.first] = *best;
        }
    }
    return slots;
}

GMDXLoadouts::slots_t GMDXLoadouts::wornFrom(const Index& idx)
{
    const std::vector<LLUUID> worn_ids = worn_attachment_ids();
    return resolve(idx, std::set<LLUUID>(worn_ids.begin(), worn_ids.end()));
}

GMDXLoadouts::slots_t GMDXLoadouts::worn()
{
    Index idx;
    buildIndex(idx);
    return wornFrom(idx);
}

void GMDXLoadouts::readLoadout(const Index& idx, const LLUUID& folder, Loadout& out)
{
    out.slots.clear();
    out.missing.clear();
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(folder, cats, items);
    if (!items)
    {
        return;
    }
    std::set<LLUUID> ids;
    for (const auto& link : *items)
    {
        if (idx.spare.count(link->getLinkedUUID()) && !idx.owner.count(link->getLinkedUUID()))
        {
            continue;   // a part since set to Not worn: still the weapon's, just not attached
        }
        if (link->getIsBrokenLink() || !idx.owner.count(link->getLinkedUUID()))
        {
            if (std::find(out.missing.begin(), out.missing.end(), link->getName()) == out.missing.end())
            {
                out.missing.push_back(link->getName());
            }
            continue;
        }
        ids.insert(link->getLinkedUUID());
    }
    out.slots = resolve(idx, ids);
}

std::vector<GMDXLoadouts::Loadout> GMDXLoadouts::loadouts()
{
    std::vector<Loadout> list;
    const LLUUID folder = loadoutsFolder();
    if (folder.isNull())
    {
        return list;
    }
    Index idx;
    buildIndex(idx);
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(folder, cats, items);
    if (cats)
    {
        for (const auto& cat : *cats)
        {
            Loadout loadout;
            loadout.id = cat->getUUID();
            loadout.name = cat->getName();
            readLoadout(idx, loadout.id, loadout);
            list.push_back(loadout);
        }
    }
    std::sort(list.begin(), list.end(), [](const Loadout& a, const Loadout& b)
    {
        return LLStringUtil::compareDict(a.name, b.name) < 0;
    });
    return list;
}

bool GMDXLoadouts::findLoadout(const LLUUID& id, Loadout& out)
{
    LLViewerInventoryCategory* cat = id.notNull() ? gInventory.getCategory(id) : nullptr;
    if (!cat || cat->getParentUUID() != loadoutsFolder())
    {
        return false;
    }
    Index idx;
    buildIndex(idx);
    out.id = id;
    out.name = cat->getName();
    readLoadout(idx, id, out);
    return true;
}

LLUUID GMDXLoadouts::wornLoadout() const
{
    LLUUID id(gSavedPerAccountSettings.getString("GMDXLoadoutWorn"));
    LLViewerInventoryCategory* cat = id.notNull() ? gInventory.getCategory(id) : nullptr;
    return (cat && cat->getParentUUID() == loadoutsFolder()) ? id : LLUUID::null;
}

std::vector<std::string> GMDXLoadouts::changedCategories()
{
    std::vector<std::string> changed;
    Loadout loadout;
    if (!findLoadout(wornLoadout(), loadout))
    {
        return changed;
    }
    const slots_t now = worn();
    for (const std::string& cat : categories())
    {
        auto a = loadout.slots.find(cat);
        auto b = now.find(cat);
        const LLUUID want = a != loadout.slots.end() ? a->second.id : LLUUID::null;
        const LLUUID have = b != now.end() ? b->second.id : LLUUID::null;
        if (want != have)
        {
            changed.push_back(cat);
        }
    }
    return changed;
}

std::vector<GMDXLoadouts::Entry> GMDXLoadouts::allWeapons()
{
    std::vector<Entry> list;
    Index idx;
    buildIndex(idx);
    for (const auto& kvp : idx.weapons)
    {
        for (const Weapon& weapon : kvp.second)
        {
            list.push_back({ kvp.first, weapon });
        }
    }
    return list;
}

bool GMDXLoadouts::isInOutdated(const LLUUID& id)
{
    // Looked up again only when the inventory changes (it runs per searched item).
    static LLUUID outdated;
    static U32 looked_up_at = 0;
    if (!gInventory.isInventoryUsable() || !gSavedPerAccountSettings.controlExists(kFolderSettings[OUTDATED]))
    {
        return false;
    }
    if (looked_up_at != GMDXLoadouts::instance().mGeneration)
    {
        looked_up_at = GMDXLoadouts::instance().mGeneration;
        outdated = GMDXLoadouts::instance().folder(OUTDATED);
    }
    return outdated.notNull() && (id == outdated || gInventory.isObjectDescendentOf(id, outdated));
}

LLUUID GMDXLoadouts::categoryFolder(const std::string& cat) const
{
    const LLUUID equip = equipmentFolder();
    return equip.notNull() ? findFolder(equip, cat) : LLUUID::null;
}

std::vector<LLUUID> GMDXLoadouts::removeFromLoadouts(const uuid_vec_t& parts)
{
    std::vector<LLUUID> held;
    LLInventoryModel::cat_array_t* loadouts = nullptr;
    LLInventoryModel::item_array_t* unused = nullptr;
    gInventory.getDirectDescendentsOf(loadoutsFolder(), loadouts, unused);
    if (!loadouts)
    {
        return held;
    }
    for (const auto& loadout : *loadouts)
    {
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* links = nullptr;
        gInventory.getDirectDescendentsOf(loadout->getUUID(), cats, links);
        uuid_vec_t remove;
        if (links)
        {
            for (const auto& link : *links)
            {
                if (link->getIsLinkType()
                    && std::find(parts.begin(), parts.end(), link->getLinkedUUID()) != parts.end())
                {
                    remove.push_back(link->getUUID());
                }
            }
        }
        for (const LLUUID& id : remove)
        {
            remove_inventory_object(id, nullptr);
        }
        if (!remove.empty())
        {
            held.push_back(loadout->getUUID());
        }
    }
    return held;
}

void GMDXLoadouts::replaceInLoadouts(const uuid_vec_t& old_parts, const std::string& cat, const Weapon& now)
{
    for (const LLUUID& loadout : removeFromLoadouts(old_parts))
    {
        linkSlots(loadout, { { cat, now } });
    }
}

//-----------------------------------------------------------------------------
// Weapon parts, removing weapons
//-----------------------------------------------------------------------------

const char* GMDXLoadouts::notWornFolderName()
{
    return kNotWorn;
}

void GMDXLoadouts::moveTo(const LLUUID& id, const LLUUID& dest)
{
    if (LLViewerInventoryItem* item = gInventory.getItem(id))
    {
        gInventory.changeItemParent(item, dest, false);
    }
    else if (LLViewerInventoryCategory* cat = gInventory.getCategory(id))
    {
        gInventory.changeCategoryParent(cat, dest, false);
    }
}

void GMDXLoadouts::withSubfolder(const LLUUID& parent, const std::string& name, std::function<void(const LLUUID&)> cb)
{
    LLUUID id = GMDXLoadouts::instance().findFolder(parent, name);
    if (id.notNull())
    {
        cb(id);
        return;
    }
    typedef std::pair<LLUUID, std::string> key_t;
    static std::map<key_t, std::vector<std::function<void(const LLUUID&)>>> in_flight;
    const key_t key(parent, name);
    auto& waiting = in_flight[key];
    waiting.push_back(cb);
    if (waiting.size() > 1)
    {
        return;
    }
    gInventory.createNewCategory(parent, LLFolderType::FT_NONE, name, [key](const LLUUID& created)
    {
        auto callbacks = std::move(in_flight[key]);
        in_flight.erase(key);
        for (auto& callback : callbacks)
        {
            callback(created);
        }
    });
}

std::vector<GMDXLoadouts::Part> GMDXLoadouts::parts(const LLUUID& weapon_folder) const
{
    std::vector<Part> list;
    if (!gInventory.getCategory(weapon_folder))
    {
        return list;
    }
    const LLUUID not_worn = findFolder(weapon_folder, kNotWorn);
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    gInventory.collectDescendents(weapon_folder, cats, items, false);
    for (const auto& item : items)
    {
        if (is_object(item))
        {
            list.push_back({ item->getUUID(), item->getName(),
                             not_worn.isNull() || !gInventory.isObjectDescendentOf(item->getUUID(), not_worn) });
        }
    }
    std::sort(list.begin(), list.end(), [](const Part& a, const Part& b)
    {
        return LLStringUtil::compareDict(a.name, b.name) < 0;
    });
    return list;
}

namespace
{
    // Parts being unticked whose Not worn folder is still being made: they count as unworn already.
    std::set<LLUUID> sPendingNotWorn;
}

bool GMDXLoadouts::sharedWithOther(const LLUUID& object, const LLUUID& weapon_id)
{
    Index idx;
    buildIndex(idx);
    auto range = idx.owner.equal_range(object);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second.second != weapon_id) return true;   // (one holster for two guns)
    }
    return false;
}

void GMDXLoadouts::setPartWorn(const LLUUID& weapon_folder, const LLUUID& part, bool worn)
{
    std::vector<Part> list = parts(weapon_folder);
    for (Part& p : list)
    {
        if (sPendingNotWorn.count(p.id)) p.worn = false;
    }
    auto it = std::find_if(list.begin(), list.end(), [&part](const Part& p) { return p.id == part; });
    if (it == list.end() || it->worn == worn)
    {
        return;
    }
    const LLViewerInventoryItem* item = gInventory.getItem(part);
    const LLUUID object = item ? item->getLinkedUUID() : part;
    if (!worn)
    {
        if (std::count_if(list.begin(), list.end(), [](const Part& p) { return p.worn; }) <= 1)
        {
            return;   // a weapon keeps at least one part to wear
        }
        sPendingNotWorn.insert(part);
        withSubfolder(weapon_folder, kNotWorn, [part](const LLUUID& dest)
        {
            if (sPendingNotWorn.erase(part) && dest.notNull()) moveTo(part, dest);   // (not re-ticked meanwhile)
        });
        if (get_is_item_worn(object) && !sharedWithOther(object, weapon_folder))
        {
            LLAppearanceMgr::instance().removeItemFromAvatar(object);
        }
        return;
    }
    sPendingNotWorn.erase(part);
    moveTo(part, weapon_folder);
    // Back on: put it on too when its weapon is the one worn.
    for (const auto& kvp : this->worn())
    {
        if (kvp.second.id == weapon_folder && !get_is_item_worn(object))
        {
            LLAppearanceMgr::instance().wearItemsOnAvatar({ object }, false, false);
        }
    }
}

void GMDXLoadouts::carryNotWorn(const LLUUID& old_folder, const LLUUID& new_folder)
{
    std::set<std::string> unworn;
    for (const Part& p : parts(old_folder))
    {
        if (!p.worn) unworn.insert(p.name);
    }
    if (unworn.empty())
    {
        return;
    }
    for (const Part& p : parts(new_folder))
    {
        if (p.worn && unworn.count(p.name))
        {
            setPartWorn(new_folder, p.id, false);
        }
    }
}

void GMDXLoadouts::removeWeapon(const Weapon& weapon)
{
    uuid_vec_t all = weapon.parts;
    for (const Part& p : parts(weapon.id))
    {
        const LLViewerInventoryItem* item = gInventory.getItem(p.id);
        all.push_back(item ? item->getLinkedUUID() : p.id);
    }
    // A part another weapon also uses stays on, and in that weapon's loadouts.
    all.erase(std::remove_if(all.begin(), all.end(), [&weapon](const LLUUID& id)
    {
        return GMDXLoadouts::instance().sharedWithOther(id, weapon.id);
    }), all.end());
    uuid_vec_t on;
    for (const LLUUID& id : all)
    {
        if (get_is_item_worn(id)) on.push_back(id);
    }
    if (!on.empty())
    {
        LLAppearanceMgr::instance().removeItemsFromAvatar(on);
    }
    removeFromLoadouts(all);
    const LLUUID id = weapon.id;
    withFolder(OUTDATED, [id](const LLUUID& outdated)
    {
        if (outdated.notNull()) moveTo(id, outdated);
    });
}

//-----------------------------------------------------------------------------
// Swapping
//-----------------------------------------------------------------------------

void GMDXLoadouts::equip(const LLUUID& loadout_id)
{
    LLViewerInventoryCategory* cat = gInventory.getCategory(loadout_id);
    if (!cat)
    {
        return;
    }
    queue({ cat->getName(), [loadout_id]
    {
        GMDXLoadouts& self = GMDXLoadouts::instance();
        Loadout loadout;
        if (!self.findLoadout(loadout_id, loadout))
        {
            return false;
        }
        std::map<std::string, Weapon> wanted;
        for (const std::string& cat : self.categories())
        {
            auto it = loadout.slots.find(cat);
            wanted[cat] = it != loadout.slots.end() ? it->second : Weapon{};  // none: take the category off
        }
        self.beginSwap(loadout.name, wanted, loadout.missing, loadout_id);
        return true;
    } });
}

void GMDXLoadouts::swap(const std::string& cat, const Weapon* weapon)
{
    Weapon target = weapon ? *weapon : Weapon{};
    const std::string label = weapon ? weapon->name : cat;
    queue({ label, [cat, target, label]
    {
        GMDXLoadouts::instance().beginSwap(label, { { cat, target } }, {}, LLUUID::null);
        return true;
    } });
}

void GMDXLoadouts::queue(Job job)
{
    if (mProgress.busy)
    {
        mQueued = std::move(job);  // the newest request replaces an older waiting one
        mProgress.queued = mQueued.label;
        return;
    }
    if (!job.run())
    {
        mProgress.busy = false;
    }
}

void GMDXLoadouts::cancelQueued()
{
    mQueued = Job{};
    mProgress.queued.clear();
}

void GMDXLoadouts::beginSwap(const std::string& label, const std::map<std::string, Weapon>& wanted,
                             const std::vector<std::string>& missing, const LLUUID& loadout_id)
{
    if (!isAgentAvatarValid())
    {
        return;
    }
    Index idx;
    buildIndex(idx);

    std::set<LLUUID> keep;
    for (const auto& kvp : wanted)
    {
        keep.insert(kvp.second.parts.begin(), kvp.second.parts.end());
    }
    mAttach.clear();
    mDetach.clear();
    mSteps.clear();
    for (const LLUUID& id : worn_attachment_ids())
    {
        auto range = idx.owner.equal_range(id);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (wanted.count(it->second.first) && !keep.count(id))
            {
                mDetach.insert(id);
            }
        }
    }
    for (const auto& kvp : wanted)
    {
        for (const LLUUID& part : kvp.second.parts)
        {
            if (!gAgentAvatarp->isWearingAttachment(part)
                && std::find(mAttach.begin(), mAttach.end(), part) == mAttach.end())
            {
                mAttach.push_back(part);
                mSteps.emplace_back(kvp.first, kvp.second.name);
            }
        }
    }
    // RLVa: what it keeps on stays on and what it keeps off stays off (reported as missing),
    // instead of the swap waiting for them until it times out.
    std::vector<std::string> all_missing = missing;
    if (RlvHandler::isEnabled())
    {
        for (auto it = mDetach.begin(); it != mDetach.end(); )
        {
            it = rlvPredCanRemoveItem(*it) ? std::next(it) : mDetach.erase(it);
        }
        for (size_t i = 0; i < mAttach.size(); )
        {
            const LLViewerInventoryItem* item = gInventory.getItem(mAttach[i]);
            if (item && rlvPredCanWearItem(item, RLV_WEAR_ADD))
            {
                ++i;
                continue;
            }
            if (std::find(all_missing.begin(), all_missing.end(), mSteps[i].second) == all_missing.end())
            {
                all_missing.push_back(mSteps[i].second);
            }
            mAttach.erase(mAttach.begin() + i);
            mSteps.erase(mSteps.begin() + i);
        }
    }
    if (loadout_id.notNull())
    {
        gSavedPerAccountSettings.setString("GMDXLoadoutWorn", loadout_id.asString());
    }

    if (!mDetach.empty())
    {
        LLAppearanceMgr::instance().removeItemsFromAvatar(uuid_vec_t(mDetach.begin(), mDetach.end()));
    }
    if (!mAttach.empty())
    {
        LLAppearanceMgr::instance().wearItemsOnAvatar(mAttach, false, false);
    }

    mProgress = Progress{};
    mProgress.busy = true;
    mProgress.label = label;
    if (loadout_id.isNull() && wanted.size() == 1)
    {
        mProgress.cat = wanted.begin()->first;
        mProgress.takeOff = wanted.begin()->second.parts.empty();
    }
    mProgress.total = S32(mAttach.size() + mDetach.size());
    mMissing = all_missing;
    mStarted = LLTimer::getTotalSeconds();
    LLFloaterReg::showInstance("gmdx_loadout_toast", LLSD(), false);
    if (!mTicking)
    {
        mTicking = true;
        doOnIdleRepeating([] { return GMDXLoadouts::instance().tick(); });
    }
}

bool GMDXLoadouts::tick()
{
    if (!isAgentAvatarValid())
    {
        mTicking = false;
        mProgress.busy = false;
        mProgress.finished = true;
        mProgress.queued.clear();
        mQueued = Job{};
        return true;
    }
    S32 done = 0;
    std::string step;
    for (const LLUUID& id : mAttach)
    {
        if (gAgentAvatarp->isWearingAttachment(id))
        {
            ++done;
        }
    }
    for (const LLUUID& id : mDetach)
    {
        if (!gAgentAvatarp->isWearingAttachment(id))
        {
            ++done;
        }
    }
    size_t i = 0;
    for (const LLUUID& id : mAttach)
    {
        if (!gAgentAvatarp->isWearingAttachment(id) && i < mSteps.size())
        {
            step = mSteps[i].first + ": " + mSteps[i].second;
            break;
        }
        ++i;
    }
    mProgress.done = done;
    mProgress.step = step;

    const bool timed_out = LLTimer::getTotalSeconds() - mStarted > kSwapTimeout;
    if (done < mProgress.total && !timed_out)
    {
        return false;
    }

    i = 0;
    for (const LLUUID& id : mAttach)
    {
        if (!gAgentAvatarp->isWearingAttachment(id) && i < mSteps.size()
            && std::find(mMissing.begin(), mMissing.end(), mSteps[i].second) == mMissing.end())
        {
            mMissing.push_back(mSteps[i].second);
        }
        ++i;
    }
    mProgress.busy = false;
    mProgress.finished = true;
    mProgress.missing = mMissing;
    mProgress.finishedAt = LLTimer::getTotalSeconds();
    bump();

    if (mQueued.run)
    {
        Job next = std::move(mQueued);
        mQueued = Job{};
        mProgress.queued.clear();
        if (next.run())
        {
            return false;  // the next swap reuses this idle callback
        }
    }
    mTicking = false;
    return true;
}

//-----------------------------------------------------------------------------
// Loadout editing
//-----------------------------------------------------------------------------

void GMDXLoadouts::linkSlots(const LLUUID& folder, const slots_t& slots)
{
    LLInventoryObject::const_object_list_t objects;
    for (const auto& kvp : slots)
    {
        for (const LLUUID& part : kvp.second.parts)
        {
            if (LLViewerInventoryItem* item = gInventory.getItem(part))
            {
                objects.push_back(item);
            }
        }
    }
    if (!objects.empty())
    {
        link_inventory_array(folder, objects, nullptr);
    }
}

void GMDXLoadouts::createLoadout(const std::string& name, const slots_t& slots, bool mark_worn)
{
    ensureFolders();
    const LLUUID parent = loadoutsFolder();
    std::string clean = clean_name(name);
    if (parent.isNull() || clean.empty())
    {
        return;
    }
    gInventory.createNewCategory(parent, LLFolderType::FT_NONE, clean, [slots, mark_worn](const LLUUID& id)
    {
        if (id.notNull())
        {
            GMDXLoadouts::instance().linkSlots(id, slots);
            if (mark_worn)
            {
                gSavedPerAccountSettings.setString("GMDXLoadoutWorn", id.asString());
            }
        }
    });
}

namespace
{
    const char* const kNotecardHeader = "GunMetal-DX loadout: ";
}

void GMDXLoadouts::shareLoadout(const LLUUID& loadout_id)
{
    Loadout loadout;
    if (!findLoadout(loadout_id, loadout))
    {
        return;
    }
    // One "<layer>: <weapon>" line per layer, in slot order; the reader matches by name.
    std::string text = kNotecardHeader + loadout.name + "\n";
    for (const std::string& cat : categories())
    {
        auto it = loadout.slots.find(cat);
        text += cat + ": " + (it != loadout.slots.end() ? it->second.name : std::string("(empty)")) + "\n";
    }
    text += "\nTo add it, drop this notecard on the Loadouts window (Combat > Loadouts...).\n";

    LLNotecard nc(LLNotecard::MAX_SIZE);
    nc.setText(text);
    std::stringstream out;
    nc.exportStream(out);
    const std::string buffer = out.str();
    const std::string name = "Loadout - " + loadout.name;
    LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback([buffer, name](const LLUUID& item_id)
    {
        const LLViewerRegion* region = gAgent.getRegion();
        const std::string url = region ? region->getCapability("UpdateNotecardAgentInventory") : std::string();
        if (item_id.isNull() || url.empty())
        {
            LL_WARNS("GMDXLoadouts") << "Could not write the loadout notecard" << LL_ENDL;
            return;
        }
        LLViewerAssetUpload::EnqueueInventoryUpload(url, std::make_shared<LLBufferedAssetUploadInfo>(
            item_id, LLAssetType::AT_NOTECARD, buffer,
            [name](LLUUID, LLUUID, LLUUID, LLSD)
            {
                LLSD args;
                args["NAME"] = name;
                LLNotificationsUtil::add("GMDXLoadoutShared", args);
            },
            nullptr));
    });
    create_inventory_item(gAgent.getID(), gAgent.getSessionID(), gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD),
                          LLTransactionID::tnull, name, "GunMetal-DX loadout", LLAssetType::AT_NOTECARD,
                          LLInventoryType::IT_NOTECARD, NO_INV_SUBTYPE, PERM_ALL, cb);
}

void GMDXLoadouts::importNotecard(const LLUUID& notecard_item)
{
    LLViewerInventoryItem* item = gInventory.getItem(notecard_item);
    if (item && item->getIsLinkType())
    {
        item = item->getLinkedItem();
    }
    if (!item || item->getType() != LLAssetType::AT_NOTECARD || item->getAssetUUID().isNull() || !gAssetStorage)
    {
        return;
    }
    gAssetStorage->getInvItemAsset(LLHost(), gAgent.getID(), gAgent.getSessionID(), item->getPermissions().getOwner(),
                                   LLUUID::null, item->getUUID(), item->getAssetUUID(), item->getType(),
                                   [](const LLUUID& asset_id, LLAssetType::EType type, void*, S32 status, LLExtStat)
    {
        std::string text;
        if (status == 0)
        {
            LLFileSystem file(asset_id, type, LLFileSystem::READ);
            std::string buffer(file.getSize(), '\0');
            if (!buffer.empty())
            {
                file.read((U8*)&buffer[0], (S32)buffer.size());
            }
            LLNotecard nc(LLNotecard::MAX_SIZE);
            std::istringstream in(buffer);
            text = buffer.compare(0, 19, "Linden text version") == 0 && nc.importStream(in) ? nc.getText() : buffer;
        }
        if (GMDXLoadouts::instanceExists())
        {
            GMDXLoadouts::instance().importText(text);
        }
    }, nullptr, true);
}

void GMDXLoadouts::importText(const std::string& text)
{
    // Header, then "<layer>: <weapon>" lines. A weapon is found by name -- the tag and version aside,
    // so an older or newer copy counts -- on that layer first, then on any.
    std::istringstream in(text);
    std::string line, name;
    std::vector<std::pair<std::string, std::string>> wanted;   // (layer, weapon name)
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (name.empty())
        {
            if (line.compare(0, strlen(kNotecardHeader), kNotecardHeader) == 0)
            {
                name = clean_name(line.substr(strlen(kNotecardHeader)));
            }
            continue;
        }
        const size_t colon = line.find(": ");
        if (colon == std::string::npos) continue;
        const std::string weapon = line.substr(colon + 2);
        if (weapon != "(empty)") wanted.emplace_back(line.substr(0, colon), weapon);
    }
    if (name.empty())
    {
        LLNotificationsUtil::add("GMDXLoadoutNotecardBad");
        return;
    }

    Index idx;
    buildIndex(idx);
    const std::vector<std::string> tags = GMDXWeaponSorter::tags();
    slots_t slots;
    std::vector<std::string> missing;
    for (const auto& [layer, weapon_name] : wanted)
    {
        const std::string key = GMDXWeaponSorter::normalize(weapon_name, tags);
        const Weapon* found = nullptr;
        std::string found_cat;
        auto look = [&](const std::string& cat)
        {
            for (const Weapon& w : idx.weapons[cat])
            {
                if (w.name == weapon_name || (!found && GMDXWeaponSorter::normalize(w.name, tags) == key))
                {
                    found = &w;
                    found_cat = cat;
                    if (w.name == weapon_name) return;   // the very same version wins
                }
            }
        };
        if (idx.weapons.count(layer)) look(layer);
        for (const std::string& cat : categories())
        {
            if (!found && cat != layer) look(cat);
        }
        if (found && !slots.count(found_cat))
        {
            slots[found_cat] = *found;
        }
        else
        {
            missing.push_back(weapon_name);
        }
    }
    // A name already taken gets a number.
    const std::vector<Loadout> have = loadouts();
    std::string unique = name;
    for (S32 n = 2; std::any_of(have.begin(), have.end(), [&unique](const Loadout& l) { return l.name == unique; }); ++n)
    {
        unique = llformat("%s (%d)", name.c_str(), n);
    }
    createLoadout(unique, slots, false);
    LLSD args;
    args["NAME"] = unique;
    args["FOUND"] = llformat("%d", S32(slots.size()));
    args["TOTAL"] = llformat("%d", S32(wanted.size()));
    std::string list;
    for (const std::string& m : missing) list += (list.empty() ? "" : ", ") + m;
    args["MISSING"] = list;
    LLNotificationsUtil::add(missing.empty() ? "GMDXLoadoutImported" : "GMDXLoadoutImportedMissing", args);
}

void GMDXLoadouts::setLoadoutSlot(const LLUUID& loadout_id, const std::string& cat, const Weapon* weapon)
{
    Index idx;
    buildIndex(idx);
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(loadout_id, cats, items);
    uuid_vec_t remove;
    if (items)
    {
        for (const auto& link : *items)
        {
            if (!link->getIsLinkType())
            {
                continue;  // a real object dropped in here is the user's; never purge it
            }
            auto range = idx.owner.equal_range(link->getLinkedUUID());
            for (auto it = range.first; it != range.second; ++it)
            {
                if (it->second.first == cat)
                {
                    remove.push_back(link->getUUID());
                    break;
                }
            }
        }
    }
    for (const LLUUID& id : remove)
    {
        remove_inventory_object(id, nullptr);
    }
    if (weapon)
    {
        linkSlots(loadout_id, { { cat, *weapon } });
    }
}

void GMDXLoadouts::saveWornInto(const LLUUID& loadout_id)
{
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(loadout_id, cats, items);
    uuid_vec_t remove;
    if (items)
    {
        for (const auto& link : *items)
        {
            if (link->getIsLinkType())  // links only: a real object dropped in here is never purged
            {
                remove.push_back(link->getUUID());
            }
        }
    }
    for (const LLUUID& id : remove)
    {
        remove_inventory_object(id, nullptr);
    }
    linkSlots(loadout_id, worn());
    gSavedPerAccountSettings.setString("GMDXLoadoutWorn", loadout_id.asString());
}

void GMDXLoadouts::renameLoadout(const LLUUID& loadout_id, const std::string& name)
{
    std::string clean = clean_name(name);
    if (!clean.empty())
    {
        rename_category(&gInventory, loadout_id, clean);
    }
}

void GMDXLoadouts::deleteLoadout(const LLUUID& loadout_id)
{
    if (wornLoadout() == loadout_id)
    {
        gSavedPerAccountSettings.setString("GMDXLoadoutWorn", "");
    }
    gInventory.removeCategory(loadout_id);  // to the trash, like any folder
}

//-----------------------------------------------------------------------------
// Weapon menus
//-----------------------------------------------------------------------------

void GMDXLoadouts::showChooser(const std::string& cat, const LLUUID& current, S32 x, S32 y,
                               std::function<void(const Weapon*)> on_pick, bool quick)
{
    if (LLContextMenu* old = mMenu.get())
    {
        gMenuHolder->removeChild(old);
        delete old;
    }
    LLContextMenu::Params menu_params;
    menu_params.name("gmdx_weapon_menu");
    menu_params.visible(false);
    LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(menu_params);

    auto add_label = [menu](const std::string& text)
    {
        LLMenuItemCallGL::Params p;
        p.name("label");
        p.label(text);
        p.enabled(false);
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(p));
    };
    auto add_check = [menu](const std::string& text, bool checked, std::function<void()> on_click,
                            const std::string& version = std::string())
    {
        WeaponMenuItem::Params p;
        p.name("weapon");
        p.label(text);
        p.version(version);
        p.on_click.function([on_click](LLUICtrl*, const LLSD&) { on_click(); });
        p.on_check.function([checked](LLUICtrl*, const LLSD&) { return checked; });
        menu->addChild(LLUICtrlFactory::create<WeaponMenuItem>(p));
    };

    if (quick)
    {
        add_label(cat);
        menu->addSeparator();
    }
    else
    {
        add_check("(empty)", current.isNull(), [on_pick] { on_pick(nullptr); });
        menu->addSeparator();
    }
    const std::vector<Weapon> list = weapons(cat);
    if (list.empty())
    {
        add_label(std::string("Nothing in ") + kFolderNames[EQUIPMENT] + "/" + cat + " yet");
    }
    // One row per weapon: copies and versions of it (same name once tag and version are set aside)
    // collapse to the latest -- or to the one worn, so its tick shows. By name, under the groups the
    // user made; the Loadouts window lists every copy.
    struct Row
    {
        const Weapon* weapon;
        std::string group, title, version;
        std::vector<std::string> parts;
    };
    const std::vector<std::string> tags = GMDXWeaponSorter::tags();
    std::vector<Row> rows;
    std::map<std::pair<std::string, std::string>, size_t> by_key;
    for (const Weapon& weapon : list)
    {
        Row r{ &weapon, shownGroup(weapon), "", "", GMDXWeaponSorter::parseVersion(weapon.name) };
        GMDXWeaponSorter::splitName(weapon.name, r.title, r.version);
        const auto key = std::make_pair(r.group, GMDXWeaponSorter::normalize(weapon.name, tags));
        auto it = by_key.find(key);
        if (it == by_key.end())
        {
            by_key.emplace(key, rows.size());
            rows.push_back(r);
            continue;
        }
        Row& have = rows[it->second];
        if (have.weapon->id == current) continue;
        if (weapon.id == current || GMDXWeaponSorter::compareVersions(r.parts, have.parts) > 0) have = r;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
    {
        if (a.group != b.group) return LLStringUtil::compareDict(a.group, b.group) < 0;
        return LLStringUtil::compareDict(a.title, b.title) < 0;
    });
    std::string group = "\n";  // matches no real group, so the first one gets its heading
    for (const Row& r : rows)
    {
        if (r.group != group)
        {
            group = r.group;
            if (!group.empty())
            {
                add_label(group);
            }
        }
        const Weapon weapon = *r.weapon;
        add_check(r.title, weapon.id == current, [on_pick, weapon] { on_pick(&weapon); }, r.version);
    }
    if (quick)
    {
        menu->addSeparator();
        add_check("Take off " + cat, false, [on_pick] { on_pick(nullptr); });
        add_check("Open Loadouts...", false, []
        {
            GMDXLoadouts::instance().mFreedMouse = false;  // keep the cursor free for the window; a world click takes it back
            LLFloaterReg::showInstance("gmdx_loadouts");
        });
    }

    mMenu = menu->getHandle();
    gMenuHolder->addChild(menu);
    menu->show(x, y);
}

void GMDXLoadouts::showQuickMenu(S32 slot)
{
    const std::vector<std::string> cats = categories();
    if (slot < 0 || slot >= S32(cats.size()) || !gInventory.isInventoryUsable())
    {
        return;
    }
    const std::string cat = cats[slot];
    const slots_t now = worn();
    auto it = now.find(cat);
    const LLUUID current = it != now.end() ? it->second.id : LLUUID::null;

    // Mouselook (and OTS) hide and pin the cursor: free it so the menu can be clicked. First: freeing
    // it deselects the grab tool held Alt switched to, and that closes every open menu.
    const bool free_mouse = gAgentCamera.cameraMouselook() && !LLToolCompGun::getInstance()->isMouseFreed();
    if (free_mouse)
    {
        LLToolCompGun::getInstance()->freeMouseForUI();
    }

    // Beside the crosshair, which is the middle of the world view.
    const LLRect world = gViewerWindow->getWorldViewRectScaled();
    showChooser(cat, current, world.getCenterX() + 14, world.getCenterY() - 6,
                [cat](const Weapon* weapon) { GMDXLoadouts::instance().swap(cat, weapon); }, true);

    if (free_mouse)
    {
        mFreedMouse = true;
        doOnIdleRepeating([]
        {
            GMDXLoadouts& self = GMDXLoadouts::instance();
            LLContextMenu* menu = self.mMenu.get();
            if (menu && menu->getVisible())
            {
                return false;
            }
            self.onQuickMenuClosed();
            return true;
        });
    }
}

namespace
{
    // GMDXQuickSwapKeys: "alt|1;alt|2;alt|3;alt|4", the XUI shortcut syntax; an empty entry is unbound.
    std::vector<std::string> hotkey_entries()
    {
        std::vector<std::string> out;
        const std::string all = gSavedSettings.getString("GMDXQuickSwapKeys");
        size_t start = 0;
        while (start <= all.size())
        {
            size_t end = all.find(';', start);
            if (end == std::string::npos) end = all.size();
            out.push_back(all.substr(start, end - start));
            start = end + 1;
        }
        out.resize(GMDXLoadouts::HOTKEY_SLOTS);
        return out;
    }

    LLMenuItemGL* hotkey_item(S32 slot)
    {
        return gMenuBarView ? gMenuBarView->findChild<LLMenuItemGL>(llformat("Quick Swap %d", slot + 1), true) : nullptr;
    }
}

void GMDXLoadouts::hotkey(S32 slot, KEY& key, MASK& mask)
{
    key = KEY_NONE;
    mask = MASK_NONE;
    const std::vector<std::string> entries = hotkey_entries();
    if (slot < 0 || slot >= HOTKEY_SLOTS || entries[slot].empty())
    {
        return;
    }
    const std::string& s = entries[slot];
    if (s.find("control") != std::string::npos) mask |= MASK_CONTROL;
    if (s.find("alt") != std::string::npos) mask |= MASK_ALT;
    if (s.find("shift") != std::string::npos) mask |= MASK_SHIFT;
    const size_t bar = s.rfind('|');
    if (!LLKeyboard::keyFromString(bar == std::string::npos ? s : s.substr(bar + 1), &key))
    {
        key = KEY_NONE;
    }
}

std::string GMDXLoadouts::hotkeyLabel(S32 slot)
{
    KEY key;
    MASK mask;
    hotkey(slot, key, mask);
    return key == KEY_NONE ? std::string() : LLKeyboard::stringFromAccelerator(mask, key);
}

bool GMDXLoadouts::setHotkey(S32 slot, KEY key, MASK mask)
{
    if (slot < 0 || slot >= HOTKEY_SLOTS)
    {
        return false;
    }
    std::vector<std::string> entries = hotkey_entries();
    std::string entry;
    if (key != KEY_NONE)
    {
        // Ours (any slot) may be taken over; anything else in the menus may not.
        bool ours = false;
        for (S32 i = 0; i < HOTKEY_SLOTS; ++i)
        {
            KEY k;
            MASK m;
            hotkey(i, k, m);
            if (k == key && m == mask)
            {
                ours = true;
                entries[i].clear();
            }
        }
        if (!ours && gMenuBarView && gMenuBarView->hasAccelerator(key, mask))
        {
            return false;
        }
        if (mask & MASK_CONTROL) entry += "control|";
        if (mask & MASK_ALT) entry += "alt|";
        if (mask & MASK_SHIFT) entry += "shift|";
        entry += LLKeyboard::stringFromKey(key, false);
    }
    entries[slot] = entry;
    std::string all;
    for (S32 i = 0; i < HOTKEY_SLOTS; ++i)
    {
        all += (i ? ";" : "") + entries[i];
    }
    gSavedSettings.setString("GMDXQuickSwapKeys", all);
    applyHotkeys();
    return true;
}

void GMDXLoadouts::applyHotkeys(bool clear)
{
    for (S32 i = 0; i < HOTKEY_SLOTS; ++i)
    {
        if (LLMenuItemGL* item = hotkey_item(i))
        {
            KEY key = KEY_NONE;
            MASK mask = MASK_NONE;
            if (!clear) hotkey(i, key, mask);
            item->setAccelerator(key, mask);
        }
    }
}

void GMDXLoadouts::onQuickMenuClosed()
{
    if (!mFreedMouse)
    {
        return;
    }
    mFreedMouse = false;
    // A click in the world while the cursor is free already took mouselook back.
    if (gAgentCamera.cameraMouselook() && LLToolCompGun::getInstance()->isMouseFreed())
    {
        LLToolCompGun::getInstance()->recaptureMouse();
    }
}
