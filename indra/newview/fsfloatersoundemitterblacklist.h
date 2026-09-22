/**
 * @file fsfloatersoundemitterblacklist.h
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

#ifndef FS_FLOATERSOUNDEMITTERBLACKLIST_H
#define FS_FLOATERSOUNDEMITTERBLACKLIST_H

#include <boost/signals2.hpp>

#include "llfloater.h"

class LLScrollListCtrl;

class FSFloaterSoundEmitterBlacklist : public LLFloater
{
public:
    FSFloaterSoundEmitterBlacklist(const LLSD& key);
    virtual ~FSFloaterSoundEmitterBlacklist();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void buildList();
    void onRemoveSelected();
    void onClearSession();

    LLScrollListCtrl*           mList{ nullptr };
    boost::signals2::connection mChangedConnection;
};

#endif // FS_FLOATERSOUNDEMITTERBLACKLIST_H
