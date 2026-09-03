/**
 * @file ssassetlist.h
 * @brief Atmo Magic: generic ordered asset list.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

#ifndef SS_ASSETLIST_H
#define SS_ASSETLIST_H

#include "lluuid.h"

#include <string>
#include <vector>

typedef std::vector<LLUUID> SSAssetList;

enum SSAssetListMode
{
    SS_ASSET_SEQUENCE = 0,
    SS_ASSET_RANDOM
};

const char* ss_asset_mode_key(SSAssetListMode mode);
SSAssetListMode ss_asset_mode_from_key(const std::string& key);

SSAssetList ss_asset_list_parse(const std::string& csv);
std::string ss_asset_list_str(const SSAssetList& list);

std::string ss_asset_name(const LLUUID& id);

bool ss_natural_less(const std::string& a, const std::string& b);

#endif
