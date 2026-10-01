/**
 * @file ssstormcells.h
 * @brief Atmo Magic: the storm-cell scheduler shell - lattice candidates, weather-at-birth gate, hero flyby, per-frame cell set.
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

#ifndef SS_STORMCELLS_H
#define SS_STORMCELLS_H

#include "llsingleton.h"
#include "sssquallcore.h" // 8a audit F3: lineEventForId's return type (SSSquall::LineEvent) - a shell may include a core
#include "ssstormcellcore.h"
#include "ssstormcouplecore.h"
#include "v2math.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct SSAtmoEnvTrack;
class SSAtmoEnvManager;
class LLViewerRegion;

// <SS:Nexii> The SHELL of doc/atmo_magic_storm_dynamics.md section 2: every frame the active storm cells are re-derived from scratch - no persisted sim state, no dt, no integrator. Inputs to every positional output, listed per the phase-1 lesson: the shared seed (SSAtmoMagic::seed), the wall clock (SSAtmoMagic::sharedTime), the weather domain's anchor (S2, phase-2b: the source region of the applied asset's from_parcel note when it has one, else the agent's own region - see update()'s anchor block for the honest limitation), the applied track's weather cube and dome-height keyframes evaluated at each candidate's BIRTH phase, and the track's two Allow flags gated by the influence master enable (S4). The camera never enters: it picks the TRACK (as it does for the sky itself, an altitude band - S1) and nothing else; each track's storm world is a pure function of that track's cube, seed and clock. Positions are GLOBAL-frame metres (region origin global + local), because the agent frame re-bases on every region crossing and a 2900m lattice hashed in it would give two clients in neighbouring regions different cells; consumers convert at the read site with toAgentXY (the same frame SSVolCloud positions puffs in). No rendering and no deck coupling here - phase 3 reads cells() from the deck builder; this phase exists so the V2/V7 debug views can show cells and a hero flyby. S6 (phase-2b): the enumerate/gate/sort/hero/cull pipeline itself lives in SSStormCell::resolveActive (ssstormcellcore.h) - this shell is an ADAPTER that builds the two weather-cube hooks and reads the result; cells() is directly the core's ActiveCell type.
class SSStormCells : public LLSingleton<SSStormCells>
{
    LLSINGLETON(SSStormCells);
    ~SSStormCells() = default;

public:
    // How far out from the anchor the lattice is enumerated (12 km, the visible field); cells whose closed-form centre
    // has drifted past this plus RADIUS_MAX_M are culled - by radius from the anchor, never by camera distance.
    static constexpr F32 FIELD_M = 12000.f;

    // <SS:Nexii> S6: directly the core's resolved-cell POD (SSStormCell::ActiveCell) - the shell adds nothing to it.
    // mCandidate.mId is the ranking key cells() is sorted ascending on; mCandidate.mOriginXY is always the LATTICE
    // draw even for the hero (whose mOrigin/mMotion are overwritten by composeHero - compare the two to see the offset).
    using ActiveCell = SSStormCell::ActiveCell;

    struct Hero
    {
        U64 mId = 0;
        F64 mBirthTime = 0.0;
        F32 mLifetimeS = 0.f;
        SSStormCell::Hero mPath;              // origin, motion, closest point/time against the anchor
        SSStormCell::Vec2 mDeath;             // centre at birth + lifetime
    };

    // <SS:Nexii> The V2 "why not" readout (doc/atmo_magic_debug_views.md): the alive candidate with the best gate score this frame, every number the gate decided on, and the terms it failed, spelled with live values. Formation debugging without reading code; when a hero exists mFailing is empty.
    struct WhyNot
    {
        bool mHaveCandidate = false;
        U64 mId = 0;
        F64 mBirthTime = 0.0;
        F32 mAge01 = 0.f;
        F32 mScore = 0.f;                     // SSStormCell::gateScore
        F32 mPotential = 0.f;
        F32 mConvection = 0.f;
        F32 mMoisture = 0.f;
        F32 mShearNoise = 0.f;
        F32 mShearStrength = 0.f;
        F32 mRotation = 0.f;
        F32 mRotationTerm = 0.f;              // SSStormCell::rotationTerm
        F32 mLifetimeS = 0.f;
        bool mAllowSupercells = false;
        bool mAllowTornadoes = false;
        SSStormCell::Gate mGate;
        std::vector<std::string> mFailing;    // human-readable failing terms, in gate order
        F64 mNextEpochInS = 0.0;              // seconds until the next epoch's candidates start being born
        S32 mCandidates = 0;                  // lattice cells x epochs in the window this frame
        S32 mAlive = 0;
        S32 mSpawned = 0;
        S32 mSupercells = 0;
        S32 mTornadoEligible = 0;
    };

    // <SS:Nexii> S12 (phase-2b audit): a consumer's stake in the scheduler running at all. Ref-counted (the
    // SSWorldField::Interest idiom); while nothing holds one, update() early-outs to an empty state before touching
    // the anchor, the memo or the lattice - a debug view that is not open should not pay this every frame. A handle
    // rather than a bool so a view that closes (or a mode that switches away) drops its claim automatically.
    class Interest
    {
    public:
        Interest() = default;
        explicit operator bool() const { return mHold != nullptr; }
    private:
        friend class SSStormCells;
        explicit Interest(std::shared_ptr<void> hold) : mHold(std::move(hold)) {}
        std::shared_ptr<void> mHold;
    };
    Interest claim();

    // Ticked from SSAtmoMagic::idle() right before SSVolCloud::update(). Reads the shared clock, never a dt. A no-op
    // (see Interest above) unless something currently claims the scheduler.
    void update();

    // False when nothing drives (Atmo off, no asset, no region): cells() is then empty and hero() null.
    bool valid() const { return mValid; }

    const std::vector<ActiveCell>& cells() const { return mCells; }
    const Hero* hero() const { return mHaveHero ? &mHero : nullptr; }
    const WhyNot& whyNot() const { return mWhyNot; }

    // <SS:Nexii> Phase 4 (doc/atmo_magic_storm_dynamics.md section 3, "Storm motion vs cloud drift"): the hero
    // ActiveCell's own mMotion (m/s, closed-form, frame-invariant under the agent frame's pure translation - no
    // toAgentXY needed for a velocity) and elapsed age in seconds (mAge01 * mCandidate.mLifetimeS, the same
    // denormalisation lifecycle() itself uses internally) - what SSDeckFrame::HeroFrame needs beyond the centre/
    // radius fillUniforms already resolves into mStormCells[0]. A lookup adapter only (no formula beyond that one
    // fraction*total unit conversion): false and both outputs untouched when no hero is currently active.
    bool heroMotionAgeS(LLVector2& out_motion_ms, F32& out_age_s) const;

    // <SS:Nexii> Phase 8 section 2 (one clock, user 2026-09-06): CYCLE time (tau) - real mode: sharedTime() minus
    // the applied track's own day offset, an asset constant so it is identical on every client sharing the asset
    // (true sync, no extra protocol); under the editor's preview override: a latched clock (see update()'s
    // mCycleRefS/mWallRefS) that equals the previewed phase's own tau at the latch instant and then advances at
    // wall rate. Every storm-STATE read runs on this - which cells exist, their ages, positions, the forced cue,
    // the epoch, the funnel's life - nothing here is SSAtmoMagic::sharedTime() any more. Animation (puff drift,
    // funnel rotation texture, debris, lightning buckets, sound) stays on sharedTime() and is never this. The
    // frame's shared inputs, for the V7 sync console.
    F64 now() const { return mNow; }
    U32 seed() const { return mSeed; }
    S32 trackIndex() const { return mTrackIndex; }
    const SSStormCell::Vec2& anchor() const { return mAnchor; }   // global metres
    S64 currentEpoch() const { return mEpochNow; }
    S32 latticeCount() const { return mLatticeCount; }

    // <SS:Nexii> FIX 1: the S2 source-region anchor logic (see the class comment and update()'s own anchor block),
    // available to a caller that holds no Interest and so never sees update() run - SSVortices' dust-devil block is
    // parentless and reads no resolved cell, but it still needs the SAME weather-domain anchor cells() would use, so
    // this exposes the identical computation (resolveAnchor, shared with update() so the two can never disagree)
    // without requiring a claim, without touching mAnchor/mValid/mCells, and without memoising anything. Global
    // metres, SSStormCell::Vec2() (world origin) when the agent has no region or Atmo has no asset - a caller must
    // already be gating on those before trusting this for placement.
    SSStormCell::Vec2 anchorNow() const;

    // <SS:Nexii> D1/D2 (7e audit, 2026-09-06): the SAME cycle-time (tau) formula now()/mNow uses, callable without a
    // claim - the anchorNow() idiom applied to the clock. SSVortices' dust-devil block is parentless (needs no
    // SSStormCells resolution, see its own FIX 1 comment) but still wants its epochs/ages on the shared clock,
    // never the raw wall clock. Real: sharedTime() minus the CURRENTLY applied track's own day offset (0 when no
    // track is applied). Preview: goes through the SAME latch-or-hold helper update() uses (latchCycleTime,
    // private, below) rather than re-resolving the nearest occurrence of the previewed phase fresh every call - the
    // old version did that, and since the "nearest occurrence" only changes at a day-cycle midpoint crossing, tau
    // held PIECEWISE CONSTANT between crossings instead of flowing (a mislabelled comment here used to call that
    // "continuous"; it was not - a parentless consumer's age/epoch math stood still under preview, the same bug 7d
    // fixed for the hero's own cue). Sharing the helper means this self-seeds the latch if update() has never run
    // this session, and holds the SAME latch (rather than re-latching) once it has - a caller inside a live claim
    // (cells->valid()) may still prefer now(), which is identical when both are driven by the same applied track.
    F64 cycleTimeNow() const;

    // <SS:Nexii> 7c NEW-2, rewritten for phase 8 section 2 (one clock, user 2026-09-06): phaseAt (private, below),
    // exposed read-only so a caller that reconstructs the scheduler's own decisions (SSAtmoInfoView::stormCellsData's
    // squall-line rebuild) uses the IDENTICAL map update() resolved this frame's cells with - never
    // mTrack->dayCyclePhaseAt unconditionally, which reads the real day offset and is silently wrong once the
    // preview override has substituted a latched clock for tau. phaseAt is now the single SSDayCycle::phaseAt(tau,
    // dayLen, 0) map with no branch of its own to disagree with; the caller passes whatever tau it has (epoch *
    // EPOCH_S, a birth time). Forwards to phaseAt with no formula of its own.
    F64 schedulerPhaseAt(F64 tau) const { return phaseAt(tau); }

    // Global-metre XY <-> the agent frame the renderer and SSVolCloud work in. Unit conversion only.
    LLVector2 toAgentXY(const SSStormCell::Vec2& global_xy) const;
    SSStormCell::Vec2 fromAgentXY(const LLVector2& agent_xy) const;

    // <SS:Nexii> Phase 3: the deck coupling's per-frame uniform export - the SAME selection SSStormCouple::selectSlots
    // decides (the first slot-preferred cell - the hero, else the first forced pin, in input order - then ascending
    // distance of centre to ANCHOR, never camera distance), converted
    // to the AGENT frame the deck positions its puffs in (SSVolCloud::buildDeck's air-cell-centre + drift is agent
    // frame; this must land in the same one). radius/boost/anvil/meso/overshoot/mammatus come straight off the
    // resolved cell's mRadiusM/mLifecycle (already intensity-scaled by resolveActive - see ssstormcellcore.h's
    // lifecycle(), whose `k` parameter IS mGate.mIntensity - so this does not multiply by intensity again), dir is
    // SSStormCouple::motionDirection(mMotion), rotSign is SSStormCouple::rotSignOf(mCandidate.mRotation). Slots at or
    // beyond the returned count are left at CellUniform's own zero-radius default (a disabled cell to sampleAt).
    // Invariants: returns min(cells().size(), cap); the same n cells in the SAME order produce the same n CellUniforms
    // (selectSlots is order-independent except for exact ties and for which of several slot-preferred entries -
    // hero plus forced pins, 7c NEW-6 - takes slot 0; mCells is id-sorted by resolveActive, so the order is shared
    // by every client); called with cap <= 0 or an empty cell set returns 0.
    S32 fillUniforms(SSStormCouple::CellUniform* out, S32 cap) const;

    // <SS:Nexii> 8a item 2 (doc/atmo_magic_phase8_show.md section 3): the active squall line's deck coupling -
    // whichever line has a currently-alive member with the LOWEST mLineId (one line at a time), AGENT frame like
    // fillUniforms above. LineBand::strength stays 0 (SSStormCouple::LineBand's own "disabled" reading) when no
    // line is alive this frame. See the .cpp for the full contract.
    void fillLineBand(SSStormCouple::LineBand& out) const;

    // <SS:Nexii> 8a audit F3: this frame's own line geometry, exactly as lineAtEpoch (update(), private lambda)
    // resolved it - hashed OR authored (SSSquall::forcedLine) - keyed by mLineId. A reconstruction (SSAtmoInfoView's
    // squall-line rebuild) reads THESE instead of re-deriving SSSquall::lineEvent() itself: lineEvent() only ever
    // reproduces the HASHED branch, so an authored line's mLineId (chained through SALT_FORCED_LINE_ID, a different
    // salt) never matches what lineEvent() would compute and the reconstruction silently drew nothing for it.
    // Returns nullptr when lineId is not one this frame resolved (should not happen for an id read off this same
    // frame's ActiveCell). Both maps are cleared and repopulated every update() exactly like mCells (see clear()
    // and update()'s own comment on mLineDescByLineId). [interaction: ssatmoinfoview.cpp squall reconstruction]
    const SSStormCell::LineDesc* lineDescForId(U64 lineId) const;
    const SSSquall::LineEvent* lineEventForId(U64 lineId) const;

private:
    // The cube at one birth instant, memoised by SSStormCell::memoKey(candidate) (7b F5: id + birth quantised to
    // 1 ms, not id alone - a forced pin shares its lattice twin's mId but carries a rewritten birth time): a pure
    // function of (track, birth time), so the memo is not state - it is dropped every MEMO_BUCKET_S of wall clock
    // (so live edits of the cube show up) and whenever the applied track changes.
    struct BirthMemo
    {
        SSStormCell::WeatherAtBirth mWeather;
        SSStormCell::Vec2 mAnvilWind;
    };
    static constexpr F64 MEMO_BUCKET_S = 2.0;

    void clear();
    // <SS:Nexii> Phase 8 section 2 (one clock, user 2026-09-06): the day-cycle phase map, applied to CYCLE time
    // (tau), never a raw wall-clock second - SSDayCycle::phaseAt(tau, mDayLengthS, 0.0). Offset 0 because tau
    // already had the track's own day offset subtracted (real mode: mNow = sharedTime() - mDayOffsetS, in
    // update()); there is no preview branch here any more - the editor's preview substitutes what tau IS (a
    // latched clock, see update()'s mCycleRefS/mWallRefS), never which map reads it. This supersedes 7d's
    // previewPhaseAt-branching version of this function and removes SSStormCell::previewPhaseAt entirely
    // (doc/atmo_magic_phase8_show.md section 2). Pure in the captured mDayLengthS, so safe to call at any time -
    // schedulerPhaseAt (public, above) calls this from the V2 view's draw path, outside update(), when mTrack is
    // null. NOTE (updated, D3 follow-up 2026-09-07): the sky's OWN currentDayCyclePhase() used to quantise to
    // whole seconds (time(nullptr)) and this note recorded that the two agreed on the FORMULA only. It now reads
    // the same continuous clock the storms do - SSAtmoMagic::sharedTime(), latched once at the top of
    // SSAtmoMagic::idle() before either consumer runs - so within one frame the sky and the storms sample the
    // SAME instant. What is still NOT claimed: the two apply different offsets (the sky's phase carries the
    // track's mDayOffsetSeconds inside phaseAt, tau has it subtracted before), so nothing here asserts
    // bit-identity between them. [interaction: SSDayCycle::phaseAt]
    F64 phaseAt(F64 tau) const;

    // <SS:Nexii> Phase 8 section 2: phaseAt's own inverse, mirroring it exactly - one map, no real/preview pair:
    // SSDayCycle::wallTimeAtPhase(phase, nearTau, mDayLengthS, 0.0). Despite the legacy name it is applied to and
    // returns CYCLE time - a forced-storm cue's authored phase converts to a tau, never a literal wall-clock
    // second, because every state read downstream (including the cue itself) runs on tau. Two call sites in
    // update(): seeding the preview latch (mCycleRefS = wallTimeAtPhase(previewPhase, sharedTime() - mDayOffsetS))
    // and converting an authored cue phase to its tau in the forced-storm block. Supersedes 7b F3's
    // previewWallTimeAt-branching version (lesson 24); SSStormCell::previewWallTimeAt is removed with it.
    F64 wallTimeAtPhase(F64 phase, F64 nearTau) const;

    // <SS:Nexii> D1 (7e audit, 2026-09-06): the ONE latch-or-hold decision now() (via update()) and cycleTimeNow()
    // both apply, factored out so a parentless consumer sees tau FLOW under preview instead of cycleTimeNow()'s old
    // per-call re-solve (see its own header comment). Real mode (previewOverride false) is not latched at all:
    // returns wallNow - dayOffsetS every call, both callers. Preview mode latches (mCycleRefS = wallTimeAtPhase-
    // equivalent nearest occurrence of previewPhase to wallNow - dayOffsetS, mWallRefS = wallNow) when previewOverride
    // just turned on, previewPhase changed, or trackIndex != mMemoTrack (D3: shares the WeatherAtBirth memo's own
    // track test rather than a second one), else HOLDS the previous latch and flows: mCycleRefS + (wallNow -
    // mWallRefS). mCycleRefS/mWallRefS/mMemoPreviewOverride/mMemoPreviewPhase are mutable so this is callable from
    // cycleTimeNow() (const, no live claim needed) as well as update() - both are caching the same pure function's
    // last-seen input, not scheduler state. update() computes its own "did the preview change" boolean from the OLD
    // mMemoPreviewOverride/mMemoPreviewPhase/mMemoTrack BEFORE calling this (this call may overwrite the first two),
    // and reuses that boolean to decide whether to drop the WeatherAtBirth memo (SSStormCells::birthMemo) - see
    // update()'s own comment. HONEST LIMITATION: if a parentless caller (cycleTimeNow, no claim) observes a preview
    // change before update() runs in the same frame, it latches first and update() then finds nothing new to latch;
    // the birth memo (already dropped every MEMO_BUCKET_S regardless) may hold a stale cube reading up to that
    // bucket longer in that ordering - cheaper and simpler than a second change-tracking channel for a 2 s window.
    // Invariants: real mode never touches mCycleRefS/mWallRefS/the memo-preview fields; two calls with an unchanged
    // (previewOverride, previewPhase, trackIndex) between them return values differing by exactly their wallNow delta.
    F64 latchCycleTime(bool previewOverride, F64 previewPhase, S32 trackIndex, F64 dayLengthS, F64 dayOffsetS, F64 wallNow) const;

    const BirthMemo& birthMemo(const SSStormCell::Candidate& c);

    // <SS:Nexii> FIX 1: the S2 anchor computation itself, extracted out of update()'s own anchor block so
    // anchorNow() (public, above) and update() (below) call the identical logic rather than a hand-kept copy of it.
    static SSStormCell::Vec2 resolveAnchor(SSAtmoEnvManager* mgr, LLViewerRegion* region);

    bool mValid = false;
    F64 mNow = 0.0;
    U32 mSeed = 0;
    S32 mTrackIndex = -1;
    S64 mEpochNow = 0;
    S32 mLatticeCount = 0;
    SSStormCell::Vec2 mAnchor;

    // Phase mapping inputs, captured per frame so phaseAt/wallTimeAtPhase are pure functions of them and safe to
    // call at ANY time (the public schedulerPhaseAt runs from the V2 view's draw path, outside update(), when
    // mTrack below is null - 7c). Both maps go through the SSDayCycle core, not a shell respelling, and both now
    // take offset 0 (phase 8 section 2): mDayOffsetS is used only to derive mNow (tau) below, in the real branch;
    // mPreviewOverride/mPreviewPhase are used only to detect the preview turning on or its phase changing (the
    // latch event, below) - they are no longer fed into a second phase map (previewPhaseAt/previewWallTimeAt are
    // gone).
    F64 mDayLengthS = 0.0;
    F64 mDayOffsetS = 0.0;
    bool mPreviewOverride = false;
    F64 mPreviewPhase = 0.0;

    const SSAtmoEnvTrack* mTrack = nullptr;  // valid during update() only

    std::vector<ActiveCell> mCells;
    bool mHaveHero = false;
    Hero mHero;
    WhyNot mWhyNot;

    // <SS:Nexii> 8a item 2: the line geometry lineAtEpoch (update(), private lambda) computed for every real line
    // this frame (forced squall or hashed), keyed by mLineId - fillLineBand's own lookup, since ActiveCell itself
    // carries only the id. Cleared at the top of every update() and in clear(), exactly like mCells.
    std::unordered_map<U64, SSStormCell::LineDesc> mLineDescByLineId;
    // <SS:Nexii> 8a audit F3: the LineEvent lineAtEpoch actually computed (SSSquall::lineEvent for a hashed line,
    // SSSquall::forcedLine for an authored one) alongside its LineDesc, for lineEventForId - a reconstruction needs
    // the full event (mMotion, mSupercellSlot, mBirthTime, mLifetimeS) for SSSquall::qlcsJunctionAt, which LineDesc
    // does not carry; storing it is simpler and more honest than re-deriving it from LineDesc's fields (see
    // lineEventForId's own comment). Cleared/repopulated in lockstep with mLineDescByLineId.
    std::unordered_map<U64, SSSquall::LineEvent> mLineEventByLineId;

    std::unordered_map<U64, BirthMemo> mMemo;
    S64 mMemoBucket = -1;
    S32 mMemoTrack = -1;
    // <SS:Nexii> Phase 8 section 2 (one clock, user 2026-09-06): the preview clock's own latch, replacing 7d's
    // mPreviewRefTimeS (which latched a WALL-time reference for the old previewPhaseAt map - that map is gone).
    // mCycleRefS (tau) and mWallRefS (sharedTime()) are taken together, once, when the preview turns on or its
    // phase changes, and held while the slider is still: mNow = mCycleRefS + (sharedTime() - mWallRefS) equals
    // mCycleRefS exactly at the latch instant (the previewed phase's own tau, nearest the real tau at that moment)
    // and then advances at wall rate, the same as the real branch (mNow = sharedTime() - mDayOffsetS) - one clock,
    // two ways to seed it. D1 (7e audit): mutable, along with the two memo-preview fields below, so latchCycleTime
    // can seed/hold them from cycleTimeNow() (const, no update() claim required) as well as from update() itself.
    mutable F64 mCycleRefS = 0.0;
    mutable F64 mWallRefS = 0.0;
    mutable bool mMemoPreviewOverride = false; // S3: memo also drops when either of these changes, not just the bucket/track - doubles as the latch's own "did it change" flag
    mutable F64 mMemoPreviewPhase = 0.0;
};

#endif
