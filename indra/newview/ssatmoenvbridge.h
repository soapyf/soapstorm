/**
 * @file ssatmoenvbridge.h
 * @brief Atmo Magic: v3 environment to v2 weather renderer bridge.
 *
 *        This whole file is scaffolding: once v2 is actually untangled and
 *        SSAtmoTrackConfig is gone, whatever SSAtmoMagic reads from lives
 *        natively in v3's own shape and this translation layer is deleted,
 *        not carried forward. It existing at all, in its own file, is what
 *        makes that deletion a one-file removal later rather than an
 *        archaeology exercise through a renderer that's been fed two
 *        formats for a while.
 *
 *        Spliced into SSAtmoMagic::refreshParams() once the rest of v3's
 *        data/logic layer had actually been exercised against a running
 *        client (create, load, revert, parcel discovery, caching) rather
 *        than only compiled - see refreshParams()'s own comment for
 *        exactly where the v2/v3 fork happens.
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

#ifndef SS_ATMOENVLEGACYBRIDGE_H
#define SS_ATMOENVLEGACYBRIDGE_H

#include "llpreprocessor.h"

#include <string>

struct SSAtmoTrackConfig;

class SSAtmoEnvBridge
{
public:
    static bool resolveActiveTrack(F32 world_z, F32 prev_world_z, bool teleported,
                                   SSAtmoTrackConfig& out_cfg, bool& out_is_ground_track);

    // <SS:Nexii> Public because the environment editor derives a new precipitation type from whatever the combo has selected, and the combo holds derivation vocabulary rather than preset names - it needs the same mapping the renderer uses, not a second copy of it.
    static std::string presetNameForType(const std::string& v3_type);

    // <SS:Nexii> The bolt-from-the-blue look-ahead: with the weather cube's keyframes as a forecast, reports how imminent an oncoming thunderstorm is (0..1) and the heading, degrees, it is coming FROM - the upwind of the current wind. The convection field's NEXT keyframe is the storm's arrival: stormier than now, gated by how far the day phase has progressed toward it. out_heading_deg is -1 when no storm is approaching.
    static F32 stormApproach(F32 world_z, F32 prev_world_z, bool teleported,
                             F32& out_heading_deg);
};

#endif
