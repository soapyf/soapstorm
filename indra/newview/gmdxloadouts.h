/**
 * @file gmdxloadouts.h
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

// <GMDX> LOADOUTS (Combat > Loadouts..., L; quick swap Alt+1..4). Everything lives in the account's
// inventory, so it follows the account:
//   Equipment/<category>/[<group>/]<weapon>
//     A category is a slot (default Layer 1, Layer 2, Layer 3, Utility). A weapon is an object right
//     in the category or in a group folder, or a folder inside a group whose objects are its parts
//     (gun + holster). Links work as well as the objects themselves.
//   Loadouts/<loadout>/  links to the parts of at most one weapon per category.
//   Outdated/  old versions the sorter / importer replaced (left out of inventory search).
// The three are known by id (per-account GMDXFolder* settings), shown like system folders: own
// icons, sorted to the top, not deletable or renamable (llinventorybridge.cpp's <GMDX> hooks).
// Folders made under their first names by early GunMetal-DX builds ("GunMetal-DX Equipment" ...) are
// renamed on first use.
// Equipping touches only the categories: what is worn from them comes off, the loadout's weapons go on,
// everything else stays. Swapping one weapon never edits a loadout until the user saves it. The only
// guard is that swaps never overlap: one asked for during a swap waits for it. The category list and
// its order are a per-account setting (GMDXLoadoutCategories); its folders are in inventory.

#pragma once

#include "llhandle.h"
#include "llsingleton.h"
#include "lluuid.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class LLContextMenu;
class LLInventoryObserver;

class GMDXLoadouts : public LLSingleton<GMDXLoadouts>
{
    LLSINGLETON(GMDXLoadouts);
    ~GMDXLoadouts();

public:
    struct Weapon
    {
        std::string name;
        std::string group;  // empty when the weapon sits right in its category
        LLUUID id;          // the object, or the weapon's folder
        uuid_vec_t parts;   // inventory ids to attach (links resolved)
    };
    typedef std::map<std::string, Weapon> slots_t;  // category -> weapon

    struct Loadout
    {
        LLUUID id;
        std::string name;
        slots_t slots;
        std::vector<std::string> missing;  // links whose item is gone
    };

    // Moves on every inventory or attachment change; windows refresh when it does.
    U64 generation() const;
    void bump() { ++mGeneration; }

    // Creates the two folders and the category folders when they are missing.
    void ensureFolders();

    std::vector<std::string> categories() const;
    void setCategories(const std::vector<std::string>& cats);
    void addCategory(const std::string& name);
    void renameCategory(const std::string& from, const std::string& to);

    std::vector<Weapon> weapons(const std::string& cat);
    slots_t worn();
    std::vector<Loadout> loadouts();
    bool findLoadout(const LLUUID& id, Loadout& out);

    LLUUID wornLoadout() const;
    // Worn weapons that differ from the worn loadout, by category.
    std::vector<std::string> changedCategories();

    // Swaps (queued while another runs).
    void equip(const LLUUID& loadout_id);
    void swap(const std::string& cat, const Weapon* weapon);  // null takes the category off
    void cancelQueued();

    // Loadout editing.
    void createLoadout(const std::string& name, const slots_t& slots, bool mark_worn);  // mark_worn: it is what you wear
    void setLoadoutSlot(const LLUUID& loadout_id, const std::string& cat, const Weapon* weapon);
    void saveWornInto(const LLUUID& loadout_id);
    void renameLoadout(const LLUUID& loadout_id, const std::string& name);
    void deleteLoadout(const LLUUID& loadout_id);
    // Sharing: a loadout as a notecard ("Loadout - <name>" in Notecards; weapon names per layer), and a
    // dropped notecard read back as a loadout of the reader's own weapons (matched by name, any version).
    void shareLoadout(const LLUUID& loadout_id);
    void importNotecard(const LLUUID& notecard_item);
    void importText(const std::string& text);

    // Quick swap: a menu of the category's weapons beside the crosshair (frees the mouse in mouselook).
    void showQuickMenu(S32 slot);
    // Its keys, one per slot (Combat > Quick Swap's items; GMDXQuickSwapKeys, default Alt+1..4).
    static const S32 HOTKEY_SLOTS = 4;
    static void hotkey(S32 slot, KEY& key, MASK& mask);
    static std::string hotkeyLabel(S32 slot);   // "Alt+1", or empty when unbound
    // false: another menu already uses the key (nothing changed). A key another slot had moves here.
    static bool setHotkey(S32 slot, KEY key, MASK mask);
    static void applyHotkeys(bool clear = false);   // clear: none on the items (while a key is being picked)
    // The weapon chooser used by the Loadouts tab, at a screen point.
    void showChooser(const std::string& cat, const LLUUID& current, S32 x, S32 y,
                     std::function<void(const Weapon*)> on_pick, bool quick);

    // The viewer's own folders.
    enum EFolder { EQUIPMENT = 0, LOADOUTS, OUTDATED, FOLDER_COUNT };
    static const char* folderName(EFolder f);
    // The folder's id, or null while it does not exist (found by its saved id, else by name).
    LLUUID folder(EFolder f) const;
    // Calls back with the folder's id, creating it when it is missing (null: the create failed).
    void withFolder(EFolder f, std::function<void(const LLUUID&)> cb);
    // 1 + EFolder when id is one of the three, else 0 (inventory icons, sort order, protection).
    static int folderKind(const LLUUID& id);
    // Outdated: old versions the sorter replaced. Nothing reads it, and inventory search leaves it
    // out unless "Search Outdated" is ticked.
    static bool isInOutdated(const LLUUID& id);

    // Weapon parts. A weapon folder's objects are worn together, except those in its "Not worn"
    // subfolder (a sling, a HUD, a box): they stay with the weapon and are never attached. Kept in
    // inventory, so the choice follows the account.
    static const char* notWornFolderName();
    // The group folder the sorter makes for weapons that are folders ("Other"): storage, not a
    // grouping the user chose, so menus and lists show no heading for it.
    static const char* folderGroupName() { return "Other"; }
    static std::string shownGroup(const Weapon& w) { return w.group == folderGroupName() ? std::string() : w.group; }
    struct Part
    {
        LLUUID id;          // the entry in the weapon folder (maybe a link)
        std::string name;
        bool worn = true;
    };
    std::vector<Part> parts(const LLUUID& weapon_folder) const;   // by name
    void setPartWorn(const LLUUID& weapon_folder, const LLUUID& part, bool worn);
    // A new version took the old one's place: its parts named like the old one's unworn ones go unworn.
    void carryNotWorn(const LLUUID& old_folder, const LLUUID& new_folder);
    // To Outdated: taken off, and out of every loadout.
    void removeWeapon(const Weapon& weapon);
    // A part (object id) another weapon in Equipment also has.
    bool sharedWithOther(const LLUUID& object, const LLUUID& weapon_id);

    // Inventory helpers. withSubfolder finds parent/name or creates it, then calls back with its id
    // (null: the create failed); callers asking while it is being created wait for that one create.
    static void moveTo(const LLUUID& id, const LLUUID& dest);
    static void withSubfolder(const LLUUID& parent, const std::string& name, std::function<void(const LLUUID&)> cb);

    // For the weapon sorter (gmdxweaponsorter.cpp).
    struct Entry
    {
        std::string cat;
        Weapon weapon;
    };
    std::vector<Entry> allWeapons();
    LLUUID findFolder(const LLUUID& parent, const std::string& name) const;
    LLUUID equipmentFolder() const { return folder(EQUIPMENT); }
    LLUUID loadoutsFolder() const { return folder(LOADOUTS); }
    LLUUID categoryFolder(const std::string& cat) const;
    // Loadouts that hold any of old_parts get the weapon now instead.
    void replaceInLoadouts(const uuid_vec_t& old_parts, const std::string& cat, const Weapon& now);

    // Swap progress for the toast.
    struct Progress
    {
        bool busy = false;
        std::string label;           // "Long Range" or "Battle Rifle"
        std::string cat;             // a single swap's category; empty for a loadout
        bool takeOff = false;        // a single swap that only takes the category off
        std::string step;            // "Layer 3: Combat Knife"
        S32 done = 0, total = 0;
        std::string queued;          // label of the swap waiting, if any
        bool finished = false;       // a swap has ended (the toast shows its result)
        std::vector<std::string> missing;  // items that are gone or never attached
        F64 finishedAt = 0.0;
    };
    const Progress& progress() const { return mProgress; }

private:
    struct Index
    {
        std::map<std::string, std::vector<Weapon>> weapons;            // by category
        std::multimap<LLUUID, std::pair<std::string, LLUUID>> owner;   // part id -> (category, weapon id)
        std::set<LLUUID> spare;   // parts in a weapon's Not worn folder (a loadout link to one is fine)
    };
    std::vector<LLUUID> removeFromLoadouts(const uuid_vec_t& parts);   // the loadouts that held any
    void buildIndex(Index& idx);
    bool looksLike(EFolder f, const LLUUID& id) const;   // a plain-named folder is really this one
    slots_t resolve(const Index& idx, const std::set<LLUUID>& ids) const;
    slots_t wornFrom(const Index& idx);
    void readLoadout(const Index& idx, const LLUUID& folder, Loadout& out);
    void linkSlots(const LLUUID& folder, const slots_t& slots);

    struct Job
    {
        std::string label;
        std::function<bool()> run;  // false: nothing to swap after all
    };
    void queue(Job job);
    void beginSwap(const std::string& label, const std::map<std::string, Weapon>& wanted,
                   const std::vector<std::string>& missing, const LLUUID& loadout_id);
    bool tick();

    void onQuickMenuClosed();

    LLInventoryObserver* mObserver = nullptr;
    U32 mGeneration = 1;
    S32 mPendingCreates = 0;
    std::vector<std::function<void(const LLUUID&)>> mFolderWaiters[FOLDER_COUNT];   // withFolder
    bool mCreateFailed = false;
    bool mFetched = false;

    Progress mProgress;
    Job mQueued;  // the newest request made during a swap; it replaces an older one
    std::vector<LLUUID> mAttach;  // in step order
    std::set<LLUUID> mDetach;
    std::vector<std::pair<std::string, std::string>> mSteps;  // (category, weapon name) per attach, in order
    std::vector<std::string> mMissing;
    F64 mStarted = 0.0;
    bool mTicking = false;
    bool mFreedMouse = false;
    LLHandle<LLContextMenu> mMenu;
};
