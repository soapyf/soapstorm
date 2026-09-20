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
class FSSoundEmitterBlacklist : public LLSingleton<FSSoundEmitterBlacklist>
{
    LLSINGLETON_EMPTY_CTOR(FSSoundEmitterBlacklist);

public:
    // Loads the persisted permanent list. Call once after login.
    void init();

    bool isBlacklisted(const LLUUID& object_id) const;
    void addEmitter(const LLUUID& object_id, const std::string& name, const std::string& region, bool permanent);
    void removeEmitters(const uuid_vec_t& ids);

    sound_emitter_blacklist_map_t getData() const { return mData; }

    using changed_signal_t = boost::signals2::signal<void()>;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb)
    {
        return mChangedSignal.connect(cb);
    }

private:
    void load();
    void save();

    std::string                   mFileName;
    sound_emitter_blacklist_map_t mData;
    changed_signal_t              mChangedSignal;
};

#endif // FS_SOUNDEMITTERBLACKLIST_H
