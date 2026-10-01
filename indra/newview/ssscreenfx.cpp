/**
 * @file ssscreenfx.cpp
 * @brief Atmo Magic: the screen-space presentation shell - see ssscreenfx.h for the naming note (SSScreenFXPost, not
 *        SSScreenFX: that identifier is the core's namespace, ssscreenfxcore.h, DO NOT EDIT).
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

#include "ssscreenfx.h"

#include "ssatmomagic.h"
#include "sslensdropcore.h"
#include "ssrainshadow.h"
#include "sswindflow.h"

#include "llappviewer.h"
#include "llfasttimer.h"
#include "llimage.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llmath.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "pipeline.h"

#include "threadpool.h"
#include "workqueue.h"

#include <cmath>
#include <cstring>

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

// One frame of every screen-space source. See ssscreenfx.h.
void SSScreenFXPost::idle(F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    LLViewerCamera* cam = LLViewerCamera::getInstance();

    // <SS:Nexii> With Atmo off there is no temperature to jump from: feed the accumulator a constant so stepThermal only ever decays it, never mistaking the system's absence for a thermal event.
    static const F32 DISABLED_BASELINE_C = 20.f;
    const F32 temp_c = atmo->isEnabled() ? atmo->temperatureC() : DISABLED_BASELINE_C;
    SSScreenFX::stepThermal(mThermal, temp_c, dt);

    const F32 sun_up01 = atmo->isEnabled() ? atmo->sunUp() : 0.f;
    const F32 baseline = SSScreenFX::mirageBaseline(temp_c, sun_up01);
    const F32 wind_ms = atmo->isEnabled() ? atmo->windSpeed() : 0.f;
    // <SS:Nexii> Snow does not cool the ground the way rain does, so only a liquid preset suppresses the shimmer.
    const F32 rain01 = (atmo->isEnabled() && !atmo->preset().isGranular()) ? atmo->precipitation() : 0.f;
    const F32 fov_deg = cam->getView() * RAD_TO_DEG;
    mMirage = SSScreenFX::mirageStrength(baseline, mThermal.mShock, wind_ms, rain01, fov_deg);

    const LLVector3 at_axis = cam->getAtAxis();
    const F32 pitch = at_axis.mV[VZ];
    const LLVector3 rain_dir = atmo->isEnabled() ? atmo->rainDirection() : LLVector3(0.f, 0.f, -1.f);
    const F32 rain_align = -(rain_dir * at_axis);

    // <SS:Nexii> [interaction: SSRainShadowMap] Shelter (defect R3): the lens now asks the SAME authority the precipitation sim asks - one
    // resolveColumn() on the camera's own column per frame, which is the call ssprecipitation.cpp makes per particle to decide it is under
    // a roof - and the core turns its answer into an exposure factor with the sim's own COVER_TOLERANCE. Before this the pass drove the
    // drops straight off the global weather intensity, so the glass streamed indoors, under an awning and in a rain shadow alike. Exposure
    // gates ACCUMULATION only: the drops already on the glass keep their own lives and evaporate normally as mWet drains.
    mLensExposure = 1.f;
    if (atmo->isEnabled())
    {
        LLVector3 hit;
        bool on_water = false;
        const bool resolved = SSRainShadowMap::getInstance()->resolveColumn(cam->getOrigin(), hit, on_water);
        mLensExposure = SSScreenFX::lensExposure(hit.mV[VZ], cam->getOrigin().mV[VZ], resolved);
    }

    const F32 intensity = atmo->isEnabled()
        ? atmo->precipitation() * (atmo->preset().isGranular() ? 0.4f : 1.0f) * mLensExposure
        : 0.f;
    const F32 demand = SSScreenFX::lensDemand(pitch, rain_align, intensity);

    const bool underwater = cam->cameraUnderWater();
    const bool surfaced = mWasUnderwater && !underwater;
    SSScreenFX::stepLens(mLens, demand, temp_c, underwater, surfaced, dt);
    mWasUnderwater = underwater;

    // <SS:Nexii> Gravity and wind projected into the screen by the rotation part of the current modelview - the same basis the lens shader's drops fall along and the heat shimmer's wind-driven ripple reads.
    const glm::mat3 rot(get_current_modelview());
    const glm::vec3 gravity_view = rot * glm::vec3(0.f, 0.f, -1.f);
    const LLVector3 wind_agent = SSWindFlowMap::getInstance()->sample(cam->getOrigin());
    const glm::vec3 wind_view = rot * glm::vec3(wind_agent.mV[VX], wind_agent.mV[VY], wind_agent.mV[VZ]);

    mHeatWindX = wind_view.x;
    mHeatWindY = wind_view.y;

    F32 dx = 0.f, dy = -1.f;
    SSScreenFX::slideDir(gravity_view.x, gravity_view.y, wind_view.x, wind_view.y, dx, dy);
    mSlideDX = dx;
    mSlideDY = dy;

    // <SS:Nexii> The lens shader's ONLY clock, accumulated rather than read off gFrameTimeSeconds so the SSAtmoLensDryRate dial can
    // stretch or compress every per-drop life without the whole field jumping phase the moment the slider moves. F64 internally per the
    // harness' accumulator rule; the shader gets an F32 and only ever divides it by a per-cell life, so precision loss is a slow phase
    // drift over hours, not a positional error.
    static LLCachedControl<F32> lens_dry(gSavedSettings, "SSAtmoLensDryRate", 1.f);
    const F32 lens_dt = dt * llclamp((F32)lens_dry, 0.05f, 8.f);
    mLensClock += (F64)lens_dt;

    // <SS:Nexii> R16: the step is HANDED OFF here rather than run here. The aspect comes from the camera rather than from a
    // render target because idle() runs outside the frame draw - the simulation only needs it to know how wide the glass is.
    postLensStep(lens_dt, LLViewerCamera::getInstance()->getAspect());
}

// <SS:Nexii> R16: THE DROP SIMULATION, on its own thread. See ssscreenfx.h for the locking rule (exactly one step in flight, so only
// the sprite snapshot is shared) and sslensdropcore.h for why the three-population model this replaced could not produce what the
// reference does. One worker, because the work is one dependent chain - arrivals, then merges, then motion - and splitting a few
// hundred drops across cores would cost more in synchronisation than the whole step costs to run.
class SSScreenFXPost::SimWorker : public LL::ThreadPool
{
public:
    // Capacity 4 and tryPost, never post: only one step is ever outstanding, so a refusal means something is already wrong rather than
    // that a backlog is forming - and a refused step is simply skipped, which is why nothing here can ever block the main thread.
    SimWorker() : LL::ThreadPool("SSLensDrop", 1, 4, false) {}
};

// <SS:Nexii> The simulation thread outlives nothing. close() joins the worker, which waits out any step already running, so by the
// time this returns no task can still be holding `this`. Ordering matters more than tidiness here: a posted step captures the singleton.
void SSScreenFXPost::cleanupSingleton()
{
    shutdownGL();
}

void SSScreenFXPost::shutdownGL()
{
    if (mSimWorker)
    {
        mSimWorker->close();
        delete mSimWorker;
        mSimWorker = nullptr;
    }
    mDrops.clear();
    mDrawSprites.clear();
    if (mDropMap)
    {
        mDropMap->release();
        delete mDropMap;
        mDropMap = nullptr;
    }
    if (mClearMap)
    {
        mClearMap->release();
        delete mClearMap;
        mClearMap = nullptr;
    }
    mCapTex = nullptr;
}

namespace
{
    // The simulation's fixed quantum, so the glass looks the same at 30 and at 144 fps. At the fastest a drop travels (SPEED_REF at
    // R_CAP, about 1.4 screen heights per second) one step moves it 0.015 of a height - under half the smallest drop's diameter - so
    // the merge test cannot step over a contact. The step cap makes a stalled frame SKIP time rather than spend the recovery frame
    // catching up on it, which is the right trade for water on a lens: nobody can tell, and the alternative is a stutter chasing a stutter.
    const F32 SS_LENS_STEP_S     = 1.f / 90.f;
    const S32 SS_LENS_MAX_STEPS  = 4;
    const S32 SS_LENS_CAP_RES    = 64;      // the baked drop sprite's resolution
    const F32 SS_LENS_FIELD_PAD  = 0.10f;   // how far past the screen edge the field runs, so drops can enter and leave rather than pop

    inline U8 ssEncodeUnit(F32 v)
    {
        return (U8)llclamp((S32)((v * 0.5f + 0.5f) * 255.f + 0.5f), 0, 255);
    }
}

// MAIN THREAD: hand one step to the worker. Never waits: if the previous step is still running the frame simply redraws the snapshot it
// already has. Falls back to running the step inline when there is no worker (the dial is off, or the pool would not start), so the
// feature never depends on the thread existing.
void SSScreenFXPost::postLensStep(F32 dt, F32 aspect)
{
    static LLCachedControl<S32> streak_budget(gSavedSettings, "SSAtmoLensStreakBudget", 12);
    static LLCachedControl<F32> lens_clear(gSavedSettings, "SSAtmoLensClearCentre", 0.75f);
    static LLCachedControl<bool> worker_on(gSavedSettings, "SSAtmoLensDropWorker", true);

    if (mSimBusy.load(std::memory_order_acquire)) return;

    // <SS:Nexii> SSAtmoLensStreakBudget now sizes the WHOLE population rather than a separate streak list - there is only one
    // population left to size. Its default of 12 maps to the full MAX_DROPS so the dial's shipped position means "as authored", and the
    // rest of its range scales either side of that. Its settings.xml wording still describes streaks and wants updating.
    const F32 wet = mLens.mWet;
    const F32 clear01 = llclamp((F32)lens_clear, 0.f, 1.f);
    const S32 budget = llclamp((S32)(((F32)streak_budget / 12.f) * (F32)SSLensDrop::MAX_DROPS), 0, SSLensDrop::MAX_DROPS);
    const bool reset = (mLens.mWet <= 0.f && mLens.mFog <= 0.f);

    // The channel map is drawn on the main thread but decays on the LENS clock, so it stretches with SSAtmoLensDryRate like everything
    // else on the glass; and a glass that has gone dry drops its channels outright rather than fading them from wherever they were.
    mLensStepDt = dt;
    if (reset) mClearMapReset = true;

    if (worker_on && !mSimWorkerTried)
    {
        mSimWorkerTried = true;
        try
        {
            mSimWorker = new SimWorker();
            mSimWorker->start();
        }
        catch (...)
        {
            delete mSimWorker;
            mSimWorker = nullptr;
            LL_WARNS("AtmoMagic") << "SSScreenFXPost: lens drop worker would not start - stepping on the main thread" << LL_ENDL;
        }
    }

    if (!mSimWorker)
    {
        runLensSteps(dt, wet, clear01, aspect, budget, reset);
        return;
    }

    mSimBusy.store(true, std::memory_order_release);
    if (!mSimWorker->getQueue().tryPost([this, dt, wet, clear01, aspect, budget, reset]()
        {
            runLensSteps(dt, wet, clear01, aspect, budget, reset);
            mSimBusy.store(false, std::memory_order_release);
        }))
    {
        mSimBusy.store(false, std::memory_order_release);
    }
}

// WORKER: spend the frame's dt in fixed quanta, then publish what the drop map should draw.
void SSScreenFXPost::runLensSteps(F32 dt, F32 wet, F32 clear01, F32 aspect, S32 budget, bool reset)
{
    if (reset)
    {
        mDrops.clear();
        mSpawnDebt = 0.f;
        mSimAccum = 0.f;
        mDropCount.store(0, std::memory_order_relaxed);
        publishSprites();
        return;
    }

    mSimAccum += llclamp(dt, 0.f, 0.5f);

    S32 steps = 0;
    while (mSimAccum >= SS_LENS_STEP_S && steps < SS_LENS_MAX_STEPS)
    {
        stepLensOnce(SS_LENS_STEP_S, wet, clear01, aspect, budget);
        mSimAccum -= SS_LENS_STEP_S;
        ++steps;
    }
    if (steps >= SS_LENS_MAX_STEPS) mSimAccum = 0.f;

    publishSprites();
}

// WORKER: one quantum. Arrivals, then merges, then motion - in that order, because a drop that arrives on top of another should be
// eaten this step rather than spend a frame overlapping it, and a merge changes the radius the motion law then reads.
void SSScreenFXPost::stepLensOnce(F32 dt, F32 wet, F32 clear01, F32 aspect, S32 budget)
{
    const F32 fieldW = llmax(aspect, 0.1f) + SS_LENS_FIELD_PAD * 2.f;
    const F32 fieldH = 1.f + SS_LENS_FIELD_PAD * 2.f;

    ++mStepSeq;

    // ---------------------------------------------------------------- arrivals
    // The ONLY source of water in this model. Everything bigger than a fresh droplet got that way by eating some of these, which is why
    // the rate is high and the radius is biased small. A debt accumulator so the rate is a rate, not a per-step count.
    const S32 target = SSLensDrop::targetDrops(wet, budget);
    mSpawnDebt += SSLensDrop::SPAWN_HZ_FULL * llclamp(wet, 0.f, 1.f) * dt;
    while (mSpawnDebt >= 1.f)
    {
        mSpawnDebt -= 1.f;
        if ((S32)mDrops.size() >= target) break;

        const F32 seq = (F32)(mSpawnSeq++);
        SSLensDrop::Drop d;
        d.mX = (SSLensDrop::hash21(seq + 0.5f, 13.7f) - 0.5f) * fieldW;
        d.mY = (SSLensDrop::hash21(seq + 19.3f, 2.1f) - 0.5f) * fieldH;
        d.mSeed = SSLensDrop::hash21(seq + 71.9f, 43.3f);
        d.mR = SSLensDrop::spawnRadius(SSLensDrop::hash21(seq + 5.1f, 31.7f));
        mDrops.push_back(d);
    }

    // ---------------------------------------------------------------- merges
    // Grid-bucketed, cell exactly one full-grown drop across, so every candidate pair is inside the 3x3 neighbourhood and the pass is
    // linear in the drop count rather than quadratic. Each pair is considered ONCE (j > i) and the larger of the two always survives,
    // so the result does not depend on which of the pair the loop reached first. A merged radius can outgrow its own cell; that is left
    // for the next step to notice rather than rebuilt mid-pass, which would make the pass order-dependent in a way this one is not.
    const S32 n = (S32)mDrops.size();
    if (n > 1)
    {
        const F32 cellSize = 2.f * SSLensDrop::R_CAP;
        const S32 gw = llclamp((S32)(fieldW / cellSize) + 1, 1, 128);
        const S32 gh = llclamp((S32)(fieldH / cellSize) + 1, 1, 128);

        auto cellX = [&](F32 x) { return llclamp((S32)((x + fieldW * 0.5f) / fieldW * (F32)gw), 0, gw - 1); };
        auto cellY = [&](F32 y) { return llclamp((S32)((y + fieldH * 0.5f) / fieldH * (F32)gh), 0, gh - 1); };

        mGridHead.assign((size_t)gw * (size_t)gh, -1);
        mGridNext.assign((size_t)n, -1);
        for (S32 i = 0; i < n; ++i)
        {
            const S32 c = cellY(mDrops[i].mY) * gw + cellX(mDrops[i].mX);
            mGridNext[i] = mGridHead[c];
            mGridHead[c] = i;
        }

        for (S32 i = 0; i < n; ++i)
        {
            if (mDrops[i].mR <= 0.f) continue;

            bool eaten = false;
            const S32 cx = cellX(mDrops[i].mX), cy = cellY(mDrops[i].mY);
            for (S32 oy = -1; oy <= 1 && !eaten; ++oy)
            {
                const S32 ny = cy + oy;
                if (ny < 0 || ny >= gh) continue;
                for (S32 ox = -1; ox <= 1 && !eaten; ++ox)
                {
                    const S32 nx = cx + ox;
                    if (nx < 0 || nx >= gw) continue;

                    for (S32 j = mGridHead[ny * gw + nx]; j >= 0; j = mGridNext[j])
                    {
                        if (j <= i || mDrops[j].mR <= 0.f) continue;
                        if (!SSLensDrop::touches(mDrops[i].mX, mDrops[i].mY, mDrops[i].mR,
                                                 mDrops[j].mX, mDrops[j].mY, mDrops[j].mR)) continue;

                        if (mDrops[i].mR >= mDrops[j].mR)
                        {
                            SSLensDrop::mergeInto(mDrops[i], mDrops[j]);
                            mDrops[j].mR = 0.f;
                        }
                        else
                        {
                            SSLensDrop::mergeInto(mDrops[j], mDrops[i]);
                            mDrops[i].mR = 0.f;
                            eaten = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- motion, and what a runner leaves behind
    std::vector<SSLensDrop::Drop> shed;
    for (S32 i = 0; i < (S32)mDrops.size(); ++i)
    {
        SSLensDrop::Drop& d = mDrops[i];
        if (d.mR <= 0.f) continue;

        const F32 tail = SSLensDrop::clearRadial(std::sqrt(d.mX * d.mX + d.mY * d.mY), clear01);
        const F32 kick = SSLensDrop::hash21(d.mSeed * 91.7f + (F32)(mStepSeq & 0xffffu), 3.3f);
        if (!SSLensDrop::stepDrop(d, dt, tail, kick))
        {
            d.mR = 0.f;
            continue;
        }

        if (!SSLensDrop::shedNow(d)) continue;

        const F32 shedR = SSLensDrop::shedRadius(d.mR);
        const F32 parentR = SSLensDrop::shedParentRadius(d.mR);
        d.mSinceShed = 0.f;
        if (shedR <= SSLensDrop::R_DIE || (S32)(mDrops.size() + shed.size()) >= SSLensDrop::MAX_DROPS) continue;

        // Deposited BEHIND the parent, and far enough behind that it is outside MERGE_REACH of it. That distance is not cosmetic: the
        // previous build had to make "a head cannot eat its own wake" structurally impossible because its beads were a different kind of
        // object; here a shed drop is an ordinary drop in the same list, so the only thing keeping the parent from swallowing it back on
        // the very next step is that it is laid down out of reach. The parent then runs away from it, and any LATER runner is free to
        // sweep it up - which is correct, and is where a channel gets cleaner the more traffic it sees.
        const F32 sp = std::sqrt(d.mVX * d.mVX + d.mVY * d.mVY);
        const F32 bx = (sp > 1.0e-5f) ? (-d.mVX / sp) : 0.f;
        const F32 by = (sp > 1.0e-5f) ? (-d.mVY / sp) : 1.f;
        const F32 gap = (parentR + shedR) * 1.05f;

        SSLensDrop::Drop s;
        s.mX = d.mX + bx * gap;
        s.mY = d.mY + by * gap;
        s.mR = shedR;
        s.mSeed = SSLensDrop::hash21(d.mSeed * 17.3f + (F32)(mStepSeq & 0xffffu), 61.1f);
        shed.push_back(s);

        d.mR = parentR;
    }

    for (const SSLensDrop::Drop& s : shed)
    {
        if ((S32)mDrops.size() >= SSLensDrop::MAX_DROPS) break;
        mDrops.push_back(s);
    }

    // ---------------------------------------------------------------- the dead and the departed
    for (S32 i = 0; i < (S32)mDrops.size(); )
    {
        const SSLensDrop::Drop& d = mDrops[i];
        const bool gone = (d.mR <= SSLensDrop::R_DIE)
                       || (d.mY < -fieldH * 0.5f - d.mR)
                       || (d.mY >  fieldH * 0.5f + d.mR)
                       || (std::fabs(d.mX) > fieldW * 0.5f + d.mR);
        if (gone)
        {
            mDrops[i] = mDrops.back();
            mDrops.pop_back();
            continue;
        }
        ++i;
    }

    mDropCount.store((S32)mDrops.size(), std::memory_order_relaxed);
}

// WORKER: the one thing that crosses threads. Everything the draw needs and nothing it does not - a centre and two radii, with the
// tension stretch already applied, so the main thread never reaches into a Drop.
void SSScreenFXPost::publishSprites()
{
    std::vector<Sprite> out;
    out.reserve(mDrops.size());
    for (const SSLensDrop::Drop& d : mDrops)
    {
        if (d.mR <= SSLensDrop::R_DIE) continue;
        Sprite s;
        s.mX = d.mX;
        s.mY = d.mY;
        // The stretch adds to one axis rather than trading between them: a drop being pulled toward what it just swallowed is briefly
        // BIGGER, not merely elongated, because the volume it swallowed is now in it. It relaxes to round over SPREAD_TAU_S.
        s.mRX = d.mR * (1.f + d.mSpreadX);
        s.mRY = d.mR * (1.f + d.mSpreadY);
        s.mRunning = d.mRunning;
        out.push_back(s);
    }

    std::lock_guard<std::mutex> lock(mSpriteMutex);
    mSprites.swap(out);
    mSpritesReady = true;
}

// MAIN/GL: the one sprite every drop is drawn with - a spherical cap in rgb (the core's capNormal, which is exact and already unit
// length) and its silhouette in alpha. Baked once. This is what lets the drop map be built by drawing rather than by evaluating: the
// per-fragment shape maths that used to run for every drop at every pixel now runs 64x64 times, at startup.
void SSScreenFXPost::bakeCapTexture()
{
    if (mCapTex.notNull()) return;

    const S32 res = SS_LENS_CAP_RES;
    LLPointer<LLImageRaw> raw = new LLImageRaw(res, res, 4);
    U8* data = raw->getData();
    if (!data) return;

    const F32 c = (F32)res * 0.5f;
    for (S32 y = 0; y < res; ++y)
    {
        for (S32 x = 0; x < res; ++x)
        {
            const F32 dx = ((F32)x + 0.5f - c) / c;
            const F32 dy = ((F32)y + 0.5f - c) / c;
            const F32 r2 = dx * dx + dy * dy;

            F32 nx = 0.f, ny = 0.f, nz = 1.f;
            SSLensDrop::capNormal(dx, dy, nx, ny, nz);

            U8* px = data + ((size_t)y * (size_t)res + (size_t)x) * 4;
            px[0] = ssEncodeUnit(nx);
            px[1] = ssEncodeUnit(ny);
            px[2] = ssEncodeUnit(nz);
            px[3] = (U8)llclamp((S32)(SSLensDrop::silhouette(r2) * 255.f + 0.5f), 0, 255);
        }
    }

    mCapTex = LLViewerTextureManager::getLocalTexture(raw.get(), false);
}

// MAIN/GL: the drop map. One quad per drop, alpha-blended, into an rgba target the post pass takes a single fetch from - which is what
// stops the per-fragment cost scaling with the drop count at all. The normals BLEND across an overlap, which is the neck between two
// drops mid-merge, and is the tension read for free.
bool SSScreenFXPost::drawDropMap(S32 w, S32 h, F32 aspect)
{
    if (w <= 0 || h <= 0) return false;

    {
        std::lock_guard<std::mutex> lock(mSpriteMutex);
        if (mSpritesReady)
        {
            mDrawSprites.swap(mSprites);
            mSpritesReady = false;
        }
    }

    if (!mDropMap) mDropMap = new LLRenderTarget();
    if (mDropMap->getWidth() != w || mDropMap->getHeight() != h)
    {
        mDropMap->release();
        if (!mDropMap->allocate(w, h, GL_RGBA, false)) return false;
    }
    if (!gUIProgram.isComplete()) return false;

    bakeCapTexture();
    if (mCapTex.isNull()) return false;

    LL_PROFILE_GPU_ZONE("ss lens drop map");

    mDropMap->bindTarget();
    glClearColor(0.5f, 0.5f, 1.f, 0.f);   // a flat normal and no coverage: bare glass
    mDropMap->clear(GL_COLOR_BUFFER_BIT);

    if (!mDrawSprites.empty())
    {
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLEnable blend(GL_BLEND);
        // Colour blends by coverage; ALPHA accumulates as a union (ONE, 1-src) rather than by the same factor, so two overlapping
        // drops read as covered rather than as two partial coverages multiplied down to a hole between them.
        gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                      LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);

        gUIProgram.bind();
        gGL.getTexUnit(0)->bind(mCapTex);

        // Stable screen space straight to clip space: x is measured in screen HEIGHTS, so the aspect divides out here and nowhere else.
        const F32 sx = 2.f / llmax(aspect, 0.01f);
        const F32 sy = 2.f;

        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.loadIdentity();

        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        gGL.begin(LLRender::TRIANGLES);
        for (const Sprite& s : mDrawSprites)
        {
            const F32 x0 = (s.mX - s.mRX) * sx, x1 = (s.mX + s.mRX) * sx;
            const F32 y0 = (s.mY - s.mRY) * sy, y1 = (s.mY + s.mRY) * sy;

            gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(x0, y0);
            gGL.texCoord2f(1.f, 0.f); gGL.vertex2f(x1, y0);
            gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(x1, y1);

            gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(x0, y0);
            gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(x1, y1);
            gGL.texCoord2f(0.f, 1.f); gGL.vertex2f(x0, y1);
        }
        gGL.end();
        gGL.flush();

        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gUIProgram.unbind();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }

    mDropMap->flush();
    return true;
}

// MAIN/GL: the channel a runner cuts through the condensation (R17). Two draws over a buffer that is NOT cleared between frames: first
// take a slice off everything already in it (the exponential re-hazing, as a blend factor - see SSLensDrop::clearDecay), then mark
// wherever a RUNNING drop is now. A drop sitting still marks nothing: it is displacing haze under itself, which the drop map already
// says, not cutting a channel.
//
// Quarter resolution, and the blur that comes with it is wanted - a wiped edge on misted glass is soft. It is also why this is affordable
// enough to keep every frame.
void SSScreenFXPost::drawClearMap(S32 w, S32 h, F32 aspect)
{
    const S32 cw = llmax(w / 4, 16);
    const S32 ch = llmax(h / 4, 16);

    if (!mClearMap) mClearMap = new LLRenderTarget();
    if (mClearMap->getWidth() != cw || mClearMap->getHeight() != ch)
    {
        mClearMap->release();
        if (!mClearMap->allocate(cw, ch, GL_RGBA, false)) return;
        mClearMapReset = true;
    }
    if (mCapTex.isNull()) return;

    LL_PROFILE_GPU_ZONE("ss lens clear map");

    mClearMap->bindTarget();

    if (mClearMapReset)
    {
        glClearColor(0.f, 0.f, 0.f, 0.f);
        mClearMap->clear(GL_COLOR_BUFFER_BIT);
        mClearMapReset = false;
    }

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLEnable blend(GL_BLEND);

    gUIProgram.bind();

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadIdentity();

    // ---- the re-hazing. dst *= (1 - decay), which is what BF_ZERO / BF_ONE_MINUS_SOURCE_ALPHA spells: no source colour is added at
    // all, the destination is simply scaled. A full-screen quad is the cheapest way to say "everything, a bit less".
    const F32 decay = SSLensDrop::clearDecay(mLensStepDt);
    if (decay > 0.0005f)
    {
        gGL.blendFunc(LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA,
                      LLRender::BF_ZERO, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4f(0.f, 0.f, 0.f, decay);
        gGL.begin(LLRender::TRIANGLES);
        gGL.vertex2f(-1.f, -1.f); gGL.vertex2f(3.f, -1.f); gGL.vertex2f(-1.f, 3.f);
        gGL.end();
        gGL.flush();
    }

    // ---- and what the runners are cutting right now. Additive by coverage, so a channel saturates after a drop or two has been over it
    // and a fast runner does not draw a fainter line than a slow one.
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE,
                  LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE);
    gGL.getTexUnit(0)->bind(mCapTex);
    gGL.color4f(1.f, 1.f, 1.f, 1.f);

    const F32 sx = 2.f / llmax(aspect, 0.01f);
    const F32 sy = 2.f;

    gGL.begin(LLRender::TRIANGLES);
    for (const Sprite& s : mDrawSprites)
    {
        if (!s.mRunning) continue;

        const F32 x0 = (s.mX - s.mRX) * sx, x1 = (s.mX + s.mRX) * sx;
        const F32 y0 = (s.mY - s.mRY) * sy, y1 = (s.mY + s.mRY) * sy;

        gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(x0, y0);
        gGL.texCoord2f(1.f, 0.f); gGL.vertex2f(x1, y0);
        gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(x1, y1);

        gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(x0, y0);
        gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(x1, y1);
        gGL.texCoord2f(0.f, 1.f); gGL.vertex2f(x0, y1);
    }
    gGL.end();
    gGL.flush();

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gUIProgram.unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    mClearMap->flush();
}

// The heat-shimmer pass. See ssscreenfx.h.
bool SSScreenFXPost::renderHeat(LLRenderTarget* src, LLRenderTarget* dst)
{
    static LLCachedControl<bool> heat_on(gSavedSettings, "SSAtmoHeatShimmer", true);
    if (!heat_on) return false;

    static LLCachedControl<F32> heat_dial(gSavedSettings, "SSAtmoHeatShimmerStrength", 1.f);
    const F32 dialed = mMirage * llmax((F32)heat_dial, 0.f);
    if (dialed < 0.005f) return false;

    if (!gSSPostHeatProgram.isComplete()) return false;

    LL_PROFILE_GPU_ZONE("ss heat shimmer");

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);

    dst->bindTarget();

    gSSPostHeatProgram.bind();
    gSSPostHeatProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_BILINEAR);
    gSSPostHeatProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen, true);

    gSSPostHeatProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)dst->getWidth(), (GLfloat)dst->getHeight());

    static LLStaticHashedString u_strength("ssHeatStrength");
    static LLStaticHashedString u_time("ssHeatTime");
    static LLStaticHashedString u_wind("ssHeatWind");
    static LLStaticHashedString u_aspect("ssHeatAspect");

    // <SS:Nexii> The core's ceiling is SHOCK_MAX * ZOOM_MAX = 6.0 before the dial; clamped here so a maxed-out dial still reads as a mirage against the shader's 0.004 offset scale, never a smear.
    gSSPostHeatProgram.uniform1f(u_strength, llmin(dialed, 6.f));
    gSSPostHeatProgram.uniform1f(u_time, gFrameTimeSeconds);
    gSSPostHeatProgram.uniform2f(u_wind, mHeatWindX, mHeatWindY);
    const F32 aspect = (dst->getHeight() > 0) ? (F32)dst->getWidth() / (F32)dst->getHeight() : 1.f;
    gSSPostHeatProgram.uniform1f(u_aspect, aspect);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    // <SS:Nexii> unbind() does not unbind textures - src and the deferred depth would otherwise stay bound on their units while src becomes the next pass's draw FBO.
    gSSPostHeatProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE);
    gSSPostHeatProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    gSSPostHeatProgram.unbind();
    dst->flush();

    return true;
}

// <SS:Nexii> R4, REFLECTION PROBES - why the drops do not sample them, stated here so nobody re-derives it. A drop on the lens IS a
// specular surface and probes would be the right source, but this pass cannot reach them: renderFinalize() runs it AFTER tonemap and
// glow (pipeline.cpp), it binds its own textures rather than going through LLPipeline::bindDeferredShader, and gSSPostLensProgram is
// registered with mFeatures.isDeferred only (llviewershadermgr.cpp) - so the program has no reflectionProbes/irradianceProbes samplers
// at all and calling gPipeline.bindReflectionProbes() on it would bind textures to uniforms that do not exist. Three things would have
// to change together: (1) mFeatures.hasReflectionProbes = mShaderLevel[SHADER_DEFERRED] > 2 on the program, as gHazeProgram sets it;
// (2) gPipeline.bindReflectionProbes(gSSPostLensProgram) / unbindReflectionProbes around the draw below; and (3) something about the
// colour space - probe radiance is LINEAR HDR while this pass sees a tonemapped frame, so either the tap gets the same tonemap applied
// or the whole pass moves ahead of it. (1) is in llviewershadermgr.cpp and (3) in pipeline.cpp, neither of them this file's to change.
// Until then the shader's Fresnel term reflects the blurred frame re-sampled along the mirrored normal, which is already in the right
// colour space and costs no new binds: honest for the rim highlight the defect is really about, wrong for anything behind the camera.
// The look is on a dial (SSAtmoLensReflection) so it can be A/B'd against 0 - the pre-fix refraction-only drop - in one build.
// The lens-drops pass. See ssscreenfx.h.
bool SSScreenFXPost::renderLens(LLRenderTarget* src, LLRenderTarget* dst)
{
    static LLCachedControl<bool> lens_on(gSavedSettings, "SSAtmoLensDrops", true);
    if (!lens_on) return false;

    static LLCachedControl<F32> lens_condensation(gSavedSettings, "SSAtmoLensCondensation", 1.f);
    const F32 fog = mLens.mFog * llmax((F32)lens_condensation, 0.f);
    if (mLens.mWet + fog <= 0.01f) return false;

    if (!gSSPostLensProgram.isComplete()) return false;

    LL_PROFILE_GPU_ZONE("ss lens drops");

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);

    const F32 aspect = (dst->getHeight() > 0) ? (F32)dst->getWidth() / (F32)dst->getHeight() : 1.f;

    // <SS:Nexii> R16: THE DROP MAP replaces the runner array. Every drop is drawn once, as a sprite, into a target this pass then takes
    // ONE fetch from - so what is on the glass costs the same per fragment whether it is one drop or five hundred, which is what lets the
    // population be a real simulation rather than 24 uniform slots. Built before the pass binds its own target, because it binds its own.
    static LLStaticHashedString u_dropmap("ssLensDropMap");
    static LLStaticHashedString u_clearmap("ssLensClearMap");
    const bool have_map = drawDropMap(dst->getWidth(), dst->getHeight(), aspect);
    drawClearMap(dst->getWidth(), dst->getHeight(), aspect);

    dst->bindTarget();

    gSSPostLensProgram.bind();
    gSSPostLensProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_BILINEAR);

    // Bound by hand onto the next free unit rather than through enableTexture, which resolves a STANDARD uniform name (normalMap and
    // friends); this sampler is the pass's own.
    S32 map_channel = -1;
    if (have_map && mDropMap && mDropMap->getWidth() > 0)
    {
        map_channel = gSSPostLensProgram.mActiveTextureChannels;
        gGL.getTexUnit(map_channel)->activate();
        gGL.getTexUnit(map_channel)->bindManual(LLTexUnit::TT_TEXTURE, mDropMap->getTexture(0));
        gGL.getTexUnit(map_channel)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gSSPostLensProgram.uniform1i(u_dropmap, map_channel);
    }

    S32 clear_channel = -1;
    if (mClearMap && mClearMap->getWidth() > 0)
    {
        clear_channel = (map_channel > -1) ? (map_channel + 1) : gSSPostLensProgram.mActiveTextureChannels;
        gGL.getTexUnit(clear_channel)->activate();
        gGL.getTexUnit(clear_channel)->bindManual(LLTexUnit::TT_TEXTURE, mClearMap->getTexture(0));
        gGL.getTexUnit(clear_channel)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gSSPostLensProgram.uniform1i(u_clearmap, clear_channel);
    }

    gSSPostLensProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)dst->getWidth(), (GLfloat)dst->getHeight());

    static LLStaticHashedString u_wet("ssLensWet");
    static LLStaticHashedString u_fog("ssLensFog");
    static LLStaticHashedString u_time("ssLensTime");
    static LLStaticHashedString u_aspect("ssLensAspect");
    static LLStaticHashedString u_scale("ssLensScale");
    static LLStaticHashedString u_streaks("ssLensStreaks");
    static LLStaticHashedString u_reflect("ssLensReflection");
    static LLStaticHashedString u_clear("ssLensClear");

    static LLCachedControl<F32> lens_scale(gSavedSettings, "SSAtmoLensDropsStrength", 1.f);
    static LLCachedControl<F32> lens_streaks(gSavedSettings, "SSAtmoLensStreaks", 1.f);
    static LLCachedControl<F32> lens_reflect(gSavedSettings, "SSAtmoLensReflection", 1.f);
    static LLCachedControl<F32> lens_clear(gSavedSettings, "SSAtmoLensClearCentre", 0.75f);

    gSSPostLensProgram.uniform1f(u_wet, mLens.mWet);
    gSSPostLensProgram.uniform1f(u_fog, fog);
    // <SS:Nexii> mLensClock, not gFrameTimeSeconds: the dry-rate dial lives in the accumulation (see idle()), and the simulation spends
    // its step from the same clock, so turning the dial stretches every drop's life rather than jumping the field's phase.
    gSSPostLensProgram.uniform1f(u_time, (F32)mLensClock);
    gSSPostLensProgram.uniform1f(u_aspect, aspect);
    gSSPostLensProgram.uniform1f(u_scale, llclamp((F32)lens_scale, 0.f, 2.f));
    gSSPostLensProgram.uniform1f(u_streaks, llclamp((F32)lens_streaks, 0.f, 2.f));
    gSSPostLensProgram.uniform1f(u_reflect, llclamp((F32)lens_reflect, 0.f, 2.f));
    gSSPostLensProgram.uniform1f(u_clear, llclamp((F32)lens_clear, 0.f, 1.f));

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    // <SS:Nexii> unbind() does not unbind textures - src would otherwise stay bound on its unit while it becomes the next pass's draw FBO.
    gSSPostLensProgram.unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE);
    if (clear_channel > -1)
    {
        gGL.getTexUnit(clear_channel)->unbind(LLTexUnit::TT_TEXTURE);
    }
    if (map_channel > -1)
    {
        gGL.getTexUnit(map_channel)->unbind(LLTexUnit::TT_TEXTURE);
    }
    gGL.getTexUnit(0)->activate();
    gSSPostLensProgram.unbind();
    dst->flush();

    return true;
}
