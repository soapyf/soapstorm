/**
 * @file sscombatlockout.h
 * @brief Mutes debug rendering that pierces walls or reveals hidden state while the player is aiming
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_COMBATLOCKOUT_H
#define SS_COMBATLOCKOUT_H

#include "stdtypes.h"

// <SS:Nexii> Combat render lockout. While the camera reports mouselook (which covers OTS, see LLAgentCamera::cameraMouselook) and for a tail after leaving it, every debug view that would let an aiming player see through geometry or find hidden objects and avatars is forced off at its draw site. Toggles that have to be forced rather than read-gated keep the user's intent here and get it back when the lockout lifts, so nothing has to be switched on again by hand. Rationale in doc/combat_render_lockout.md.
class SSCombatLockout
{
public:
    static void update();                                   // once per frame before any render pass, outside every render type push/pop
    static bool active() { return sActive; }
    static U64  debugMask() { return sActive ? sLockedDebugMask : 0; }   // LLRenderDebugMask bits hidden from hasRenderDebugMask while active

    // Intent routing for the forced toggles: the live flag when the lockout is off, the stash when it is on.
    static void setWireframe(bool on);
    static bool getWireframe();
    static void setHighlightTransparent(bool on);
    static bool getHighlightTransparent();
    static void setHighlightTransparentRigged(bool on);
    static bool getHighlightTransparentRigged();
    static void setHiddenSelections(bool on);
    static bool getHiddenSelections();
    static void toggleRenderType(U32 type);
    static bool hasRenderType(U32 type);

    static const U32 MAX_RENDER_TYPES = 64;     // stash size, checked against LLPipeline::NUM_RENDER_TYPES in the .cpp

private:
    static void engage();
    static void release();
    static bool isLockedRenderType(U32 type);

    static const U64 sLockedDebugMask;
    static const F64 TAIL_SECONDS;

    static bool sActive;
    static F64  sLastAimSeconds;
    static bool sWireframe;
    static bool sHighlightTransparent;
    static bool sHighlightTransparentRigged;
    static bool sHiddenSelections;
    static bool sRenderTypes[MAX_RENDER_TYPES];
};
// </SS:Nexii>

#endif // SS_COMBATLOCKOUT_H
