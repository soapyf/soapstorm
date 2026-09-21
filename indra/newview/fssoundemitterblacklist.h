/**
 * @file fssoundemitterblacklist.h
 * @brief Per-emitter sound blacklist (silence a specific noisy object).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, SkoomaStorm
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
 * $/LicenseInfo$
 */

#ifndef FS_SOUNDEMITTERBLACKLIST_H
#define FS_SOUNDEMITTERBLACKLIST_H

#include <map>
#include <boost/signals2.hpp>

#include "llsingleton.h"
#include "lldate.h"
#include "llsd.h"
#include "lluuid.h"

struct FSSoundEmitterBlacklistData
{
    // Owner of the emitter (for a worn attachment, the wearer). Stable across
    // re-rez, unlike the object UUID, so owner+name re-identifies the emitter.
    // Null on entries added before the name/owner were known.
    LLUUID      owner;
    std::string name;
    std::string region;
    LLDate      date;
    bool        permanent{ false };

    LLSD toLLSD() const;
    static FSSoundEmitterBlacklistData fromLLSD(const LLSD& data);
};

using sound_emitter_blacklist_map_t = std::map<LLUUID, FSSoundEmitterBlacklistData>;

// Blocks sounds emitted by specific objects (the emitter), as opposed to the
// per-asset Sound Blacklist. Lets a user silence a noisy scripted attachment
// (e.g. a music gauntlet) without derendering the whole object. Two tiers:
// session-only (lost on logout) and permanent (persisted per account). Keyed on
// the emitter object's UUID; child-prim emitters are matched via the linkset
// root in the sound path (see is_sound_blacklisted).
//
// A worn attachment is issued a new object UUID every time it is re-rezzed or
// detached and re-worn, so a permanent entry also records owner+name, which do
// survive. When an attachment arrives on an avatar that owns a permanent entry,
// LLVOAvatar::attachObject probes it for properties and noteObjectProperties()
// re-points the entry at the new UUID. The probe is gated on hasPermanentFor(),
// so a user with no permanent entries pays one empty-map test per attach.
class FSSoundEmitterBlacklist : public LLSingleton<FSSoundEmitterBlacklist>
{
    LLSINGLETON_EMPTY_CTOR(FSSoundEmitterBlacklist);

public:
    // Loads the persisted permanent list. Call once after login.
    void init();

    bool isBlacklisted(const LLUUID& object_id) const;
    void addEmitter(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name, const std::string& region, bool permanent);
    void removeEmitters(const uuid_vec_t& ids);

    // Remember that we asked the sim for this object's properties so the reply can
    // fill in the owner/name an entry was added without (the avatar path knows the
    // wearer but not which attachment it tagged until the properties come back).
    void noteProbePending(const LLUUID& object_id);

    // True if any permanent entry names this owner, i.e. an attachment of theirs
    // is worth probing for properties. Cheap: fails on an empty map.
    bool hasPermanentFor(const LLUUID& owner_id) const;

    // Fed by every ObjectProperties reply. If owner+name match a permanent entry
    // whose object UUID has since changed, re-point the entry at object_id and
    // return true, meaning the caller should silence it now.
    bool noteObjectProperties(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name);

    // Tear down an object's audio source (and its children's) so the clip playing
    // right now stops, rather than only the next one being suppressed.
    static void silenceObject(const LLUUID& object_id);

    // Nothing blacklisted at all. Lets the sound path skip its object lookups
    // entirely for the common case, as FSAssetBlacklist::isBlacklisted does.
    bool isEmpty() const { return mData.empty(); }

    sound_emitter_blacklist_map_t getData() const { return mData; }

    using changed_signal_t = boost::signals2::signal<void()>;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb)
    {
        return mChangedSignal.connect(cb);
    }

private:
    void load();
    void save();
    // Rebuilds mPermIndex from mData. Called after any change to the permanent
    // tier; the list is user-sized (a handful of entries), so O(n) is fine.
    void reindex();

    // owner -> object name -> the object UUID that entry currently points at.
    // Only permanent entries that know both owner and name appear here.
    using perm_index_t = std::map<LLUUID, std::map<std::string, LLUUID>>;

    std::string                   mFileName;
    sound_emitter_blacklist_map_t mData;
    perm_index_t                  mPermIndex;
    uuid_set_t                    mPendingProbe;
    changed_signal_t              mChangedSignal;
};

#endif // FS_SOUNDEMITTERBLACKLIST_H
