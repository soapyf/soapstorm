/**
 * @file fsfloatersoundemitterblacklist.cpp
 * @brief Floater listing blacklisted sound emitters.
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

#include "fsfloatersoundemitterblacklist.h"

#include "fssoundemitterblacklist.h"
#include "llbutton.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"

FSFloaterSoundEmitterBlacklist::FSFloaterSoundEmitterBlacklist(const LLSD& key)
:   LLFloater(key)
{
}

FSFloaterSoundEmitterBlacklist::~FSFloaterSoundEmitterBlacklist()
{
    if (mChangedConnection.connected())
    {
        mChangedConnection.disconnect();
    }
}

bool FSFloaterSoundEmitterBlacklist::postBuild()
{
    mList = getChild<LLScrollListCtrl>("result_list");

    getChild<LLButton>("remove_btn")->setCommitCallback(boost::bind(&FSFloaterSoundEmitterBlacklist::onRemoveSelected, this));
    getChild<LLButton>("clear_session_btn")->setCommitCallback(boost::bind(&FSFloaterSoundEmitterBlacklist::onClearSession, this));
    getChild<LLButton>("close_btn")->setCommitCallback(boost::bind(&LLFloater::closeFloater, this, false));

    mChangedConnection = FSSoundEmitterBlacklist::instance().setChangedCallback(
        boost::bind(&FSFloaterSoundEmitterBlacklist::buildList, this));

    // onOpen() builds the list on every open (matching FSFloaterAssetBlacklist).
    return true;
}

void FSFloaterSoundEmitterBlacklist::onOpen(const LLSD& key)
{
    buildList();
}

void FSFloaterSoundEmitterBlacklist::buildList()
{
    if (!mList)
    {
        return;
    }

    mList->deleteAllItems();

    const sound_emitter_blacklist_map_t data = FSSoundEmitterBlacklist::instance().getData();
    for (const auto& pair : data)
    {
        const LLUUID& id = pair.first;
        const FSSoundEmitterBlacklistData& entry = pair.second;

        LLSD row;
        row["id"] = id;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"]  = entry.name.empty() ? id.asString() : entry.name;
        row["columns"][1]["column"] = "region";
        row["columns"][1]["value"]  = entry.region;
        row["columns"][2]["column"] = "type";
        row["columns"][2]["value"]  = entry.permanent ? getString("type_permanent") : getString("type_session");
        row["columns"][3]["column"] = "date";
        row["columns"][3]["value"]  = entry.date.asString();
        mList->addElement(row);
    }
}

void FSFloaterSoundEmitterBlacklist::onRemoveSelected()
{
    if (!mList)
    {
        return;
    }

    uuid_vec_t ids;
    std::vector<LLScrollListItem*> selected = mList->getAllSelected();
    for (LLScrollListItem* item : selected)
    {
        if (item)
        {
            ids.push_back(item->getUUID());
        }
    }

    if (!ids.empty())
    {
        FSSoundEmitterBlacklist::instance().removeEmitters(ids);
    }
}

void FSFloaterSoundEmitterBlacklist::onClearSession()
{
    const sound_emitter_blacklist_map_t data = FSSoundEmitterBlacklist::instance().getData();
    uuid_vec_t ids;
    for (const auto& pair : data)
    {
        if (!pair.second.permanent)
        {
            ids.push_back(pair.first);
        }
    }

    if (!ids.empty())
    {
        FSSoundEmitterBlacklist::instance().removeEmitters(ids);
    }
}
