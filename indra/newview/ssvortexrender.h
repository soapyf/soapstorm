/**
 * @file ssvortexrender.h
 * @brief Atmo Magic: the vortex funnel renderer - collar-quad funnels plus a debris skirt.
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

#ifndef SS_VORTEXRENDER_H
#define SS_VORTEXRENDER_H

#include "llpointer.h"
#include "llsingleton.h"
#include "llviewerpartsource.h"
#include "ssvortexcore.h"
#include "ssvortices.h"

#include <map>
#include <vector>

// <SS:Nexii> The RENDERER half of doc/atmo_magic_storm_dynamics.md section 4 - see ssvortexcore.h's own file-top
// comment for the invariants this must hold (subdivide before squashScale, real alpha in the sky forward pass,
// never mPuffs/the puff budget, own MAX_ACTIVE * COLLARS cap). Reads SSVortices::active() fresh every frame - no
// PLACEMENT state of its own beyond the debris skirt's particle-source bookkeeping (a live LLViewerPartSource is
// not re-derivable from scratch each frame the way the funnel geometry is, so it is the one thing here that
// persists across frames; its SPAWN POSITIONS are still a pure function of (vortex id, spawn index) - see
// ssvortexrender.cpp's SSVortexDebrisSource). mDebrisKeepScratch/mDrawScratch are pure per-frame SCRATCH (cleared
// and refilled at the top of every render(), never read across frames) - members only so the std::map/std::vector
// backing storage is reused instead of heap-churned every frame (review finding 12), not additional simulation
// state.
class SSVortexRender : public LLSingleton<SSVortexRender>
{
    LLSINGLETON(SSVortexRender);
    // Retires every debris-skirt source still live at shutdown (SSVortexDebrisSource::setDead - see
    // ssvortexrender.cpp). Out-of-line so the definition sits beside that class, not because the type here is
    // incomplete (llviewerpartsource.h is included above).
    ~SSVortexRender();

public:
    // Draws every live funnel (SSVortices::active(), mHasFunnel) as a stack of Z-billboarded collar quads, right
    // after SSVolCloud::render() in the same sky forward pass - see the call site in pipeline.cpp. Also owns the
    // debris skirt's particle sources (spawned/retired here, ticked independently by LLViewerPartSim thereafter).
    void render();

private:
    // One dust-devil-skirt particle source per live, touched-down funnel, keyed on SSVortex::Candidate::mId so a
    // vortex that survives several frames keeps the SAME source (and so the same hashed spawn sequence) rather than
    // restarting it - see SSVortexDebrisSource's own comment for what "deterministic" means for a particle sim.
    std::map<U64, LLPointer<LLViewerPartSource>> mDebrisSources;

    // Per-frame scratch reused across calls (review finding 12): the "keep" side of mDebrisSources' swap-and-retire
    // pass, and the ranked list of funnel-having vortices about to be drawn. Cleared at the top of each use, never
    // read from a prior frame.
    std::map<U64, LLPointer<LLViewerPartSource>> mDebrisKeepScratch;
    struct DrawVortex
    {
        const SSVortices::LiveVortex* mVortex = nullptr;
        S32 mSlot = 0;
    };
    std::vector<DrawVortex> mDrawScratch;
};

#endif
