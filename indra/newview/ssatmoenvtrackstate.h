/**
 * @file ssatmoenvtrackstate.h
 * @brief Atmo Magic: altitude track resolver.
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

#ifndef SS_ATMOENVTRACKSTATE_H
#define SS_ATMOENVTRACKSTATE_H

#include "ssatmoenvasset.h"

struct SSAtmoEnvTrackBlend
{
    S32 mPrimaryTrack   = 0;
    S32 mNeighborTrack  = -1;
    F32 mNeighborWeight = 0.f;

    bool mInstantCut = false;
};

class SSAtmoEnvTrackResolver
{
public:
    static SSAtmoEnvTrackBlend resolve(const SSAtmoEnvAsset& asset, F32 world_z,
                                       F32 prev_world_z, bool teleported);

private:
    static S32 trackContaining(const SSAtmoEnvAsset& asset, F32 world_z);

    static bool nearestBoundary(const SSAtmoEnvAsset& asset, S32 primary, F32 world_z,
                                S32& out_neighbor, F32& out_distance);
};

#endif
