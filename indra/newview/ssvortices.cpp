/**
 * @file ssvortices.cpp
 * @brief Atmo Magic: the vortex scheduler shell - see ssvortices.h.
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

#include "ssvortices.h"

#include "ssatmoenvapplier.h"
#include "ssatmoenvasset.h"
#include "ssatmoenvmanager.h"
#include "ssatmomagic.h"
#include "ssvolcloud.h"
#include "sswindprofilecore.h"

#include "llstring.h"
#include "llsurface.h"
#include "llviewerregion.h"
#include "llworld.h"

#include <algorithm>

namespace
{
    // <SS:Nexii> The DUST lattice enumeration's output buffer size: (2 * DUST_FIELD_M / DUST_LATTICE_M + 2)^2 with
    // the task's own 3km field and the core's 2000m pitch is (2*1.5+2)^2 = 25; generous margin over that.
    constexpr S32 DUST_LATTICE_CAP = 64;

    // Field-for-field: SSStormCell::Vec2 and SSVortex::Vec2 are the same shape in two namespaces (both core headers,
    // neither may include the other's Vec2 - see each core's own include list), so every crossing is this copy, not
    // a cast. No formula.
    SSVortex::Vec2 toVortexVec2(const SSStormCell::Vec2& v)
    {
        SSVortex::Vec2 out;
        out.x = v.x;
        out.y = v.y;
        return out;
    }

    // The vortex core's Parent from one resolved storm cell - a field-for-field copy of what SSStormCell::resolveActive
    // already resolved this frame (ssstormcellcore.h), no new formula. mAllowTornadoes rides the cell's OWN
    // birth-memoised weather (SSStormCells::birthMemo), same as every other tornado-family gate input. 7f: mIsHero
    // is cell.mIsHero directly; for the hero, mClosestAge01 is (hero->mClosestTime - birth) / lifetime, clamped
    // [0,1] - the fraction of the cell's OWN life at which the composed flyby passes closest, which childVortex
    // pins slot 0's funnel window around (never re-derived there: this is the one place the hero's closest-approach
    // TIME becomes a life FRACTION). `cells` supplies hero() only for that one read; a non-hero cell never reaches
    // the branch that needs it, so it may be null when the caller has no hero this frame.
    SSVortex::Parent toParent(const SSStormCells::ActiveCell& cell, const SSStormCells* cells)
    {
        SSVortex::Parent p;
        p.mId = cell.mCandidate.mId;
        p.mBirthTime = cell.mCandidate.mBirthTime;
        p.mLifetimeS = cell.mCandidate.mLifetimeS;
        p.mIntensity = cell.mGate.mIntensity;
        p.mRotation = cell.mCandidate.mRotation;
        p.mPotential = cell.mCandidate.mPotential;
        p.mSupercell = cell.mGate.mSupercell;
        p.mTornadoEligible = cell.mGate.mTornadoEligible;
        p.mCentre = toVortexVec2(cell.mCentre);
        p.mRadiusM = cell.mRadiusM;
        p.mLife = cell.mLifecycle;
        p.mAllowTornadoes = cell.mWeather.mAllowTornadoes;
        p.mForcedKind = cell.mForcedKind; // 7d: the authored kind guarantees the funnel (ssvortexcore.h childVortex)
        p.mIsHero = cell.mIsHero;
        if (cell.mIsHero && cells)
        {
            const SSStormCells::Hero* hero = cells->hero();
            if (hero && cell.mCandidate.mLifetimeS > 0.f)
            {
                const F32 frac = (F32)((hero->mPath.mClosestTime - cell.mCandidate.mBirthTime) / (F64)cell.mCandidate.mLifetimeS);
                p.mClosestAge01 = llclamp(frac, 0.f, 1.f);
            }
        }
        return p;
    }

    // <SS:Nexii> The waterspout water test itself (RULES: "LLViewerRegion::getWaterHeight vs the land height at that
    // point"): the ONE region query the shell adds beyond the core's pure functions. contact_global is a GLOBAL XY
    // (the same frame every storm-cell/vortex centre lives in - section 2 of the storm design). false (never
    // relabel) when no simulated region covers that point this client-side - an honest "can't tell" rather than a
    // guess; the same region resolves both readings, so the two never disagree by straddling a region boundary.
    bool contactOverWater(const SSVortex::Vec2& contact_global)
    {
        if (!LLWorld::instanceExists())
        {
            return false;
        }
        const LLVector3d pos_global((F64)contact_global.x, (F64)contact_global.y, 0.0);
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global);
        if (!region)
        {
            return false;
        }
        const F32 water_z = region->getWaterHeight();
        const F32 land_z = region->getLand().resolveHeightGlobal(pos_global);
        return water_z > land_z;
    }

    // The per-COLLARS-row table (SSVortex::collarRadius/collarAltitudeM at COLLARS evenly spaced height fractions,
    // index 0 == contact, COLLARS - 1 == wall cloud) for one funnel-having live vortex.
    void buildCollars(const SSVortex::Candidate& c, const SSVortex::State& s,
                       F32 ground_z, F32 wall_cloud_z, std::array<SSVortices::Collar, SSVortex::COLLARS>& out)
    {
        for (S32 i = 0; i < SSVortex::COLLARS; ++i)
        {
            const F32 h01 = (F32)i / (F32)(SSVortex::COLLARS - 1);
            SSVortices::Collar& collar = out[(size_t)i];
            collar.mRadiusM = SSVortex::collarRadius(h01, s.mIntensity, c.mTaper, s.mCondensation);
            collar.mAltitudeM = SSVortex::collarAltitudeM(ground_z, wall_cloud_z, h01);
        }
    }

    // Whether kind is one of the funnel-having kinds childVortex's own hasFunnel local decides (ssvortexcore.h) -
    // evaluated on the PRE-relabel candidate kind, so a landspout/weak-meso the water test relabels to
    // KIND_WATERSPOUT still gets a collar table (a waterspout is a real, if thinner, funnel - only its label and
    // the water test differ from a landspout).
    bool candidateHasFunnel(SSVortex::EKind kind)
    {
        return kind == SSVortex::KIND_MESOCYCLONIC || kind == SSVortex::KIND_ANTICYCLONIC
            || kind == SSVortex::KIND_LANDSPOUT || kind == SSVortex::KIND_SATELLITE;
    }

    // <SS:Nexii> This frame's LIVE weather for dustGate - never the storm scheduler's birth-time memo (dust devils
    // are gated on CURRENT conditions, not conditions at some hashed birth instant: real dust devils spin up and
    // die with the wind/heat/sun of the moment). All four reads are pure functions of (track, applied phase) or the
    // deck's own live figures - never the camera, never the eased SSAtmoMagic::mWind: temperature is
    // SSAtmoMagic::temperatureC() (the same resolved-blend figure every other live weather readout uses); wind
    // speed is the CURVE-RESOLVED SSWindProfile::Params::mSpeed10MS at the applied phase (SSAtmoEnvApplier::
    // windProfileAt, the same pure profile birthMemo samples at birth - here sampled at NOW's phase instead);
    // coverage is the SAME resolver's coverage (SSAtmoEnvApplier::windProfileFieldCoverage, cached alongside
    // windProfileBaseZ - SCHEDULER fix 4), never SSVolCloud::weatherCoverage()'s LIVE deck, which can still be
    // last frame's build depending on idle() tick order; sun elevation sine is
    // SSAtmoEnvApplier::sunElevationSinAt(track, phase), the same figure twilight/rainbow gating reads internally.
    bool buildDustWeather(SSAtmoEnvApplier* applier, SSAtmoEnvManager* mgr, SSAtmoMagic* atmo,
                          SSVortex::DustWeather& out)
    {
        const S32 track_index = applier->appliedTrackIndex();
        if (track_index < 0 || !mgr->hasAsset() || track_index >= (S32)mgr->asset().mTracks.size())
        {
            return false;
        }
        const SSAtmoEnvTrack& track = mgr->asset().mTracks[(size_t)track_index];
        const F64 phase = applier->appliedPhase();

        out.mTemperatureC = atmo->temperatureC();
        out.mWindSpeedMS = SSAtmoEnvApplier::windProfileAt(track, phase).mSpeed10MS;
        out.mCoverage = applier->windProfileFieldCoverage();
        out.mSunElevationSin = applier->sunElevationSinAt(track, phase);
        return true;
    }
}

// Singleton shell.
SSVortices::SSVortices()
{
}

// <SS:Nexii> The tornado "why not" readout - see the header. Rebuilds slot 0's candidate/state fresh from the
// hero's OWN ActiveCell entry in cells() (never the hero-vortex already kept in mActive, which may not exist yet
// this walk) so this is self-contained and correct even if update() calls it before the main vortex loop.
void SSVortices::buildWhyNot(SSStormCells* cells, U32 seed, F64 now)
{
    // <SS:Nexii> SCHEDULER fix 12: built directly into the member mWhyNot rather than a fresh local WhyNotTornado
    // copy-assigned at the end - mFailing.clear() keeps its capacity across ticks instead of every frame
    // destroying and reallocating a temporary's vector. WhyNotTornado's own scalar fields are re-set below in the
    // same order the struct declares them, so a stale value from a previous frame's early return (no hero, etc.)
    // never survives into this one.
    WhyNotTornado& why = mWhyNot;
    why.mFailing.clear();

    const SSStormCells::Hero* hero = cells->hero();
    why.mHaveHero = hero != nullptr;
    if (!hero)
    {
        why.mHeroId = 0;
        why.mKind = SSVortex::KIND_NONE;
        why.mAlive = false;
        why.mMeso = 0.f;
        why.mPotential = 0.f;
        why.mSupercell = false;
        why.mTornadoEligible = false;
        why.mAllowTornadoes = false;
        why.mFailing.push_back("no hero storm cell active");
        return;
    }
    why.mHeroId = hero->mId;

    const SSStormCells::ActiveCell* hero_cell = nullptr;
    for (const SSStormCells::ActiveCell& c : cells->cells())
    {
        if (c.mIsHero)
        {
            hero_cell = &c;
            break;
        }
    }
    if (!hero_cell)
    {
        // <SS:Nexii> Should not happen (SSStormCells always keeps the hero in cells() when mHaveHero) but honest
        // rather than a crash if that invariant is ever broken upstream.
        why.mKind = SSVortex::KIND_NONE;
        why.mAlive = false;
        why.mMeso = 0.f;
        why.mPotential = 0.f;
        why.mSupercell = false;
        why.mTornadoEligible = false;
        why.mAllowTornadoes = false;
        why.mFailing.push_back("hero id present but its cell entry is missing from cells() this frame");
        return;
    }

    const SSVortex::Parent parent = toParent(*hero_cell, cells);
    why.mMeso = parent.mLife.mMeso;
    why.mPotential = parent.mPotential;
    why.mSupercell = parent.mSupercell;
    why.mTornadoEligible = parent.mTornadoEligible;
    why.mAllowTornadoes = parent.mAllowTornadoes;

    const SSVortex::Candidate c0 = SSVortex::childVortex(seed, parent, 0);
    const SSVortex::State s0 = SSVortex::vortexAt(c0, parent, now);
    why.mKind = c0.mKind;

    const bool is_tornado_kind = c0.mKind == SSVortex::KIND_MESOCYCLONIC || c0.mKind == SSVortex::KIND_ANTICYCLONIC
                               || c0.mKind == SSVortex::KIND_LANDSPOUT || c0.mKind == SSVortex::KIND_WATERSPOUT;
    why.mAlive = s0.mAlive && is_tornado_kind;

    if (!why.mAlive)
    {
        // <SS:Nexii> Judgement call on ORDER: mirrors childVortex's own gate order (ssvortexcore.h) - Allow
        // Tornadoes first (it gates every tornado-family kind identically), then the two spawn rules in the order
        // childVortex tries them (mesocyclonic before landspout), then "the kind was decided but its hashed
        // birth/duration window has not opened, or has already closed, at the hero's current age" for the case
        // childVortex DID pick a tornado-family kind and only vortexAt's own life-span test failed it.
        if (!parent.mAllowTornadoes)
        {
            why.mFailing.push_back("Allow Tornadoes off for this track");
        }
        else if (c0.mKind == SSVortex::KIND_GUSTNADO)
        {
            why.mFailing.push_back("only a funnel-less gustnado formed on the hero (no meso/landspout gate passed)");
        }
        else if (c0.mKind == SSVortex::KIND_NONE)
        {
            if (!parent.mSupercell)
            {
                why.mFailing.push_back(llformat(
                    "hero is not a supercell, and potential %.2f < landspout minimum %.2f",
                    why.mPotential, SSVortex::LANDSPOUT_POT_MIN));
            }
            else if (why.mMeso < SSVortex::MESO_TORNADO_MIN)
            {
                why.mFailing.push_back(llformat("hero meso %.2f < %.2f", why.mMeso, SSVortex::MESO_TORNADO_MIN));
            }
            else
            {
                why.mFailing.push_back("hero clears every threshold but the gustnado hash did not fire this epoch");
            }
        }
        else
        {
            why.mFailing.push_back(llformat(
                "%s decided but hero age01 is outside its vortex window [%.2f, %.2f)",
                (c0.mKind == SSVortex::KIND_MESOCYCLONIC) ? "mesocyclonic"
                    : (c0.mKind == SSVortex::KIND_ANTICYCLONIC) ? "anticyclonic" : "landspout",
                c0.mBirthAge01, c0.mBirthAge01 + c0.mDurationAge01));
        }
    }
}

// <SS:Nexii> The per-frame re-derivation - see the header. FIX 1: mValid governs only the storm-cell-children
// output (mActive/collars/WhyNot) now - those stay a no-op (previous frame's outputs kept, mValid false) unless
// SSStormCells is itself valid this frame, the same anchor/track/region gate as before (there is no funnel world
// without a storm world under it). mDust does NOT share that gate any more (see the dust block below, and its own
// FIX 1 comment): it is rebuilt from scratch every tick this function runs at all, valid() true or false.
//
// <SS:Nexii> SCHEDULER fix 4 (ordering note): SSAtmoMagic::idle() ticks this BEFORE SSVolCloud::update() by design
// (see ssatmomagic.cpp) - the wall-cloud base below and the dust block's own coverage read the deterministic
// resolver (SSAtmoEnvApplier::windProfileBaseZ()/windProfileFieldCoverage()) rather than SSVolCloud's live deck
// precisely so this ordering is safe: nothing here needs the deck to have already built this frame.
void SSVortices::update()
{
    SSStormCells* cells = SSStormCells::getInstance();
    SSAtmoEnvApplier* applier = SSAtmoEnvApplier::getInstance();
    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();
    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    // --- dust devils: parentless, own lattice, gated as a whole on live weather ---
    // <SS:Nexii> FIX 1 (BLOCKING, was below the cells->valid() early-return): a dust devil is parentless - the
    // core's own file-top comment says this mini-scheduler "needs no parent cell" - so it must never depend on
    // SSStormCells having resolved anything this frame. Before this fix the block below sat AFTER the valid() gate
    // and after this consumer's own storm Interest claim, so on the common case (the applied track's Allow
    // Supercells/Allow Tornadoes both off - the default) wants_tornado_family below is false, mStormInterest is
    // never claimed, and cells->valid() reads false the moment nothing else (e.g. SSVolCloud's own Interest, which
    // also lapses once coverage drops below ITS OWN claim threshold) happens to be holding one either - dust
    // devils vanished right along with the storm-cell machinery they have nothing to do with, including on a
    // clear, dry, hot day, exactly the weather dustGate wants to show them in. Hoisted here with its own inputs,
    // never cells->seed()/anchor()/valid(): SSAtmoMagic::seed() (bit-identical to what cells->seed() would read
    // once resolved - both ultimately read atmo->seed(), see SSStormCells::update()), SSStormCells::anchorNow()
    // (the same S2 source-region logic cells->anchor() uses, callable with no claim held - see its own comment),
    // and SSStormCells::cycleTimeNow() (phase 8 section 2, one clock, user 2026-09-06: dust-devil epochs/ages are
    // STATE, so they run on the shared cycle-time clock like every other storm-scheduler state read, never the raw
    // wall clock - cycleTimeNow() is the anchorNow() idiom applied to the clock, since a dust devil must never
    // depend on cells->valid()/now() having been resolved this frame either). mDust is therefore fresh every tick
    // this function runs, independent of mValid below (which now covers only the storm-cell-children output) - see
    // valid()'s own header comment and SSVortexRender::render()'s own early-return, updated to draw dust with no
    // funnels present.
    mDust.clear();
    {
        const U32 dust_seed = atmo->seed();
        const F64 dust_now = cells->cycleTimeNow();
        const SSVortex::Vec2 dust_anchor = toVortexVec2(cells->anchorNow());

        SSVortex::DustWeather weather;
        if (buildDustWeather(applier, mgr, atmo, weather) && SSVortex::dustGate(weather))
        {
            S32 lx[DUST_LATTICE_CAP];
            S32 ly[DUST_LATTICE_CAP];
            const S32 found = SSVortex::enumerateDustLattice(dust_anchor, DUST_FIELD_M, lx, ly, DUST_LATTICE_CAP);
            const S32 lattice_count = llmin(found, DUST_LATTICE_CAP);

            const S64 epoch_now = SSVortex::dustEpochOf(dust_now);
            for (S32 i = 0; i < lattice_count; ++i)
            {
                // <SS:Nexii> epoch_now - 1 and epoch_now are the only epochs that can still be alive at `now`: a
                // candidate's latest possible life end is its epoch's own start + 2 * DUST_EPOCH_S (birth just
                // before the epoch ends, plus the maximum DUST_EPOCH_S-long duration), so an epoch two or more back
                // than epoch_now can never still be alive - the same reasoning SSStormCell::resolveActive applies
                // with its own LIFE_MAX_S window, sized here to DUST_EPOCH_S because dustDevil()'s duration cap
                // equals it.
                for (S64 epoch = epoch_now - 1; epoch <= epoch_now; ++epoch)
                {
                    const SSVortex::DustCandidate d = SSVortex::dustDevil(dust_seed, lx[(size_t)i], ly[(size_t)i], epoch);
                    if (!SSVortex::dustAlive(d, dust_now))
                    {
                        continue;
                    }
                    DustVortex dv;
                    dv.mCandidate = d;
                    dv.mIntensity = SSVortex::dustLiveIntensity(d.mIntensity, weather.mTemperatureC);
                    mDust.push_back(dv);
                }
            }
        }
    }

    // <SS:Nexii> Review NEW-3: SCHEDULER fix 9's claim was unconditional and never released; this is the cost gate
    // that replaces it. SSStormCells::resolveActive() is the one thing this consumer's Interest pays for, and the
    // only thing downstream of it (the storm-cell-children block below) is the childVortex() gate on mesocyclonic/
    // anticyclonic/landspout/satellite/gustnado, which fails every candidate before it reads a single hashed number
    // whenever the applied track's own mAllowSupercells and mAllowTornadoes are BOTH off (ssvortexcore.h's mesoOk/
    // landspoutOk both require mAllowTornadoes; the gustnado hash needs neither Allow flag, but a gustnado is not a
    // tornado-family kind and the design gives it no reason to justify this consumer's own claim on its own - see
    // the WhyNot readout's own "Allow Tornadoes off" case for the flag this consumer is really gating on). FIX 1:
    // dust devils (above) no longer share this claim or SSStormCells' valid() gate at all - they used to, and that
    // was the bug this fix closes; this cost gate is now scoped purely to the storm-cell-children block below.
    bool wants_tornado_family = false;
    {
        const S32 gate_track = applier->appliedTrackIndex();
        if (mgr->hasAsset() && gate_track >= 0 && gate_track < (S32)mgr->asset().mTracks.size())
        {
            const SSAtmoEnvWeatherInfluence& infl = mgr->asset().mTracks[(size_t)gate_track].mWeatherInfluence;
            wants_tornado_family = infl.mEnabled && (infl.mAllowSupercells || infl.mAllowTornadoes);
        }
    }
    if (wants_tornado_family)
    {
        if (!mStormInterest)
        {
            mStormInterest = cells->claim();
        }
    }
    else if (mStormInterest)
    {
        mStormInterest = SSStormCells::Interest();
    }

    if (!cells->valid())
    {
        mValid = false;
        return;
    }

    const U32 seed = cells->seed();
    // <SS:Nexii> Phase 8 section 2 (one clock, user 2026-09-06): CYCLE time, not the raw wall clock - every
    // storm-cell-child read below (vortexAt/childVortex windows, buildWhyNot) is STATE (which vortex exists, its
    // age, its funnel's descent), so it runs on the SAME tau cells->cells() was just resolved with, on the far side
    // of the cells->valid() gate above - cells->now() IS mNow, never a second read of sharedTime() that could
    // disagree with it by a hair.
    const F64 now = cells->now();
    const SSVortex::Vec2 anchor = toVortexVec2(cells->anchor());
    const F32 ground_z = applier->windProfileGroundZ();
    // <SS:Nexii> SCHEDULER fix 4: the wall cloud sits on the RESOLVER's deck base (SSAtmoEnvApplier::
    // windProfileBaseZ(), the same deterministic figure the drift accumulator integrates at), never
    // SSVolCloud::cloudBaseZ() - the live deck's own mPrimary.mBaseZ, which on a frame where this update() runs
    // before SSVolCloud::update() (see update()'s own doc comment) is still last frame's build.
    const F32 wall_cloud_z = SSVortex::wallCloudAltitudeM(ground_z, applier->windProfileBaseZ());

    // --- storm-cell children: mesocyclonic/anticyclonic/landspout/satellite/gustnado, waterspout relabel ---
    // <SS:Nexii> SCHEDULER fix 12: mAliveScratch (the member, declared in the header) rather than a fresh local
    // vector every tick - cleared, not reallocated, so its backing storage settles at the high-water mark instead
    // of being freed and rebuilt every frame. `alive` is just this function's short name for it.
    std::vector<Ranked>& alive = mAliveScratch;
    alive.clear();
    alive.reserve(cells->cells().size() * (size_t)SSVortex::SLOTS_PER_CELL); // no-op if capacity already covers it

    for (const SSStormCells::ActiveCell& cell : cells->cells())
    {
        const SSVortex::Parent parent = toParent(cell, cells);
        for (S32 slot = 0; slot < SSVortex::SLOTS_PER_CELL; ++slot)
        {
            const SSVortex::Candidate c = SSVortex::childVortex(seed, parent, slot);
            if (c.mKind == SSVortex::KIND_NONE)
            {
                continue;
            }
            const SSVortex::State s = SSVortex::vortexAt(c, parent, now);
            if (!s.mAlive)
            {
                continue;
            }
            alive.push_back({cell.mCandidate.mId, cell.mIsHero, parent, c, s});
        }
    }

    // <SS:Nexii> Ranking: hero-parent first, then ascending parent id (task's own words), then ascending candidate
    // id as an IMPLEMENTER tiebreak - the task names no order between a hero's own two slots, but std::sort is not
    // stable and leaving that tie unresolved would make the kept set (once MAX_ACTIVE < alive.size()) depend on the
    // sort implementation rather than the inputs, breaking the determinism rule. mCandidate.mId is already a pure
    // hash of (seed, parentId, slot), so this costs nothing and is bit-identical across clients.
    std::sort(alive.begin(), alive.end(), [](const Ranked& a, const Ranked& b)
    {
        if (a.mParentIsHero != b.mParentIsHero)
        {
            return a.mParentIsHero;
        }
        if (a.mParentId != b.mParentId)
        {
            return a.mParentId < b.mParentId;
        }
        return a.mCandidate.mId < b.mCandidate.mId;
    });

    const S32 keep = llmin((S32)alive.size(), SSVortex::MAX_ACTIVE);
    mActive.clear();
    mActive.reserve((size_t)keep);
    for (S32 i = 0; i < keep; ++i)
    {
        const Ranked& r = alive[(size_t)i];

        LiveVortex v;
        v.mParentId = r.mParentId;
        v.mParentIsHero = r.mParentIsHero;
        v.mCandidate = r.mCandidate;
        v.mState = r.mState;
        v.mKind = r.mCandidate.mKind;
        v.mContactGlobal = r.mState.mContact;
        v.mHasFunnel = candidateHasFunnel(r.mCandidate.mKind);
        v.mWallCloudZ = wall_cloud_z;

        // <SS:Nexii> Waterspout relabel (RULES: "re-label a landspout/weak-meso as KIND_WATERSPOUT when the region
        // water height at the contact point exceeds the ground there"): the core's waterspoutEligible names WHICH
        // candidates are eligible (a landspout, or a meso/anti too weak to be a true tornado - see its own comment
        // for the SATELLITE_INTENSITY_MIN judgement call), this shell decides water via the one region query
        // (contactOverWater). Display-only: mCandidate.mKind is left untouched (the pre-relabel kind, so a caller
        // can tell a relabelled waterspout from one that... never happens, dustDevil()/childVortex() never emit
        // KIND_WATERSPOUT directly - the enum value exists solely for this relabel).
        if (SSVortex::waterspoutEligible(r.mCandidate) && contactOverWater(r.mState.mContact))
        {
            v.mKind = SSVortex::KIND_WATERSPOUT;
            v.mWasRelabelledWaterspout = true;
        }

        if (v.mHasFunnel)
        {
            buildCollars(r.mCandidate, r.mState, ground_z, wall_cloud_z, v.mCollars);
        }
        else
        {
            v.mCollars.fill(Collar());
        }

        mActive.push_back(v);
    }

    // FIX 1: dust devils are built above now, before the cells->valid() gate - see that block's own comment.

    buildWhyNot(cells, seed, now);

    mValid = true;
}
