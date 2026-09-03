/**
 * @file sswater.cpp
 * @brief Atmo Magic: the SSWater plane family and the SSWaterWorld swap.
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

#include "sswater.h"

#include "llagent.h"
#include "llsurface.h"
#include "llviewercamera.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"
#include "ssatmoenvapplier.h"
#include "ssatmoenvasset.h"
#include "ssatmoenvmanager.h"

namespace
{
    // The tile grid's unit - a stock region, matching the 256m global grid every region origin sits on.
    constexpr F32 SS_WATER_TILE_M = 256.f;

    // <SS:Nexii> The water height the SS family mirrors: the environment's own, the lowest enabled track water, so the authored tide - or a sky build's ocean dialled kilometres below its platform - is what actually renders. Each track's tide is authored relative to that track's floor and visibleWaterHeight() lifts it back to world metres; with no track carrying a water plane the region height stands in, the applier derendering water outright in that state anyway. update() drives this same height into the region water height store (the hijack below), so underwater detection, fog flips, the water clip plane, precipitation landing and every other getWaterHeight() consumer follow the Atmo plane wholesale.
    F32 ss_atmo_water_height(bool& out_valid)
    {
        out_valid = false;
        SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
        if (!mgr || !mgr->hasAsset()) return 0.f;

        F32 height = 0.f;
        if (mgr->asset().visibleWaterHeight(height))
        {
            out_valid = true;
            return height;
        }
        LLViewerRegion* agent_region = gAgent.getRegion();
        return agent_region ? agent_region->getWaterHeight() : 0.f;
    }

    // True when any connected region's footprint covers the point - the same list the SSWater planes are built from, so a cell is either a region plane or a void tile, never both or neither.
    bool ss_water_region_at(F64 x, F64 y)
    {
        for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
        {
            const LLVector3d& origin = regionp->getOriginGlobal();
            const F64 width = (F64)regionp->getWidth();
            if (x >= origin.mdV[0] && x < origin.mdV[0] + width && y >= origin.mdV[1] && y < origin.mdV[1] + width)
            {
                return true;
            }
        }
        return false;
    }
}

// Stock pcode, Atmo flag: LLVOWater's constructor already sizes the plane to its region and picks the water render type and partition.
SSWater::SSWater(const LLUUID& id, LLViewerRegion* regionp)
    : LLVOWater(id, LLViewerObject::LL_VO_WATER, regionp)
{
    mIsAtmoWater = true;
}

// Edge patch from birth: LLDrawPoolWater::pushWaterPlanes uses the flag to keep void tiles from forcing reflection and distortion updates.
SSEdgeWater::SSEdgeWater(const LLUUID& id, LLViewerRegion* regionp)
    : LLVOVoidWater(id, LLViewerObject::LL_VO_VOID_WATER, regionp)
{
    mIsAtmoWater = true;
    setIsEdgePatch(true);
}

// Stub - never instantiated yet; see rebuildFarWater.
SSFarWater::SSFarWater(const LLUUID& id, LLViewerRegion* regionp)
    : LLVOVoidWater(id, LLViewerObject::LL_VO_VOID_WATER, regionp)
{
    mIsAtmoWater = true;
    setIsEdgePatch(true);
}

bool SSWaterWorld::sAtmoWaterLive = false;

// The family gate: stock planes draw while Atmo is off, the SS family draws while it owns the scene. Faces with no object cannot be classified and default to drawing.
bool SSWaterWorld::drawsThisFrame(const LLVOWater* vo)
{
    if (!vo)
    {
        return true;
    }
    return vo->getIsAtmoWater() == sAtmoWaterLive;
}

// Per frame, right after the applier: decide which family owns the frame, drive the region water
// heights the environment's plane, and rebuild the SS set when the world it mirrors has moved
// underneath it.
void SSWaterWorld::update()
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    const bool active = agent_region && SSAtmoEnvApplier::instance().isActive();

    F32 atmo_height = 0.f;
    bool atmo_height_valid = false;
    if (active)
    {
        atmo_height = ss_atmo_water_height(atmo_height_valid);
    }

    // <SS:Nexii> The height hijack runs before the signature is read: the State's region-height fields then already hold the environment's plane, so an authored height change costs one rebuild instead of two.
    if (active && atmo_height_valid)
    {
        hijackRegionHeights(atmo_height);
    }
    else
    {
        restoreRegionHeights();
    }

    State state;
    state.mActive = active;
    if (active)
    {
        state.mTransparentWater = LLPipeline::sRenderTransparentWater;
        state.mAgentHandle = agent_region->getHandle();
        state.mAgentWaterHeight = agent_region->getWaterHeight();
        state.mAtmoWaterHeight = atmo_height;
        state.mAtmoWaterHeightValid = atmo_height_valid;
        for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
        {
            ++state.mRegionCount;
            state.mHandleHash ^= regionp->getHandle() * 0x9E3779B97F4A7C15ULL;
            state.mHeightSum += regionp->getWaterHeight();
        }
    }

    const bool dirty = !(state == mState) || (active && anyDead());

    sAtmoWaterLive = active;

    if (dirty)
    {
        // <SS:Nexii> Signature is stamped after the rebuild, not before: rebuild's first act is clearWaterObjects, which resets mState so the teardown path (LLWorld::resetClass) leaves nothing stale behind. Stamping first meant the reset wiped the new signature, every frame came out dirty, and the set was killed and recreated before any object survived to the next frame's updateGeom - so no SS plane was ever built and, with stock water suppressed, no water rendered at all.
        rebuild(active);
        mState = state;
    }
}

// Drives every connected region's water height store to the environment's plane. Directly against
// the stock water object - LLSurface::setWaterHeight would also rebuild the stock hole/skirt water
// per region per change, and while Atmo owns the scene that stock set is suppressed, so the one
// LLWorld::updateWaterObjects() on restore is the only churn worth paying.
void SSWaterWorld::hijackRegionHeights(F32 height)
{
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        const U64 handle = regionp->getHandle();
        const F32 cur = regionp->getWaterHeight();

        auto it = mRegionHeights.find(handle);
        if (it == mRegionHeights.end())
        {
            // First contact: whatever sits there now is the sim's own truth.
            mRegionHeights.emplace(handle, cur);
        }
        else if (cur != mAppliedWaterHeight)
        {
            // Not our write - a sim handshake or god tool landed since the last frame. Keep it.
            it->second = cur;
        }

        if (cur != height)
        {
            LLVOWater* waterp = regionp->getLand().getWaterObj();
            if (waterp)
            {
                LLVector3 pos = waterp->getPositionRegion();
                pos.mV[VZ] = height;
                waterp->setPositionRegion(pos);
            }
        }
    }
    mAppliedWaterHeight = height;
}

// Puts every hijacked region's own height back, and rebuilds the stock water set once so the
// hole/skirt slabs (positioned from the agent region's height while hijacked) agree with it.
void SSWaterWorld::restoreRegionHeights()
{
    if (mRegionHeights.empty())
    {
        mAppliedWaterHeight = -1.0e30f;
        return;
    }

    bool restored = false;
    for (const auto& entry : mRegionHeights)
    {
        LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(entry.first);
        LLVOWater* waterp = regionp ? regionp->getLand().getWaterObj() : nullptr;
        if (waterp)
        {
            LLVector3 pos = waterp->getPositionRegion();
            if (pos.mV[VZ] != entry.second)
            {
                pos.mV[VZ] = entry.second;
                waterp->setPositionRegion(pos);
                restored = true;
            }
        }
    }
    mRegionHeights.clear();
    mAppliedWaterHeight = -1.0e30f;

    if (restored)
    {
        LLWorld::getInstance()->updateWaterObjects();
    }
}

// Kills and forgets the whole SS set, and resets the signature so the next active frame rebuilds from scratch (also called from LLWorld::resetClass at teardown). The height-hijack bookkeeping
// is deliberately untouched: rebuilds fire while staying hijacked, and wiping the recorded originals here would make the next hijack record the Atmo height itself as "the sim's own".
void SSWaterWorld::clearWaterObjects()
{
    for (LLPointer<LLVOWater>& waterp : mRegionWater)
    {
        gObjectList.killObject(waterp);
    }
    mRegionWater.clear();

    for (LLPointer<LLVOWater>& waterp : mEdgeWater)
    {
        gObjectList.killObject(waterp);
    }
    mEdgeWater.clear();

    mState = State();
}

// True when any SS water object was killed out from under us - region teardown and disconnects kill objects without asking, so the sweep recreates rather than chases ownership.
bool SSWaterWorld::anyDead() const
{
    for (const LLPointer<LLVOWater>& waterp : mRegionWater)
    {
        if (!waterp || waterp->isDead())
        {
            return true;
        }
    }
    for (const LLPointer<LLVOWater>& waterp : mEdgeWater)
    {
        if (!waterp || waterp->isDead())
        {
            return true;
        }
    }
    return false;
}

// Recreates the full SS set: one SSWater per connected region at the environment's water height, then the SSEdgeWater tile ring. Kill-and-recreate rather than reposition - the set is ~100 small
// objects and rebuilds only on region or water height changes, so simplicity wins over the stock code's slot reuse.
void SSWaterWorld::rebuild(bool active)
{
    clearWaterObjects();

    if (!active)
    {
        return;
    }

    LLViewerRegion* agent_region = gAgent.getRegion();
    if (!agent_region)
    {
        return;
    }

    F32 atmo_height = 0.f;
    bool atmo_height_valid = false;
    atmo_height = ss_atmo_water_height(atmo_height_valid);

    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        LLUUID id;
        id.generate();
        LLVOWater* waterp = (LLVOWater*)gObjectList.adoptViewerObject(new SSWater(id, regionp));
        if (!waterp)
        {
            continue;
        }
        const F64 width = (F64)regionp->getWidth();
        const LLVector3d& origin = regionp->getOriginGlobal();
        // Region water convention (llsurface.cpp): the plane sits exactly at water height with zero Z scale. Every region shares the environment's one height - the Atmo resolved tide (floor-relative authored heights lifted to world), not each sim's own.
        waterp->setPositionGlobal(LLVector3d(origin.mdV[0] + width * 0.5, origin.mdV[1] + width * 0.5, (F64)atmo_height));
        waterp->setScale(LLVector3((F32)width, (F32)width, 0.f));
        gPipeline.createObject(waterp);
        mRegionWater.push_back(waterp);
    }

    // <SS:Nexii> The tile ring covers the whole far disc: out past the squash cap so every direction's outermost tiles fold onto the cap edge (see SS_WATER_SQUASH_CAP_FRAC). The old reach stopped short of MAX_FAR_CLIP because a triangle the projection slices through rasterises black - the squash now folds every vertex back inside before that can happen, so the ring may extend freely and the ocean reads to the horizon. Floor keeps a sane ring on huge var regions.
    const F32 rwidth = agent_region->getWidth();
    mSquashCap = MAX_FAR_CLIP * SS_WATER_SQUASH_CAP_FRAC;
    mSquashKnee = mSquashCap * 0.8f;
    const F32 ring_reach = mSquashCap + rwidth * 0.5f + SS_WATER_TILE_M;
    mSquashReach = ring_reach;

    const F32 water_height = atmo_height;
    const LLVector3d& agent_origin = agent_region->getOriginGlobal();
    const F64 anchor_x = agent_origin.mdV[0] + rwidth * 0.5;
    const F64 anchor_y = agent_origin.mdV[1] + rwidth * 0.5;

    const S32 tiles_out = llceil(ring_reach / SS_WATER_TILE_M);
    const S32 tiles_across = tiles_out + (S32)(rwidth / SS_WATER_TILE_M);

    for (S32 i = -tiles_out; i < tiles_across; ++i)
    {
        for (S32 j = -tiles_out; j < tiles_across; ++j)
        {
            const F64 cx = agent_origin.mdV[0] + i * (F64)SS_WATER_TILE_M + SS_WATER_TILE_M * 0.5;
            const F64 cy = agent_origin.mdV[1] + j * (F64)SS_WATER_TILE_M + SS_WATER_TILE_M * 0.5;

            const F64 dx = cx - anchor_x;
            const F64 dy = cy - anchor_y;
            if (dx * dx + dy * dy > (F64)ring_reach * (F64)ring_reach)
            {
                continue;
            }

            if (ss_water_region_at(cx, cy))
            {
                continue;
            }

            LLUUID id;
            id.generate();
            LLVOWater* waterp = (LLVOWater*)gObjectList.adoptViewerObject(new SSEdgeWater(id, agent_region));
            if (!waterp)
            {
                continue;
            }
            // Void slab convention (LLWorld::updateWaterObjects): position Z at 256 above the waterline with Z scale 512 puts the rendered quad at water height while the bounding slab
            // reaches up for visibility culling.
            waterp->setPositionGlobal(LLVector3d(cx, cy, 256.f + water_height));
            waterp->setScale(LLVector3(SS_WATER_TILE_M, SS_WATER_TILE_M, 512.f));
            gPipeline.createObject(waterp);
            mEdgeWater.push_back(waterp);
        }
    }

    rebuildFarWater();
}

// Placeholder: SSFarWater will one day carry the water surface from the tile ring's edge to the true horizon, past MAX_FAR_CLIP - which needs its own geometry and projection answer, not more
// tiles (doc/atmo_magic_water.md, deferred; the removed far-sea experiment in git history holds the constraints already learned).
void SSWaterWorld::rebuildFarWater()
{
}
