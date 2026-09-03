/**
 * @file ssfloatersim.h
 * @brief Atmo Magic: simulation settings floater.
 *
 *        Both maps are built once and then left alone, so the tuning here has
 *        no effect until something asks for a rebuild. Every control that
 *        changes what a solve produces triggers one on commit.
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

#ifndef SS_FLOATERSIM_H
#define SS_FLOATERSIM_H

#include "llfloater.h"

#include <boost/signals2.hpp>
#include <vector>

class SSFloaterSimulation : public LLFloater
{
public:
    SSFloaterSimulation(const LLSD& key);

    bool postBuild() override;

private:
    void onClickRecaptureShadow();
    void onClickRebuildFlow();

    enum class EInvalidate { SHADOW, FLOW };

    void watch(const std::string& control, EInvalidate what);

    std::vector<boost::signals2::scoped_connection> mConnections;
};

#endif
