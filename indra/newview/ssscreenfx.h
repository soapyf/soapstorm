/**
 * @file ssscreenfx.h
 * @brief Atmo Magic: the screen-space presentation shell - thermal shock and mirage strength driving a heat-shimmer
 *        post pass, and lens-drops wet/condensation state driving a rain-on-glass post pass.
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

#ifndef SS_SCREENFX_H
#define SS_SCREENFX_H

#include "llpointer.h"
#include "llsingleton.h"
#include "sslensdropcore.h"
#include "ssscreenfxcore.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class LLRenderTarget;
class LLViewerTexture;

// <SS:Nexii> NAMING NOTE: doc/atmo_magic_surface_weather.md section 8 spells this shell "class SSScreenFX", but
// ssscreenfxcore.h (DO NOT EDIT, harness-green) already claims that identifier as `namespace SSScreenFX` - a class
// and a namespace cannot share one name in the same scope (a hard redefinition error, not a style choice), and this
// shell's own .cpp needs both the core namespace (to call its free functions) and this class' own declaration
// visible in the same translation unit. Named SSScreenFXPost instead - the shell that drives the SSScreenFX core's
// two post-processing passes - mirroring the established sibling pattern of a differently-named shell over a core
// namespace (SSVortex core / SSVortices shell, SSStormCell core / SSStormCells shell). Report this rename up: any
// call site written against the literal "SSScreenFX::getInstance()" text needs "SSScreenFXPost::getInstance()".
class SSScreenFXPost : public LLSingleton<SSScreenFXPost>
{
    LLSINGLETON_EMPTY_CTOR(SSScreenFXPost);

public:
    // One frame of every screen-space source: thermal shock, lens wet/condensation, and the screen-space slide
    // direction (gravity plus wind) the lens shader drifts drops along and the heat shader's ripple reads. Wall-clock
    // dt; touches no world state; every closed form is the core's.
    void idle(F32 dt);

    // The heat-shimmer pass: draws src into dst through gSSPostHeatProgram. Returns false (nothing drawn, caller
    // does not swap) when SSAtmoHeatShimmer is off or the dialed mirage strength is below the shader's noise floor.
    bool renderHeat(LLRenderTarget* src, LLRenderTarget* dst);

    // The lens-drops pass: draws src into dst through gSSPostLensProgram. Returns false when SSAtmoLensDrops is off
    // or there is nothing on the lens worth drawing (wet plus dialed condensation at or below the floor).
    bool renderLens(LLRenderTarget* src, LLRenderTarget* dst);

    F32 shock() const { return mThermal.mShock; }
    F32 mirage() const { return mMirage; }
    F32 lensWet() const { return mLens.mWet; }
    F32 lensFog() const { return mLens.mFog; }

    // How much rain reaches the lens where the camera is, 0 fully sheltered .. 1 open sky - SSScreenFX::lensExposure() over
    // SSRainShadowMap's answer for the camera's column, the same authority the precipitation sim culls particles with.
    F32 lensExposure() const { return mLensExposure; }

    // How many drops are on the glass right now, for the info overlay. Read off an atomic because the simulation that owns the list
    // runs on its own thread.
    S32 lensDrops() const { return mDropCount.load(std::memory_order_relaxed); }

    // <SS:Nexii> Takes the simulation thread down before this object does. The posted step captures `this`, so the pool has to be
    // closed - which joins its thread, and therefore waits out any step already running - while the singleton is still alive.
    void cleanupSingleton() override;

    // GL teardown (SSAtmoMagic::shutdownGL): what cleanupSingleton does, run while the context still exists.
    void shutdownGL();

private:
    // <SS:Nexii> R16 THREADING. The drop simulation does not run on the main thread. It is a few hundred drops with an all-pairs
    // (grid-bucketed) merge step, which is cheap - the reference runs the same thing in JavaScript - but it is also pure arithmetic
    // over state nothing else needs, which makes it the easiest kind of work to move off the frame's critical path.
    //
    // THE WHOLE LOCKING DISCIPLINE IS ONE RULE: exactly one step task exists at a time (mSimBusy). While it is in flight the main
    // thread does not touch mDrops or any of the simulation's own counters, so none of them need a mutex; the ONLY thing shared is the
    // sprite snapshot the worker publishes at the end of a step, and that has one. If a step has not finished by the time the next
    // frame draws, the main thread redraws the previous snapshot rather than waiting: this is water on a lens, and a frame of
    // staleness is not visible. That also means the sim degrades to a lower step rate under load instead of stalling the frame.
    class SimWorker;

    // One drop as the drop map needs it: centre, the two radii its tension stretch leaves it with, and whether it is RUNNING - which
    // only the channel map cares about, because a drop sitting still does not cut a channel through the condensation.
    struct Sprite { F32 mX = 0.f, mY = 0.f, mRX = 0.f, mRY = 0.f; bool mRunning = false; };

    // MAIN: hands one step to the worker, or runs it inline if there is no worker to hand it to.
    void postLensStep(F32 dt, F32 aspect);
    // WORKER: the simulation. Fixed quanta, so the look is the same at 30 and 144 fps.
    void runLensSteps(F32 dt, F32 wet, F32 clear01, F32 aspect, S32 budget, bool reset);
    void stepLensOnce(F32 dt, F32 wet, F32 clear01, F32 aspect, S32 budget);
    void publishSprites();
    // MAIN/GL: the baked drop sprite, and the sprites drawn into the drop map.
    void bakeCapTexture();
    bool drawDropMap(S32 w, S32 h, F32 aspect);
    // MAIN/GL: the persistent channel the runners cut through the condensation (R17). Not cleared per frame - it decays.
    void drawClearMap(S32 w, S32 h, F32 aspect);

    SSScreenFX::Thermal mThermal;
    SSScreenFX::Lens mLens;

    F32 mMirage = 0.f;            // this frame's undialed mirageStrength(), cached for the render gate and the info overlay
    bool mWasUnderwater = false;  // last frame's underwater state, so stepLens sees the surfacing frame
    F32 mLensExposure = 1.f;      // this frame's shelter answer, 1 = open sky (defect R3)

    // The lens shader's own clock: dt accumulated through the SSAtmoLensDryRate dial rather than read off gFrameTimeSeconds, so moving
    // the dial stretches every per-drop life instead of jumping the whole field's phase. F64 per the harness' accumulator rule.
    F64 mLensClock = 0.0;

    // <SS:Nexii> WORKER-OWNED, all of it. The drops, and the counters the step needs to stay frame-rate independent and free of any
    // runtime RNG (a monotone spawn sequence and a monotone step index, both hashed rather than drawn). The list deliberately SURVIVES
    // teleports, region crossings and camera cuts: the water is on the LENS, not in the world, and a lens does not shed its drops
    // because the avatar moved. The only things that clear it are the two that physically would - the camera going under water (which
    // also zeroes the wet channel) and the pass being switched off.
    std::vector<SSLensDrop::Drop> mDrops;
    std::vector<S32> mGridHead;    // grid bucket -> first drop index, rebuilt each step (see stepLensOnce)
    std::vector<S32> mGridNext;    // drop index -> next drop in the same bucket, -1 to end
    F32 mSpawnDebt = 0.f;          // fractional arrivals carried across steps
    U32 mSpawnSeq = 0;             // monotone arrival counter, hashed for each new drop's position, radius and seed
    U32 mStepSeq = 0;              // monotone step counter, hashed for each running drop's sideways kick
    F32 mSimAccum = 0.f;           // unspent dt, so the fixed quantum below survives a variable frame time

    // <SS:Nexii> Raw, not unique_ptr, and deliberately: both of these are types this header only FORWARD-DECLARES, and a unique_ptr
    // member of an incomplete type needs the complete type wherever the implicit destructor gets instantiated - which for an LLSingleton
    // is any translation unit that touches getInstance(). Owned explicitly in cleanupSingleton() instead, which is where the worker has
    // to be taken down by hand anyway.
    SimWorker* mSimWorker = nullptr;
    bool mSimWorkerTried = false;
    std::atomic<bool> mSimBusy{false};
    std::atomic<S32> mDropCount{0};

    // The handoff. mSprites is written by the worker at the end of a step and swapped into mDrawSprites by the main thread; nothing
    // else crosses.
    std::mutex mSpriteMutex;
    std::vector<Sprite> mSprites;
    bool mSpritesReady = false;
    std::vector<Sprite> mDrawSprites;

    // The drop map (rgb = the cap normal, a = coverage) and the one sprite every drop is drawn with.
    LLRenderTarget* mDropMap = nullptr;
    // <SS:Nexii> R17: the swept channel. PERSISTENT - the only buffer here that is not cleared every frame, because what it holds is a
    // property of the glass that outlives the drop that put it there. Quarter resolution: it is a soft mask on a haze, and its own
    // blurriness is the softness a wiped edge should have anyway.
    LLRenderTarget* mClearMap = nullptr;
    bool mClearMapReset = true;   // clear it outright rather than decay it: first use, a resize, or the glass having gone dry
    F32 mLensStepDt = 0.f;        // the lens clock's dt from the last idle(), which is what the channel decays on
    LLPointer<LLViewerTexture> mCapTex;

    // Screen-space projections refreshed once per idle() from the rotation part of the current modelview: the slide
    // direction the lens drops fall along (unit length), and the raw wind projection the heat shimmer ripples with.
    F32 mSlideDX = 0.f;
    F32 mSlideDY = -1.f;
    F32 mHeatWindX = 0.f;
    F32 mHeatWindY = 0.f;
};

#endif
