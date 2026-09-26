/**
 * @file ssstormcells.cpp
 * @brief Atmo Magic: the storm-cell scheduler shell - see ssstormcells.h.
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

#include "ssstormcells.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvasset.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvweatherstate.h"
#include "ssatmomagic.h"
#include "ssdaycyclecore.h"
#include "sssquallcore.h"
#include "ssvortexcore.h" // 7f: heroContact's Parent/childVortex/contactOffsetM - a core may be included from this shell
#include "sswindprofilecore.h"

#include "llagent.h"
#include "llregionhandle.h" // <SS:Nexii> S2: from_region_handle - the source anchor's 256m-aligned origin when its region is not currently simulated
#include "llstring.h"
#include "llviewerregion.h"
#include "llworld.h"        // <SS:Nexii> S2: LLWorld::getRegionFromHandle - resolving the asset's noted source region

#include <algorithm>
#include <cmath>

namespace
{
    // The lattice within FIELD_M of the anchor: (2 * 12000 / 2900 + 2)^2 is under 110 cells; the cap is comfortably above.
    constexpr S32 LATTICE_CAP = 256;

    // <SS:Nexii> S6: resolveActive's output buffer size. Generous margin over the S7 density calibration's worst
    // measured concurrent count (extreme weather, mean 38.49 over 960 anchor-hours - see ssstormcellcore.h's
    // calibration comment); resolveActive silently drops any excess rather than overflow.
    constexpr S32 ACTIVE_CAP = 512;

    // <SS:Nexii> S12: the scheduler's consumer refcount (the SSWorldField::Interest idiom) - file-local since only
    // claim()/update() touch it.
    S32 sInterestCount = 0;

    // <SS:Nexii> SCHEDULER (doc/atmo_magic_storm_dynamics.md section 6 layer 3): the cube's authored kind STRING ->
    // SSSquall::ForcedOverride::mKind's small int (see sssquallcore.h's own doc comment: 0 none, 1 supercell,
    // 2 tornado, 3 waterspout-preferred, 4 anticyclonic). A fixed-vocabulary lookup, not a numeric formula - stays
    // shell-side the same way mPrecipitationOverride's kind string is read directly by its consumers.
    S32 stormOverrideKindFromString(const std::string& kind)
    {
        if (kind == "supercell") return 1;
        if (kind == "tornado") return 2;
        if (kind == "waterspout") return 3;
        if (kind == "anticyclonic") return 4;
        if (kind == "squall") return SSSquall::KIND_SQUALL; // 8a: an authored squall LINE, not a pinned cell - see the forced block's own comment
        return 0; // "none" and anything unrecognised
    }
}

// Singleton shell.
SSStormCells::SSStormCells()
{
}

// S12: hands out a ref-counted stake in the scheduler running; dropping the last one lets update() early-out.
SSStormCells::Interest SSStormCells::claim()
{
    ++sInterestCount;
    return Interest(std::shared_ptr<void>((void*)1, [](void*) { --sInterestCount; }));
}

// Drops the frame's outputs; the memo survives (it is pure) until its bucket or track changes.
void SSStormCells::clear()
{
    mValid = false;
    mCells.clear();
    mHaveHero = false;
    mHero = Hero();
    mWhyNot = WhyNot();
    mTrack = nullptr;
    mLineDescByLineId.clear(); // 8a: fillLineBand's lookup - dropped with everything else this frame's outputs
    mLineEventByLineId.clear(); // 8a audit F3: lineEventForId's lookup, same lifetime as mLineDescByLineId
}

// <SS:Nexii> FIX 1: the S2 source-region anchor rule, extracted out of update()'s own anchor block (previously
// inline there) so anchorNow() below and update() call the identical logic instead of two hand-kept copies of it -
// the SOURCE region's centre when the applied asset was noted from a parcel (SSAtmoEnvManager::sourceRegionHandle),
// else region's own centre (a personal preview: nothing broadcast this asset, so there is no sync domain to anchor
// on). HONEST LIMITATION: the source handle is the region gAgent stood in AT NOTE TIME, not a server-authoritative
// id - a neighbour region whose OWN parcel happens to reference the same asset re-notes ITS region when a client
// standing there discovers it, so that client's hero re-derives against a DIFFERENT anchor. Two clients in the SAME
// region agree; two clients in different regions sharing an asset by coincidence do not, until a server-authoritative
// anchor exists. mgr/region are the caller's own reads (never fetched here) so this stays a pure function of them.
// [interaction: SSAtmoEnvManager::sourceRegionHandle] [interaction: LLWorld::getRegionFromHandle]
SSStormCell::Vec2 SSStormCells::resolveAnchor(SSAtmoEnvManager* mgr, LLViewerRegion* region)
{
    SSStormCell::Vec2 out;
    LLViewerRegion* anchor_region = region;
    const U64 source_handle = mgr->sourceRegionHandle();
    if (source_handle != 0)
    {
        LLViewerRegion* source_region = LLWorld::instanceExists()
                                         ? LLWorld::getInstance()->getRegionFromHandle(source_handle) : nullptr;
        if (source_region)
        {
            anchor_region = source_region;
        }
        else
        {
            // Not currently simulated for this client (a neighbour, or the agent has since moved away): the
            // handle still gives its 256m-aligned origin, so approximate its centre at the legacy region width
            // rather than silently falling back to the agent's own region - a region the asset was never noted from.
            F32 ox, oy;
            from_region_handle(source_handle, &ox, &oy);
            out.x = ox + 0.5f * REGION_WIDTH_METERS;
            out.y = oy + 0.5f * REGION_WIDTH_METERS;
            anchor_region = nullptr;
        }
    }
    if (anchor_region)
    {
        const LLVector3d& origin = anchor_region->getOriginGlobal();
        const F64 half = 0.5 * (F64)anchor_region->getWidth();
        out.x = (F32)(origin.mdV[VX] + half);
        out.y = (F32)(origin.mdV[VY] + half);
    }
    return out;
}

// <SS:Nexii> FIX 1: resolveAnchor() above, callable without an Interest claim - SSVortices' dust-devil block needs
// the same weather-domain anchor cells() would use, but is a parentless mini-scheduler that must run even when
// nothing has claimed this scheduler (so update() never touched mAnchor this frame, or ever). Reads gAgent/mgr
// fresh every call rather than mAnchor: never memoised, never gates mValid/mCells, so a caller mixing this with a
// live claim still sees exactly this frame's anchor either way (both routes call the identical resolveAnchor).
// SSStormCell::Vec2() (world origin) when the agent has no region or Atmo has no asset - a caller must already be
// gating on those before trusting this for placement.
SSStormCell::Vec2 SSStormCells::anchorNow() const
{
    LLViewerRegion* region = gAgent.getRegion();
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    if (!region || !mgr->hasAsset())
    {
        return SSStormCell::Vec2();
    }
    return resolveAnchor(mgr, region);
}

// <SS:Nexii> D1 (7e audit): the anchorNow() idiom applied to the clock, now sharing latchCycleTime with update() (see
// its own comment) instead of re-solving the nearest occurrence of the previewed phase fresh every call - that used
// to hold tau piecewise-constant between day-cycle midpoint crossings under preview (a mislabelled comment here used
// to call it "continuous"). Real: sharedTime() minus the currently applied track's own day offset; with NO track
// applied it returns sharedTime() itself (7f F9: the old comment said 0.0, which would have put dust devils at the
// 1970 epoch - the code never did that).
F64 SSStormCells::cycleTimeNow() const
{
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    const F64 wall_now = SSAtmoMagic::getInstance()->sharedTime();

    const S32 track_index = applier->appliedTrackIndex();
    if (!mgr->hasAsset() || track_index < 0 || track_index >= (S32)mgr->asset().mTracks.size())
    {
        return wall_now;
    }
    const SSAtmoEnvTrack& track = mgr->asset().mTracks[(size_t)track_index];
    return latchCycleTime(mgr->hasPreviewPhaseOverride(), mgr->previewPhaseOverride(), track_index,
                           track.mDayLengthSeconds, track.mDayOffsetSeconds, wall_now);
}

// <SS:Nexii> D1 (7e audit): see the header comment for the full contract. Real mode is not latched at all - every
// caller, every frame, gets sharedTime() - dayOffsetS directly. Preview mode latches onto (mCycleRefS, mWallRefS)
// when previewOverride/previewPhase/trackIndex differ from what was last latched (mMemoPreviewOverride,
// mMemoPreviewPhase, mMemoTrack - D3: trackIndex shares the WeatherAtBirth memo's own track field rather than a
// second one), else holds the existing latch and flows at wall rate.
F64 SSStormCells::latchCycleTime(bool previewOverride, F64 previewPhase, S32 trackIndex,
                                  F64 dayLengthS, F64 dayOffsetS, F64 wallNow) const
{
    const F64 real_tau = wallNow - dayOffsetS;
    if (!previewOverride)
    {
        return real_tau;
    }
    const bool changed = previewOverride != mMemoPreviewOverride
                        || previewPhase != mMemoPreviewPhase
                        || trackIndex != mMemoTrack;
    if (changed)
    {
        mCycleRefS = (dayLengthS > 0.0) ? SSDayCycle::wallTimeAtPhase(previewPhase, real_tau, dayLengthS, 0.0) : real_tau;
        mWallRefS = wallNow;
        mMemoPreviewOverride = previewOverride;
        mMemoPreviewPhase = previewPhase;
    }
    return mCycleRefS + (wallNow - mWallRefS);
}

// <SS:Nexii> Phase 8 section 2 (one clock, user 2026-09-06): CYCLE time (tau) -> the applied track's day phase, one
// map, no preview branch - SSDayCycle::phaseAt(tau, mDayLengthS, 0.0), offset 0 because tau already had the
// track's own day offset subtracted (real mode: mNow = sharedTime() - mDayOffsetS, in update()). The editor's
// preview no longer swaps in a second map (SSStormCell::previewPhaseAt, removed) - it substitutes what tau IS, a
// latched clock (see update()'s mCycleRefS/mWallRefS), so scrubbing to an authored cue's phase still shows the
// forced storm mature at that moment, through the SAME map real time uses. Never mTrack itself: mTrack is a
// borrowed per-frame pointer, null outside update(), and the 7c opus check caught the public schedulerPhaseAt
// (called from the V2 view's draw path) dereferencing it. Pure in the captured mDayLengthS, safe at any time.
// [interaction: SSDayCycle::phaseAt]
F64 SSStormCells::phaseAt(F64 tau) const
{
    if (mDayLengthS <= 0.0)
    {
        return 0.0;
    }
    return SSDayCycle::phaseAt(tau, mDayLengthS, 0.0);
}

// <SS:Nexii> Phase 8 section 2: phaseAt's own inverse - see the header. One map, no preview branch: a forced-storm
// cue phase (read through phaseAt) converts to a tau through the identical map real time uses, never a second
// real/preview pair (superseding 7b F3's previewWallTimeAt-branching version, lesson 24).
F64 SSStormCells::wallTimeAtPhase(F64 phase, F64 nearTau) const
{
    if (mDayLengthS <= 0.0)
    {
        return nearTau;
    }
    return SSDayCycle::wallTimeAtPhase(phase, nearTau, mDayLengthS, 0.0);
}

// <SS:Nexii> The weather cube AT THE CANDIDATE'S BIRTH TIME, never at now: moisture and convection are the cube's own curves at the birth phase (the same valueAt reads SSAtmoEnvApplier::computeModulation makes for the deck base), the shear strength is the resolver's S at that phase (auto-derived or authored, SSAtmoEnvWeatherResolver::resolve), the Allow flags are the track's gated by the influence MASTER enable (S4, phase-2b audit: mWeatherInfluence.mEnabled off means no supercells or tornadoes at all, matching what the Weather Influence floater shows the author), and the anvil wind is SSWindProfile::windAt at the birth-phase profile's own anvil AGL (SSAtmoEnvApplier::windProfileAt - pure, never the live mWindProfile). [interaction: SSAtmoEnvWeatherResolver] [interaction: SSAtmoEnvApplier::windProfileAt]
const SSStormCells::BirthMemo& SSStormCells::birthMemo(const SSStormCell::Candidate& c)
{
    // 7b F5: keyed on SSStormCell::memoKey(c) (id ^ birth-quantised-to-1ms), not c.mId alone - a forced pin shares
    // its lattice twin's mId but carries a REWRITTEN birth time, so keying on mId alone let whichever variant
    // memoised first (within this 2 s memo bucket) serve the other, across every client.
    const U64 key = SSStormCell::memoKey(c);
    auto it = mMemo.find(key);
    if (it != mMemo.end())
    {
        return it->second;
    }

    const F64 phase = phaseAt(c.mBirthTime);
    const SSAtmoEnvWeatherState state = SSAtmoEnvWeatherResolver::resolve(mTrack->mWeather, phase);
    const bool influence_enabled = mTrack->mWeatherInfluence.mEnabled;

    BirthMemo memo;
    memo.mWeather.mMoisture        = llclamp(mTrack->mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
    memo.mWeather.mConvection      = llclamp(mTrack->mWeather.mConvection.valueAt(phase), 0.f, 1.f);
    memo.mWeather.mShearStrength   = llclamp(state.mShearStrength, 0.f, 1.f);
    memo.mWeather.mAllowSupercells = influence_enabled && mTrack->mWeatherInfluence.mAllowSupercells;
    memo.mWeather.mAllowTornadoes  = influence_enabled && mTrack->mWeatherInfluence.mAllowTornadoes;

    const SSWindProfile::Params profile = SSAtmoEnvApplier::windProfileAt(*mTrack, phase);
    const SSWindProfile::Vec2 wind = SSWindProfile::windAt(profile.mAnvilAglM, profile);
    memo.mAnvilWind.x = wind.x;
    memo.mAnvilWind.y = wind.y;

    return mMemo.emplace(key, memo).first->second;
}

// Global XY -> agent frame, via the agent's region origin. [interaction: gAgent]
LLVector2 SSStormCells::toAgentXY(const SSStormCell::Vec2& global_xy) const
{
    const LLVector3 a = gAgent.getPosAgentFromGlobal(LLVector3d((F64)global_xy.x, (F64)global_xy.y, 0.0));
    return LLVector2(a.mV[VX], a.mV[VY]);
}

// Agent frame -> global XY.
SSStormCell::Vec2 SSStormCells::fromAgentXY(const LLVector2& agent_xy) const
{
    const LLVector3d g = gAgent.getPosGlobalFromAgent(LLVector3(agent_xy.mV[0], agent_xy.mV[1], 0.f));
    SSStormCell::Vec2 v;
    v.x = (F32)g.mdV[VX];
    v.y = (F32)g.mdV[VY];
    return v;
}

// <SS:Nexii> Phase 3 (deck coupling): see the header - this is only the adapter, SSStormCouple::selectSlots decides
// the ranking. Global frame throughout the selection (mCells' mCentre and mAnchor are both global, so the anchor
// distances selectSlots ranks by are the same whichever frame they are read in - a uniform translation does not
// change a distance ordering); the AGENT conversion happens only for the cells actually kept, so a build with 40
// alive cells pays toAgentXY four times, not forty.
S32 SSStormCells::fillUniforms(SSStormCouple::CellUniform* out, S32 cap) const
{
    if (!out || cap <= 0)
    {
        return 0;
    }

    const S32 n = (S32)mCells.size();
    if (n <= 0)
    {
        return 0;
    }

    std::vector<SSStormCouple::SelectKey> keys((size_t)n);
    for (S32 i = 0; i < n; ++i)
    {
        const ActiveCell& c = mCells[(size_t)i];
        keys[(size_t)i].id = c.mCandidate.mId;
        keys[(size_t)i].x = c.mCentre.x;
        keys[(size_t)i].y = c.mCentre.y;
        // 7c NEW-6: an authored pin (mIsForced) is slot-preferred the same as the hero - it is guaranteed-active
        // by the same authoring intent a hero flyby is, so selectSlots must not bump it for an ordinary cell.
        // 7f F3: slot preference needs the flag AND reach - a 30 km hero holds no slot while it cannot influence the deck.
        // The same reach applies to an authored pin placed beyond it (7f re-check N5): it cannot influence the deck either.
        keys[(size_t)i].hero = SSStormCouple::slotPreferred(c.mIsHero || c.mIsForced, c.mCentre.x - mAnchor.x, c.mCentre.y - mAnchor.y,
                                                           FIELD_M + SSStormCell::RADIUS_MAX_M);
    }

    const S32 cap_eff = llmin(cap, SSStormCouple::MAX_CELLS);
    S32 idx[SSStormCouple::MAX_CELLS];
    const S32 picked = SSStormCouple::selectSlots(keys.data(), n, mAnchor.x, mAnchor.y, idx, cap_eff);

    for (S32 s = 0; s < picked; ++s)
    {
        const ActiveCell& c = mCells[(size_t)idx[s]];
        SSStormCouple::CellUniform& u = out[s];
        u = SSStormCouple::CellUniform();

        const LLVector2 agent_xy = toAgentXY(c.mCentre);
        u.x = agent_xy.mV[0];
        u.y = agent_xy.mV[1];
        u.radius = c.mRadiusM;
        // mLifecycle's fields are already scaled by mGate.mIntensity inside lifecycle() (its `k` parameter) - not
        // multiplied again here.
        u.boost = c.mLifecycle.mTowerBoost;
        u.anvil = c.mLifecycle.mAnvil;
        u.meso = c.mLifecycle.mMeso;
        u.overshoot = c.mLifecycle.mOvershoot;
        u.mammatus = c.mLifecycle.mMammatus;
        SSStormCouple::motionDirection(c.mMotion.x, c.mMotion.y, u.dirX, u.dirY);
        u.rotSign = SSStormCouple::rotSignOf(c.mCandidate.mRotation);
    }
    return picked;
}

// <SS:Nexii> 8a item 2 (doc/atmo_magic_phase8_show.md section 3, ssstormcouplecore.h's own LineBand comment): the
// active squall line's deck coupling, filled next to fillUniforms above and read the same way (AGENT frame, same
// toAgentXY conversion, so lineField's px/py agree with sampleAt's world_x/world_y at the SAME call sites). "The
// active line" (8a audit F2/F7) is whichever line has a currently-alive member AND mLineDescByLineId's mForced -
// an authored squall owns the deck's one line slot over any hashed line, even one with a lower id; among lines
// that agree on forced-ness, the LOWEST mLineId wins (one line at a time, per the design doc). mCells carries only
// the id per member, so the line's own geometry (origin/direction/windAnvil/halfLen) is looked up in
// mLineDescByLineId, populated by lineAtEpoch this same update(). Left at LineBand()'s own zero default (strength
// 0, which SSStormCouple::lineField reads as "disabled") when no line is alive, its LineDesc went missing (should
// not happen - defensive only), or the alive members' mean tower boost is 0.
void SSStormCells::fillLineBand(SSStormCouple::LineBand& out) const
{
    out = SSStormCouple::LineBand();

    bool have_line = false;
    bool best_is_forced = false;
    U64 line_id = 0;
    for (const ActiveCell& c : mCells)
    {
        if (c.mLineId == 0)
        {
            continue;
        }
        const auto it = mLineDescByLineId.find(c.mLineId);
        const bool forced = (it != mLineDescByLineId.end()) && it->second.mForced;
        if (!have_line || (forced && !best_is_forced) || (forced == best_is_forced && c.mLineId < line_id))
        {
            have_line = true;
            best_is_forced = forced;
            line_id = c.mLineId;
        }
    }
    if (!have_line)
    {
        return;
    }

    const auto it = mLineDescByLineId.find(line_id);
    if (it == mLineDescByLineId.end() || it->second.mMemberCount <= 0)
    {
        return; // defensive: should be unreachable - every mLineId in mCells came from a LineDesc lineAtEpoch stored
    }
    const SSStormCell::LineDesc& d = it->second;

    // 8a audit F7: strength is the MEAN over alive members of SSStormCell::lifecycle(...).mTowerBoost - a lifecycle
    // envelope (bell over each member's own life, see ssstormcellcore.h) rather than the gate's raw mIntensity, so
    // the wall grows in as its members mature and fades as they die instead of holding at full strength then
    // snapping to 0 the instant the last member's lifetime clock runs out.
    F32 sum_boost = 0.f;
    S32 n_alive = 0;
    for (const ActiveCell& c : mCells)
    {
        if (c.mLineId == line_id)
        {
            const SSStormCell::Lifecycle life = SSStormCell::lifecycle(
                c.mAge01, c.mGate.mIntensity, c.mCandidate.mRotation, c.mGate.mSupercell);
            sum_boost += life.mTowerBoost;
            ++n_alive;
        }
    }
    const F32 strength = (n_alive > 0) ? (sum_boost / (F32)n_alive) : 0.f;
    if (strength <= 0.f)
    {
        return;
    }

    // 8a audit F2 (no respelled multiply): the advected centre through the SAME functions a line member's own
    // motion/position use in resolveActive - stormMotion(windAnvil, 0.f, false) (a line member rides IN the line:
    // rotation 0, no supercell deviation, ssstormcellcore.h's own comment) and centreAt(origin, motion, birth, now).
    const F64 birth = d.mMembers[0].mBirthTime;
    const SSStormCell::Vec2 raw_mot = SSStormCell::stormMotion(d.mWindAnvil, 0.f, false);
    const SSStormCell::Vec2 centre_global = SSStormCell::centreAt(d.mOrigin, raw_mot, birth, mNow);
    const LLVector2 centre_agent = toAgentXY(centre_global);

    out.ox = centre_agent.mV[0];
    out.oy = centre_agent.mV[1];
    out.dirX = d.mDirection.x;
    out.dirY = d.mDirection.y;

    // unitOrNorth-STYLE, but zero (never north) when still - a stalled line has no "ahead": lineField's shelf term
    // is a function of `ahead = dot(p - o, mot)`, which is identically 0 for a zero vector, and a stalled gust
    // front has no shelf to project.
    const F32 speed = std::sqrt(raw_mot.x * raw_mot.x + raw_mot.y * raw_mot.y);
    if (speed < 1e-6f)
    {
        out.motX = 0.f;
        out.motY = 0.f;
    }
    else
    {
        out.motX = raw_mot.x / speed;
        out.motY = raw_mot.y / speed;
    }

    out.halfLen = d.mHalfLengthM;
    out.bandM = SSSquall::LINE_BAND_M;
    out.shelfM = SSSquall::LINE_SHELF_M;
    out.strength = strength;
}

// <SS:Nexii> 8a audit F3: plain lookups into this frame's own line maps (populated by lineAtEpoch, update()) -
// nullptr when lineId names no line this frame resolved. See the header's own comment.
const SSStormCell::LineDesc* SSStormCells::lineDescForId(U64 lineId) const
{
    const auto it = mLineDescByLineId.find(lineId);
    return (it != mLineDescByLineId.end()) ? &it->second : nullptr;
}
const SSSquall::LineEvent* SSStormCells::lineEventForId(U64 lineId) const
{
    const auto it = mLineEventByLineId.find(lineId);
    return (it != mLineEventByLineId.end()) ? &it->second : nullptr;
}

// <SS:Nexii> Phase 4: a plain scan of mCells for mIsHero - see the header note. Not memoised (mCells is already
// this frame's resolved set; a second walk of at most a few dozen entries costs nothing next to resolveActive).
bool SSStormCells::heroMotionAgeS(LLVector2& out_motion_ms, F32& out_age_s) const
{
    for (const ActiveCell& c : mCells)
    {
        if (c.mIsHero)
        {
            out_motion_ms = LLVector2(c.mMotion.x, c.mMotion.y);
            out_age_s = c.mAge01 * c.mCandidate.mLifetimeS;
            return true;
        }
    }
    return false;
}

// <SS:Nexii> S6/S12 (phase-2b audit): the per-frame re-derivation. Early-outs (S12) unless a consumer currently
// claims the scheduler (see Interest), before touching the anchor, the memo or the lattice at all - a consumer
// that is not claiming it should not pay this every frame. That consumer is not only a debug view: SSVolCloud's
// coupled deck build (buildDeck, phase 3) holds a claim for as long as weatherDeck() keeps building, and pays this
// cost the same as V2/V7 do. When claimed: resolve the anchor (S2), roll the memo bucket
// (S3), then SSStormCell::resolveActive runs the WHOLE enumerate/gate/sort/hero/cull pipeline - this function
// supplies only the two weather-cube hooks (birthMemo, memoised) and reads the result back; no enumerate, sort,
// cull or hero logic is respelled here. Nothing here reads a frame counter, a dt, a render setting or the eased
// SSAtmoMagic::mWind. S1 (ACCEPTED, phase-2b audit): appliedTrackIndex() above IS camera-derived - the camera's
// altitude band picks WHICH TRACK's storm world is observed, exactly as it picks the sky (SSAtmoEnvApplier); that
// is the one accepted camera input, and it is honest to name it, not to claim "nothing reads the camera" and mean
// it literally. It is never a ranking or positional input INSIDE a track's world: each track's storm world is a
// pure function of that track's cube, seed and clock, and the camera cannot select a cell, a hero or a position
// within it. Cross-client agreement of the resolved cell set is what scenario_storm_two_clients demonstrates for
// the core composition; the shell adds only the anchor and the cube reads.
void SSStormCells::update()
{
    using namespace SSStormCell;

    if (sInterestCount <= 0)
    {
        clear();
        mTrackIndex = -1;
        return;
    }

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    LLViewerRegion* region = gAgent.getRegion();

    const S32 track_index = applier->appliedTrackIndex();
    if (!atmo || !mgr->hasAsset() || track_index < 0
        || track_index >= (S32)mgr->asset().mTracks.size() || !region)
    {
        clear();
        mTrackIndex = -1;
        return;
    }

    mSeed = atmo->seed();
    mTrackIndex = track_index;
    mTrack = &mgr->asset().mTracks[(size_t)track_index];
    mDayLengthS = mTrack->mDayLengthSeconds;
    mDayOffsetS = mTrack->mDayOffsetSeconds; // 7c: captured so phaseAt/wallTimeAtPhase never touch mTrack (null outside update())
    mPreviewOverride = mgr->hasPreviewPhaseOverride();
    mPreviewPhase = mgr->previewPhaseOverride();

    // <SS:Nexii> D1/D3 (7e audit): mNow becomes CYCLE time (tau), not the raw wall clock - see the header's own doc
    // on now(). Real: tau = sharedTime() - mDayOffsetS (an asset constant, so every client sharing the asset
    // agrees). Preview: the slider substitutes the CLOCK, not the map - latchCycleTime (shared with cycleTimeNow(),
    // private, above) latches mCycleRefS/mWallRefS once, the instant the preview turns on, its phase changes, or
    // the track changes (D3: folded into preview_changed below, sharing the memo's own mTrackIndex != mMemoTrack
    // test rather than a second one - previously a track swap while previewing held the OLD track's latched tau
    // until the next unrelated phase nudge), and flows at wall rate thereafter, so scrubbing to an authored cue's
    // phase shows the forced storm mature at that instant and un-scrubbing lets it keep living. preview_changed is
    // captured from the OLD mMemoPreviewOverride/mMemoPreviewPhase/mMemoTrack BEFORE calling latchCycleTime (which
    // may overwrite the first two on this exact call) and reused below for the WeatherAtBirth memo-drop decision -
    // the memo-drop and the latch trigger share one "did it change" test, computed once.
    const bool preview_changed = mPreviewOverride != mMemoPreviewOverride
                                || (mPreviewOverride && mPreviewPhase != mMemoPreviewPhase)
                                || mTrackIndex != mMemoTrack;
    const F64 wall_now = atmo->sharedTime();
    mNow = latchCycleTime(mPreviewOverride, mPreviewPhase, mTrackIndex, mDayLengthS, mDayOffsetS, wall_now);
    mEpochNow = epochOf(mNow);

    // <SS:Nexii> S2 (phase-2b audit): the weather domain's anchor - resolveAnchor() below, extracted (FIX 1) so
    // anchorNow() can call the identical logic without a claim. See resolveAnchor's own comment for the
    // source-region rule and its honest limitation.
    mAnchor = resolveAnchor(mgr, region);

    // <SS:Nexii> D5 (7e audit): birthMemo is pure in (track, birth time) alone - a candidate's birth time already
    // maps to a fixed cube reading once the track (and its day offset/length) is fixed, and preview only changes
    // WHICH tau "now" is, never re-scores an already-memoised birth. It is dropped on a wall-clock bucket (so a
    // live cube edit shows within MEMO_BUCKET_S) and on a track change (birth times then map to a different cube's
    // curves - mTrackIndex != mMemoTrack, folded into preview_changed above). The preview-changed drop is kept for
    // one concrete reason: scrubbing the preview slider can jump mNow (and so which epochs are even alive) by
    // hours in one frame, and while that alone does not invalidate any ALREADY-memoised birth's reading, it does
    // mean the memo would otherwise accumulate entries for births the lattice will never revisit at the new tau
    // (harmless but unbounded until the next bucket edge) - dropping on the jump keeps the memo's size tied to one
    // bucket's worth of actual candidates rather than every phase ever scrubbed past in a session.
    const S64 memo_bucket = bucket(mNow, MEMO_BUCKET_S);
    if (memo_bucket != mMemoBucket || preview_changed)
    {
        mMemo.clear();
        mMemoBucket = memo_bucket;
        mMemoTrack = mTrackIndex;
        mMemoPreviewOverride = mPreviewOverride;
        mMemoPreviewPhase = mPreviewPhase;
    }

    // The two hooks resolveActive calls for every alive candidate - both route through the memoised cube read.
    auto weatherAtBirth = [this](const Candidate& c) -> WeatherAtBirth { return birthMemo(c).mWeather; };
    auto anvilWindAtBirth = [this](const Candidate& c) -> Vec2 { return birthMemo(c).mAnvilWind; };

    // <SS:Nexii> S13 (phase-2b re-audit fixup 3): the V2 "why not" readout's tallies and best-scoring-alive-candidate
    // search, collected from resolveActive's OWN enumerate/epoch/alive/gate walk via its diag hook - this used to be
    // a second, hand-rolled traversal beside this call (re-deriving candidate()/alive()/gate() for the same lattice
    // x epoch window); now there is exactly one. Captured by reference into the lambda below, read back into `why`
    // once resolveActive returns.
    bool have_best = false;
    F32 best_score = -1.f;
    Candidate best;
    WeatherAtBirth best_weather;
    S32 alive_count = 0;
    S32 spawned_count = 0;
    S32 supercell_count = 0;
    S32 tornado_count = 0;
    auto diag = [&](const Candidate& c, const WeatherAtBirth& w, const Gate& g)
    {
        ++alive_count;
        const F32 score = gateScore(c, w);
        if (!have_best || score > best_score || (score == best_score && c.mId < best.mId))
        {
            have_best = true;
            best_score = score;
            best = c;
            best_weather = w;
        }
        if (g.mSpawn) ++spawned_count;
        if (g.mSupercell) ++supercell_count;
        if (g.mTornadoEligible) ++tornado_count;
    };

    // <SS:Nexii> SCHEDULER (doc/atmo_magic_storm_dynamics.md sections 5-6): the squall-line hook resolveActive calls
    // once per epoch. "The epoch's phase" (both here and for severe below) is phaseAt(epoch start) - one weather-cube
    // read stands in for the whole epoch's line decision, the same single-sample-per-epoch discipline birthMemo
    // applies per-candidate; windAnvil is sampled at that same phase since a line's birth (inside lineEvent's own
    // hash) is not known until lineEvent runs. severe = SSWindProfile::consolidation(moisture, convection) at the
    // epoch's phase >= SSSquall::SEVERE_CONSOLIDATION_MIN (7c NEW-5: the named constant, not a literal 0.5f - the
    // V2 view's reconstruction reads the SAME constant) - the SAME consolidation figure ssvolcloud.cpp's builder
    // and SSStormCouple both read, never a second-guessed copy.
    const SSSquall::Vec2 anchorSq{ mAnchor.x, mAnchor.y };

    // 8a: this frame's own line geometry, rebuilt from scratch every update() exactly like mCells - lineAtEpoch
    // (below) repopulates it as resolveActive calls it; fillLineBand reads it after resolveActive returns.
    mLineDescByLineId.clear();
    mLineEventByLineId.clear(); // 8a audit F3: repopulated alongside mLineDescByLineId, same lambda, same keys

    // <SS:Nexii> 8a item 1 (doc/atmo_magic_phase8_show.md section 3): filled by the forced block below (which runs
    // after this lambda is captured by reference, but before resolveActive actually calls it) when the cube's
    // forced override is KIND_SQUALL. lineAtEpoch then substitutes SSSquall::forcedLine for squallEpoch - the ONE
    // epoch containing (cueTime - FORCED_LINE_LEAD_S) - in place of that epoch's hashed lineEvent decision; every
    // other epoch is unaffected. [interaction: sssquallcore.h forcedLine]
    bool squallActive = false;
    S64 squallEpoch = 0;
    SSSquall::ForcedOverride squallOv;
    SSSquall::Vec2 squallWindAnvil;

    auto lineAtEpoch = [this, &anchorSq, &squallActive, &squallEpoch, &squallOv, &squallWindAnvil](S64 epoch) -> SSStormCell::LineDesc
    {
        SSStormCell::LineDesc d;

        // 8a item 1: an authored squall's own epoch is decided by its cue, not the hash, and is gated on the
        // Weather Influence MASTER enable ALONE - the Squall Lines checkbox (tested below) gates only the
        // SPONTANEOUS hashed line, never an authored cue (doc section 3 item 1: "the Squall Lines influence flag
        // does NOT gate an authored line - the master enable does").
        if (squallActive && epoch == squallEpoch && mTrack->mWeatherInfluence.mEnabled)
        {
            const SSSquall::LineEvent e = SSSquall::forcedLine(mSeed, squallOv, anchorSq, squallWindAnvil);
            d.mIsLine = e.mIsLine;
            if (!d.mIsLine)
            {
                return d;
            }
            d.mLineId = e.mLineId;
            d.mOrigin = SSStormCell::Vec2{ e.mOrigin.x, e.mOrigin.y };
            d.mDirection = SSStormCell::Vec2{ e.mDirection.x, e.mDirection.y };
            d.mHalfLengthM = SSSquall::LINE_LENGTH_M * 0.5f;
            d.mSuppressRadiusM = SSSquall::LINE_SUPPRESS_M;
            d.mWindAnvil = SSStormCell::Vec2{ squallWindAnvil.x, squallWindAnvil.y };
            d.mMemberCount = llmin((S32)SSSquall::LINE_MEMBERS_MAX, SSStormCell::LINE_MEMBERS_CAP);
            for (S32 i = 0; i < d.mMemberCount; ++i)
            {
                d.mMembers[i] = SSSquall::lineMember(e, i);
            }
            // 8a audit F1/F2: forcedLine sets mForced/mHasFloor/mWeatherFloor on the LineEvent (the weather floor an
            // authored pin gets, without which the generator's deliberately dry pre-cue sky gates every member out) -
            // carry them onto the LineDesc the scheduler actually gates members through (resolveActive reads
            // line.mHasFloor/mWeatherFloor, never the LineEvent itself) and let fillLineBand prefer this line over a
            // hashed one.
            d.mForced = e.mForced;
            d.mHasFloor = e.mHasFloor;
            d.mWeatherFloor = e.mWeatherFloor;
            mLineDescByLineId[d.mLineId] = d; // 8a: fillLineBand's own lookup - ActiveCell carries only the id
            mLineEventByLineId[d.mLineId] = e; // 8a audit F3: the full event, for a reconstruction's qlcsJunctionAt
            return d;
        }

        // 7b F11: authored track state, deterministic (never epoch-hashed) - the Weather Influence master enable
        // AND the Squall Lines flag both have to be on, same influence_enabled gate birthMemo applies to the two
        // Allow flags. Off means every epoch resolves discrete cells only, matching the floater's tooltip.
        if (!(mTrack->mWeatherInfluence.mEnabled && mTrack->mWeatherInfluence.mSquallLines))
        {
            return d;
        }

        const F64 phase = phaseAt((F64)epoch * SSStormCell::EPOCH_S);
        const F32 moisture = llclamp(mTrack->mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
        const F32 convection = llclamp(mTrack->mWeather.mConvection.valueAt(phase), 0.f, 1.f);
        const bool severe = SSWindProfile::consolidation(moisture, convection) >= SSSquall::SEVERE_CONSOLIDATION_MIN;

        const SSWindProfile::Params profile = SSAtmoEnvApplier::windProfileAt(*mTrack, phase);
        const SSWindProfile::Vec2 wind = SSWindProfile::windAt(profile.mAnvilAglM, profile);
        const SSSquall::Vec2 windAnvil{ wind.x, wind.y };

        const SSSquall::LineEvent e = SSSquall::lineEvent(mSeed, epoch, anchorSq, windAnvil, severe);
        d.mIsLine = e.mIsLine;
        if (!d.mIsLine)
        {
            return d;
        }
        d.mLineId = e.mLineId;
        d.mOrigin = SSStormCell::Vec2{ e.mOrigin.x, e.mOrigin.y };
        d.mDirection = SSStormCell::Vec2{ e.mDirection.x, e.mDirection.y };
        d.mHalfLengthM = SSSquall::LINE_LENGTH_M * 0.5f;
        // 7b F6: how close a discrete lattice draw must be to the line segment before it is replaced by the line -
        // LINE_SUPPRESS_M (== LINE_SPACING_M), not the whole line's length; farther draws in the same epoch survive.
        d.mSuppressRadiusM = SSSquall::LINE_SUPPRESS_M;
        // 7b F2: the ONE anvil-wind sample this epoch's line was decided with (the same `windAnvil` fed to
        // lineEvent above) - every member's stormMotion must derive from this single sample, never a per-member
        // birth-phase read (lesson 21).
        d.mWindAnvil = SSStormCell::Vec2{ windAnvil.x, windAnvil.y };
        d.mMemberCount = llmin((S32)SSSquall::LINE_MEMBERS_MAX, SSStormCell::LINE_MEMBERS_CAP);
        for (S32 i = 0; i < d.mMemberCount; ++i)
        {
            d.mMembers[i] = SSSquall::lineMember(e, i);
        }
        // 8a audit F1/F2: a hashed line's LineEvent leaves mForced/mHasFloor false and mWeatherFloor default -
        // copied explicitly (not left to LineDesc's own default) so both branches agree on WHERE this comes from.
        d.mForced = e.mForced;
        d.mHasFloor = e.mHasFloor;
        d.mWeatherFloor = e.mWeatherFloor;
        mLineDescByLineId[d.mLineId] = d; // 8a: fillLineBand's own lookup - ActiveCell carries only the id
        mLineEventByLineId[d.mLineId] = e; // 8a audit F3: the full event, for a reconstruction's qlcsJunctionAt
        return d;
    };

    // <SS:Nexii> SCHEDULER: the cube's forced-storm override keyframes (kind, cue phase, track-floor-relative XY
    // offset), all read at the CURRENT phase (never a candidate's birth phase - an authored cue is one wall-clock
    // instant, not an epoch-keyed draw). The cue phase converts to a wall-clock cueTime via THIS SHELL's own
    // wallTimeAtPhase (7b F3) - phaseAt's inverse, mirroring its preview/real switch, never always
    // SSAtmoEnvTrack::wallTimeAtPhase regardless of whether a preview override is active, so the cue converts
    // through the SAME map phaseNow below was read through. Reference "now" so every client landing on the same
    // wall clock resolves the SAME nearest occurrence. SSSquall::forcedCandidate then pins the lattice cell
    // nearest (anchor + offset); ForcedDesc::mPreferHero is set only for kind "tornado" (SSStormCell::resolveActive
    // then prefers this candidate as the hero over the ordinary ascending-id search - see its own comment).
    // 7c NEW-3: the WHOLE block is gated on mTrack->mWeatherInfluence.mEnabled - the master switch, not the two
    // Allow checkboxes: an authored cue still outranks Allow Supercells/Tornadoes (the floor above grants both
    // regardless), but with Weather Influence itself off there is no forced pin at all. Master off leaves
    // forcedDesc at its default (mPinned false), matching the kind-combo tooltip.
    SSStormCell::ForcedDesc forcedDesc;
    if (mTrack->mWeatherInfluence.mEnabled)
    {
        const F64 phaseNow = phaseAt(mNow);
        SSSquall::ForcedOverride ov;
        ov.mKind = stormOverrideKindFromString(mTrack->mWeather.mStormOverride.valueAt(phaseNow));
        if (ov.mKind != 0)
        {
            ov.mActive = true;
            const F32 cuePhase = mTrack->mWeather.mStormOverridePhase.valueAt(phaseNow);
            ov.mCueTime = wallTimeAtPhase((F64)cuePhase, mNow);
            ov.mOffsetM.x = mTrack->mWeather.mStormOverrideOffsetXM.valueAt(phaseNow);
            ov.mOffsetM.y = mTrack->mWeather.mStormOverrideOffsetYM.valueAt(phaseNow);
        }

        // 8a item 1 (doc/atmo_magic_phase8_show.md section 3): KIND_SQUALL is a LINE, not a pinned cell - do NOT
        // build a ForcedDesc pin for it (forcedDesc stays default/unpinned, so forcedAt's caller sees "no forced
        // cell" exactly as it would with the override off). Instead hand lineAtEpoch (above, captured by
        // reference) the cue and the ONE anvil-wind sample the line is decided with, at phaseAt(cueTime -
        // FORCED_LINE_LEAD_S) - the same single-sample-per-epoch discipline the hashed line branch already
        // follows, read through the SAME wind-profile call (SSAtmoEnvApplier::windProfileAt / SSWindProfile::windAt).
        // 3b F11 (doc section 3 item 4b), corrected 8a audit F5: mPreferHero is left false here exactly as "no
        // override" would leave it - resolveActive already skips the spontaneous hero search on its own while any
        // line member is alive (a composed hero would be dragged into the band the line's suppression keeps clear).
        // The earlier version of this comment claimed an authored tornado cue "still wins... cued independently of
        // the squall" - false: mStormOverride is a single HOLD string curve (ssatmoenvasset.h), so ov.mKind is
        // exactly ONE value at phaseNow and this if/else is mutually exclusive - a squall and a tornado cue can
        // never be active at the same phase, so there is no coexistence to win. The false claim mattered only as a
        // comment; the code below already does the right thing (skips this whole pinned-candidate branch while
        // KIND_SQUALL is the active kind).
        if (ov.mActive && ov.mKind == SSSquall::KIND_SQUALL)
        {
            squallActive = true;
            squallOv = ov;
            squallEpoch = SSStormCell::epochOf(ov.mCueTime - SSSquall::FORCED_LINE_LEAD_S);
            const F64 windPhase = phaseAt(ov.mCueTime - SSSquall::FORCED_LINE_LEAD_S);
            const SSWindProfile::Params profile = SSAtmoEnvApplier::windProfileAt(*mTrack, windPhase);
            const SSWindProfile::Vec2 wind = SSWindProfile::windAt(profile.mAnvilAglM, profile);
            squallWindAnvil = SSSquall::Vec2{ wind.x, wind.y };
        }
        else
        {
            const SSSquall::Pinned pinned = SSSquall::forcedCandidate(mSeed, ov, anchorSq);
            forcedDesc.mPinned = pinned.mPinned;
            if (forcedDesc.mPinned)
            {
                forcedDesc.mCandidate = pinned.mCandidate;
                forcedDesc.mWeatherFloor.mMoisture = pinned.mWeatherFloor.mMoisture;
                forcedDesc.mWeatherFloor.mConvection = pinned.mWeatherFloor.mConvection;
                forcedDesc.mWeatherFloor.mShearStrength = pinned.mWeatherFloor.mShearStrength;
                forcedDesc.mWeatherFloor.mAllowSupercells = pinned.mWeatherFloor.mAllowSupercells;
                forcedDesc.mWeatherFloor.mAllowTornadoes = pinned.mWeatherFloor.mAllowTornadoes;
                forcedDesc.mPreferHero = (ov.mKind == 2); // 2 == tornado, sssquallcore.h's ForcedOverride::mKind
                forcedDesc.mKind = ov.mKind;              // 7d: reaches SSVortex::childVortex through ActiveCell::mForcedKind
                // 7b F1: composeForced (ssstormcellcore.h) reads these two off ForcedDesc directly - without them the
                // pinned candidate is placed with a zero offset and cueTime 0.0 regardless of what was authored.
                forcedDesc.mOffsetM = SSStormCell::Vec2{ ov.mOffsetM.x, ov.mOffsetM.y };
                forcedDesc.mCueTime = ov.mCueTime;
            }
        }
    }
    auto forcedAt = [&forcedDesc]() -> SSStormCell::ForcedDesc { return forcedDesc; };

    // <SS:Nexii> 7f item 1 (doc/atmo_magic_phase8_show.md section 3c): the FUNNEL's contact passes the anchor, not
    // the cell centre - resolveActive calls this on the hero candidate (pre-composition) at HERO_CLOSEST_AGE01, so
    // composeHero can subtract the offset from the closest point BEFORE placing the origin. Builds an
    // SSVortex::Parent from the hero ActiveCell AT THE CLOSEST AGE (never "now": mRadiusM/mLife are resolved at
    // closestAge01 with SSStormCell::radiusAt/lifecycle, the same two functions resolveActive's own final loop
    // calls at "now" for every other cell - just evaluated at a different age here), runs slot 0 through
    // SSVortex::childVortex, and returns its contact offset (SSVortex::contactOffsetM) when that slot resolved to a
    // real funnel kind (SSVortex::hasFunnel), else zero - a GUSTNADO's mOffsetFrac is a ring share, not a funnel
    // contact (see hasFunnel's own comment). Pure in its inputs (seed, the hero ActiveCell, closestAge01): no now(),
    // no camera.
    auto heroContact = [seed = mSeed](const SSStormCell::ActiveCell& hero, F32 closestAge01) -> SSStormCell::Vec2
    {
        SSVortex::Parent p;
        p.mId = hero.mCandidate.mId;
        p.mBirthTime = hero.mCandidate.mBirthTime;
        p.mLifetimeS = hero.mCandidate.mLifetimeS;
        p.mIntensity = hero.mGate.mIntensity;
        p.mRotation = hero.mCandidate.mRotation;
        p.mPotential = hero.mCandidate.mPotential;
        p.mSupercell = hero.mGate.mSupercell;
        p.mTornadoEligible = hero.mGate.mTornadoEligible;
        p.mRadiusM = SSStormCell::radiusAt(closestAge01, hero.mGate.mIntensity);
        p.mLife = SSStormCell::lifecycle(closestAge01, hero.mGate.mIntensity, hero.mCandidate.mRotation, hero.mGate.mSupercell);
        p.mAllowTornadoes = hero.mWeather.mAllowTornadoes;
        p.mForcedKind = hero.mForcedKind;
        p.mIsHero = true;
        p.mClosestAge01 = closestAge01;

        const SSVortex::Candidate c0 = SSVortex::childVortex(seed, p, 0);
        if (!SSVortex::hasFunnel(c0.mKind))
        {
            return SSStormCell::Vec2();
        }
        const SSVortex::Vec2 off = SSVortex::contactOffsetM(c0, p.mRadiusM);
        return SSStormCell::Vec2{ off.x, off.y };
    };

    ActiveCell resolved[ACTIVE_CAP];
    SSStormCell::Hero heroPath; // qualified: SSStormCells::Hero (this class's own nested type) would shadow it otherwise
    S32 heroIndex = -1;
    const S32 n = resolveActive(mSeed, mNow, mAnchor, FIELD_M, weatherAtBirth, anvilWindAtBirth,
                                 resolved, ACTIVE_CAP, &heroPath, &heroIndex, diag, lineAtEpoch, forcedAt, heroContact);

    mCells.assign(resolved, resolved + n);
    mHaveHero = heroIndex >= 0;
    if (mHaveHero)
    {
        const ActiveCell& hc = mCells[(size_t)heroIndex];
        mHero.mId = hc.mCandidate.mId;
        mHero.mBirthTime = hc.mCandidate.mBirthTime;
        mHero.mLifetimeS = hc.mCandidate.mLifetimeS;
        mHero.mPath = heroPath;
        mHero.mDeath = centreAt(heroPath.mOrigin, heroPath.mMotion,
                                mHero.mBirthTime, mHero.mBirthTime + (F64)mHero.mLifetimeS);
    }
    else
    {
        mHero = Hero();
    }

    // <SS:Nexii> S9/S13 (phase-2b re-audit fixup 3): the V2 "why not" readout, built entirely from the diag hook's
    // tallies above - no second candidate/epoch/alive/gate walk. mLatticeCount (S9, exposed to the V7 console) still
    // needs its own enumerateLattice call since resolveActive's internal enumeration is not returned to the caller,
    // but that is lattice geometry, not a candidate traversal; mCandidates is arithmetic (lattice cells x epochs in
    // the window), since every (cell, epoch) pair is a candidate slot whether or not it turned out alive.
    WhyNot why;
    {
        S32 lx[LATTICE_CAP];
        S32 ly[LATTICE_CAP];
        mLatticeCount = llmin(enumerateLattice(mAnchor, FIELD_M, lx, ly, LATTICE_CAP), LATTICE_CAP);

        const S64 epoch_first = epochOf(mNow - (F64)LIFE_MAX_S);
        const S64 epoch_last = mEpochNow;
        why.mNextEpochInS = (F64)(epoch_last + 1) * EPOCH_S - mNow;
        why.mCandidates = mLatticeCount * (S32)(epoch_last - epoch_first + 1);
        why.mAlive = alive_count;
        why.mSpawned = spawned_count;
        why.mSupercells = supercell_count;
        why.mTornadoEligible = tornado_count;

        // The "why not" readout for the best-scoring alive candidate.
        if (have_best)
        {
            const Candidate& c = best;
            const WeatherAtBirth& w = best_weather;
            why.mHaveCandidate = true;
            why.mId = c.mId;
            why.mBirthTime = c.mBirthTime;
            why.mAge01 = age01(c, mNow);
            why.mScore = best_score;
            why.mPotential = c.mPotential;
            why.mConvection = w.mConvection;
            why.mMoisture = w.mMoisture;
            why.mShearNoise = c.mShearNoise;
            why.mShearStrength = w.mShearStrength;
            why.mRotation = c.mRotation;
            why.mRotationTerm = rotationTerm(c, w);
            why.mLifetimeS = c.mLifetimeS;
            why.mAllowSupercells = w.mAllowSupercells;
            why.mAllowTornadoes = w.mAllowTornadoes;
            why.mGate = gate(c, w);

            if (!mHaveHero)
            {
                if (!why.mGate.mSpawn)
                {
                    why.mFailing.push_back(llformat("score %.3f < %.2f (P %.2f x conv %.2f x moist %.2f x (0.5 + 0.5 x SH %.2f))",
                                                    why.mScore, SPAWN_THRESHOLD, why.mPotential, why.mConvection,
                                                    why.mMoisture, why.mShearNoise));
                }
                else if (!why.mGate.mSupercell)
                {
                    if (!w.mAllowSupercells)
                    {
                        why.mFailing.push_back("Allow Supercells off");
                    }
                    else
                    {
                        why.mFailing.push_back(llformat("|rotation| %.2f x S %.2f = %.3f < %.2f",
                                                        std::fabs(why.mRotation), why.mShearStrength,
                                                        why.mRotationTerm, SUPERCELL_ROT_MIN));
                    }
                }
                else if (!why.mGate.mTornadoEligible)
                {
                    if (!w.mAllowTornadoes)
                    {
                        why.mFailing.push_back("Allow Tornadoes off");
                    }
                    else
                    {
                        why.mFailing.push_back(llformat("lifetime %.0f s < %.0f s", why.mLifetimeS, HERO_MIN_LIFE_S));
                    }
                }
                why.mFailing.push_back(llformat("next epoch in %.1f min", why.mNextEpochInS / 60.0));
            }
        }
        else
        {
            why.mFailing.push_back("no candidate alive in the window");
            why.mFailing.push_back(llformat("next epoch in %.1f min", why.mNextEpochInS / 60.0));
        }
    }
    mWhyNot = why;

    mTrack = nullptr;
    mValid = true;
}
