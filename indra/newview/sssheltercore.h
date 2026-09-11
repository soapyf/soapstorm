/**
 * @file sssheltercore.h
 * @brief Atmo Magic shelter: the ONE predicate that decides whether something is
 *        under cover, and the surface field's broad-cover verdict.
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

// <SS:Nexii> Atmo Magic shelter predicate

#ifndef SS_SHELTER_CORE_H
#define SS_SHELTER_CORE_H

#include "lldefs.h"
#include <cmath>

// THREE CONSUMERS, ONE QUESTION: "is this point under cover?"
//
// The precipitation sim has asked it since the beginning (ssprecipitation.cpp, per
// particle). The lens pass gained it this session (ssscreenfx.cpp, per frame, on the
// camera's column) and spelled the threshold a second time. The surface field does not
// ask it at all. Two spellings of a gate is how PLAN.md lesson 9 happens, so the
// threshold and the predicate live here and every consumer calls them.
//
// The authority in all three cases is SSRainShadowMap::resolveColumn(pos), which
// answers "where would a drop falling through pos land". If it lands ABOVE pos by more
// than the tolerance, something is in the way.
//
// WHY THE SURFACE FIELD IS NOT ONE OF THE THREE, stated so nobody adds an inert gate:
// its cells come from SSRainShadowMap::buildSurfaceGrid, which reads the shadow map's
// own depth render taken from above along the fall direction (ssrainshadow.cpp:1606).
// Every cell in that grid is therefore, BY CONSTRUCTION, the topmost surface of its
// column and fully exposed - a drop falling through it lands on it. A resolveColumn
// gate over those cells would return "open" for all of them. The surface that is
// actually sheltered - a floor under a roof - has no cell in the field at all; the
// field stores one height per column and that height IS the roof. So shelter for the
// surface pass is entirely a question about fragments BELOW the stored top, which is
// ssFieldAt's job, and the broad-cover verdict at the bottom of this file is it.

namespace SSShelter
{
    // Something this far above a point counts as cover rather than as the point's own
    // surface. LOCKSTEP with ssprecipitation.cpp COVER_TOLERANCE and
    // ssscreenfxcore.h LENS_COVER_TOL_M, both of which spell 2 m today.
    const F32 COVER_TOL_M = 2.f;

    // The predicate. resolved is what SSRainShadowMap::resolveColumn returned (false
    // means the column is off the map, which is not evidence of cover); hit_z is the
    // landing height it wrote; pos_z the point being asked about.
    inline bool underCover(bool resolved, F32 hit_z, F32 pos_z, F32 tol_m = COVER_TOL_M)
    {
        return resolved && (hit_z - pos_z) > tol_m;
    }

    // The same question as a 0..1 exposure with a soft edge, for consumers that want to
    // fade rather than switch. 1 in the open, 0 under cover, ramping over one tolerance
    // so a point drifting under a lip does not pop.
    inline F32 coverExposure(bool resolved, F32 hit_z, F32 pos_z, F32 tol_m = COVER_TOL_M)
    {
        if (!resolved) return 1.f;
        const F32 above = hit_z - pos_z;
        if (above <= tol_m) return 1.f;
        const F32 t = llclamp((above - tol_m) / llmax(tol_m, 1.0e-4f), 0.f, 1.f);
        return 1.f - t * t * (3.f - 2.f * t);
    }

    // ------------------------------------------------------------------
    // THE SURFACE FIELD'S BROAD-COVER VERDICT.
    //
    // ssFieldAt marched five rays back up the fall direction to decide exposure for a
    // fragment below its column's stored top. For an UP-FACING fragment that march was
    // structurally blind: it starts cell*0.75 out along the surface normal (which on a
    // floor is straight up, buying nothing) and steps cell*1.25, so its first sample
    // sits at
    //
    //     cell * (0.75 + 1.25) + cell * 0.35  =  2.35 cells
    //
    // above the fragment, and the test is "is the stored top above THIS sample". Any
    // ceiling lower than 2.35 cells is stepped straight over. At the shipping cell of
    // 2 m (a 256 m region at GEOM_RES 128) that is 4.7 m - taller than every ordinary
    // room, which is why an indoor floor read as open sky.
    //
    // The verdict that replaces it needs no march: reaching the branch at all means the
    // stored top is more than half a cell above the fragment, and the stored top is
    // where a drop falling through this column lands. It is only weighted, not marched.
    //
    // RESOLUTION, stated plainly: the window holds ONE height per column, so a thin
    // wall's ridge and a roof are the same texel value. The verdict therefore takes the
    // stored top as the MINIMUM over the fragment's column and its four neighbours -
    // cover has to be BROAD to shelter an up-facing surface. At a 2 m cell that means
    // cover at least three cells (6 m) across; a narrower lip reads as open, which is
    // what it did before, so the error is toward wet. Genuinely resolving a 2 m eave
    // would need the grid at ~0.5 m (GEOM_RES 512, sixteen times the cells) or a
    // per-fragment ray against real geometry, and neither is a threshold tweak.
    // ------------------------------------------------------------------

    // How much an up-facing fragment's own column shelters it: 0 while the stored top is
    // within half a cell - which is exactly ssFieldAt's own on_top tolerance, the kerb and
    // road-crown allowance its comments describe - and 1 by a whole cell above it. The
    // ramp closes at ONE cell rather than one and a half so that an ordinary ceiling is
    // fully covering: at the shipping 2 m cell it completes at 2.0 m, and the lowest
    // habitable ceiling measured against it is 2.4 m. Spelled to 1.5 cells it left a
    // 2.4 m room at 0.22 exposure, which is still visibly wet.
    inline F32 columnCover(F32 cover_top_z, F32 frag_z, F32 cell_m)
    {
        const F32 above = cover_top_z - frag_z;
        const F32 e0 = cell_m * 0.5f;
        const F32 e1 = cell_m * 1.0f;
        const F32 t = llclamp((above - e0) / llmax(e1 - e0, 1.0e-6f), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // The factor ssFieldAt multiplies its marched exposure by. Weighted by how up-facing
    // the fragment is: total for a floor (nothing gets rain under a roof), NOTHING for a
    // vertical wall - a wall under an eave still catches driven rain from the side, and
    // the cone march is what answers for it. At up_z == 0 this returns exactly 1, so the
    // wall path is bit-identical to what it was before this term existed.
    inline F32 broadCoverFactor(F32 cover_top_z, F32 frag_z, F32 cell_m, F32 up_z)
    {
        const F32 upness = llclamp(up_z, 0.f, 1.f);
        return 1.f - columnCover(cover_top_z, frag_z, cell_m) * upness;
    }

    // The first sample height the old five-ray march could test, above the fragment, on
    // an up-facing surface. Its only purpose is to be measured against a room height.
    inline F32 marchBlindHeightM(F32 cell_m)
    {
        return cell_m * (0.75f + 1.25f + 0.35f);
    }
}

#endif
