/**
 * @file ssgranular.h
 * @brief Atmo Magic: granular transport - lift, creep, deposit and spill on the surface field.
 *
 *        The one place the wind's effect on settled snow is integrated. Runs as a pure stepped
 *        subsystem over one region's field: no singletons, no settings lookups, no back-pointers
 *        - every input arrives in SSGranularParams, assembled once per tick by the caller. That
 *        seam is what keeps the transport deterministic and testable; see
 *        doc/atmo_magic_snow_architecture.md.
 *
 *        Mass ledger rules: lift erodes the cell it leaves, creep debits the cell it leaves and
 *        credits the cell (or eave store) it enters, deposit banks against the same repose room
 *        settle uses, and everything is capped so one step can never move more than exists.
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

#ifndef SS_GRANULAR_H
#define SS_GRANULAR_H

#include "sssurfacefield.h"

#include "v4math.h"

#include <vector>

// Plain aggregate on purpose. The moment this struct reaches out for the world - settings,
// singletons, LLSD - the extraction collapses back into the monolith it exists to avoid.
struct SSGranularParams
{
    F32 mLiftLo = 3.5f;        // m/s at the ground where saltation begins
    F32 mLiftHi = 8.0f;        // m/s where transport saturates
    F32 mLiftRate = 0.f;       // preset: metres of depth per second eroded at full lift
    F32 mDepositRate = 0.f;    // preset: metres of depth per second banking in a lee
    F32 mCreepRate = 0.f;      // preset: creep advection strength
    F32 mDepositGap = 0.7f;    // deposit gate as a fraction of the lift threshold - the
                               // hysteresis keeping a cell from eroding and banking at once
    F32 mLiftTemp = 1.5f;      // degrees C at which lift dies entirely; wet snow does not blow
    F32 mSnowDepth = 0.f;      // preset: the depth ceiling a slope's lie can hold
    F32 mReposeRad = 0.8f;     // preset repose angle, radians
    F32 mGust = 1.f;           // the shared gust envelope, applied once per tick - never per cell
    F32 mTemperatureC = 0.f;   // ambient air temperature this tick - the wet-blow gate's input

    // Grade-vs-structure depth scaling from the capture's height-above-terrain channel: a cell
    // mStructAboveH metres over terrain holds mStructDepth of the ordinary ceiling, fading in
    // from half that height. Deep piles belong at grade; a tower deck still whitens but wind
    // and repose both treat it as the scoured thing it is.
    F32 mStructAboveH = 12.f;
    F32 mStructDepth = 1.f;

    // n*n ground-flow cells for one region (xyz wind, w exposure), sampled from the solved
    // flowmap's bottom slab without the gust layer; caller-owned for the call's duration.
    const LLVector4* mFlow = nullptr;
};

namespace SSGranular
{
    // One fixed-step integration over one region. Settle/melt already ran in the field's own
    // tick; this is what the wind does to what settle left.
    void step(SSSurfaceField::Field& fld, const SSSurfaceField::Geometry& geom,
              const SSGranularParams& p, F32 dt);

    // Landing credit for a runoff clump: repose-capped, overflow discarded (a clump on a full
    // pile barely changes it - see the architecture doc's sharp edges).
    void depositAt(SSSurfaceField::Field& fld, const SSSurfaceField::Geometry& geom,
                   S32 index, F32 depth, F32 ceiling, F32 repose_rad);

    // How much depth the repose rule lets a cell hold - the same lie the settle path computes.
    F32 roomAt(const SSSurfaceField::Geometry& geom, S32 index, F32 ceiling, F32 repose_rad);
}

#endif
