/**
 * @file gmdxweaponsorter.cpp
 * @brief GunMetal-DX: offers to sort newly received weapons (names with a watched tag) into the equipment folders.
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

#include "gmdxweaponsorter.h"

#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryobserver.h"
#include "llappearancemgr.h"
#include "llvoavatarself.h"
#include "lldate.h"
#include "lllineeditor.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "llui.h"
#include "llviewermenu.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"

#include <algorithm>
#include <map>
#include <regex>

namespace
{
    const F32 kSettleSeconds = 2.f;            // a given folder's contents arrive after the folder
    const std::string kGroupForFolders = GMDXLoadouts::folderGroupName();  // a multi-part weapon needs a group folder to sit in

    class SorterObserver : public LLInventoryObserver
    {
    public:
        void changed(U32 mask) override
        {
            // Every add: the login fetch's too (it "adds" the whole inventory, and with a large one it
            // may never report everything fetched); considerAll keeps only what is new this session.
            if (mask & ADD)
            {
                const auto& added = gInventory.getAddedIDs();
                GMDXWeaponSorter::instance().onAdded(std::set<LLUUID>(added.begin(), added.end()));
            }
        }
    };

    bool contains_tag(const std::string& name, const std::vector<std::string>& tags)
    {
        std::string lower = name;
        LLStringUtil::toLower(lower);
        for (const std::string& tag : tags)
        {
            if (lower.find(tag) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // When a version came into the inventory: the item's creation date (a link's: its target's), or
    // a folder's newest.
    time_t version_date(const LLUUID& id, bool folder)
    {
        if (!folder)
        {
            const LLViewerInventoryItem* item = gInventory.getItem(id);
            if (item && item->getLinkedItem()) item = item->getLinkedItem();
            return item ? item->getCreationDate() : (time_t)0;
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(id, cats, items, false);
        time_t newest = 0;
        for (const auto& item : items)
        {
            const LLViewerInventoryItem* real = item->getLinkedItem() ? item->getLinkedItem() : item.get();
            newest = std::max(newest, real->getCreationDate());
        }
        return newest;
    }

    // Objects inside a folder, links resolved to their items.
    uuid_vec_t object_parts(const LLUUID& folder)
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(folder, cats, items, false);
        uuid_vec_t parts;
        for (const auto& item : items)
        {
            if (item->getType() == LLAssetType::AT_OBJECT && !item->getIsBrokenLink())
            {
                parts.push_back(item->getLinkedUUID());
            }
        }
        return parts;
    }

    // The new version's parts to wear: its objects, less those named like a part the old version
    // has in Not worn (the sling it was told to leave off). carryNotWorn moves them there after.
    uuid_vec_t parts_like(const LLUUID& new_folder, const std::vector<LLUUID>& old_folders)
    {
        std::set<std::string> unworn;
        for (const LLUUID& old : old_folders)
        {
            for (const GMDXLoadouts::Part& p : GMDXLoadouts::instance().parts(old))
            {
                if (!p.worn) unworn.insert(p.name);
            }
        }
        uuid_vec_t out;
        for (const GMDXLoadouts::Part& p : GMDXLoadouts::instance().parts(new_folder))
        {
            if (!p.worn || unworn.count(p.name)) continue;
            const LLViewerInventoryItem* item = gInventory.getItem(p.id);
            out.push_back(item ? item->getLinkedUUID() : p.id);
        }
        return out.empty() ? object_parts(new_folder) : out;
    }

    // Packaging, not a weapon: a rez-to-unpack box would otherwise "update" the gun it holds.
    bool is_packaging(const std::string& name)
    {
        static const std::regex box(R"(\b(box|boxed|unpack|unpacker|packed)\b)", std::regex::icase);
        return std::regex_search(name, box);
    }
}

GMDXWeaponSorter::GMDXWeaponSorter()
{
    mObserver = new SorterObserver();
    gInventory.addObserver(mObserver);
}

GMDXWeaponSorter::~GMDXWeaponSorter()
{
    // At shutdown the inventory deletes the observers it still holds, this one included.
    if (gInventory.containsObserver(mObserver))
    {
        gInventory.removeObserver(mObserver);
        delete mObserver;
    }
}

std::vector<std::string> GMDXWeaponSorter::tags()
{
    std::vector<std::string> list;
    std::string all = gSavedPerAccountSettings.getString("GMDXLoadoutWatchTags");
    LLStringUtil::toLower(all);
    size_t start = 0;
    while (start <= all.size())
    {
        size_t end = all.find(',', start);
        if (end == std::string::npos)
        {
            end = all.size();
        }
        std::string tag = all.substr(start, end - start);
        LLStringUtil::trim(tag);
        if (!tag.empty())
        {
            list.push_back(tag);
        }
        start = end + 1;
    }
    return list;
}

bool GMDXWeaponSorter::findVersion(const std::string& name, size_t& pos, size_t& len, std::string& digits)
{
    // The last "v1.2.3" / "v2" in the name; failing that, a dotted number ending it ("Levitate 0.31").
    // Other bare numbers are the model or calibre (AR-10, Glock 17, Mk2, 1.5x scope).
    // Known limit: a name ending in a calibre ("Rifle 7.62") reads as a version; skip the trailing form
    // if a group names guns that way.
    static const std::regex ver(R"(\bv\s*(\d+(?:\.\d+)*))", std::regex::icase);
    static const std::regex trailing(R"((?:^|\s)(\d+(?:\.\d+)+)\s*$)");
    std::smatch m, last;
    bool found = false;
    for (auto it = std::sregex_iterator(name.begin(), name.end(), ver); it != std::sregex_iterator(); ++it)
    {
        last = *it;
        found = true;
    }
    if (!found)
    {
        found = std::regex_search(name, last, trailing);
        if (found)
        {
            pos = last.position(1);
            len = last.length(1);
            digits = last[1].str();
            return true;
        }
        return false;
    }
    pos = last.position(0);
    len = last.length(0);
    digits = last[1].str();
    return true;
}

std::string GMDXWeaponSorter::normalize(const std::string& name, const std::vector<std::string>& tags)
{
    // "[Ash] Rifle v2.1 (Update)" and "[Ash] Rifle 2.2" both become "rifle".
    std::string s = name;
    size_t pos, len;
    std::string digits;
    if (findVersion(s, pos, len, digits) && s[pos] != 'v' && s[pos] != 'V')
    {
        s.replace(pos, len, " ");   // the trailing bare form; "v1.2a" / "v1_2" go whole with the regex below
    }
    LLStringUtil::toLower(s);
    for (const std::string& tag : tags)
    {
        for (size_t at = s.find(tag); at != std::string::npos; at = s.find(tag))
        {
            s.erase(at, tag.size());
        }
    }
    static const std::regex version(R"(\bv\d+([._-]\d+)*[a-z]?\b)");   // (earlier ones: "v2 ... v2.1")
    static const std::regex words(R"(\b(updated?|new|latest|version|ver|rel|release|copy)\b)");
    static const std::regex other(R"([^a-z0-9]+)");
    s = std::regex_replace(s, version, " ");
    s = std::regex_replace(s, words, " ");
    s = std::regex_replace(s, other, " ");
    LLStringUtil::trim(s);
    return s;
}

void GMDXWeaponSorter::splitName(const std::string& name, std::string& title, std::string& version)
{
    // "[Ash] - Chitin Throwing Stars v1.25" -> "Chitin Throwing Stars", "v1.25" (menus and lists).
    title = name;
    version.clear();
    size_t pos, len;
    std::string digits;
    if (findVersion(title, pos, len, digits))
    {
        version = (title[pos] == 'v' || title[pos] == 'V') ? "v" + digits : digits;
        title.erase(pos, len);
    }
    std::string lower = title;
    LLStringUtil::toLower(lower);
    for (const std::string& tag : tags())
    {
        for (size_t at = lower.find(tag); at != std::string::npos; at = lower.find(tag))
        {
            lower.erase(at, tag.size());
            title.erase(at, tag.size());
        }
    }
    static const std::regex edges(R"(^[\s\-:|_.,]+|[\s\-:|_.,]+$)");
    static const std::regex spaces(R"(\s{2,})");
    title = std::regex_replace(std::regex_replace(title, edges, ""), spaces, " ");
    if (title.empty())
    {
        title = name;   // nothing but the tag and a number: show it as it is
        version.clear();
    }
}

void GMDXWeaponSorter::onAdded(const std::set<LLUUID>& ids)
{
    if (tags().empty())
    {
        return;
    }
    mPending.insert(ids.begin(), ids.end());
    if (!mScheduled)
    {
        mScheduled = true;
        doAfterInterval([]
        {
            GMDXWeaponSorter& self = GMDXWeaponSorter::instance();
            self.mScheduled = false;
            std::vector<LLUUID> ids(self.mPending.begin(), self.mPending.end());
            self.mPending.clear();
            self.considerAll(ids);
        }, kSettleSeconds);
    }
}

void GMDXWeaponSorter::scanInventory()
{
    // Everything already in inventory goes through the importer's table, not one prompt each.
    mQueue.clear();
    showWindow();
    LLFloaterReg::showInstance("gmdx_weapon_import");
}

std::vector<std::string> GMDXWeaponSorter::parseVersion(const std::string& name)
{
    // The parts stay as written: "03" and "3" order differently (see compareVersions).
    std::vector<std::string> out;
    size_t pos, len;
    std::string digits;
    if (!findVersion(name, pos, len, digits))
    {
        return out;
    }
    size_t start = 0;
    while (start < digits.size())
    {
        size_t dot = digits.find('.', start);
        if (dot == std::string::npos) dot = digits.size();
        out.push_back(digits.substr(start, dot - start));
        start = dot + 1;
    }
    return out;
}

int GMDXWeaponSorter::compareVersions(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    // Creators number two ways -- decimal (v1.03 < v1.1 < v1.11 < v1.2) and dotted (v1.2 < v1.10) --
    // and switch between them (Combat HUD v0.67, then v0.11.3). Only versions of one shape (as many
    // parts, the differing part as many digits) are ordered by number; for the rest the name can't
    // tell: 0, and the caller goes by date.
    if (a.empty() || b.empty() || a.size() != b.size()) return 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i] == b[i]) continue;
        if (a[i].size() != b[i].size()) return 0;
        return a[i] > b[i] ? 1 : -1;   // (same digit count: text order is number order)
    }
    return 0;
}

namespace
{
    // Every object id a version stands for, links resolved: the item (or its target), or each
    // object inside the folder. What must never be trashed while the kept version uses it.
    std::set<LLUUID> version_objects(const LLUUID& id, bool folder)
    {
        std::set<LLUUID> out;
        if (!folder)
        {
            if (const LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                out.insert(item->getUUID());
                out.insert(item->getLinkedUUID());
            }
            return out;
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(id, cats, items, false);
        for (const auto& item : items)
        {
            out.insert(item->getUUID());
            out.insert(item->getLinkedUUID());
        }
        return out;
    }

    bool version_worn(const LLUUID& id, bool folder)
    {
        for (const LLUUID& o : version_objects(id, folder))
        {
            if (get_is_item_worn(o)) return true;
        }
        return false;
    }
}

std::vector<GMDXWeaponSorter::ImportRow> GMDXWeaponSorter::buildImport(const std::vector<std::string>& watched)
{
    std::vector<ImportRow> rows;
    if (watched.empty() || !gInventory.isInventoryUsable())
    {
        return rows;
    }
    std::map<std::string, size_t> by_key;
    auto add = [&](ImportVersion v)
    {
        const std::string key = normalize(v.name, watched);
        if (key.empty()) return;
        auto it = by_key.find(key);
        if (it == by_key.end())
        {
            it = by_key.emplace(key, rows.size()).first;
            rows.push_back(ImportRow());
            rows.back().key = key;
        }
        rows[it->second].versions.push_back(v);
    };
    auto date_of = version_date;
    auto parent_of = [](const LLUUID& id)
    {
        const LLInventoryObject* obj = gInventory.getObject(id);
        return obj ? obj->getParentUUID() : LLUUID::null;
    };
    auto where_of = [](const LLUUID& id)
    {
        const LLInventoryObject* obj = gInventory.getObject(id);
        const LLViewerInventoryCategory* parent = obj ? gInventory.getCategory(obj->getParentUUID()) : nullptr;
        return parent ? parent->getName() : std::string();
    };

    // Already sorted: the weapons in Equipment. Their objects -- links resolved -- are never
    // offered again as unsorted versions (the original behind a link in Equipment IS that weapon).
    std::set<LLUUID> sorted_objects;
    for (const GMDXLoadouts::Entry& e : GMDXLoadouts::instance().allWeapons())
    {
        if (!contains_tag(e.weapon.name, watched)) continue;
        ImportVersion v;
        v.id = e.weapon.id;
        v.name = e.weapon.name;
        v.folder = gInventory.getCategory(e.weapon.id) != nullptr;
        v.date = date_of(v.id, v.folder);
        v.version = parseVersion(v.name);
        v.sorted = true;
        v.entry = e;
        v.where = e.weapon.group.empty() ? e.cat : e.cat + " / " + e.weapon.group;
        v.parent = parent_of(v.id);
        const std::set<LLUUID> objs = version_objects(v.id, v.folder);
        sorted_objects.insert(objs.begin(), objs.end());
        add(v);
    }
    // Everything else with the tag, Outdated included: a version set aside there by mistake (or by
    // an older, wrong comparison) is offered back beside the live copies of its weapon.
    const LLUUID outdated = GMDXLoadouts::instance().folder(GMDXLoadouts::OUTDATED);
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    gInventory.collectDescendents(gInventory.getRootFolderID(), cats, items, false);
    std::set<LLUUID> seen;
    auto consider = [&](const LLUUID& id, const std::string& name)
    {
        if (!contains_tag(name, watched)) return;   // (cheap test first: 1000s of items)
        Arrival a;
        if (!makeArrival(id, watched, a, true) || seen.count(a.id)) return;
        seen.insert(a.id);
        ImportVersion v;
        v.id = a.id;
        v.name = a.name;
        v.folder = a.folder;
        for (const LLUUID& o : version_objects(v.id, v.folder))
        {
            if (sorted_objects.count(o)) return;   // already in Equipment (as itself or a link)
        }
        v.date = date_of(v.id, v.folder);
        v.version = parseVersion(v.name);
        v.where = where_of(v.id);
        v.parent = parent_of(v.id);
        v.outdated = outdated.notNull() && gInventory.isObjectDescendentOf(v.id, outdated);
        add(v);
    };
    for (const auto& cat : cats) consider(cat->getUUID(), cat->getName());
    for (const auto& item : items) consider(item->getUUID(), item->getName());

    // A weapon only in Outdated stays retired.
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const ImportRow& r)
    {
        return std::all_of(r.versions.begin(), r.versions.end(), [](const ImportVersion& v) { return v.outdated; });
    }), rows.end());

    for (ImportRow& r : rows)
    {
        // Keep: the later version where the names tell; else (no version, or numbering the name
        // can't settle) the newest. Copies of one version: the sorted one, then a live one over
        // Outdated, then the newest.
        auto place_rank = [](const ImportVersion& v) { return v.sorted ? 2 : (v.outdated ? 0 : 1); };
        r.keep = 0;
        for (int i = 1; i < (int)r.versions.size(); ++i)
        {
            const ImportVersion& a = r.versions[i];
            const ImportVersion& b = r.versions[r.keep];
            const int order = compareVersions(a.version, b.version);
            bool better;
            if (order != 0) better = order > 0;
            else if (!a.version.empty() && a.version == b.version && place_rank(a) != place_rank(b))
                better = place_rank(a) > place_rank(b);
            else if (a.date != b.date) better = a.date > b.date;
            else better = place_rank(a) > place_rank(b);
            if (better) r.keep = i;
        }
        // Pre-filled destination only when exactly one version is sorted (an update or duplicates
        // of a weapon in one place). Several sorted copies in different places: the user decides.
        int sorted = 0;
        for (const ImportVersion& v : r.versions) sorted += v.sorted ? 1 : 0;
        if (sorted == 1)
        {
            for (const ImportVersion& v : r.versions)
            {
                if (v.sorted)
                {
                    r.cat = v.entry.cat;
                    r.group = v.entry.weapon.group;
                }
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const ImportRow& a, const ImportRow& b)
    {
        return LLStringUtil::compareDict(a.key, b.key) < 0;
    });
    return rows;
}

void GMDXWeaponSorter::applyImport(const std::vector<ImportRow>& rows, EOthers others)
{
    GMDXLoadouts& lo = GMDXLoadouts::instance();
    const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    const LLUUID outdated = lo.folder(GMDXLoadouts::OUTDATED);
    // Still where the scan saw it, and not thrown away since (one scanned in Outdated may be there).
    auto current = [&](const ImportVersion& v)
    {
        const LLInventoryObject* obj = gInventory.getObject(v.id);
        return obj && obj->getParentUUID() == v.parent && !gInventory.isObjectDescendentOf(v.id, trash)
            && (v.outdated || outdated.isNull() || !gInventory.isObjectDescendentOf(v.id, outdated));
    };
    for (const ImportRow& r : rows)
    {
        if (r.cat.empty() || r.versions.empty()) continue;
        const ImportVersion keep = r.versions[r.keep];
        if (!current(keep)) continue;
        const LLUUID cat_folder = lo.categoryFolder(r.cat);
        if (cat_folder.isNull()) continue;

        // Everything after the kept version has landed (a new group folder may take a reply):
        // relink the loadouts, then the other versions -- never one the kept version uses, never
        // one being worn, never a sorted copy kept on purpose somewhere else.
        auto finish = [r, keep, others]()
        {
            GMDXLoadouts& lo = GMDXLoadouts::instance();
            std::vector<LLUUID> replaced;   // the sorted versions this one takes the place of
            for (int i = 0; i < (int)r.versions.size(); ++i)
            {
                const ImportVersion& v = r.versions[i];
                if (i != r.keep && v.sorted && v.folder && v.entry.cat == r.cat && v.entry.weapon.group == r.group)
                    replaced.push_back(v.id);
            }
            GMDXLoadouts::Weapon now{ keep.name, r.group, keep.id,
                                      keep.folder ? parts_like(keep.id, replaced) : uuid_vec_t{ gInventory.getItem(keep.id)->getLinkedUUID() } };
            if (keep.folder)
            {
                for (const LLUUID& old : replaced) lo.carryNotWorn(old, keep.id);
            }
            std::set<LLUUID> protect = version_objects(keep.id, keep.folder);
            protect.insert(keep.id);
            for (int i = 0; i < (int)r.versions.size(); ++i)
            {
                const ImportVersion& v = r.versions[i];
                if (i == r.keep) continue;
                if (v.sorted && (v.entry.cat != r.cat || v.entry.weapon.group != r.group)) continue;
                bool shared = protect.count(v.id) > 0;
                for (const LLUUID& o : version_objects(v.id, v.folder))
                {
                    if (protect.count(o)) shared = true;
                }
                if (shared || !gInventory.getObject(v.id)) continue;
                // (every object of the old folder: loadouts may link parts since set to Not worn)
                if (v.sorted) lo.replaceInLoadouts(v.folder ? object_parts(v.id) : v.entry.weapon.parts, r.cat, now);
                // (one already in Outdated stays there, whatever was chosen)
                if (others == OTHERS_LEAVE || v.outdated || version_worn(v.id, v.folder)) continue;
                const LLUUID id = v.id;
                if (others == OTHERS_TRASH)
                {
                    gInventory.removeObject(id);   // to the Trash
                }
                else
                {
                    lo.withFolder(GMDXLoadouts::OUTDATED, [id](const LLUUID& outdated)
                    {
                        if (outdated.notNull()) GMDXLoadouts::moveTo(id, outdated);
                    });
                }
            }
        };

        const bool in_place = keep.sorted && keep.entry.cat == r.cat && keep.entry.weapon.group == r.group;
        if (in_place)
        {
            finish();
            continue;
        }
        Arrival a;
        a.id = keep.id; a.name = keep.name; a.folder = keep.folder;
        if (r.group.empty())
        {
            place(a, cat_folder, true, finish);
        }
        else
        {
            GMDXLoadouts::withSubfolder(cat_folder, r.group, [a, finish](const LLUUID& dest)
            {
                if (dest.isNull()) return;   // the group folder failed: nothing else moves either
                GMDXWeaponSorter::instance().place(a, dest, false, finish);
            });
        }
    }
}

bool GMDXWeaponSorter::isCandidate(const LLUUID& id, const std::vector<std::string>& tags, bool outdated_too) const
{
    const LLInventoryObject* obj = gInventory.getObject(id);
    if (!obj || obj->getIsLinkType() || !contains_tag(obj->getName(), tags) || is_packaging(obj->getName()))
    {
        return false;
    }
    if (const LLViewerInventoryItem* item = gInventory.getItem(id))
    {
        if (item->getType() != LLAssetType::AT_OBJECT)
        {
            return false;
        }
    }
    else if (const LLViewerInventoryCategory* cat = gInventory.getCategory(id))
    {
        if (cat->getPreferredType() != LLFolderType::FT_NONE || object_parts(id).empty())
        {
            return false;
        }
    }

    // Not already sorted, not in a place it belongs (Trash, outfits, #RLV), and not inside a tagged
    // folder: that folder is offered as one weapon instead.
    GMDXLoadouts& lo = GMDXLoadouts::instance();
    const LLUUID root = gInventory.getRootFolderID();
    const std::set<LLUUID> skip = {
        lo.equipmentFolder(), lo.loadoutsFolder(), outdated_too ? LLUUID::null : lo.folder(GMDXLoadouts::OUTDATED),
        lo.findFolder(root, "#RLV"),
        gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH),
        gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT),
        gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS),
        gInventory.findCategoryUUIDForType(LLFolderType::FT_LOST_AND_FOUND),
        gInventory.findCategoryUUIDForType(LLFolderType::FT_MARKETPLACE_LISTINGS) };
    for (LLUUID parent = obj->getParentUUID(); parent != root; )
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(parent);
        if (!cat || skip.count(parent) || contains_tag(cat->getName(), tags))
        {
            return false;  // no category: outside My Inventory (the Library)
        }
        parent = cat->getParentUUID();
    }
    return true;
}

bool GMDXWeaponSorter::makeArrival(LLUUID id, const std::vector<std::string>& watched, Arrival& arrival, bool outdated_too) const
{
    // An item inside a tagged folder stands for that folder (its contents can arrive after it).
    const LLUUID root = gInventory.getRootFolderID();
    for (const LLInventoryObject* obj = gInventory.getObject(id); obj && obj->getParentUUID() != root; )
    {
        const LLViewerInventoryCategory* parent = gInventory.getCategory(obj->getParentUUID());
        if (parent && contains_tag(parent->getName(), watched))
        {
            id = parent->getUUID();
        }
        obj = parent;
    }
    if (!isCandidate(id, watched, outdated_too))
    {
        return false;
    }
    arrival = Arrival();
    arrival.id = id;
    arrival.name = gInventory.getObject(id)->getName();
    arrival.folder = gInventory.getCategory(id) != nullptr;
    if (arrival.folder)
    {
        // A folder holding one object (a gift box with a notecard) is really that object -- the
        // object itself, never a link's target somewhere else.
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(id, cats, items, false);
        const LLViewerInventoryItem* only = nullptr;
        int objects = 0;
        for (const auto& item : items)
        {
            if (item->getType() == LLAssetType::AT_OBJECT && !item->getIsBrokenLink())
            {
                ++objects;
                only = item;
            }
        }
        if (objects == 1 && only && !only->getIsLinkType())
        {
            arrival.id = only->getUUID();
            arrival.name = only->getName();
            arrival.folder = false;
        }
    }
    return true;
}

void GMDXWeaponSorter::considerAll(const std::vector<LLUUID>& ids)
{
    const std::vector<std::string> watched = tags();
    if (watched.empty() || !gInventory.isInventoryUsable())
    {
        return;
    }
    for (const LLUUID& id : ids)
    {
        Arrival arrival;
        // New this session only: a fetched folder's old items are "added" too (the importer is for
        // those, and for anything delivered while logged out). Five minutes' slack for clock skew.
        if (!makeArrival(id, watched, arrival) || version_date(arrival.id, arrival.folder) + 300 < mSessionStart
            || mSeen.count(arrival.id)
            || std::any_of(mQueue.begin(), mQueue.end(), [&](const Arrival& a) { return a.id == arrival.id; }))
        {
            continue;
        }
        mSeen.insert(arrival.id);
        mQueue.push_back(arrival);
    }
    showWindow();
}

void GMDXWeaponSorter::rematch(Arrival& arrival)
{
    // Matched when it comes up, not when queued: replacing an earlier version (v2 over v1) must make a
    // later one (v3) match v2, not the v1 now in Outdated.
    const std::vector<std::string> watched = tags();
    const std::string key = normalize(arrival.name, watched);
    arrival.hasMatch = false;
    if (key.empty())
    {
        return;
    }
    for (const GMDXLoadouts::Entry& entry : GMDXLoadouts::instance().allWeapons())
    {
        if (normalize(entry.weapon.name, watched) == key)
        {
            arrival.hasMatch = true;
            arrival.match = entry;
            return;
        }
    }
}

bool GMDXWeaponSorter::stillThere(const Arrival& arrival) const
{
    // Rezzed, deleted or emptied since it was offered.
    if (!gInventory.getObject(arrival.id))
    {
        return false;
    }
    return !arrival.folder || !object_parts(arrival.id).empty();
}

void GMDXWeaponSorter::prepareFront()
{
    while (!mQueue.empty() && !stillThere(mQueue.front()))
    {
        mQueue.pop_front();
    }
    if (!mQueue.empty())
    {
        rematch(mQueue.front());
    }
}

void GMDXWeaponSorter::requeue(const Arrival& arrival)
{
    mQueue.push_front(arrival);  // a folder create failed: offer it again
    showWindow();
}

void GMDXWeaponSorter::showWindow()
{
    prepareFront();
    if (mQueue.empty())
    {
        LLFloaterReg::hideInstance("gmdx_weapon_sort");
        return;
    }
    if (LLFloater* floater = LLFloaterReg::findInstance("gmdx_weapon_sort"); floater && floater->getVisible())
    {
        floater->refresh();
        return;
    }
    LLFloaterReg::showInstance("gmdx_weapon_sort", LLSD(), false);  // the floater never takes focus (see its constructor)
}

void GMDXWeaponSorter::place(const Arrival& arrival, const LLUUID& dest, bool dest_is_category, std::function<void()> then)
{
    // A folder of parts straight in a category would read as a group of single weapons.
    if (arrival.folder && dest_is_category)
    {
        GMDXLoadouts::withSubfolder(dest, kGroupForFolders, [arrival, then](const LLUUID& group)
        {
            if (group.isNull())
            {
                GMDXWeaponSorter::instance().requeue(arrival);
                return;
            }
            GMDXLoadouts::moveTo(arrival.id, group);
            then();
        });
        return;
    }
    GMDXLoadouts::moveTo(arrival.id, dest);
    then();
}

void GMDXWeaponSorter::replaceOld()
{
    prepareFront();
    if (mQueue.empty() || !mQueue.front().hasMatch)
    {
        showWindow();
        return;
    }
    const Arrival arrival = mQueue.front();
    mQueue.pop_front();
    const GMDXLoadouts::Entry old = arrival.match;
    if (const LLInventoryObject* old_obj = gInventory.getObject(old.weapon.id))
    {
        const LLUUID dest = old_obj->getParentUUID();
        place(arrival, dest, dest == GMDXLoadouts::instance().categoryFolder(old.cat), [arrival, old]
        {
            const LLViewerInventoryItem* item = gInventory.getItem(arrival.id);
            GMDXLoadouts::Weapon now{ arrival.name, old.weapon.group, arrival.id,
                                      arrival.folder ? parts_like(arrival.id, { old.weapon.id })
                                                     : uuid_vec_t{ item ? item->getLinkedUUID() : arrival.id } };
            GMDXLoadouts::instance().replaceInLoadouts(gInventory.getCategory(old.weapon.id) ? object_parts(old.weapon.id)
                                                                                              : old.weapon.parts, old.cat, now);
            if (arrival.folder) GMDXLoadouts::instance().carryNotWorn(old.weapon.id, arrival.id);

            // Wearing the old version: the new one goes on in its place. Done here, not as a queued swap,
            // because once the old one is in Outdated no swap knows it belongs to the category.
            uuid_vec_t worn_old;
            for (const LLUUID& part : old.weapon.parts)
            {
                if (isAgentAvatarValid() && gAgentAvatarp->isWearingAttachment(part))
                {
                    worn_old.push_back(part);
                }
            }
            if (!worn_old.empty())
            {
                LLAppearanceMgr::instance().removeItemsFromAvatar(worn_old);
                LLAppearanceMgr::instance().wearItemsOnAvatar(now.parts, false, false);
            }

            const LLUUID old_id = old.weapon.id;
            GMDXLoadouts::instance().withFolder(GMDXLoadouts::OUTDATED, [old_id](const LLUUID& outdated)
            {
                if (outdated.notNull())
                {
                    GMDXLoadouts::moveTo(old_id, outdated);
                }
            });
        });
    }
    showWindow();
}

void GMDXWeaponSorter::keepBoth()
{
    prepareFront();
    if (mQueue.empty() || !mQueue.front().hasMatch)
    {
        showWindow();
        return;
    }
    const Arrival arrival = mQueue.front();
    mQueue.pop_front();
    if (const LLInventoryObject* old_obj = gInventory.getObject(arrival.match.weapon.id))
    {
        const LLUUID dest = old_obj->getParentUUID();
        place(arrival, dest, dest == GMDXLoadouts::instance().categoryFolder(arrival.match.cat), [] {});
    }
    showWindow();
}

void GMDXWeaponSorter::moveInto(const std::string& cat, const std::string& group)
{
    prepareFront();
    const LLUUID cat_folder = GMDXLoadouts::instance().categoryFolder(cat);
    if (mQueue.empty() || cat_folder.isNull())
    {
        showWindow();
        return;  // no category to move into: the offer stays
    }
    const Arrival arrival = mQueue.front();
    mQueue.pop_front();
    if (group.empty())
    {
        place(arrival, cat_folder, true, [] {});
    }
    else
    {
        GMDXLoadouts::withSubfolder(cat_folder, group, [arrival](const LLUUID& dest)
        {
            if (dest.isNull())
            {
                GMDXWeaponSorter::instance().requeue(arrival);
                return;
            }
            GMDXWeaponSorter::instance().place(arrival, dest, false, [] {});
        });
    }
    showWindow();
}

void GMDXWeaponSorter::skip()
{
    if (!mQueue.empty())
    {
        mQueue.pop_front();
    }
    showWindow();
}

//-----------------------------------------------------------------------------
// GMDXFloaterWeaponSort
//-----------------------------------------------------------------------------

GMDXFloaterWeaponSort::GMDXFloaterWeaponSort(const LLSD& key)
:   LLFloater(key)
{
    setAutoFocus(false);  // pops up mid-fight: WASD and Return must stay with the avatar and chat
}

bool GMDXFloaterWeaponSort::postBuild()
{
    mItemText = getChild<LLTextBox>("item_text");
    mMatchText = getChild<LLTextBox>("match_text");
    mCountText = getChild<LLTextBox>("count_text");
    mCatCombo = getChild<LLComboBox>("cat_combo");
    mGroupCombo = getChild<LLComboBox>("group_combo");

    mCatCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshGroups(); });
    getChild<LLButton>("replace_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { GMDXWeaponSorter::instance().replaceOld(); });
    getChild<LLButton>("keep_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { GMDXWeaponSorter::instance().keepBoth(); });
    getChild<LLButton>("move_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const std::string group = mGroupCombo->getCurrentIndex() > 0 ? mGroupCombo->getSimple() : std::string();
        GMDXWeaponSorter::instance().moveInto(mCatCombo->getSimple(), group);
    });
    getChild<LLButton>("skip_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { GMDXWeaponSorter::instance().skip(); });
    getChild<LLButton>("table_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { GMDXWeaponSorter::instance().scanInventory(); });
    return true;
}

void GMDXFloaterWeaponSort::onOpen(const LLSD& key)
{
    // Right edge, under the swap toast: it never takes focus, so a focused window in the middle
    // (the Loadouts window) would otherwise cover it.
    if (LLView* parent = getParent())
    {
        const LLRect& area = parent->getLocalRect();
        setOrigin(area.getWidth() - getRect().getWidth() - 16, area.getHeight() - getRect().getHeight() - 110);
    }
    refresh();
}

void GMDXFloaterWeaponSort::refresh()
{
    GMDXWeaponSorter& sorter = GMDXWeaponSorter::instance();
    if (!sorter.hasNext())
    {
        closeFloater();
        return;
    }
    const GMDXWeaponSorter::Arrival& a = sorter.next();
    mItemText->setText(a.name);
    LLStringUtil::format_map_t args;
    args["[COUNT]"] = llformat("%d", S32(sorter.queued()));
    mCountText->setText(sorter.queued() > 1 ? getString("more", args) : LLStringUtil::null);
    getChild<LLButton>("table_btn")->setVisible(sorter.queued() > 1);

    getChild<LLButton>("replace_btn")->setVisible(a.hasMatch);
    getChild<LLButton>("keep_btn")->setVisible(a.hasMatch);
    getChild<LLButton>("move_btn")->setVisible(!a.hasMatch);
    mCatCombo->setVisible(!a.hasMatch);
    mGroupCombo->setVisible(!a.hasMatch);
    if (a.hasMatch)
    {
        args["[OLD]"] = a.match.weapon.name;
        args["[WHERE]"] = a.match.weapon.group.empty() ? a.match.cat : a.match.cat + " / " + a.match.weapon.group;
        mMatchText->setText(getString("update_of", args));
        return;
    }
    mMatchText->setText(getString("move_into"));
    const std::string current = mCatCombo->getSimple();
    mCatCombo->removeall();
    for (const std::string& cat : GMDXLoadouts::instance().categories())
    {
        mCatCombo->add(cat);
    }
    if (current.empty() || !mCatCombo->setSimple(current))
    {
        mCatCombo->selectFirstItem();
    }
    refreshGroups();
}

void GMDXFloaterWeaponSort::refreshGroups()
{
    mGroupCombo->removeall();
    mGroupCombo->add(getString("no_group"));
    const LLUUID cat_folder = GMDXLoadouts::instance().categoryFolder(mCatCombo->getSimple());
    LLInventoryModel::cat_array_t* groups = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(cat_folder, groups, items);
    if (groups)
    {
        std::vector<std::string> names;
        for (const auto& group : *groups)
        {
            names.push_back(group->getName());
        }
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return LLStringUtil::compareDict(a, b) < 0; });
        for (const std::string& name : names)
        {
            mGroupCombo->add(name);
        }
    }
    mGroupCombo->selectFirstItem();
}

//-----------------------------------------------------------------------------
// GMDXFloaterWeaponImport
//-----------------------------------------------------------------------------

namespace
{
    LLHandle<LLContextMenu> sVersionMenu;

    std::string version_label(const GMDXWeaponSorter::ImportVersion& v)
    {
        std::string s = v.name;
        if (v.date > 0) s += "   " + LLDate((F64)v.date).toHTTPDateString("%Y-%m-%d");
        if (!v.where.empty()) s += "   (" + v.where + ")";
        return s;
    }
}

GMDXFloaterWeaponImport::GMDXFloaterWeaponImport(const LLSD& key)
:   LLFloater(key)
{
}

bool GMDXFloaterWeaponImport::postBuild()
{
    mPrefix = getChild<LLLineEditor>("prefix_edit");
    mList = getChild<LLScrollListCtrl>("import_list");
    mSummary = getChild<LLTextBox>("summary_text");
    mCatCombo = getChild<LLComboBox>("cat_combo");
    mGroupCombo = getChild<LLComboBox>("group_combo");
    mOthersCombo = getChild<LLComboBox>("others_combo");
    mOthersCombo->selectFirstItem();

    mPrefix->setCommitOnFocusLost(false);
    mPrefix->setCommitCallback([this](LLUICtrl*, const LLSD&) { scan(); });
    getChild<LLButton>("scan_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { scan(); });
    mCatCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshGroups(); });
    mGroupCombo->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (mGroupCombo->getSimple() != getString("new_group")) return;
        const std::string cat = mCatCombo->getSimple();
        LLSD args;
        LLStringUtil::format_map_t fargs;
        fargs["[CAT]"] = cat;
        args["TITLE"] = getString("new_group_title", fargs);
        args["NAME"] = "";
        LLHandle<LLFloater> self = getHandle();
        LLNotificationsUtil::add("GMDXLoadoutName", args, LLSD(), [self](const LLSD& n, const LLSD& r)
        {
            GMDXFloaterWeaponImport* f = static_cast<GMDXFloaterWeaponImport*>(self.get());
            std::string name = r["message"].asString();
            LLStringUtil::trim(name);
            if (!f) return false;
            if (LLNotificationsUtil::getSelectedOption(n, r) != 0 || name.empty())
            {
                f->mGroupCombo->selectFirstItem();
                return false;
            }
            f->mGroupCombo->add(name);
            f->mGroupCombo->setSimple(name);
            return false;
        });
    });
    getChild<LLButton>("set_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetSelected(); });
    getChild<LLButton>("skip_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSkipSelected(); });
    getChild<LLButton>("pick_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPickVersion(); });
    mList->setDoubleClickCallback([this] { onPickVersion(); });
    getChild<LLButton>("sort_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSort(); });
    getChild<LLButton>("close_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    return true;
}

void GMDXFloaterWeaponImport::onOpen(const LLSD& key)
{
    GMDXLoadouts::instance().ensureFolders();
    mPrefix->setText(gSavedPerAccountSettings.getString("GMDXLoadoutWatchTags"));
    mCatCombo->removeall();
    for (const std::string& cat : GMDXLoadouts::instance().categories())
    {
        mCatCombo->add(cat);
    }
    mCatCombo->selectFirstItem();
    refreshGroups();
    scan();
}

void GMDXFloaterWeaponImport::scan()
{
    // The selection follows the weapons (keys), not the row numbers of the old list.
    std::set<std::string> selected;
    for (int i : selectedRows()) selected.insert(mRows[i].key);
    gSavedPerAccountSettings.setString("GMDXLoadoutWatchTags", mPrefix->getText());
    mRows = GMDXWeaponSorter::instance().buildImport(GMDXWeaponSorter::tags());
    mSortPending = false;
    fillList(&selected);
}

void GMDXFloaterWeaponImport::fillList(const std::set<std::string>* keep_selected)
{
    std::set<std::string> selected;
    if (keep_selected) selected = *keep_selected;
    else for (int i : selectedRows()) selected.insert(mRows[i].key);
    const S32 scroll = mList->getScrollPos();
    mList->deleteAllItems();
    size_t items = 0, dupes = 0, to_sort = 0;
    for (size_t i = 0; i < mRows.size(); ++i)
    {
        const GMDXWeaponSorter::ImportRow& r = mRows[i];
        const GMDXWeaponSorter::ImportVersion& keep = r.versions[r.keep];
        items += r.versions.size();
        if (r.versions.size() > 1) ++dupes;
        if (!r.cat.empty()) ++to_sort;
        LLSD row;
        row["value"] = (S32)i;
        row["columns"][0]["column"] = "weapon";
        row["columns"][0]["value"] = keep.name;
        row["columns"][1]["column"] = "versions";
        row["columns"][1]["value"] = llformat("%d", (S32)r.versions.size());
        row["columns"][2]["column"] = "dest";
        row["columns"][2]["value"] = r.cat.empty() ? getString("unsorted") : (r.group.empty() ? r.cat : r.cat + " / " + r.group);
        row["columns"][3]["column"] = "now";
        row["columns"][3]["value"] = keep.sorted ? "Equipment / " + keep.where : keep.where;
        if (!r.cat.empty())
        {
            row["columns"][2]["font"]["style"] = "BOLD";
        }
        LLScrollListItem* item = mList->addElement(row);
        if (item && selected.count(r.key)) item->setSelected(true);
    }
    mList->setScrollPos(scroll);
    LLStringUtil::format_map_t args;
    args["[ROWS]"] = llformat("%d", (S32)mRows.size());
    args["[ITEMS]"] = llformat("%d", (S32)items);
    args["[DUPES]"] = llformat("%d", (S32)dupes);
    mSummary->setText(getString("summary", args));
    LLStringUtil::format_map_t sargs;
    sargs["[COUNT]"] = llformat("%d", (S32)to_sort);
    getChild<LLButton>("sort_btn")->setLabel(getString("sort_label", sargs));
    getChild<LLButton>("sort_btn")->setEnabled(to_sort > 0 && !mSortPending);
}

void GMDXFloaterWeaponImport::refreshGroups()
{
    mGroupCombo->removeall();
    mGroupCombo->add(getString("no_group"));
    const LLUUID cat_folder = GMDXLoadouts::instance().categoryFolder(mCatCombo->getSimple());
    LLInventoryModel::cat_array_t* groups = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(cat_folder, groups, items);
    if (groups)
    {
        std::vector<std::string> names;
        for (const auto& g : *groups) names.push_back(g->getName());
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return LLStringUtil::compareDict(a, b) < 0; });
        for (const std::string& n : names) mGroupCombo->add(n);
    }
    mGroupCombo->add(getString("new_group"));
    mGroupCombo->selectFirstItem();
}

std::vector<int> GMDXFloaterWeaponImport::selectedRows() const
{
    std::vector<int> out;
    for (LLScrollListItem* item : mList->getAllSelected())
    {
        const int i = item->getValue().asInteger();
        if (i >= 0 && i < (int)mRows.size()) out.push_back(i);
    }
    return out;
}

void GMDXFloaterWeaponImport::onSetSelected()
{
    const std::string cat = mCatCombo->getSimple();
    std::string group = mGroupCombo->getCurrentIndex() > 0 ? mGroupCombo->getSimple() : std::string();
    if (group == getString("new_group")) group.clear();
    if (cat.empty()) return;
    for (int i : selectedRows())
    {
        mRows[i].cat = cat;
        mRows[i].group = group;
    }
    fillList();
}

void GMDXFloaterWeaponImport::onSkipSelected()
{
    for (int i : selectedRows())
    {
        mRows[i].cat.clear();
        mRows[i].group.clear();
    }
    fillList();
}

void GMDXFloaterWeaponImport::onPickVersion()
{
    const std::vector<int> sel = selectedRows();
    if (sel.size() != 1) return;
    const int row = sel.front();
    if (LLContextMenu* old = sVersionMenu.get())
    {
        gMenuHolder->removeChild(old);
        delete old;
    }
    LLContextMenu::Params mp;
    mp.name("gmdx_version_menu");
    mp.visible(false);
    LLContextMenu* menu = LLUICtrlFactory::create<LLContextMenu>(mp);
    LLHandle<LLFloater> self = getHandle();
    std::vector<int> order(mRows[row].versions.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    // Newest first (by date: version numbers don't always order, see compareVersions).
    std::stable_sort(order.begin(), order.end(), [&](int a, int b)
    {
        return mRows[row].versions[a].date > mRows[row].versions[b].date;
    });
    for (int i : order)
    {
        LLMenuItemCheckGL::Params p;
        p.name("version");
        p.label(version_label(mRows[row].versions[i]));
        const bool checked = i == mRows[row].keep;
        const std::string key = mRows[row].key;
        p.on_check.function([checked](LLUICtrl*, const LLSD&) { return checked; });
        p.on_click.function([self, key, i](LLUICtrl*, const LLSD&)
        {
            GMDXFloaterWeaponImport* f = static_cast<GMDXFloaterWeaponImport*>(self.get());
            if (!f) return;
            for (auto& r : f->mRows)
            {
                if (r.key == key && i < (int)r.versions.size()) r.keep = i;
            }
            f->fillList();
        });
        menu->addChild(LLUICtrlFactory::create<LLMenuItemCheckGL>(p));
    }
    sVersionMenu = menu->getHandle();
    gMenuHolder->addChild(menu);
    S32 x = 0, y = 0;
    LLUI::getInstance()->getMousePositionScreen(&x, &y);
    menu->show(x, y);
}

void GMDXFloaterWeaponImport::onSort()
{
    const GMDXWeaponSorter::EOthers others = (GMDXWeaponSorter::EOthers)std::clamp(mOthersCombo->getCurrentIndex(), 0, 2);
    size_t extra = 0;
    for (const auto& r : mRows)
    {
        if (r.cat.empty()) continue;
        for (int i = 0; i < (int)r.versions.size(); ++i)
        {
            if (i != r.keep && !r.versions[i].outdated) ++extra;   // (those in Outdated stay)
        }
    }
    LLHandle<LLFloater> self = getHandle();
    auto run = [self, others]()
    {
        GMDXFloaterWeaponImport* f = static_cast<GMDXFloaterWeaponImport*>(self.get());
        if (!f) return;
        GMDXWeaponSorter::instance().applyImport(f->mRows, others);
        f->mSortPending = true;   // no second Sort on the same rows before the rescan
        f->getChild<LLButton>("sort_btn")->setEnabled(false);
        // The moves are local at once; folder creates answer later: look again shortly.
        doAfterInterval([self]
        {
            if (GMDXFloaterWeaponImport* g = static_cast<GMDXFloaterWeaponImport*>(self.get())) g->scan();
        }, 1.5f);
    };
    if (others == GMDXWeaponSorter::OTHERS_TRASH && extra > 0)
    {
        LLSD args;
        args["COUNT"] = llformat("%d", (S32)extra);
        LLNotificationsUtil::add("GMDXImportTrash", args, LLSD(), [run](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0) run();
            return false;
        });
        return;
    }
    run();
}
