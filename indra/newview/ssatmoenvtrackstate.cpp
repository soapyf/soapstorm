/**
 * @file ssatmoenvtrackstate.cpp
 * @brief See ssatmoenvtrackstate.h.
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

#include "llviewerprecompiledheaders.h"

#include "ssatmoenvtrackstate.h"

#include <cmath>

// Which track's altitude band contains world_z; track 0 is the implicit ground fallback. A band whose ceiling is the region ceiling is open ended: alt-cam can put the camera past 4096m, and the sky up there is still the topmost track's, not a fall back to the ground.
S32 SSAtmoEnvTrackResolver::trackContaining(const SSAtmoEnvAsset& asset, F32 world_z)
{
    for (size_t i = 1; i < asset.mTracks.size(); ++i)
    {
        const SSAtmoEnvTrack& t = asset.mTracks[i];
        const F32 ceiling = asset.trackCeilingZ((S32)i);
        if (world_z >= t.mFloorZ &&
            (world_z < ceiling || ceiling >= SS_ATMOENV_REGION_CEILING)) return (S32)i;
    }
    return 0;
}

// Closest other track floor, for transition-buffer blending near a boundary.
bool SSAtmoEnvTrackResolver::nearestBoundary(const SSAtmoEnvAsset& asset, S32 primary, F32 world_z,
                                             S32& out_neighbor, F32& out_distance)
{
    bool found = false;
    F32 best = FLT_MAX;
    S32 best_idx = -1;

    for (size_t i = 0; i < asset.mTracks.size(); ++i)
    {
        if ((S32)i == primary) continue;
        const SSAtmoEnvTrack& t = asset.mTracks[i];

        const F32 floor_dist = llabs(world_z - t.mFloorZ);
        if (floor_dist < best)
        {
            best = floor_dist;
            best_idx = (S32)i;
            found = true;
        }

    }

    if (found)
    {
        out_neighbor = best_idx;
        out_distance = best;
    }
    return found;
}

// Primary track plus neighbour blend for an altitude; teleports and water crossings cut instantly instead of smearing a transition.
SSAtmoEnvTrackBlend SSAtmoEnvTrackResolver::resolve(const SSAtmoEnvAsset& asset, F32 world_z,
                                                   F32 prev_world_z, bool teleported)
{
    SSAtmoEnvTrackBlend result;
    result.mPrimaryTrack = trackContaining(asset, world_z);

    F32 water_height = 0.f;
    const bool has_water = asset.visibleWaterHeight(water_height);
    const bool crossed_water = has_water &&
        ((prev_world_z >= water_height) != (world_z >= water_height));

    if (teleported || crossed_water)
    {
        result.mInstantCut = true;
        return result;
    }

    S32 neighbor = -1;
    F32 distance = 0.f;
    const SSAtmoEnvTrack& primary_track = asset.mTracks[result.mPrimaryTrack];
    if (primary_track.mTransitionBuffer > 0.f &&
        nearestBoundary(asset, result.mPrimaryTrack, world_z, neighbor, distance) &&
        distance < primary_track.mTransitionBuffer)
    {
        result.mNeighborTrack = neighbor;
        result.mNeighborWeight = 1.f - (distance / primary_track.mTransitionBuffer);
    }

    return result;
}
