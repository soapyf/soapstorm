/**
 * @file fssoundemitterblacklist.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fssoundemitterblacklist.h"

#include "lldir.h"
#include "llfile.h"
#include "llsdserialize.h"

LLSD FSSoundEmitterBlacklistData::toLLSD() const
{
    LLSD sd;
    sd["name"]      = name;
    sd["region"]    = region;
    sd["date"]      = date;
    sd["permanent"] = permanent;
    return sd;
}

FSSoundEmitterBlacklistData FSSoundEmitterBlacklistData::fromLLSD(const LLSD& sd)
{
    FSSoundEmitterBlacklistData d;
    d.name      = sd["name"].asString();
    d.region    = sd["region"].asString();
    d.date      = sd["date"].asDate();
    d.permanent = sd["permanent"].asBoolean();
    return d;
}

void FSSoundEmitterBlacklist::init()
{
    mFileName = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "sound_emitter_blacklist.xml");
    load();
}

bool FSSoundEmitterBlacklist::isBlacklisted(const LLUUID& object_id) const
{
    if (mData.empty() || object_id.isNull())
    {
        return false;
    }
    return mData.find(object_id) != mData.end();
}

void FSSoundEmitterBlacklist::addEmitter(const LLUUID& object_id, const std::string& name, const std::string& region, bool permanent)
{
    if (object_id.isNull())
    {
        return;
    }

    FSSoundEmitterBlacklistData entry;
    entry.name      = name;
    entry.region    = region;
    entry.date      = LLDate::now();
    entry.permanent = permanent;

    auto it = mData.find(object_id);
    if (it != mData.end())
    {
        // Keep the original add time; only ever upgrade session -> permanent.
        entry.date      = it->second.date;
        entry.permanent = permanent || it->second.permanent;
        if (entry.name.empty())   entry.name   = it->second.name;
        if (entry.region.empty()) entry.region = it->second.region;
    }

    mData[object_id] = entry;

    if (entry.permanent)
    {
        save();
    }
    mChangedSignal();
}

void FSSoundEmitterBlacklist::removeEmitters(const uuid_vec_t& ids)
{
    bool removed_permanent = false;
    bool removed_any = false;
    for (const LLUUID& id : ids)
    {
        auto it = mData.find(id);
        if (it == mData.end())
        {
            continue;
        }
        removed_permanent |= it->second.permanent;
        mData.erase(it);
        removed_any = true;
    }
    if (removed_permanent)
    {
        save();
    }
    if (removed_any)
    {
        mChangedSignal();
    }
}

void FSSoundEmitterBlacklist::load()
{
    llifstream file(mFileName.c_str());
    if (!file.is_open())
    {
        return;
    }

    LLSD data;
    if (LLSDSerialize::fromXML(data, file) == LLSDParser::PARSE_FAILURE)
    {
        file.close();
        LL_WARNS() << "Failed to parse sound emitter blacklist file." << LL_ENDL;
        return;
    }
    file.close();

    if (!data.isMap())
    {
        return;
    }

    for (LLSD::map_const_iterator it = data.beginMap(); it != data.endMap(); ++it)
    {
        LLUUID id(it->first);
        if (id.isNull())
        {
            continue;
        }
        FSSoundEmitterBlacklistData entry = FSSoundEmitterBlacklistData::fromLLSD(it->second);
        entry.permanent = true; // only permanent entries are ever persisted
        mData[id] = entry;
    }
}

void FSSoundEmitterBlacklist::save()
{
    LLSD data;
    for (const auto& entry : mData)
    {
        if (entry.second.permanent)
        {
            data[entry.first.asString()] = entry.second.toLLSD();
        }
    }

    llofstream file(mFileName.c_str());
    if (!file.is_open())
    {
        LL_WARNS() << "Unable to open sound emitter blacklist file for writing." << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(data, file);
    file.close();
}
