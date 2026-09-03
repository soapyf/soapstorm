/**
 * @file ssfloaterworldfield.h
 * @brief Atmo Magic: the shared world field settings floater.
 *
 *        The field is the one region-anchored band-sliced capture behind the
 *        surface, coverage, air and drainage channels, so its tuning has the
 *        widest blast radius of any dial in the suite: cell size decides the
 *        horizontal resolution of every channel, band height the vertical one,
 *        and the ceiling where the stack stops. A change here drops the cached
 *        tiles and they rebuild at the new settings, exactly like the rain
 *        shadow and flowmap floaters - it just invalidates one store instead
 *        of one map.
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

#ifndef SS_FLOATERWORLDFIELD_H
#define SS_FLOATERWORLDFIELD_H

#include "llfloater.h"

#include <boost/signals2.hpp>
#include <vector>

class SSFloaterWorldField : public LLFloater
{
public:
    SSFloaterWorldField(const LLSD& key);

    bool postBuild() override;

private:
    // Drops the cached field tiles on a tuning control's commit so
    // they rebuild under the new setting.
    void watch(const std::string& control);

    std::vector<boost::signals2::scoped_connection> mConnections;
};

#endif
