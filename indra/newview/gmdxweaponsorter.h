/**
 * @file gmdxweaponsorter.h
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

// <GMDX> WEAPON SORTER. GMDXLoadoutWatchTags (Loadouts > Categories, e.g. "[Ash]") names the tags a
// group puts on its weapons. When an object or folder whose name holds one arrives in inventory outside
// the GunMetal-DX folders, a small window offers to move it into Equipment. When it matches
// a weapon already there once versions and words like "update" are set aside, it offers to replace that
// one: the new version takes the old one's place (and its place in every loadout), and the old one goes
// to Outdated, which nothing reads. "Scan now" offers everything already in inventory.

#pragma once

#include "llfloater.h"
#include "llsingleton.h"
#include "gmdxloadouts.h"

#include <deque>
#include <set>

class LLComboBox;
class LLInventoryObserver;
class LLTextBox;

class GMDXWeaponSorter : public LLSingleton<GMDXWeaponSorter>
{
    LLSINGLETON(GMDXWeaponSorter);
    ~GMDXWeaponSorter();

public:
    // An arrival to sort: an object, or a folder of parts.
    struct Arrival
    {
        LLUUID id;
        std::string name;
        bool folder = false;
        bool hasMatch = false;
        GMDXLoadouts::Entry match;  // the weapon it updates, when hasMatch
    };

    void onAdded(const std::set<LLUUID>& ids);
    void scanInventory();

    bool hasNext() const { return !mQueue.empty(); }
    const Arrival& next() const { return mQueue.front(); }
    size_t queued() const { return mQueue.size(); }

    void replaceOld();                                             // next() replaces its match
    void keepBoth();                                               // next() goes beside its match
    void moveInto(const std::string& cat, const std::string& group);  // group empty: the category itself
    void skip();                                                   // not now

    static std::string normalize(const std::string& name, const std::vector<std::string>& tags);
    // For display: the name without the watched tags and the version, and the version ("v1.25").
    static void splitName(const std::string& name, std::string& title, std::string& version);
    // Where the version sits in name (pos, len) and its digits ("1.2.5").
    static bool findVersion(const std::string& name, size_t& pos, size_t& len, std::string& digits);
    static std::vector<std::string> tags();

    // IMPORTER (Loadouts > Categories > Scan now; GMDXFloaterWeaponImport): every weapon in
    // inventory with a watched tag -- sorted or not -- grouped by weapon (the name without the tag,
    // versions and words like "update"), the version to keep picked: the higher version where the
    // names tell, else the newest; between copies of one version, the one already sorted.
    struct ImportVersion
    {
        LLUUID id;
        std::string name;
        bool folder = false;
        time_t date = 0;
        std::vector<std::string> version;   // "v1.1.06" -> {"1", "1", "06"}; empty: none in the name
        bool sorted = false;        // already a weapon in Equipment (entry)
        bool outdated = false;      // in Outdated (set aside earlier: offered back only beside a live copy)
        GMDXLoadouts::Entry entry;
        std::string where;          // the folder it sits in
        LLUUID parent;              // ... its id at the scan (a move since: the row is stale)
    };
    struct ImportRow
    {
        std::string key;
        std::vector<ImportVersion> versions;
        int keep = 0;
        std::string cat, group;     // where the kept version goes; cat empty: leave this row
    };
    enum EOthers { OTHERS_OUTDATED = 0, OTHERS_TRASH, OTHERS_LEAVE };
    std::vector<ImportRow> buildImport(const std::vector<std::string>& tags);
    // Moves each row's kept version into its category / group (unless it is there already) and the
    // other versions to Outdated or the Trash; loadouts holding a replaced sorted version get the
    // kept one. Rows without a category are left alone.
    void applyImport(const std::vector<ImportRow>& rows, EOthers others);
    static std::vector<std::string> parseVersion(const std::string& name);
    // 1: a is the later version, -1: b is, 0: the same, or the names can't tell (no version, or
    // versions of different shapes: v1.11 vs v1.2, v0.67 vs v0.11.3).
    static int compareVersions(const std::vector<std::string>& a, const std::vector<std::string>& b);
    void clearQueue() { mQueue.clear(); showWindow(); }

private:
    bool makeArrival(LLUUID id, const std::vector<std::string>& tags, Arrival& out, bool outdated_too = false) const;
    void considerAll(const std::vector<LLUUID>& ids);
    void rematch(Arrival& arrival);
    bool stillThere(const Arrival& arrival) const;
    void prepareFront();  // drops gone arrivals, matches the front one against the equipment as it is now
    void requeue(const Arrival& arrival);
    bool isCandidate(const LLUUID& id, const std::vector<std::string>& tags, bool outdated_too = false) const;
    void place(const Arrival& arrival, const LLUUID& dest, bool dest_is_category, std::function<void()> then);
    void showWindow();

    LLInventoryObserver* mObserver = nullptr;
    std::set<LLUUID> mPending;      // added ids waiting for their contents to arrive
    bool mScheduled = false;
    std::deque<Arrival> mQueue;
    std::set<LLUUID> mSeen;         // asked about this session already
    const time_t mSessionStart = time(nullptr);   // (made at login: llstartup.cpp)
};

class LLLineEditor;
class LLScrollListCtrl;

class GMDXFloaterWeaponImport : public LLFloater
{
public:
    GMDXFloaterWeaponImport(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void scan();
    void fillList(const std::set<std::string>* keep_selected = nullptr);
    void refreshGroups();
    void onSetSelected();
    void onSkipSelected();
    void onPickVersion();
    void onSort();
    std::vector<int> selectedRows() const;

    LLLineEditor* mPrefix = nullptr;
    LLScrollListCtrl* mList = nullptr;
    LLTextBox* mSummary = nullptr;
    LLComboBox* mCatCombo = nullptr;
    LLComboBox* mGroupCombo = nullptr;
    LLComboBox* mOthersCombo = nullptr;
    std::vector<GMDXWeaponSorter::ImportRow> mRows;
    bool mSortPending = false;   // Sort ran; locked until the rescan shows what it did
};

class GMDXFloaterWeaponSort : public LLFloater
{
public:
    GMDXFloaterWeaponSort(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void refresh() override;

private:
    void refreshGroups();

    LLTextBox* mItemText = nullptr;
    LLTextBox* mMatchText = nullptr;
    LLTextBox* mCountText = nullptr;
    LLComboBox* mCatCombo = nullptr;
    LLComboBox* mGroupCombo = nullptr;
};
