/**
 * @file sslightning.cpp
 * @brief See sslightning.h.
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

#include "sslightning.h"

#include "ssatmomagic.h"
#include "ssatmoenvweatherstate.h"
#include "sssoundscape.h"
#include "sssurfacefield.h"
#include "sswindflow.h"
#include "ssvolcloud.h"

#include "llagent.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvieweroctree.h"
#include "llworld.h"
#include "llsurface.h"
#include "llvector4a.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "pipeline.h"

namespace
{

    const F32 ANTICIPATION_MAX_S = 8.f;

    // Matches the settings.xml default: the anticipation effect ships switched on.
    const F32 ANTICIPATION_DEFAULT_S = 3.f;

    const F32 LEADER_MIN_S = 0.05f;
    const F32 LEADER_MAX_S = 1.2f;
    const F32 LEADER_GLOW = 0.12f;

    // <SS:Nexii> Visible speed the leader front crawls the channel at, m/s. Long bolts visibly take time to travel; short ones still snap.
    const F32 LEADER_SPEED_M_S = 2800.f;

    const F32 THUNDER_SHADOW_ZONE_M = 20000.f;

    const F32 STRIKE_NEAR_M = 50.f;
    const F32 STRIKE_FAR_M = 12000.f;

    const F32 CLOUD_BASE_M = 600.f;
    const F32 CLOUD_TOP_M = 1400.f;

    const F32 ATTACH_SEARCH_M = 120.f;

    const S32 MAX_CHANNEL_NODES = 1000;

    // Ground-strike branch exclusion: a cone about the main line's foot - apex held over the
    // attachment, half-angle rolled per strike - and a floor above ground.
    const F32 BRANCH_CONE_APEX_M = 80.f;
    const F32 BRANCH_CONE_HALF_ANGLE_MIN_DEG = 20.f;
    const F32 BRANCH_CONE_HALF_ANGLE_MAX_DEG = 40.f;
    const F32 BRANCH_FLOOR_MIN_M = 20.f;
    const F32 BRANCH_FLOOR_MAX_M = 40.f;

    const S32 BRANCH_TRIES = 4;

    const F32 PREPARE_LEAD_S = 10.f;

    // <SS:Nexii> Charge network's temperature mix, spanning the SAME season rack as cloud altitudes: +35C and warmer is all negative cloud-base discharges (summer heatwave norm), sliding through spring and autumn, -15C and colder all positive anvil bolts (deep winter), the summer's few positive strikes growing out of that slide where the ridges offer them.
    const F32 POSITIVE_WARM_C = 35.f;
    const F32 POSITIVE_COLD_C = -15.f;

    // <SS:Nexii> How many positive discharges become bolts from the blue - the long near-horizontal trunk out of the anvil striking ground miles from the storm. The rest still come off the cloud top, just closer to their deck.
    const F32 BLUE_ODDS_POSITIVE = 0.30f;

    // <SS:Nexii> Bolt-from-the-blue geometry, tuned for the 4km weather region and 2km draw: the origin is the storm itself, miles off at the anvil crown (beyond the far clip, exactly where the storm appears to be), the ground strike landing within view - the visible centre of its own far-flung trunk.
    const F32 BLUE_ORIGIN_MIN_M = 5000.f;
    const F32 BLUE_ORIGIN_MAX_M = 14000.f;
    const F32 BLUE_GROUND_MIN_M = 400.f;
    const F32 BLUE_GROUND_MAX_M = 3200.f;

    // <SS:Nexii> The ground crawl's bounds: how far a strike trails along the surface at the dial's 1.0 (quadratic roll, so most are short), the hard cap, and the step and node budgets that keep it minimal.
    const F32 CRAWL_MAX_M = 30.f;
    const F32 CRAWL_CAP_M = 45.f;
    const S32 CRAWL_STEPS_MAX = 12;
    const F32 CRAWL_JUMP_M = 2.f;

    // <SS:Nexii> The impact spark table: how many a full-intensity strike throws, their flight-time and landing-distance rolls (every one lands inside its life - the old speed x rise roll flew for 2-5s against a 1.4s life and never did).
    const S32 SPARK_COUNT = 48;
    const F32 SPARK_HIT_MIN_S = 0.28f;
    const F32 SPARK_HIT_MAX_S = 0.95f;
    const F32 SPARK_REACH_MIN_M = 3.f;
    const F32 SPARK_REACH_MAX_M = 28.f;
    const F32 SPARK_GRAVITY = 9.8f;

    const S32 FIRE_CRAWL_MAX = 14;
    const S32 FIRE_EMBER_MAX = 16;

    // The steam burst's disc budget, foot included - one every couple of metres over the crawl's 45m cap, and every one of them costs a field query at contact.
    const S32 STEAM_MAX = 18;

    // Setting read with a fallback when it does not exist.
    F32 settingF(const char* name, F32 fallback)
    {
        return gSavedSettings.controlExists(name) ? (F32)gSavedSettings.getF32(name) : fallback;
    }

    // Debug strikes spawn as far ahead as the anticipation effect needs, so the slider's charge
    // window fits inside a button-triggered strike as it does a scheduled one. The floor keeps
    // the buttons feeling deliberate when anticipation is low or zero.
    F32 debugStrikeLeadS()
    {
        const F32 anticipation = llclamp(settingF("SSAtmoLightningAnticipation", ANTICIPATION_DEFAULT_S), 0.f, ANTICIPATION_MAX_S);
        return llmax(3.f, anticipation + 0.25f);
    }

    // Stateless integer hash behind the ground show's spawn-time rolls - the renderer's twin, so every client's tables match.
    U32 ss_hash3(U32 x)
    {
        x ^= x >> 16; x *= 0x7feb352du;
        x ^= x >> 15; x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }
    // Hash to [0,1).
    F32 ss_hash_unit(U32 x) { return (F32)(ss_hash3(x) & 0xffffffu) / (F32)0x1000000; }

    // One-dimensional value noise on the hash, for the spatially coherent plasma pop thresholds.
    F32 ss_vnoise1(F32 x, U32 salt)
    {
        const F32 fl = floorf(x);
        const F32 f = x - fl;
        const U32 i = (U32)(S32)fl;
        const F32 a = ss_hash_unit(i * 7919u ^ salt);
        const F32 b = ss_hash_unit((i + 1u) * 7919u ^ salt);
        const F32 s = f * f * (3.f - 2.f * f);
        return a + (b - a) * s;
    }

    // Wet score of a surface-field sample: wetness plus twice the puddle presence (5-10mm of standing water is a full puddle).
    F32 ss_wet_score(const SSSurfaceField::Sample& s)
    {
        if (!s.mValid) return 0.f;
        return llclamp(s.mWet, 0.f, 1.f) + 2.f * llclamp(s.mPuddle / 0.01f, 0.f, 1.f);
    }

    // Hands a strike's pooled occlusion query name back, if it holds one.
    void ss_release_strike_query(const SSStrike& strike)
    {
        if (strike.mOccQuery != 0)
        {
            LLOcclusionCullingGroup::releaseOcclusionQueryObjectName(strike.mOccQuery);
            strike.mOccQuery = 0;
        }
    }
}

// Debug label for a strike kind.
const char* SSLightning::kindName(SSStrikeKind k)
{
    switch (k)
    {
        case STRIKE_SHEET:  return "sheet";
        case STRIKE_FORK:   return "fork";
        case STRIKE_GROUND: return "ground";
        default:            return "?";
    }
}

// Debug overlay colour per strike kind - shared by markers and countdown labels so a glance tells the kinds apart.
const LLColor4& SSLightning::kindDebugColor(SSStrikeKind k)
{
    static const LLColor4 sheet(0.2f, 0.9f, 1.f, 0.75f);
    static const LLColor4 fork(1.f, 0.75f, 0.15f, 0.75f);
    static const LLColor4 ground(1.f, 0.15f, 0.85f, 0.75f);
    switch (k)
    {
        case STRIKE_SHEET:  return sheet;
        case STRIKE_FORK:   return fork;
        default:            return ground;
    }
}

// <SS:Nexii> Charge network's temperature mix: +35C and above is summer's all-negative world, sliding across the whole season range, -15C and below winter's all-positive one. Spawn rolls each strike against this; the overlay reads it to label the mood.
F32 SSLightning::positiveSkew(F32 temperature_c)
{
    return llclamp((POSITIVE_WARM_C - temperature_c)
                   / (POSITIVE_WARM_C - POSITIVE_COLD_C), 0.f, 1.f);
}

// The surface under a point for the ground show: surface field, then the flowmap's height capture, then terrain, never below water.
F32 SSLightning::surfaceZ(const LLVector3& pos_agent)
{
    F32 z = 0.f;
    bool have = false;

    const SSSurfaceField::Sample s = SSSurfaceField::getInstance()->sample(pos_agent);
    if (s.mValid)
    {
        z = s.mSurfaceZ;
        have = true;
    }
    if (!have)
    {
        F32 top = 0.f;
        if (SSWindFlowMap::getInstance()->surfaceAt(pos_agent, top))
        {
            z = top;
            have = true;
        }
    }
    if (!have)
    {
        z = LLWorld::getInstance()->resolveLandHeightAgent(pos_agent);
    }
    if (LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosAgent(pos_agent))
    {
        z = llmax(z, regionp->getWaterHeight());
    }
    return z;
}

// Exports the brightest live strikes as deferred point lights, one per channel at the node nearest the camera, then the amber ground fire behind them.
S32 SSLightning::sceneLights(std::vector<LLVector4>& out_pos_radius,
                             std::vector<LLColor3>& out_color, S32 max_count) const
{
    out_pos_radius.clear();
    out_color.clear();
    if (max_count <= 0) return 0;

    static LLCachedControl<F32> scene_light(gSavedSettings, "SSAtmoLightningSceneLight", 1.f);
    const F32 strength = llclamp((F32)scene_light, 0.f, 4.f);
    if (strength <= 0.f) return 0;

    const LLColor3 tint = SSAtmoMagic::getInstance()->lightningCoreColor();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();

    for (const SSStrike& strike : mStrikes)
    {
        if ((S32)out_pos_radius.size() >= max_count) break;

        const F32 b = strike.mChannelBrightness * strike.mIntensity;
        if (b <= 0.01f) continue;

        LLVector3 best = strike.mOrigin;
        F32 best_d2 = (strike.mOrigin - cam).magVecSquared();
        for (const SSStrikeNode& node : strike.mChannel)
        {
            if (node.mReachedAt > strike.mLeaderProgress) continue;
            const F32 d2 = (node.mPos - cam).magVecSquared();
            if (d2 < best_d2) { best_d2 = d2; best = node.mPos; }
        }

        // <SS:Nexii> A positive bolt's power throws its light further - the anvil strike is the storm's heavy hitter, ten times a negative discharge's current.
        const F32 radius = llclamp(sqrtf(best_d2) * 0.6f * llmax(strike.mPower, 1.f), 60.f, 1500.f);

        if (sqrtf(best_d2) - radius > MAX_FAR_CLIP) continue;

        out_pos_radius.push_back(LLVector4(best.mV[VX], best.mV[VY], best.mV[VZ], radius));
        out_color.push_back(tint * (b * strength));
    }

    // <SS:Nexii> The ground fire's own amber light, after the bolt lights so it is the first to drop at the light budget: what turns the wet road orange for the half second after contact, as the recorded strike does. Driven by the fire envelope, not the channel brightness, so it outlasts the column.
    static LLCachedControl<F32> fire_light(gSavedSettings, "SSAtmoLightningFireLight", 1.f);
    const F32 fire_str = llclamp((F32)fire_light, 0.f, 4.f) * strength;
    if (fire_str > 0.f)
    {
        static const LLColor3 FIRE_LIGHT(1.f, 0.5f, 0.12f);
        for (const SSStrike& strike : mStrikes)
        {
            if ((S32)out_pos_radius.size() >= max_count) break;
            if (strike.mKind != STRIKE_GROUND) continue;

            const F32 env = llmax(strike.mFire, 0.5f * strike.mHit) * strike.mIntensity;
            if (env <= 0.01f) continue;

            LLVector3 at = strike.mGround;
            if (strike.mCrawlCount > 0)
            {
                const S32 mid = strike.mCrawlStart + strike.mCrawlCount / 3;
                at = (strike.mGround + strike.mChannel[(size_t)mid].mPos) * 0.5f;
            }
            at.mV[VZ] += 1.5f;

            const F32 radius = llclamp(25.f + 35.f * strike.mIntensity + strike.mCrawlLenM, 25.f, 120.f);
            if ((at - cam).magVec() - radius > MAX_FAR_CLIP) continue;

            out_pos_radius.push_back(LLVector4(at.mV[VX], at.mV[VY], at.mV[VZ], radius));
            out_color.push_back(FIRE_LIGHT * (env * fire_str));
        }
    }

    return (S32)out_pos_radius.size();
}

// Kills a strike's debug HUD text.
static void ss_kill_strike_text(SSStrike& strike)
{
    if (strike.mDebugText)
    {
        strike.mDebugText->markDead();
        strike.mDebugText = nullptr;
    }
}

// True when a point falls inside a ground strike's branch exclusion: below the floor held over
// the attachment, or inside the cone about the main line's descent - apex 80m over the ground,
// half-angle rolled 20-40deg. Keeps trunk branches away from the strike point; the trunk itself
// never asks.
static bool ss_branch_forbidden(const SSStrike& strike, const LLVector3& pos)
{
    if (!strike.mBranchLimits) return false;

    if (pos.mV[VZ] < strike.mBranchFloorZ) return true;

    const LLVector3 from_apex = pos - strike.mBranchConeApex;
    const F32 along = from_apex * strike.mBranchConeAxis;
    if (along <= 0.f) return false; // above the apex - the cone only reaches down

    const F32 len = from_apex.magVec();
    if (len < 0.01f) return true;
    return (along / len) >= strike.mBranchConeDot;
}

// Drops all strikes and scheduling - the off switch.
void SSLightning::clear()
{
    for (SSStrike& strike : mStrikes)
    {
        ss_kill_strike_text(strike);
        ss_release_strike_query(strike);
    }
    mStrikes.clear();
    mNextStrikeAt = -1.0;
    mFlash = 0.f;
    mFlashDir.clear();
    mNextBlueAt = -1.0;
    mBluePrepared = false;
}

// Seconds until the next scheduled strike, for the overlay.
F64 SSLightning::nextStrikeIn() const
{
    if (mNextStrikeAt < 0.0) return -1.0;
    return llmax(0.0, mNextStrikeAt - SSAtmoMagic::getInstance()->sharedTime());
}

// Per-frame drive: schedules strikes from convection (or the track's intervals), prepares each a lead early, advances the live ones, gathers the frame's flash.
void SSLightning::idle(F32 dt)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoLightning", true);
    if (!atmo->isEnabled() || !enabled || !atmo->lightningOn())
    {
        if (!mStrikes.empty()) clear();
        return;
    }

    const F64 now = atmo->sharedTime();

    const F32 convection = atmo->turbulence();

    // <SS:Nexii> Cold stretches the intervals: the convection that rattles a summer sky every few seconds is the rare winter storm when temperature is against it. The resolver bakes the same scale into its own answers; this is the no-interval fallback's twin, so track-weather and cube-weather never disagree about the season.
    const F32 season = llmax(
        SSAtmoEnvWeatherResolver::lightningTemperatureScale(atmo->temperatureC()), 0.02f);

    F32 interval_min = atmo->lightningIntervalMin();
    F32 interval_max = atmo->lightningIntervalMax();
    if (interval_max < 0.f)
    {
        // <SS:Nexii> The resolver's wet gate, twinned: precipitation is the cube's moisture verbatim, so the same band keeps this fallback from thundering out of a dry sky.
        const F32 wet = SSAtmoEnvWeatherResolver::lightningMoistureScale(atmo->precipitation());
        interval_min = 0.f;
        interval_max = 0.f;
        if (wet > 0.f)
        {
            const F32 scale = season * wet;
            if (convection >= 0.75f)      { interval_min = 2.f / scale;  interval_max = 5.f / scale; }
            else if (convection >= 0.55f) { interval_min = 30.f / scale; interval_max = 60.f / scale; }
        }
    }

    // The strike-rate multiplier, shared by the ordinary scheduler and the blue-bolt clock.
    const F32 rate = llclamp(settingF("SSAtmoLightningRate", 1.f), 0.05f, 8.f);

    if (interval_max <= 0.f)
    {
        mNextStrikeAt = -1.0;
    }
    else
    {
        if (mNextStrikeAt < 0.0)
        {
            SSRandStream rng((U32)(now * 1000.0) ^ atmo->seed());
            mNextStrikeAt = now + rng.frand(interval_min, interval_max) / rate;
        }
        else if (now >= mNextStrikeAt - (F64)PREPARE_LEAD_S && !mPrepared)
        {
            const F32 fierce = (atmo->lightningIntensity() >= 0.f)
                ? atmo->lightningIntensity() : convection;
            spawn(fierce, mNextStrikeAt);
            mPrepared = true;
        }

        if (mPrepared && now >= mNextStrikeAt)
        {
            SSRandStream rng((U32)(now * 977.0) ^ atmo->seed() ^ 0x5eed1u);
            mNextStrikeAt = now + rng.frand(interval_min, interval_max) / rate;
            mPrepared = false;
        }
    }

    // <SS:Nexii> The bolt from the blue: while a storm approaches (the cube's next keyframe stormier than now, upwind), lightning reaches ahead - rare positive anvil bolts out of the upwind, striking ground miles from their cloud, rumble trailing in over the speed of sound. This scheduler is its OWN clock: it wants strikes before the weather has turned thundery, so it ignores the ordinary interval and only needs the weather live.
    static LLCachedControl<bool> blue_setting(gSavedSettings, "SSAtmoLightningBlue", true);
    const F32 approach = atmo->stormApproach();
    if (!blue_setting || approach <= 0.02f || !atmo->isEnabled() || !enabled || !atmo->lightningOn())
    {
        mNextBlueAt = -1.0;
        mBluePrepared = false;
    }
    else
    {
        if (mNextBlueAt < 0.0)
        {
            SSRandStream rng((U32)(now * 104729.0) ^ atmo->seed() ^ 0x8a1eu);
            // <SS:Nexii> Season stretches this clock exactly as the ordinary one - the winter approach announces itself with rarer bolts, not summer-cadence ones. It was the single interval path that skipped the scale.
            const F32 interval = lerp(75.f, 18.f, llclamp(approach, 0.f, 1.f))
                                 / (llmax((F32)rate, 0.05f) * season);
            mNextBlueAt = now + rng.frand(interval * 0.6f, interval * 1.5f);
        }
        else if (!mBluePrepared && now >= mNextBlueAt - (F64)PREPARE_LEAD_S)
        {
            const F32 fierce = (atmo->lightningIntensity() >= 0.f)
                ? atmo->lightningIntensity() : llmax(convection, 0.5f);
            const F32 bearing = (atmo->stormApproachHeadingDeg() >= 0.f)
                ? atmo->stormApproachHeadingDeg() * DEG_TO_RAD : -1.f;
            spawn(fierce, mNextBlueAt, bearing, -1.f, STRIKE_KIND_COUNT, nullptr, true);
            mBluePrepared = true;
        }

        if (mBluePrepared && now >= mNextBlueAt)
        {
            mNextBlueAt = -1.0;
            mBluePrepared = false;
        }
    }

    mFlash = 0.f;
    mFlashDir.clear();

    F32 brightest = 0.f;
    for (SSStrike& strike : mStrikes)
    {
        advance(strike, dt);

        mFlash += strike.mFlash;
        if (strike.mFlash > brightest)
        {
            brightest = strike.mFlash;
            LLVector3 dir = strike.mOrigin - gAgent.getPositionAgent();
            if (dir.normalize() > 0.f) mFlashDir = dir;
        }
    }
    mFlash = llclamp(mFlash, 0.f, 1.f);

    mStrikes.erase(std::remove_if(mStrikes.begin(), mStrikes.end(),
                                  [](const SSStrike& s) { return s.mDone; }),
                   mStrikes.end());
}

// Debug button: a strike ahead of the camera with a short lead, still sounding scheduled.
void SSLightning::triggerNow()
{
    const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
    const F32 bearing = atan2f(at.mV[VY], at.mV[VX]);

    SSRandStream rng((U32)(SSAtmoMagic::getInstance()->sharedTime() * 31.0));
    const F32 vis = MAX_FAR_CLIP * 0.8f;

    const F32 t = rng.frand();
    const F32 dist = 60.f + t * t * (llclamp(vis, 500.f, 2500.f) - 60.f);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    spawn(llmax(atmo->turbulence(), 0.6f), atmo->sharedTime() + debugStrikeLeadS(), bearing, dist);
    mPrepared = false;
}

// Debug button: a ground strike where the camera ray through the view's lower third meets land or a build - the mark lands where you look, not wherever the roll put it.
void SSLightning::triggerGroundNow()
{
    if (!gViewerWindow) return;

    const LLRect& view = gViewerWindow->getWorldViewRectScaled();
    const S32 x = (S32)view.getCenterX();
    const S32 y = (S32)(view.mBottom + (F32)view.getHeight() / 3.f);

    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const LLVector3 dir = gViewerWindow->mouseDirectionGlobal(x, y);

    LLVector4a start, end, pos;
    start.load3(cam.mV);
    end.load3((cam + dir * MAX_FAR_CLIP).mV);

    LLVector3 at;
    bool found = false;
    if (gPipeline.lineSegmentIntersectWorldGeometry(start, end, &pos))
    {
        at.set(pos.getF32ptr());
        found = true;
    }
    else if (dir.mV[VZ] < -0.01f)
    {
        // Open sky along the whole ray (surface past the far clip): walk it down to where it dips under terrain.
        for (F32 d = 50.f; d < MAX_FAR_CLIP; d += 32.f)
        {
            const LLVector3 p = cam + dir * d;
            if (p.mV[VZ] <= LLWorld::getInstance()->resolveLandHeightAgent(p))
            {
                at = p;
                found = true;
                break;
            }
        }
    }
    if (!found) return;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    spawn(llmax(atmo->turbulence(), 0.6f), atmo->sharedTime() + debugStrikeLeadS(), -1.f, -1.f, STRIKE_GROUND, &at);
    mPrepared = false;
}

// Builds a full strike for a future fire time: kind, polarity, placement, attachment, channel, ground show, thunder to the soundscape with its lead. A forced kind or ground point (debug buttons) skips that part's rolls; a forced blue (storm-approach anticipation) is always a positive bolt from the blue.
void SSLightning::spawn(F32 intensity, F64 fire_at, F32 force_bearing, F32 force_dist,
                        SSStrikeKind force_kind, const LLVector3* force_ground,
                        bool force_blue)
{
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const F64 now = atmo->sharedTime();

    SSRandStream rng((U32)(fire_at * 4093.0) ^ atmo->seed() ^ 0xb01du);

    SSStrike strike;
    strike.mCreatedAt = now;
    strike.mFireAt = fire_at;
    strike.mIntensity = llclamp(intensity, 0.f, 1.f);

    // Debug buttons force their kind and/or attachment; deliberately aimed, they are exempt from
    // the under deck's re-route to cloud-to-cloud. Same for the bolt from the blue: it aims at
    // the storm it reaches ahead of.
    strike.mForced = (force_kind < STRIKE_KIND_COUNT) || (force_ground != nullptr) || force_blue;

    if (force_kind < STRIKE_KIND_COUNT)
    {
        strike.mKind = force_kind;
    }
    else
    {
        F32 w[STRIKE_KIND_COUNT];
        w[STRIKE_SHEET]  = llmax(settingF("SSAtmoLightningWeightSheet",  6.f), 0.f);
        w[STRIKE_FORK]   = llmax(settingF("SSAtmoLightningWeightFork",   3.f), 0.f);
        w[STRIKE_GROUND] = llmax(settingF("SSAtmoLightningWeightGround", 1.f), 0.f);

        const F32 total = w[0] + w[1] + w[2];
        if (total <= 0.f) return;

        F32 roll = rng.frand(0.f, total);
        strike.mKind = STRIKE_SHEET;
        for (S32 k = 0; k < STRIKE_KIND_COUNT; ++k)
        {
            if (roll < w[k]) { strike.mKind = (SSStrikeKind)k; break; }
            roll -= w[k];
        }
    }

    // <SS:Nexii> Polarity by temperature: summer runs negative off the cloud base, winter's storm is the positive anvil's world, sliding between. A bolt from the blue is always positive - it IS an anvil discharge, just one that travelled. Rolled from the same deterministic stream as everything else, so every client agrees on the charge.
    if (!force_blue)
    {
        static LLCachedControl<bool> polarity_setting(gSavedSettings, "SSAtmoLightningPolarity", true);
        if (polarity_setting)
        {
            strike.mPositive = (rng.frand() < positiveSkew(atmo->temperatureC()));
        }
    }
    else
    {
        strike.mPositive = true;
    }

    // <SS:Nexii> Some positive discharges are bolts from the blue: out of the anvil at the storm's edge, running the gap sideways for miles before falling far away. The rest of the positive strikes come off the cloud top, closer to home.
    strike.mBlue = force_blue || (strike.mPositive && rng.frand() < BLUE_ODDS_POSITIVE);
    if (strike.mBlue)
    {
        strike.mKind = STRIKE_GROUND;
    }

    // <SS:Nexii> The positive stroke's character: a rapid series of quick pulses holding the glow longer - the winter storm's heavy hitter, ten times a negative discharge's current, throwing its light further. Negative bolts keep the sharp single snap.
    if (strike.mPositive)
    {
        strike.mStrokesMin = 4;
        strike.mStrokesMax = 9;
        strike.mRestrikeMinS = 0.015f;
        strike.mRestrikeMaxS = 0.040f;
        strike.mStrokeDecayS = 0.085f;
        strike.mPower = 1.8f;
    }

    const LLVector3 cam = gAgent.getPositionAgent();

    F32 band_lo = cam.mV[VZ] + CLOUD_BASE_M;
    F32 band_hi = cam.mV[VZ] + CLOUD_TOP_M;
    SSVolCloud* field = SSVolCloud::getInstance();
    if (!field->empty())
    {
        band_lo = field->cloudBaseZ();
        band_hi = llmax(field->cloudTopZ(), band_lo + 50.f);
    }

    // <SS:Nexii> Where the bolt comes from, by polarity: negative charges sit low in the deck (a summer storm's base concentrates the charge), positive bolts are anvil discharges - out of the cloud's TOP, up to and above its lid. A bolt from the blue sits above the deck entirely, at the anvil crown.
    F32 origin_lo = band_lo;
    F32 origin_hi = band_hi;
    if (!strike.mBlue)
    {
        const F32 band = llmax(band_hi - band_lo, 50.f);
        if (strike.mPositive)
        {
            origin_lo = band_lo + 0.45f * band;
            origin_hi = band_hi + 0.45f * band;
        }
        else
        {
            origin_lo = band_lo + 0.05f * band;
            origin_hi = band_lo + 0.55f * band;
        }
    }

    // Resolves land height at a point, then - for ground strikes - lets the tall-structure
    // capture pull the attachment toward anything worth hitting nearby, and wet ground or a
    // puddle pull it across open ground.
    auto resolve_ground = [&](const LLVector3& at) -> LLVector3
    {
        LLVector3 ground = at;
        ground.mV[VZ] = cam.mV[VZ];
        if (LLViewerRegion* regionp = gAgent.getRegion())
        {
            const LLVector3 local = ground - regionp->getOriginAgent();
            if (local.mV[VX] >= 0.f && local.mV[VY] >= 0.f &&
                local.mV[VX] < 256.f && local.mV[VY] < 256.f)
            {
                ground.mV[VZ] = regionp->getLand().resolveHeightRegion(local);
            }
        }
        if (strike.mKind == STRIKE_GROUND)
        {
            const F32 penalty = llclamp(settingF("SSAtmoLightningAttachBias", 1.f), 0.f, 4.f);
            const F32 wet_pull = llclamp(settingF("SSAtmoLightningCrawlWet", 1.f), 0.f, 3.f);

            F32 best_score = -1.0e9f;
            LLVector3 best;
            bool found = false;

            SSWindFlowMap::getInstance()->forEachColumn(ground, ATTACH_SEARCH_M,
                [&](const LLVector3& pos, F32 top)
                {
                    const F32 dx = pos.mV[VX] - ground.mV[VX];
                    const F32 dy = pos.mV[VY] - ground.mV[VY];
                    const F32 lateral = sqrtf(dx * dx + dy * dy);

                    const F32 score = top - lateral * penalty;
                    if (score > best_score)
                    {
                        best_score = score;
                        best = pos;
                        found = true;
                    }
                });

            // <SS:Nexii> Puddles on open ground: the column search only ever sees captured structures, so a ring of surface probes lets standing water on a bare road bid for the attachment too - a full puddle is worth three metres of height. Probe rolls are consumed whether or not the field is live here, so the rest of the stream stays aligned.
            if (wet_pull > 0.f)
            {
                SSSurfaceField* fieldp = SSSurfaceField::getInstance();
                for (S32 i = 0; i < 10; ++i)
                {
                    const F32 ang = rng.frand(0.f, F_TWO_PI);
                    const F32 rad = rng.frand(6.f, 40.f);
                    const LLVector3 probe = ground + LLVector3(cosf(ang) * rad, sinf(ang) * rad, 0.f);
                    const F32 wet = ss_wet_score(fieldp->sample(probe));
                    if (wet <= 0.f) continue;
                    const F32 score = ground.mV[VZ] + 3.f * wet * wet_pull - rad * penalty;
                    if (score > best_score)
                    {
                        best_score = score;
                        best = probe;
                        best.mV[VZ] = surfaceZ(probe);
                        found = true;
                    }
                }
            }

            if (found) ground = best;
        }
        return ground;
    };

    if (force_ground)
    {
        // Debug placement: the bolt anchors at the given point exactly - no terrain resolve, no
        // attach-bias search - with the cloud origin straight above it.
        strike.mGround = *force_ground;
        strike.mOrigin.set(strike.mGround.mV[VX], strike.mGround.mV[VY],
                           origin_lo + rng.frand() * (origin_hi - origin_lo));
    }
    else if (strike.mBlue)
    {
        // <SS:Nexii> Bolt-from-the-blue geometry: the origin IS the storm - the anvil crown, miles off, far past the draw distance where the approaching storm sits - the ground strike landing within view. The trunk runs the whole gap: a positive discharge's long near-horizontal travel, arriving at the far clip and striking ground. The forced bearing (the storm-approach scheduler's upwind) aims the whole bolt; otherwise it rolls like any other.
        const F32 bearing = (force_bearing > -10.f) ? force_bearing
            : rng.frand(0.f, F_TWO_PI);
        const F32 origin_d = rng.frand(BLUE_ORIGIN_MIN_M, BLUE_ORIGIN_MAX_M);
        strike.mOrigin.set(cam.mV[VX] + cosf(bearing) * origin_d,
                           cam.mV[VY] + sinf(bearing) * origin_d,
                           band_hi + rng.frand(0.15f, 0.7f) * llmax(band_hi - band_lo, 200.f));

        const F32 ground_d = rng.frand(BLUE_GROUND_MIN_M, BLUE_GROUND_MAX_M);
        strike.mGround = resolve_ground(cam
            + LLVector3(cosf(bearing) * ground_d, sinf(bearing) * ground_d, 0.f));
    }
    else
    {
        const F32 t = rng.frand();
        const F32 dist = (force_dist >= 0.f) ? force_dist
            : STRIKE_NEAR_M + (STRIKE_FAR_M - STRIKE_NEAR_M) * t * t;
        const F32 bearing = (force_bearing > -10.f && force_dist >= 0.f) ? force_bearing
            : rng.frand(0.f, F_TWO_PI);

        strike.mOrigin.set(cam.mV[VX] + cosf(bearing) * dist,
                           cam.mV[VY] + sinf(bearing) * dist,
                           origin_lo + rng.frand() * (origin_hi - origin_lo));

        strike.mGround = resolve_ground(strike.mOrigin);
    }

    // Falloff distance: a blue bolt's visible moment is its ground strike, not its far anvil
    // origin - the flash reads by the part the light actually reaches.
    strike.mDistanceM = (strike.mBlue)
        ? (strike.mGround - cam).magVec()
        : (strike.mOrigin - cam).magVec();

    if (strike.mKind != STRIKE_SHEET)
    {
        buildChannel(strike, strike.mIntensity);

        // <SS:Nexii> Positive bolt's heavier current: the same channel, a thicker bolt.
        if (strike.mPositive)
        {
            for (SSStrikeNode& node : strike.mChannel) node.mWidth *= 1.35f;
        }
    }

    buildGroundShow(strike);

    LLVector3 thunder_pos = strike.mGround;
    F32 thunder_d_sq = (thunder_pos - cam).magVecSquared();
    for (const SSStrikeNode& node : strike.mChannel)
    {
        const F32 d_sq = (node.mPos - cam).magVecSquared();
        if (d_sq < thunder_d_sq) { thunder_d_sq = d_sq; thunder_pos = node.mPos; }
    }
    const F32 thunder_d = sqrtf(thunder_d_sq);
    strike.mAudible = thunder_d < THUNDER_SHADOW_ZONE_M;

    F32 muffle = 0.f;
    {
        SSVolCloud* vol = SSVolCloud::getInstance();
        if (!vol->empty())
        {
            muffle = 1.f - vol->transmittance(cam, thunder_pos, 1.f);
        }
        if (strike.mKind != STRIKE_GROUND) muffle = llmax(muffle, 0.35f);
    }

    if (strike.mAudible)
    {
        SSSoundscape::getInstance()->scheduleThunder(
            thunder_pos, thunder_d, strike.mIntensity, strike.mFireAt, muffle);
    }
    strike.mThunderSent = true;

    mStrikes.push_back(strike);
}

// One midpoint-displaced run of channel between two points - the self-similar kinked geometry everything hangs off. With sag, an in-cloud run settles below its endpoints in
// a smooth arc: the dip taking a horizontally-travelling bolt under the deck and back up. Ground trunks and branches never ask.
void SSLightning::growPath(SSStrike& strike, S32 parent,
                           const LLVector3& from, const LLVector3& to,
                           S32 levels, F32 width_start, F32 width_end,
                           F32 t_start, F32 t_end, bool trunk,
                           SSRandStream& rng, std::vector<S32>& out_nodes,
                           bool sag)
{
    out_nodes.clear();

    LLVector3 axis = to - from;
    const F32 len = axis.magVec();
    if (len < 0.5f) return;

    const LLVector3 dir = axis / len;

    const LLVector3 ref = (llabs(dir.mV[VZ]) > 0.9f)
        ? LLVector3(1.f, 0.f, 0.f) : LLVector3(0.f, 0.f, 1.f);
    LLVector3 side_u = dir % ref;
    if (side_u.normalize() <= 0.f) return;
    LLVector3 side_v = dir % side_u;
    if (side_v.normalize() <= 0.f) return;

    std::vector<LLVector3> pts;
    pts.reserve((size_t)1 << (levels + 1));
    pts.push_back(from);
    pts.push_back(to);

    F32 amp = len * 0.09f;
    for (S32 l = 0; l < levels; ++l)
    {
        std::vector<LLVector3> next;
        next.reserve(pts.size() * 2);

        for (size_t k = 0; k + 1 < pts.size(); ++k)
        {
            next.push_back(pts[k]);

            LLVector3 mid = (pts[k] + pts[k + 1]) * 0.5f;
            mid += side_u * rng.frand(-amp, amp);
            mid += side_v * rng.frand(-amp, amp);

            mid += dir * rng.frand(-amp * 0.35f, amp * 0.35f);

            next.push_back(mid);
        }
        next.push_back(pts.back());

        pts.swap(next);
        amp *= 0.55f;
    }

    // The in-cloud dip: a smooth sag between the run's endpoints, deepest mid-run, so the bolt
    // reads as diving under its own chord (and the deck) and climbing back rather than drifting
    // flat. Capped so a short run never folds in on itself.
    if (sag && strike.mCloudDipM > 1.f)
    {
        const F32 dip = llmin(strike.mCloudDipM, len * 0.30f);
        const F32 n = (F32)(pts.size() - 1);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            const F32 f = (F32)k / n;
            pts[k].mV[VZ] -= dip * sinf(f * F_PI);
        }
    }

    S32 prev = parent;
    const F32 count = (F32)(pts.size() - 1);
    for (size_t k = 0; k < pts.size(); ++k)
    {
        if ((S32)strike.mChannel.size() >= MAX_CHANNEL_NODES) return;

        if (k == 0 && parent >= 0) continue;

        const F32 f = (F32)k / llmax(count, 1.f);

        SSStrikeNode node;
        node.mPos = pts[k];
        node.mParent = prev;
        node.mTrunk = trunk;
        node.mWidth = lerp(width_start, width_end, f);
        node.mReachedAt = llclamp(lerp(t_start, t_end, f), 0.f, 1.f);
        strike.mChannel.push_back(node);

        prev = (S32)strike.mChannel.size() - 1;
        out_nodes.push_back(prev);
    }
}

// Recursive branches off a run: count and reach proportional to the run, deviated from local travel direction.
void SSLightning::growBranches(SSStrike& strike, const std::vector<S32>& along,
                               S32 depth, S32 levels, F32 intensity,
                               SSRandStream& rng, F32 fecundity)
{
    if (depth <= 0 || along.size() < 3) return;
    if ((S32)strike.mChannel.size() >= MAX_CHANNEL_NODES) return;

    const LLVector3& run_a = strike.mChannel[(size_t)along.front()].mPos;
    const LLVector3& run_b = strike.mChannel[(size_t)along.back()].mPos;
    const F32 run_len = (run_b - run_a).magVec();
    const F32 len_gain = llclamp(run_len / 700.f, 1.f, 4.f);

    const S32 count = llmax(1, (S32)(rng.frand(1.6f, 3.4f) * (0.55f + intensity * 0.45f) * len_gain * fecundity));

    for (S32 b = 0; b < count; ++b)
    {
        if ((S32)strike.mChannel.size() >= MAX_CHANNEL_NODES) return;

        // A candidate that would come down inside the trunk cone or under the ground floor
        // is aborted and re-rolled from another node - a few tries, then the branch is dropped.
        for (S32 attempt = 0; attempt < BRANCH_TRIES; ++attempt)
        {
            const S32 pick = 1 + rng.rand((S32)along.size() - 2);
            const S32 from_idx = along[(size_t)pick];
            const SSStrikeNode& from_node = strike.mChannel[(size_t)from_idx];
            if (from_node.mParent < 0) continue;

            LLVector3 travel = from_node.mPos - strike.mChannel[(size_t)from_node.mParent].mPos;
            if (travel.normalize() <= 0.f) continue;

            const F32 vertical = llabs(travel.mV[VZ]);
            LLVector3 dir = travel;
            dir.mV[VX] += rng.frand(-0.55f, 0.55f);
            dir.mV[VY] += rng.frand(-0.55f, 0.55f);
            dir.mV[VZ] += rng.frand(-0.35f, 0.15f) * llmax(vertical, 0.25f);
            if (dir.normalize() <= 0.f) continue;

            const F32 reach = llclamp(rng.frand(0.10f, 0.30f) * run_len, 30.f, 900.f);
            const LLVector3 end = from_node.mPos + dir * reach;

            if (ss_branch_forbidden(strike, end)) continue;

            const F32 t0 = from_node.mReachedAt;
            const F32 t1 = llmin(1.f, t0 + 0.12f * (F32)depth);

            const size_t grown = strike.mChannel.size();

            std::vector<S32> sub;
            growPath(strike, from_idx, from_node.mPos, end,
                     llmax(levels - 1, 1),
                     from_node.mWidth * 0.55f, from_node.mWidth * 0.2f,
                     t0, t1, false, rng, sub);

            // Midpoint wander can still carry the run into the cone or under the floor:
            // roll the grown nodes back and try elsewhere.
            bool rejected = false;
            for (const S32 idx : sub)
            {
                if (ss_branch_forbidden(strike, strike.mChannel[(size_t)idx].mPos))
                {
                    rejected = true;
                    break;
                }
            }
            if (rejected)
            {
                strike.mChannel.resize(grown);
                continue;
            }

            growBranches(strike, sub, depth - 1, llmax(levels - 1, 1),
                         intensity * 0.75f, rng, fecundity);
            break;
        }
    }
}

// The ground crawl off the trunk's foot: a surface-following heading walk on its own stream, steered toward wet ground and puddles, ending at a wall or a drop.
void SSLightning::growCrawl(SSStrike& strike, S32 foot, F32 intensity)
{
    if (foot < 0 || foot >= (S32)strike.mChannel.size()) return;

    const F32 dial = llclamp(settingF("SSAtmoLightningCrawl", 1.f), 0.f, 3.f);
    if (dial <= 0.f) return;
    const F32 wet_dial = llclamp(settingF("SSAtmoLightningCrawlWet", 1.f), 0.f, 3.f);

    SSRandStream rng((U32)(strike.mFireAt * 6151.0) ^ 0xc4a1u);

    SSSurfaceField* fieldp = SSSurfaceField::getInstance();

    const LLVector3 foot_pos = strike.mChannel[(size_t)foot].mPos;
    const F32 foot_width = strike.mChannel[(size_t)foot].mWidth;

    // Every roll below is drawn whether or not it is used, so the length, arm and heading
    // rolls stay aligned across clients; only the wet steering and the surface heights are local.
    const F32 u = rng.frand();
    const F32 arm2_roll = rng.frand();
    const F32 bearing0 = rng.frand(0.f, F_TWO_PI);
    const F32 arm2_dev = rng.frand(-0.6f, 0.6f);

    const F32 foot_wet = ss_wet_score(fieldp->sample(foot_pos));
    F32 total = CRAWL_MAX_M * powf(u, 1.6f) * dial * (0.85f + 0.15f * intensity)
        * (1.f + 0.5f * foot_wet * wet_dial);
    total = llmin(total, CRAWL_CAP_M);
    if (total < 1.5f) return;

    const S32 arms = (arm2_roll < 0.35f) ? 2 : 1;
    const S32 start = (S32)strike.mChannel.size();
    strike.mCrawlBearing = bearing0;

    F32 longest = 0.f;
    for (S32 a = 0; a < arms; ++a)
    {
        F32 heading = (a == 0) ? bearing0 : bearing0 + F_PI + arm2_dev;
        const F32 arm_len = (a == 0) ? total : total * 0.4f;
        F32 travelled = 0.f;
        LLVector3 pos = foot_pos;
        S32 prev = foot;

        for (S32 step = 0; step < CRAWL_STEPS_MAX && travelled < arm_len; ++step)
        {
            if ((S32)strike.mChannel.size() >= MAX_CHANNEL_NODES) break;

            const F32 step_m = rng.frand(1.5f, 3.f);
            F32 cand[3];
            cand[0] = heading - 0.7f + rng.frand(-0.35f, 0.35f);
            cand[1] = heading + rng.frand(-0.35f, 0.35f);
            cand[2] = heading + 0.7f + rng.frand(-0.35f, 0.35f);

            // The three headings bid by the wetness two steps ahead; a straight run keeps a small edge.
            // <SS:Nexii> Each candidate is also walked for real and its own step height taken, because the continuity guard belongs to the CANDIDATE, not to the arm: a heading that would climb a wall is simply not bid, and only a step boxed in on all three sides ends the arm. Breaking on the first steep candidate is what stopped a crawl dead at the foot of anything with relief - it never got to try the two headings that ran along the obstacle instead of into it.
            S32 best = -1;
            F32 best_score = -1.0e9f;
            LLVector3 best_next;
            for (S32 k = 0; k < 3; ++k)
            {
                LLVector3 step_to = pos + LLVector3(cosf(cand[k]), sinf(cand[k]), 0.f) * step_m;
                step_to.mV[VZ] = surfaceZ(step_to);
                if (llabs(step_to.mV[VZ] - pos.mV[VZ]) > CRAWL_JUMP_M) continue;

                const LLVector3 probe = pos + LLVector3(cosf(cand[k]), sinf(cand[k]), 0.f) * (step_m * 2.f);
                const F32 score = ss_wet_score(fieldp->sample(probe)) * wet_dial + ((k == 1) ? 0.15f : 0.f);
                if (score > best_score)
                {
                    best_score = score;
                    best = k;
                    best_next = step_to;
                }
            }
            if (best < 0) break;
            heading = cand[best];

            const LLVector3 next = best_next;

            SSStrikeNode node;
            node.mPos = next;
            node.mParent = prev;
            node.mTrunk = false;
            node.mCrawl = true;
            node.mWidth = foot_width * lerp(0.6f, 0.25f, llclamp(travelled / llmax(arm_len, 1.f), 0.f, 1.f));
            node.mTipDistM = 0.f;
            strike.mChannel.push_back(node);

            prev = (S32)strike.mChannel.size() - 1;
            pos = next;
            travelled += step_m;
        }
        longest = llmax(longest, travelled);
    }

    strike.mCrawlCount = (S32)strike.mChannel.size() - start;
    strike.mCrawlStart = (strike.mCrawlCount > 0) ? start : -1;
    strike.mCrawlLenM = longest;
}

// Chooses the morphology (spider, bidirectional crawler, cloud-to-air, or ground trunk with a fork-style cloud spread) and grows the whole channel.
void SSLightning::buildChannel(SSStrike& strike, F32 intensity)
{
    SSRandStream rng((U32)(strike.mFireAt * 7919.0) ^ 0xfa11u);

    const bool to_ground = (strike.mKind == STRIKE_GROUND);

    // <SS:Nexii> Under-deck re-route: with the under deck between origin and ground, most ground strikes - especially those over the void or falling through a floating build's gaps - stop being bolts to nowhere and become cloud-to-cloud forks with branching ends inside that deck. Forced debug strikes keep their kind. [interaction: SSVolCloud under deck]
    if (to_ground && underDeckDivert(strike, rng))
    {
        finishChannel(strike);
        return;
    }

    if (!to_ground)
    {
        // The in-cloud dip: how far the channel's runs settle below their chord as they travel,
        // taking the bolt under the deck's base and back up. Ground trunks stay straight.
        const F32 dip_scale = llclamp(settingF("SSAtmoLightningDip", 1.f), 0.f, 2.f);
        strike.mCloudDipM = rng.frand(70.f, 240.f) * (0.6f + intensity * 0.8f) * dip_scale;
    }

    if (to_ground)
    {
        // Keeps forked channels off the strike point: a cone about the main line's descent,
        // apex 80m over the attachment with a 20-40deg half-angle, and a floor 20-40m over it.
        LLVector3 axis = strike.mGround - strike.mOrigin;
        if (axis.normalize() <= 0.f) axis.set(0.f, 0.f, -1.f);
        strike.mBranchLimits = true;
        strike.mBranchConeAxis = axis;
        strike.mBranchConeApex = strike.mGround + LLVector3(0.f, 0.f, BRANCH_CONE_APEX_M);
        strike.mBranchConeDot = cosf(rng.frand(BRANCH_CONE_HALF_ANGLE_MIN_DEG, BRANCH_CONE_HALF_ANGLE_MAX_DEG) * DEG_TO_RAD);
        strike.mBranchFloorZ = strike.mGround.mV[VZ] + rng.frand(BRANCH_FLOOR_MIN_M, BRANCH_FLOOR_MAX_M);
    }

    S32 levels = 3;
    if (strike.mDistanceM < 800.f)       levels = 6;
    else if (strike.mDistanceM < 2500.f) levels = 5;
    else if (strike.mDistanceM < 6000.f) levels = 4;

    static LLCachedControl<S32> depth_setting(gSavedSettings, "SSAtmoLightningBranchDepth", 3);
    const S32 depth = llclamp((S32)depth_setting, 0, 5);

    strike.mChannel.reserve(256);

    if (rng.frand() < (to_ground ? 0.25f : 0.35f))
    {
        // All primary geometry grows before any branches - the horizontal spread must not fight
        // the trunk's forks for the last channel nodes.
        std::vector<S32> trunk;
        if (to_ground)
        {
            growPath(strike, -1, strike.mOrigin, strike.mGround, levels,
                     1.f, 0.6f, 0.f, 1.f, true, rng, trunk, false);
            growCrawl(strike, trunk.empty() ? -1 : trunk.back(), intensity);
        }

        const S32 arm_levels = llmax(levels - 2, 3);
        const S32 arms = 4 + rng.rand(4);
        const F32 base_bearing = rng.frand(0.f, F_TWO_PI);

        std::vector<std::vector<S32>> arm_nodes((size_t)arms);
        for (S32 a = 0; a < arms; ++a)
        {
            const F32 bearing = base_bearing + (F32)a * F_TWO_PI / (F32)arms + rng.frand(-0.5f, 0.5f);
            const F32 reach = rng.frand(400.f, 1300.f);
            const LLVector3 tip = strike.mOrigin
                + LLVector3(cosf(bearing) * reach, sinf(bearing) * reach, rng.frand(-120.f, 120.f));
            growPath(strike, -1, strike.mOrigin, tip, arm_levels,
                     0.85f, 0.3f, 0.f, 1.f, !to_ground && a == 0, rng, arm_nodes[(size_t)a],
                     !to_ground && strike.mCloudDipM > 0.f);
        }
        if (to_ground)
        {
            growBranches(strike, trunk, depth, levels, intensity, rng);
        }
        for (S32 a = 0; a < arms; ++a)
        {
            growBranches(strike, arm_nodes[(size_t)a], depth, arm_levels, intensity * 0.85f, rng, 2.f);
        }
        finishChannel(strike);
        return;
    }

    if (!to_ground && rng.frand() < 0.5f)
    {
        // Bidirectional crawler: two runs out of the origin in roughly opposite directions,
        // forked along their length, so both ends carry the branching forks real in-cloud
        // discharges show. The runs sag: the bolt dives under the deck and climbs back.
        const F32 bearing = rng.frand(0.f, F_TWO_PI);

        std::vector<S32> run_a, run_b;
        {
            const F32 reach = rng.frand(600.f, 2400.f);
            const LLVector3 tip = strike.mOrigin
                + LLVector3(cosf(bearing) * reach, sinf(bearing) * reach, rng.frand(-150.f, 150.f));
            growPath(strike, -1, strike.mOrigin, tip, levels,
                     1.f, 0.6f, 0.f, 1.f, true, rng, run_a, true);
        }
        {
            const F32 back = bearing + F_PI + rng.frand(-0.5f, 0.5f);
            const F32 reach = rng.frand(400.f, 1600.f);
            const LLVector3 tip = strike.mOrigin
                + LLVector3(cosf(back) * reach, sinf(back) * reach, rng.frand(-150.f, 150.f));
            growPath(strike, -1, strike.mOrigin, tip, levels,
                     0.9f, 0.5f, 0.f, 1.f, true, rng, run_b, true);
        }
        growBranches(strike, run_a, depth, levels, intensity, rng);
        growBranches(strike, run_b, depth, levels, intensity * 0.85f, rng);
        finishChannel(strike);
        return;
    }

    const LLVector3 tip = to_ground
        ? strike.mGround
        : strike.mOrigin + (strike.mGround - strike.mOrigin) * rng.frand(0.4f, 0.7f);

    std::vector<S32> trunk;
    growPath(strike, -1, strike.mOrigin, tip, levels,
             1.f, 0.6f, 0.f, 1.f, true, rng, trunk,
             !to_ground && strike.mCloudDipM > 0.f);

    if (to_ground)
    {
        growCrawl(strike, trunk.empty() ? -1 : trunk.back(), intensity);

        // Horizontal spread off the top, fork-style: a run out of the origin in each direction,
        // so a ground bolt carries the cloud-level channel a fork does instead of standing as a
        // bare vertical line.
        const S32 arm_levels = llmax(levels - 2, 3);
        const F32 base_bearing = rng.frand(0.f, F_TWO_PI);

        std::vector<std::vector<S32>> runs(2);
        for (S32 r = 0; r < 2; ++r)
        {
            const F32 run_bearing = base_bearing + (F32)r * F_PI + rng.frand(-0.5f, 0.5f);
            const F32 reach = rng.frand(400.f, 1300.f);
            const LLVector3 run_tip = strike.mOrigin
                + LLVector3(cosf(run_bearing) * reach, sinf(run_bearing) * reach, rng.frand(-120.f, 120.f));
            growPath(strike, -1, strike.mOrigin, run_tip, arm_levels,
                     0.85f, 0.3f, 0.f, 1.f, false, rng, runs[(size_t)r]);
        }
        for (S32 r = 0; r < 2; ++r)
        {
            growBranches(strike, runs[(size_t)r], depth, arm_levels, intensity * 0.85f, rng, 2.f);
        }
    }

    growBranches(strike, trunk, depth, levels, intensity, rng);

    finishChannel(strike);
}

// After every run, branch, crawl and re-route is grown: the leader front sweeps the whole bolt as one
// continuous crawl, so each node's reach becomes its path distance from the root (normalized to
// the leader's progress), not a per-run clock - a long channel takes visible time to travel, and
// a side branch is reached when the front gets there, not when its own run would have finished.
// Path metres, tip distances and the plasma thresholds ride the same walk.
void SSLightning::finishChannel(SSStrike& strike)
{
    strike.mChannelLenM = 0.f;
    if (strike.mChannel.empty()) return;

    strike.mChannel[0].mReachedAt = 0.f;
    for (size_t i = 1; i < strike.mChannel.size(); ++i)
    {
        const S32 p = strike.mChannel[i].mParent;
        if (p < 0) continue;
        const F32 d = (strike.mChannel[i].mPos - strike.mChannel[(size_t)p].mPos).magVec();
        strike.mChannel[i].mReachedAt = strike.mChannel[(size_t)p].mReachedAt + d;
    }
    for (SSStrikeNode& node : strike.mChannel)
    {
        node.mPathM = node.mReachedAt;
        strike.mChannelLenM = llmax(strike.mChannelLenM, node.mReachedAt);
    }
    // <SS:Nexii> The leader clock is normalised by the TRUNK's own length, never the longest chain: the ground crawl and the branch chains hang off the same cumulative walk, and dividing by their maximum put the trunk foot at a fraction well short of 1 - the visible leader touched the ground up to half a second before mT 0, then hung there dim while the clock traced chains nothing draws, and the hit flare and return stroke read as arriving late. With the foot at exactly 1, connection and first stroke are the same instant; a branch chain longer than the trunk clamps to 1 and completes at contact, and the crawl is gated to mT >= 0 anyway. mChannelLenM carries the same trunk length so the leader's duration is the distance the front actually traces.
    F32 trunk_len = 0.f;
    for (const SSStrikeNode& node : strike.mChannel)
    {
        if (node.mTrunk) trunk_len = llmax(trunk_len, node.mReachedAt);
    }
    if (trunk_len > 1.f)
    {
        strike.mChannelLenM = trunk_len;
    }
    if (strike.mChannelLenM > 1.f)
    {
        for (SSStrikeNode& node : strike.mChannel)
        {
            node.mReachedAt = llclamp(node.mReachedAt / strike.mChannelLenM, 0.f, 1.f);
        }
    }

    // <SS:Nexii> Tip distances for the amber gradient: the trunk node nearest the attachment is the foot; the walk up its parents accumulates path metres, and every other node inherits its parent's figure plus its own segment, so a branch forking low is amber where it forks and the crawl is amber throughout. Anything on a chain that never meets the foot keeps the huge default.
    if (strike.mKind == STRIKE_GROUND)
    {
        S32 foot = -1;
        F32 foot_d2 = 1.0e30f;
        for (size_t i = 0; i < strike.mChannel.size(); ++i)
        {
            const SSStrikeNode& node = strike.mChannel[i];
            if (!node.mTrunk) continue;
            const F32 d2 = (node.mPos - strike.mGround).magVecSquared();
            if (d2 < foot_d2)
            {
                foot_d2 = d2;
                foot = (S32)i;
            }
        }
        if (foot >= 0)
        {
            F32 acc = 0.f;
            S32 at = foot;
            while (at >= 0)
            {
                SSStrikeNode& node = strike.mChannel[(size_t)at];
                node.mTipDistM = llmin(node.mTipDistM, acc);
                const S32 p = node.mParent;
                if (p < 0) break;
                acc += (node.mPos - strike.mChannel[(size_t)p].mPos).magVec();
                at = p;
            }
            for (size_t i = 1; i < strike.mChannel.size(); ++i)
            {
                SSStrikeNode& node = strike.mChannel[i];
                const S32 p = node.mParent;
                if (p < 0 || node.mCrawl) continue;
                const SSStrikeNode& parent = strike.mChannel[(size_t)p];
                if (parent.mTipDistM >= 1.0e8f) continue;
                const F32 d = (node.mPos - parent.mPos).magVec();
                node.mTipDistM = llmin(node.mTipDistM, parent.mTipDistM + d);
            }
        }
    }

    // <SS:Nexii> Plasma pop thresholds: value noise along the path (a 25m period) blended with a per-node jitter, so the column breaks into coherent stretches of a few segments rather than random confetti, and the same pattern every frame and on every client.
    const U32 salt = (U32)(strike.mFireAt * 3571.0) ^ 0x11feu;
    for (size_t i = 0; i < strike.mChannel.size(); ++i)
    {
        SSStrikeNode& node = strike.mChannel[i];
        const F32 n = ss_vnoise1(node.mPathM / 25.f, salt);
        const F32 j = ss_hash_unit(salt ^ ((U32)i * 1313u));
        node.mThr = llclamp(0.7f * n + 0.3f * j, 0.f, 1.f);
    }
}

// The ground show's spawn-time tables: aura discs for every kind, then for a ground strike the impact sparks' ballistics, the fire blobs and the bounding box.
// <SS:Nexii> The flash-boil, run once as the stroke lands: every candidate disc takes the water actually under it out of the surface field and keeps the figure as its own burst strength, so a strike into a flooded street steams along its whole crawl and the same strike into a dry one produces nothing. Deliberately NOT deterministic across viewers - it reads the local surface field, whose wetness, puddles and snow are local by construction, the same divergence the crawl's wet-steering and every surface height in this file already carry. The hold is scaled by the dial so a viewer running the effect down does not leave scorch behind it.
void SSLightning::vaporiseGround(SSStrike& strike)
{
    static LLCachedControl<F32> steam_setting(gSavedSettings, "SSAtmoLightningSteam", 1.f);
    const F32 dial = llclamp((F32)steam_setting, 0.f, 3.f);
    if (dial <= 0.f) return;

    static LLCachedControl<F32> hold_setting(gSavedSettings, "SSAtmoLightningSteamHold", 6.f);
    const F32 hold_s = llclamp((F32)hold_setting, 0.f, 60.f);

    SSSurfaceField* fieldp = SSSurfaceField::getInstance();
    strike.mSteamPeak = 0.f;

    for (SSStrikeSteam& sb : strike.mSteam)
    {
        sb.mWater = llclamp(fieldp->vaporise(sb.mPos, sb.mRadius, hold_s), 0.f, 1.f);
        strike.mSteamPeak = llmax(strike.mSteamPeak, sb.mWater);
    }
}

void SSLightning::buildGroundShow(SSStrike& strike)
{
    const U32 seed = ss_hash3((U32)(strike.mFireAt * 271.0) ^ 0xc0fau);
    const F32 I = strike.mIntensity;
    const bool ground = (strike.mKind == STRIKE_GROUND);

    // The aura's patch discs: fixed ring positions around the point, radius by intensity, resting on the surface.
    for (S32 i = 0; i < SSGroundShow::AURA_PATCHES; ++i)
    {
        const U32 h = seed + (U32)i * 61u;
        const F32 ang = ss_hash_unit(h) * F_TWO_PI;
        const F32 rad = 1.5f + 2.5f * ss_hash_unit(h ^ 3u);
        const F32 r = (1.2f + 1.0f * I) * (0.7f + 0.6f * ss_hash_unit(h ^ 11u));
        LLVector3 pos = strike.mGround + LLVector3(cosf(ang) * rad, sinf(ang) * rad, 0.f);
        const F32 surf = ground ? surfaceZ(pos) : strike.mGround.mV[VZ];
        pos.mV[VZ] = surf + 0.5f * r;
        strike.mAuraPos[i] = pos;
        strike.mAuraR[i] = r;
        strike.mAuraSurfZ[i] = surf;
    }
    strike.mAuraCentreR = 2.f + 2.f * I;

    strike.mSparks.clear();
    strike.mFireBlobs.clear();
    strike.mGroundBoxMin = strike.mGround - LLVector3(6.f, 6.f, 1.f);
    strike.mGroundBoxMax = strike.mGround + LLVector3(6.f, 6.f, 6.f);
    if (!ground || strike.mChannel.empty()) return;

    const F32 spread = llclamp(settingF("SSAtmoLightningSparkSpread", 1.f), 0.f, 3.f);
    const F32 ground_z = strike.mGround.mV[VZ];

    LLVector3 box_min = strike.mGroundBoxMin;
    LLVector3 box_max = strike.mGroundBoxMax;
    auto grow_box = [&](const LLVector3& p, F32 pad)
    {
        box_min.mV[VX] = llmin(box_min.mV[VX], p.mV[VX] - pad);
        box_min.mV[VY] = llmin(box_min.mV[VY], p.mV[VY] - pad);
        box_min.mV[VZ] = llmin(box_min.mV[VZ], p.mV[VZ] - pad);
        box_max.mV[VX] = llmax(box_max.mV[VX], p.mV[VX] + pad);
        box_max.mV[VY] = llmax(box_max.mV[VY], p.mV[VY] + pad);
        box_max.mV[VZ] = llmax(box_max.mV[VZ], p.mV[VZ] + pad);
    };

    // Impact sparks: a horizontal fan, most of it along the crawl, every one landing inside its life.
    const S32 count = (S32)(SPARK_COUNT * (0.4f + 0.6f * I));
    strike.mSparks.reserve((size_t)count);
    for (S32 i = 0; i < count; ++i)
    {
        const U32 h = ss_hash3(seed ^ 0x5a7au) + (U32)i * 131u;

        SSStrikeSpark sp;
        F32 heading = ss_hash_unit(h) * F_TWO_PI;
        sp.mFrom = strike.mGround;
        sp.mFrom.mV[VZ] += 0.2f;

        if (strike.mCrawlCount > 0)
        {
            const bool from_crawl = ss_hash_unit(h ^ 1u) < 0.4f;
            const bool along = ss_hash_unit(h ^ 4u) < 0.6f;
            if (from_crawl)
            {
                const S32 idx = strike.mCrawlStart
                    + llmin((S32)(ss_hash_unit(h ^ 2u) * (F32)strike.mCrawlCount), strike.mCrawlCount - 1);
                const SSStrikeNode& node = strike.mChannel[(size_t)idx];
                sp.mFrom = node.mPos;
                sp.mFrom.mV[VZ] += 0.15f;
                if (along && node.mParent >= 0)
                {
                    const LLVector3 d = node.mPos - strike.mChannel[(size_t)node.mParent].mPos;
                    heading = atan2f(d.mV[VY], d.mV[VX]) + (ss_hash_unit(h ^ 5u) - 0.5f) * 1.1f;
                }
            }
            else if (along)
            {
                heading = strike.mCrawlBearing + (ss_hash_unit(h ^ 5u) - 0.5f) * 1.1f
                    + ((ss_hash_unit(h ^ 6u) < 0.35f) ? F_PI : 0.f);
            }
        }
        sp.mCos = cosf(heading);
        sp.mSin = sinf(heading);

        const F32 t_hit = SPARK_HIT_MIN_S + (SPARK_HIT_MAX_S - SPARK_HIT_MIN_S) * ss_hash_unit(h ^ 7u);
        sp.mVZ = 0.5f * SPARK_GRAVITY * t_hit;
        const F32 reach = lerp(SPARK_REACH_MIN_M, SPARK_REACH_MAX_M, powf(ss_hash_unit(h ^ 8u), 0.8f))
            * (0.6f + 0.4f * I) * spread;
        sp.mVH = llclamp(reach / t_hit, 4.f, 40.f);
        sp.mT0 = ss_hash_unit(h ^ 9u) * 0.06f;
        sp.mLife = t_hit + 0.05f + 0.25f * ss_hash_unit(h ^ 10u);
        sp.mRadius = 0.05f + 0.05f * ss_hash_unit(h ^ 11u);
        sp.mSeed = h;

        // <SS:Nexii> A third of the fan leaves steep: risers thrown up by the blast that visibly climb before gravity takes them back, where the flat ballistic roll alone sent every spark skimming to the ground. Slower over the ground, and living most of their own taller arc so the climb reads; the landing solve below already works off the raised launch speed.
        if (ss_hash_unit(h ^ 21u) < 0.30f)
        {
            sp.mVZ *= 2.2f + 1.8f * ss_hash_unit(h ^ 22u);
            sp.mVH *= 0.45f;
            sp.mLife = (2.f * sp.mVZ / SPARK_GRAVITY) * (0.55f + 0.35f * ss_hash_unit(h ^ 23u));
        }

        // The real surface crossing of the arc, solved once: a landing within reach of the launch
        // height keeps the spark; a roof edge or a wall (more than 2.5m of drop or rise) lets it fly off.
        LLVector3 land = sp.mFrom + LLVector3(sp.mCos, sp.mSin, 0.f) * (sp.mVH * t_hit);
        land.mV[VZ] = surfaceZ(land);
        const F32 dz = land.mV[VZ] - sp.mFrom.mV[VZ];
        const F32 disc = sp.mVZ * sp.mVZ - 2.f * SPARK_GRAVITY * dz;
        if (llabs(dz) <= 2.5f && disc > 0.f)
        {
            sp.mHit = (sp.mVZ + sqrtf(disc)) / SPARK_GRAVITY;
            sp.mLandZ = land.mV[VZ];
            sp.mLife = llmax(sp.mLife, sp.mHit + 0.05f);
            grow_box(land, 1.f);
        }
        else
        {
            sp.mHit = 0.f;
            sp.mLandZ = sp.mFrom.mV[VZ] - 50.f;
            sp.mLife = t_hit + 0.6f;
        }
        strike.mSparks.push_back(sp);
    }

    // Fire blobs along the crawl, igniting outward at the crawl's arc speed.
    F32 last_fire_m = -10.f;
    S32 crawl_fires = 0;
    if (strike.mCrawlCount > 0)
    {
        const F32 foot_path = strike.mChannel[(size_t)strike.mChannel[(size_t)strike.mCrawlStart].mParent].mPathM;
        for (S32 i = 0; i < strike.mCrawlCount && crawl_fires < FIRE_CRAWL_MAX; ++i)
        {
            const SSStrikeNode& node = strike.mChannel[(size_t)(strike.mCrawlStart + i)];
            const F32 dist = llmax(node.mPathM - foot_path, 0.f);
            if (i > 0 && dist - last_fire_m < 1.8f && dist >= last_fire_m) continue;
            last_fire_m = dist;

            const U32 h = ss_hash3(seed ^ 0xf1e5u) + (U32)i * 97u;
            SSStrikeFire fb;
            fb.mPos = node.mPos;
            fb.mRadius = (1.f + 1.2f * ss_hash_unit(h)) * (0.7f + 0.6f * I);
            fb.mIgnite = dist / SSGroundShow::CRAWL_ARC_M_S;
            fb.mLifeMul = 0.6f + 0.8f * ss_hash_unit(h ^ 3u);
            fb.mSeed = h;
            strike.mFireBlobs.push_back(fb);
            grow_box(fb.mPos, fb.mRadius + 0.5f);
            ++crawl_fires;
        }
    }

    // <SS:Nexii> Steam candidates: the attachment itself, then one every couple of metres down the crawl, blowing outward at the crawl's own arc speed so the burst runs along the channel the way the fire ignition does. Rolled here with everything else, but carrying no water yet - what is actually under them is a question only contact can answer. Radii run wider than the fire blobs because a boil throws a cloud well past the wet it came from.
    if (ground)
    {
        SSStrikeSteam sb;
        sb.mPos = strike.mGround;
        sb.mRadius = (1.6f + 1.4f * I);
        sb.mDelay = 0.f;
        sb.mSeed = ss_hash3(seed ^ 0x57eau);
        strike.mSteam.push_back(sb);
        grow_box(sb.mPos, sb.mRadius + 1.f);

        if (strike.mCrawlCount > 0)
        {
            const F32 foot_path = strike.mChannel[(size_t)strike.mChannel[(size_t)strike.mCrawlStart].mParent].mPathM;
            F32 last_steam_m = -10.f;
            for (S32 i = 0; i < strike.mCrawlCount && (S32)strike.mSteam.size() < STEAM_MAX; ++i)
            {
                const SSStrikeNode& node = strike.mChannel[(size_t)(strike.mCrawlStart + i)];
                const F32 dist = llmax(node.mPathM - foot_path, 0.f);
                if (i > 0 && dist - last_steam_m < 2.2f && dist >= last_steam_m) continue;
                last_steam_m = dist;

                const U32 h = ss_hash3(seed ^ 0x57eau) + (U32)i * 131u;
                SSStrikeSteam cb;
                cb.mPos = node.mPos;
                cb.mRadius = (1.1f + 1.0f * ss_hash_unit(h)) * (0.7f + 0.6f * I);
                cb.mDelay = dist / SSGroundShow::CRAWL_ARC_M_S;
                cb.mSeed = h;
                strike.mSteam.push_back(cb);
                grow_box(cb.mPos, cb.mRadius + 1.f);
            }
        }
    }

    // A short crawl still gets the impact-time fire line: a lateral fan of blobs around the foot.
    if (strike.mCrawlLenM < 4.f)
    {
        for (S32 i = 0; i < 5; ++i)
        {
            const U32 h = ss_hash3(seed ^ 0xfa4eu) + (U32)i * 89u;
            const F32 ang = ss_hash_unit(h) * F_TWO_PI;
            const F32 rad = 1.5f + 3.5f * ss_hash_unit(h ^ 3u);
            SSStrikeFire fb;
            fb.mPos = strike.mGround + LLVector3(cosf(ang) * rad, sinf(ang) * rad, 0.f);
            fb.mPos.mV[VZ] = surfaceZ(fb.mPos);
            if (llabs(fb.mPos.mV[VZ] - ground_z) > 2.5f) fb.mPos.mV[VZ] = ground_z;
            fb.mRadius = (0.9f + 1.0f * ss_hash_unit(h ^ 5u)) * (0.7f + 0.6f * I);
            fb.mIgnite = rad / SSGroundShow::CRAWL_ARC_M_S;
            fb.mLifeMul = 0.5f + 0.6f * ss_hash_unit(h ^ 7u);
            fb.mSeed = h;
            strike.mFireBlobs.push_back(fb);
            grow_box(fb.mPos, fb.mRadius + 0.5f);
        }
    }

    // Landing embers: the scatter of small short blobs beside the main chain where sparks come down.
    S32 embers = 0;
    for (size_t i = 0; i < strike.mSparks.size() && embers < FIRE_EMBER_MAX; ++i)
    {
        const SSStrikeSpark& sp = strike.mSparks[i];
        if (sp.mHit <= 0.f) continue;
        const U32 h = ss_hash3(sp.mSeed ^ 0xe3bu);
        if (ss_hash_unit(h) >= 0.5f) continue;

        SSStrikeFire fb;
        fb.mPos = sp.mFrom + LLVector3(sp.mCos, sp.mSin, 0.f) * (sp.mVH * sp.mHit);
        fb.mPos.mV[VZ] = sp.mLandZ;
        fb.mRadius = 0.35f + 0.35f * ss_hash_unit(h ^ 3u);
        fb.mIgnite = sp.mT0 + sp.mHit;
        fb.mLifeMul = 0.35f + 0.25f * ss_hash_unit(h ^ 5u);
        fb.mSeed = h;
        fb.mEmber = true;
        strike.mFireBlobs.push_back(fb);
        ++embers;
    }

    for (S32 i = 0; i < SSGroundShow::AURA_PATCHES; ++i)
    {
        grow_box(strike.mAuraPos[i], strike.mAuraR[i]);
    }
    if (strike.mCrawlCount > 0)
    {
        for (S32 i = 0; i < strike.mCrawlCount; ++i)
        {
            grow_box(strike.mChannel[(size_t)(strike.mCrawlStart + i)].mPos, 2.f);
        }
    }
    box_min.mV[VZ] = llmin(box_min.mV[VZ], ground_z - 1.f);
    box_max.mV[VZ] = llmax(box_max.mV[VZ], ground_z + 6.f);
    strike.mGroundBoxMin = box_min;
    strike.mGroundBoxMax = box_max;
}

// The under-deck re-route for ground strikes: with the under deck (the cloud band below a
// floating build) between origin and ground, the bolt would fall through the void or a gap to
// nothing. Most such strikes become a cloud-to-cloud crawler inside that deck instead, branching
// at both ends like the in-cloud discharge it now is. The trunk must actually cross the deck
// band for the re-route to apply, and the roll scales with how solid the deck is right there -
// a bolt diving through a hole still re-routes, just a little less certainly than one swallowed
// by solid cloud. Grows the channel; true when re-routed, so the caller skips all the ground
// morphologies.
bool SSLightning::underDeckDivert(SSStrike& strike, SSRandStream& rng)
{
    if (strike.mForced) return false;

    SSVolCloud* vol = SSVolCloud::getInstance();
    if (!vol->underPresent()) return false;

    F32 divert = llclamp(settingF("SSAtmoLightningDeckRedirect", 0.65f), 0.f, 1.f);
    if (divert <= 0.f) return false;

    const F32 deck_lo = vol->underBaseZ();
    const F32 deck_hi = vol->underTopZ();
    if (deck_hi - deck_lo < 20.f) return false;

    const F32 trunk_lo = llmin(strike.mOrigin.mV[VZ], strike.mGround.mV[VZ]);
    const F32 trunk_hi = llmax(strike.mOrigin.mV[VZ], strike.mGround.mV[VZ]);
    if (trunk_hi < deck_lo || trunk_lo > deck_hi) return false;

    const F32 presence = vol->underPresenceAt(strike.mGround);
    const F32 p = divert * (0.55f + 0.45f * presence);
    if (rng.frand() >= p) return false;

    static LLCachedControl<S32> depth_setting(gSavedSettings, "SSAtmoLightningBranchDepth", 3);
    const S32 depth = llclamp((S32)depth_setting, 0, 5);

    // The re-route's root: where the old trunk would have crossed the deck band's middle,
    // jittered, the strike re-held as a fork - stroke physics, sparks and markers all read
    // cloud-to-cloud from here on.
    const F32 band_mid = (deck_lo + deck_hi) * 0.5f;
    const F32 dz = strike.mGround.mV[VZ] - strike.mOrigin.mV[VZ];
    F32 t = (dz != 0.f) ? (band_mid - strike.mOrigin.mV[VZ]) / dz : 0.5f;
    t = llclamp(t, 0.f, 1.f);
    LLVector3 centre = strike.mOrigin + (strike.mGround - strike.mOrigin) * t;
    centre.mV[VX] += rng.frand(-160.f, 160.f);
    centre.mV[VY] += rng.frand(-160.f, 160.f);
    centre.mV[VZ] = band_mid + rng.frand(-0.45f, 0.45f) * (deck_hi - deck_lo);

    strike.mKind = STRIKE_FORK;
    strike.mOrigin = centre;
    strike.mGround = centre;
    strike.mDistanceM = (centre - gAgent.getPositionAgent()).magVec();
    const F32 dip_scale = llclamp(settingF("SSAtmoLightningDip", 1.f), 0.f, 2.f);
    strike.mCloudDipM = llmax(40.f, (deck_hi - deck_lo) * 0.45f) * dip_scale;

    S32 levels = 3;
    if (strike.mDistanceM < 800.f)       levels = 6;
    else if (strike.mDistanceM < 2500.f) levels = 5;
    else if (strike.mDistanceM < 6000.f) levels = 4;

    strike.mChannel.reserve(256);

    // A bidirectional crawler inside the deck band, forked along both runs - the "branching
    // forks at either end" the re-route grows into.
    const F32 bearing = rng.frand(0.f, F_TWO_PI);
    std::vector<S32> run_a, run_b;
    {
        const F32 reach = rng.frand(600.f, 2400.f);
        const LLVector3 tip = centre
            + LLVector3(cosf(bearing) * reach, sinf(bearing) * reach, rng.frand(-120.f, 120.f));
        growPath(strike, -1, centre, tip, levels,
                 1.f, 0.6f, 0.f, 1.f, true, rng, run_a, true);
        growBranches(strike, run_a, depth, levels, strike.mIntensity, rng, 1.6f);
    }
    {
        const F32 back = bearing + F_PI + rng.frand(-0.5f, 0.5f);
        const F32 reach = rng.frand(400.f, 1600.f);
        const LLVector3 tip = centre
            + LLVector3(cosf(back) * reach, sinf(back) * reach, rng.frand(-120.f, 120.f));
        growPath(strike, -1, centre, tip, levels,
                 0.9f, 0.5f, 0.f, 1.f, true, rng, run_b, true);
        growBranches(strike, run_b, depth, levels, strike.mIntensity * 0.85f, rng, 1.6f);
    }
    return true;
}

// Runs one strike through its phases: charge, leader descent, summed return strokes plus an optional late restrike, the flare and fire envelopes, flash decay, retirement once the whole ground show is over.
void SSLightning::advance(SSStrike& strike, F32 dt)
{
    strike.mT = (F32)(SSAtmoMagic::getInstance()->sharedTime() - strike.mFireAt);

    // <SS:Nexii> Contact is the only moment the flash-boil can be resolved: the blobs were rolled ten seconds ago, but what is under them is whatever the field holds NOW. Once per strike, never per stroke - the second stroke falls on ground the first already boiled dry.
    if (!strike.mVaporised && strike.mT >= 0.f && !strike.mSteam.empty())
    {
        vaporiseGround(strike);
        strike.mVaporised = true;
    }

    SSRandStream rng((U32)(strike.mFireAt * 3571.0) ^ 0x11feu);

    // The leader's duration is the channel's own: the trunk's path length (what the front actually
    // traces - see finishChannel's normalisation) at a visible crawl speed, so a kilometre-spanning
    // bolt takes clear time to trace while a nearby short one snaps. Sheet strikes have no channel
    // and keep the plain roll.
    const F32 leader_s = (strike.mChannelLenM > 1.f)
        ? llclamp(strike.mChannelLenM
                  / llmax(settingF("SSAtmoLightningLeaderSpeed", LEADER_SPEED_M_S), 100.f)
                  * rng.frand(0.85f, 1.2f),
                  LEADER_MIN_S, LEADER_MAX_S)
        : rng.frand(LEADER_MIN_S, LEADER_MAX_S);

    // <SS:Nexii> Stroke count and timing rolled at spawn: negative bolts keep the sharp single snap, positive anvil bolts fire their rapid series of quick pulses. Each strike reads identically every frame - the stream is seeded by the fire time.
    const S32 strokes = strike.mStrokesMin
        + rng.rand(strike.mStrokesMax - strike.mStrokesMin + 1);

    const F32 charge_s = SSAtmoMagic::getInstance()->lightningCharge()
        ? llclamp(settingF("SSAtmoLightningAnticipation", ANTICIPATION_DEFAULT_S), 0.f, ANTICIPATION_MAX_S)
        : 0.f;
    if (charge_s > 0.f && strike.mT < -leader_s)
    {
        const F32 until = -leader_s - strike.mT;
        strike.mCharge = (until < charge_s) ? (1.f - until / charge_s) : 0.f;
        strike.mChargeHeld = llmax(strike.mChargeHeld, strike.mCharge);

        if (strike.mCharge > 0.f && !strike.mChargeSent)
        {
            strike.mChargeSent = true;
            SSSoundscape::getInstance()->playCharge(strike.mGround, strike.mIntensity);
        }
    }
    else
    {
        strike.mCharge = 0.f;
    }

    F32 brightness = 0.f;

    strike.mStrokeCount = 0;
    strike.mHit = 0.f;
    strike.mFire = 0.f;
    strike.mPlasmaSince = -1.f;
    strike.mLastGap = 0.f;

    static LLCachedControl<F32> fire_life_setting(gSavedSettings, "SSAtmoLightningGroundFireLife", 0.9f);
    static LLCachedControl<F32> late_setting(gSavedSettings, "SSAtmoLightningLateRestrike", 0.45f);
    static LLCachedControl<F32> plasma_setting(gSavedSettings, "SSAtmoLightningPlasma", 1.f);
    const F32 tau_f = llclamp((F32)fire_life_setting, 0.2f, 2.5f) / 2.5f;
    const F32 hit_tau = strike.mStrokeDecayS * 1.5f;

    if (strike.mT < -leader_s)
    {
        strike.mLeaderProgress = 0.f;
    }
    else if (strike.mT < 0.f)
    {
        strike.mLeaderProgress = 1.f - (-strike.mT / leader_s);
        brightness = (strike.mKind == STRIKE_SHEET) ? 0.f : LEADER_GLOW;

        if (brightness > 0.f)
        {
            strike.mStrokeCount = 1;
            strike.mStrokeAt[0] = 0.f;
            strike.mStrokeBright[0] = brightness;
            strike.mStrokeScale[0] = brightness;
            strike.mStrokeDrift[0] = 0.f;
        }
    }
    else
    {
        strike.mLeaderProgress = 1.f;

        // One fired stroke: its decayed glow into the channel, the flare and fire envelopes it feeds,
        // and its record for the renderer.
        auto fire_stroke = [&](F32 at_k, F32 scale_k, F32 gap_k, F32 drift_k)
        {
            if (strike.mT < at_k) return;
            const F32 since = strike.mT - at_k;
            const F32 glow = scale_k * expf(-since / strike.mStrokeDecayS);
            brightness += glow;

            if (strike.mStrokeCount < SSStrike::MAX_STROKES)
            {
                strike.mStrokeAt[strike.mStrokeCount] = at_k;
                strike.mStrokeBright[strike.mStrokeCount] = glow;
                strike.mStrokeScale[strike.mStrokeCount] = scale_k;
                strike.mStrokeDrift[strike.mStrokeCount] = drift_k;
                ++strike.mStrokeCount;
            }

            // The flare and the fire belong to a strike that reached the ground; a fork or a sheet has no contact to flare.
            if (strike.mKind == STRIKE_GROUND)
            {
                strike.mHit += scale_k * expf(-since / hit_tau) * sqrtf(llmin(1.f, since / 0.02f));
                const F32 rise = llmin(1.f, since / SSGroundShow::FIRE_RISE_S);
                const F32 fire_k = scale_k * rise * rise
                    * expf(-llmax(0.f, since - SSGroundShow::FIRE_RISE_S) / tau_f);
                strike.mFire = llmax(strike.mFire, fire_k);
            }
            strike.mPlasmaSince = since;
            strike.mLastGap = gap_k;
        };

        F32 at = 0.f;
        F32 gap = 0.f;
        F32 drift = 0.f;
        F32 last_at = 0.f;
        for (S32 i = 0; i < strokes; ++i)
        {
            const F32 scale = 1.f / (1.f + (F32)i * 0.6f);
            fire_stroke(at, scale, gap, drift);
            last_at = at;
            gap = rng.frand(strike.mRestrikeMinS, strike.mRestrikeMaxS);
            drift += llmin(gap, 0.09f);
            at += gap;
        }

        // <SS:Nexii> The late restrike: the recorded column re-lights 0.36s after impact, far outside the fast series, nearly as bright, over its own cooling plasma. Two unconditional rolls after the series keep every earlier draw where it was; the odds are the dial's, positive bolts (already a rapid series) take fewer.
        const F32 late_roll = rng.frand();
        const F32 late_gap = rng.frand(0.25f, 0.50f);
        const F32 late_odds = llclamp((F32)late_setting, 0.f, 1.f) * (strike.mPositive ? 0.65f : 1.f);
        if (late_roll < late_odds)
        {
            const F32 late_at = last_at + late_gap;
            fire_stroke(late_at, 0.75f, late_gap, drift + 0.03f);
            last_at = late_at;
        }

        // The stroke's honest exponential decay is only half the story: the tail after it is
        // the plasma the column cools into, the sparks landing, the ground fire burning down.
        // The strike lives through all of that - the longest of them plus a margin - before
        // retiring, so nothing is cut off mid-air.
        {
            const F32 dissolve = llclamp(settingF("SSAtmoLightningDissolve", 1.f), 0.f, 2.f);
            F32 tail_s = 0.f;
            if (dissolve > 0.f)
            {
                tail_s = SSDissolve::LAG_S + SSDissolve::SPAN_S / dissolve + 0.1f;
                tail_s += (plasma_setting > 0.f)
                    ? SSDissolve::PLASMA_S * SSDissolve::PLASMA_FOOT_MULT
                    : SSDissolve::EMBER_S / dissolve;
            }
            if (strike.mKind == STRIKE_GROUND)
            {
                tail_s = llmax(tail_s, SSGroundShow::FIRE_RISE_S + 3.5f * tau_f * 1.4f);
                F32 spark_tail = 0.f;
                for (const SSStrikeSpark& sp : strike.mSparks)
                {
                    spark_tail = llmax(spark_tail, sp.mT0 + ((sp.mHit > 0.f)
                        ? sp.mHit + SSGroundShow::SECONDARY_LIFE_S : sp.mLife));
                }
                tail_s = llmax(tail_s, spark_tail);

                // The far end of a long crawl blows its steam a beat after the foot did, so the retirement has to wait on the LAST blob, not the first - otherwise the crawl's own cloud is cut off mid-rise.
                if (strike.mSteamPeak > 0.f)
                {
                    F32 steam_tail = 0.f;
                    for (const SSStrikeSteam& sb : strike.mSteam)
                    {
                        if (sb.mWater > 0.f) steam_tail = llmax(steam_tail, sb.mDelay + SSGroundShow::STEAM_LIFE_S);
                    }
                    tail_s = llmax(tail_s, steam_tail);
                }
            }
            if (strike.mT > last_at + strike.mStrokeDecayS * 6.f + tail_s)
            {
                brightness = 0.f;
                strike.mHit = 0.f;
                strike.mFire = 0.f;
                strike.mDone = true;
                ss_release_strike_query(strike);
            }
        }
    }

    // During the dissolve tail the real channel is dark but keeps a whisper of presence so the
    // renderer stays live for the plasma and the ground show - the beam is gone, only its
    // afterglow remains, and it needs the channel pass running to draw.
    F32 channel_brightness = llclamp(brightness, 0.f, 1.f);
    if (channel_brightness <= 0.001f && !strike.mChannel.empty()
        && strike.mT >= 0.f && !strike.mDone)
    {
        channel_brightness = LEADER_GLOW * 0.02f;
    }
    strike.mChannelBrightness = channel_brightness;

    static LLCachedControl<bool> markers(gSavedSettings, "SSAtmoDebugStrikeMarkers", false);
    if (markers && strike.mT <= -MARKER_HIDE_S && !strike.mDone)
    {
        if (!strike.mDebugText)
        {
            strike.mDebugText = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
            if (strike.mDebugText)
            {
                strike.mDebugText->setDoFade(false);
                strike.mDebugText->setZCompare(false);
                strike.mDebugText->setColor(LLColor4(1.f, 0.25f, 0.9f, 1.f));
            }
        }
        if (strike.mDebugText)
        {
            // Ground strikes label at the attachment; sheet and fork live in the sky, so float the countdown at their centre - sheet at the flash origin, fork at the channel's centroid.
            LLVector3 label_pos = strike.mGround + LLVector3(0.f, 0.f, 4.f);
            if (strike.mKind == STRIKE_SHEET)
            {
                label_pos = strike.mOrigin;
            }
            else if (strike.mKind == STRIKE_FORK && !strike.mChannel.empty())
            {
                LLVector3 centre;
                for (const SSStrikeNode& node : strike.mChannel) centre += node.mPos;
                label_pos = centre / (F32)strike.mChannel.size();
            }
            strike.mDebugText->setPositionAgent(label_pos);
            strike.mDebugText->setColor(kindDebugColor(strike.mKind));
            strike.mDebugText->setString(llformat("%s strike in %.1fs%s%s",
                kindName(strike.mKind), -strike.mT,
                strike.mCharge > 0.f ? llformat("  charge %.0f%%", strike.mCharge * 100.f).c_str() : "",
                strike.mCrawlCount > 0 ? llformat("  crawl %.0fm", strike.mCrawlLenM).c_str() : ""));
        }
    }
    else
    {
        ss_kill_strike_text(strike);
    }

    const F32 falloff = 1.f / (1.f + (strike.mDistanceM / 4000.f) * (strike.mDistanceM / 4000.f));
    strike.mFlash = llclamp(brightness, 0.f, 1.f) * strike.mIntensity * falloff;

}
