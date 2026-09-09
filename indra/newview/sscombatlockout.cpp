/**
 * @file sscombatlockout.cpp
 * @brief Mutes debug rendering that pierces walls or reveals hidden state while the player is aiming
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "sscombatlockout.h"

#include "llagentcamera.h"
#include "llappviewer.h"
#include "lldrawpoolalpha.h"
#include "llframetimer.h"
#include "llselectmgr.h"
#include "pipeline.h"

// <SS:Nexii> Combat render lockout

// Debug mask bits that draw collision, bounds, joints, raycast targets or geometry-derived maps, most of them with depth test off.
const U64 SSCombatLockout::sLockedDebugMask =
      LLPipeline::RENDER_DEBUG_BBOXES
    | LLPipeline::RENDER_DEBUG_OCTREE
    | LLPipeline::RENDER_DEBUG_OCCLUSION
    | LLPipeline::RENDER_DEBUG_RAYCAST
    | LLPipeline::RENDER_DEBUG_AVATAR_VOLUME
    | LLPipeline::RENDER_DEBUG_AVATAR_JOINTS
    | LLPipeline::RENDER_DEBUG_AGENT_TARGET
    | LLPipeline::RENDER_DEBUG_PHYSICS_SHAPES
    | LLPipeline::RENDER_DEBUG_IMPOSTORS
    | LLPipeline::RENDER_DEBUG_WIND_FLOW
    | LLPipeline::RENDER_DEBUG_RAIN_SHADOW
    | LLPipeline::RENDER_DEBUG_ROOF_RUNOFF
    | LLPipeline::RENDER_DEBUG_GEOM_SETTLE
    | LLPipeline::RENDER_DEBUG_SURFACE_FIELD
    | LLPipeline::RENDER_DEBUG_WORLD_FIELD;

// Long enough that flicking out of mouselook and back gains nothing, short enough not to get in the way of authoring.
const F64 SSCombatLockout::TAIL_SECONDS = 5.0;

// Render types that hold world geometry: any of them off is walls off with avatars still drawn. Avatars, particles, sky and glow stay the user's to hide.
static const U32 LOCKED_RENDER_TYPES[] =
{
    LLPipeline::RENDER_TYPE_SIMPLE,
    LLPipeline::RENDER_TYPE_MATERIALS,
    LLPipeline::RENDER_TYPE_ALPHA,
    LLPipeline::RENDER_TYPE_ALPHA_MASK,
    LLPipeline::RENDER_TYPE_FULLBRIGHT_ALPHA_MASK,
    LLPipeline::RENDER_TYPE_FULLBRIGHT,
    LLPipeline::RENDER_TYPE_TREE,
    LLPipeline::RENDER_TYPE_TERRAIN,
    LLPipeline::RENDER_TYPE_WATER,
    LLPipeline::RENDER_TYPE_VOIDWATER,
    LLPipeline::RENDER_TYPE_VOLUME,
    LLPipeline::RENDER_TYPE_GRASS,
    LLPipeline::RENDER_TYPE_BUMP,
    LLPipeline::RENDER_TYPE_GLTF_PBR,
};

static_assert(LLPipeline::NUM_RENDER_TYPES <= SSCombatLockout::MAX_RENDER_TYPES, "SSCombatLockout render type stash is too small");

bool SSCombatLockout::sActive = false;
F64  SSCombatLockout::sLastAimSeconds = -1.0e9;
bool SSCombatLockout::sWireframe = false;
bool SSCombatLockout::sHighlightTransparent = false;
bool SSCombatLockout::sHighlightTransparentRigged = false;
bool SSCombatLockout::sHiddenSelections = false;
bool SSCombatLockout::sRenderTypes[SSCombatLockout::MAX_RENDER_TYPES] = {};

// Samples the camera mode and steps the lockout across its edges.
void SSCombatLockout::update()
{
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (gAgentCamera.cameraMouselook())
    {
        sLastAimSeconds = now;
    }
    const bool want = (now - sLastAimSeconds) < TAIL_SECONDS;
    if (want == sActive)
    {
        return;
    }
    sActive = want;
    if (sActive)
    {
        engage();
    }
    else
    {
        release();
    }
}

// Stashes the user's toggles and forces the live flags safe.
void SSCombatLockout::engage()
{
    sWireframe = gUseWireframe;
    gUseWireframe = false;

    sHighlightTransparent = LLDrawPoolAlpha::sShowDebugAlpha;
    sHighlightTransparentRigged = LLDrawPoolAlpha::sShowDebugAlphaRigged;
    LLDrawPoolAlpha::sShowDebugAlpha = false;
    LLDrawPoolAlpha::sShowDebugAlphaRigged = false;

    sHiddenSelections = LLSelectMgr::sRenderHiddenSelections;
    LLSelectMgr::sRenderHiddenSelections = false;

    for (U32 type : LOCKED_RENDER_TYPES)
    {
        sRenderTypes[type] = gPipeline.hasRenderType(type);
        if (!sRenderTypes[type])
        {
            gPipeline.setRenderTypeMask(type, LLPipeline::END_RENDER_TYPES);
        }
    }

    // Invisible faces only build render batches while highlighted, so rebuild on the flip exactly as the menu toggle does.
    if (sHighlightTransparent)
    {
        gPipeline.rebuildDrawInfo();
    }
}

// Puts the user's toggles back exactly as they were.
void SSCombatLockout::release()
{
    gUseWireframe = sWireframe;

    LLDrawPoolAlpha::sShowDebugAlpha = sHighlightTransparent;
    LLDrawPoolAlpha::sShowDebugAlphaRigged = sHighlightTransparentRigged;

    LLSelectMgr::sRenderHiddenSelections = sHiddenSelections;

    for (U32 type : LOCKED_RENDER_TYPES)
    {
        if (!sRenderTypes[type])
        {
            gPipeline.clearRenderTypeMask(type, LLPipeline::END_RENDER_TYPES);
        }
    }

    if (sHighlightTransparent)
    {
        gPipeline.rebuildDrawInfo();
    }
}

// True for a render type the lockout forces on.
bool SSCombatLockout::isLockedRenderType(U32 type)
{
    for (U32 locked : LOCKED_RENDER_TYPES)
    {
        if (locked == type)
        {
            return true;
        }
    }
    return false;
}

// Wireframe intent: live flag when free, stash when locked.
void SSCombatLockout::setWireframe(bool on)
{
    if (sActive)
    {
        sWireframe = on;
    }
    else
    {
        gUseWireframe = on;
    }
}

// Wireframe as the user last asked for it.
bool SSCombatLockout::getWireframe()
{
    return sActive ? sWireframe : gUseWireframe;
}

// Highlight Transparent intent, rebuilding batches on a live flip as the menu always has.
void SSCombatLockout::setHighlightTransparent(bool on)
{
    if (sActive)
    {
        sHighlightTransparent = on;
    }
    else
    {
        LLDrawPoolAlpha::sShowDebugAlpha = on;
        gPipeline.rebuildDrawInfo();
    }
}

// Highlight Transparent as the user last asked for it.
bool SSCombatLockout::getHighlightTransparent()
{
    return sActive ? sHighlightTransparent : LLDrawPoolAlpha::sShowDebugAlpha;
}

// Highlight Rigged Transparent intent.
void SSCombatLockout::setHighlightTransparentRigged(bool on)
{
    if (sActive)
    {
        sHighlightTransparentRigged = on;
    }
    else
    {
        LLDrawPoolAlpha::sShowDebugAlphaRigged = on;
    }
}

// Highlight Rigged Transparent as the user last asked for it.
bool SSCombatLockout::getHighlightTransparentRigged()
{
    return sActive ? sHighlightTransparentRigged : LLDrawPoolAlpha::sShowDebugAlphaRigged;
}

// Show Hidden Selection intent; the saved setting stays with the menu handler.
void SSCombatLockout::setHiddenSelections(bool on)
{
    if (sActive)
    {
        sHiddenSelections = on;
    }
    else
    {
        LLSelectMgr::sRenderHiddenSelections = on;
    }
}

// Show Hidden Selection as the user last asked for it.
bool SSCombatLockout::getHiddenSelections()
{
    return sActive ? sHiddenSelections : LLSelectMgr::sRenderHiddenSelections;
}

// Render type toggle from the UI: a locked type flips its stash while the lockout holds, and water carries void water with it as the pipeline toggle does.
void SSCombatLockout::toggleRenderType(U32 type)
{
    if (sActive && isLockedRenderType(type))
    {
        sRenderTypes[type] = !sRenderTypes[type];
        if (type == LLPipeline::RENDER_TYPE_WATER)
        {
            sRenderTypes[LLPipeline::RENDER_TYPE_VOIDWATER] = !sRenderTypes[LLPipeline::RENDER_TYPE_VOIDWATER];
        }
        return;
    }
    LLPipeline::toggleRenderType(type);
}

// Render type as the user last asked for it, for menu checks.
bool SSCombatLockout::hasRenderType(U32 type)
{
    if (sActive && isLockedRenderType(type))
    {
        return sRenderTypes[type];
    }
    return gPipeline.hasRenderType(type);
}
// </SS:Nexii>
