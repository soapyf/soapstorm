/**
 * @file sshazecore.h
 * @brief Atmo Magic: altitude falloff for the windlight haze - the exponential-atmosphere path integral the shared atmospherics shader replicates. Header-only core. CONTRACT.
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

#ifndef SS_HAZECORE_H
#define SS_HAZECORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath> only). Design: doc/atmo_magic_surface_weather.md section 15. The stock windlight haze is HOMOGENEOUS - optical depth is the ray length times density_multiplier and nothing knows about altitude (atmosphericsFuncs.glsl calcAtmosphericVars), so a skybox at 3 km wears exactly the ground's haze and washes out. This core is the one formula that fixes it: the exact path integral through an exponential atmosphere rho(h) = rho0 * exp(-h/H), which is closed form (no marching, about six ALU) and reduces EXACTLY to the stock expression when the falloff is off. Two properties the passes depend on: it is symmetric (the same air sits between two points whichever end you look from, so a sky build is equally hazed seen from above or below), and it multiplies the optical depth only - the in-scatter follows for free because atmosphericsFuncs.glsl:201 builds `additive` from (1 - transmittance). H is NOT max_y: max_y is a calibrated column LENGTH for the sunlight term (llsettingssky.cpp:1590 passes it to a line integral as a distance) and doubles as a ray clamp, and the ratio between it and the sky's actual height varies 2x to 6x between environments - so H is derived from the authored cirrus/dome height instead (SSAtmoEnvCloudDome::mHeightM at phase, the deterministic keyframe, never the live cirrusAltitudeMetres() which reads per-client geometry). Bodies marked STUB are the implementer's; the invariants in the comments are the test author's spec. LOCKSTEP with the SS_ATMO block in class1/windlight/atmosphericsFuncs.glsl; the twin test reads both sources.
#include "lldefs.h"

#include <cmath>

namespace SSHaze
{
    // The recommended share of the cirrus altitude to use as the scale height: at the cirrus itself the air is then
    // exp(-1/THIN_FRAC_DEFAULT) = about an eighth of the ground's, while a 1 km platform under a 6 km cirrus keeps
    // roughly three quarters of it. Authored per environment; 0 disables the whole feature.
    constexpr F32 THIN_FRAC_DEFAULT = 0.5f;

    // Below this the derived scale height is treated as degenerate and the falloff is switched off rather than
    // dividing by something tiny (a 1 m scale height would erase the atmosphere in a single storey).
    constexpr F32 MIN_HEIGHT_M = 1.f;

    // The uniform the shell uploads: 1/H, or 0 when the falloff is off. Taking the inverse CPU-side is what lets the
    // shader stay branchless and makes "off" exactly the stock expression (see pathFactor).
    // Invariants: 0 when frac <= 0, when domeHeightM <= 0, or when domeHeightM * frac < MIN_HEIGHT_M; otherwise
    // exactly 1 / (domeHeightM * frac); monotone non-increasing in domeHeightM and in frac over the enabled range;
    // finite and non-negative for every finite input; pure.
    inline F32 invHeight(F32 domeHeightM, F32 frac)
    {
        if (frac <= 0.f || domeHeightM <= 0.f)
        {
            return 0.f;
        }
        F32 heightM = domeHeightM * frac;
        if (heightM < MIN_HEIGHT_M)
        {
            return 0.f;
        }
        return 1.f / heightM;
    }

    // The factor the stock optical depth is multiplied by, LOCKSTEP the SS_ATMO block in atmosphericsFuncs.glsl.
    // camHeightM is the camera's altitude above the fog base (the owning track's floor); dHeightM is the fragment's
    // altitude MINUS the camera's (positive looking up, negative looking down) - in the shader that is
    // dot(rel_pos, ss_haze_up_view), NOT rel_pos.y alone: rel_pos is a VIEW-space vector (camera-relative but in
    // the CAMERA's own axes), so rel_pos.y only equals world Delta-altitude when the camera happens to be level;
    // ss_haze_up_view is the world-up axis expressed in that same view-space frame. invH is invHeight() above.
    //
    //   factor = exp(-camHeightM * invH) * (1 - exp(-dh)) / dh,  dh = dHeightM * invH
    //
    // with the removable singularity at dh -> 0 taken as 1 (a level view integrates a constant density).
    //
    // Invariants:
    //  - EXACTLY 1.0 when invH == 0 (the falloff is off and the caller's expression is bit-for-bit stock);
    //  - 1.0 when camHeightM == 0 and dHeightM == 0;
    //  - in (0, 1] for camHeightM >= 0 and dHeightM >= 0 (never amplifies haze for an upward view from the base);
    //  - strictly decreasing in camHeightM for fixed dHeightM and invH > 0;
    //  - SYMMETRIC: factor(a, b - a, invH) * |b - a| ... i.e. the optical depth between two altitudes does not
    //    depend on which end the camera is at - concretely factor(hA, hB - hA, invH) == factor(hB, hA - hB, invH)
    //    to within 1e-5 for every hA, hB >= 0;
    //  - continuous through dHeightM == 0 (no step at the removable singularity: the value at dh = 1e-9 and at
    //    dh = -1e-9 are both within 1e-6 of the value at 0);
    //  - finite for every finite input, including large negative dHeightM (looking down from altitude), where the
    //    factor grows above 1 but stays bounded by exp(-camHeightM * invH) * |dh| ^ -1 * exp(|dh|);
    //  - pure; no clamping of the inputs.
    inline F32 pathFactor(F32 camHeightM, F32 dHeightM, F32 invH)
    {
        F32 dh = dHeightM * invH;
        F32 ratio = (std::fabs(dh) < 1e-4f) ? 1.f : (1.f - std::exp(-dh)) / dh;
        return std::exp(-camHeightM * invH) * ratio;
    }

    // Convenience for the tests and the editor's read-out: the share of the ground's haze a view between two
    // altitudes receives, over the same path length. Equals pathFactor(fromM, toM - fromM, invH).
    // Invariants: equals its pathFactor spelling exactly; symmetric in its two altitude arguments to within 1e-5;
    // 1.0 when invH == 0.
    inline F32 shareOfGround(F32 fromM, F32 toM, F32 invH)
    {
        return pathFactor(fromM, toM - fromM, invH);
    }
}

#endif
