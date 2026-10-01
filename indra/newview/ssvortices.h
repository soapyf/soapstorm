/**
 * @file ssvortices.h
 * @brief Atmo Magic: the vortex scheduler shell - storm-cell children, waterspout relabelling, dust devils, collar tables.
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

#ifndef SS_VORTICES_H
#define SS_VORTICES_H

#include "llsingleton.h"
#include "ssstormcells.h"
#include "ssvortexcore.h"

#include <array>
#include <string>
#include <vector>

// <SS:Nexii> The SHELL of doc/atmo_magic_storm_dynamics.md section 4: ticked from SSAtmoMagic::idle() right after
// SSStormCells::update() (this frame's cells() are already resolved by the time this runs), everything positional
// re-derived from scratch every tick - no persisted vortex state, no dt. Per SSStormCells::ActiveCell this builds a
// pure SSVortex::Parent (a field-for-field copy of what resolveActive already resolved - no new formula), asks the
// core for both SLOTS_PER_CELL children (childVortex) and their live state at SSAtmoMagic::sharedTime() (vortexAt),
// and keeps every ALIVE one. The two things the core deliberately leaves to the shell (its own file-top comment):
// the water test for waterspout relabelling (LLViewerRegion::getWaterHeight vs the land height at the vortex's own
// contact point, both queried at that GLOBAL point - never the agent's own region) and the ground altitude the
// collar table's bottom sits on (SSAtmoEnvApplier::windProfileGroundZ, the same single scalar the wind profile
// itself treats as "the ground" - not a per-point terrain query; True Ground is gone, see sswindflow.h). Dust
// devils are a SEPARATE, parentless mini-scheduler on their own DUST_LATTICE_M lattice within DUST_FIELD_M of the
// same weather-domain anchor SSStormCells::anchorNow() computes (FIX 1: the same S2 source-region logic
// SSStormCells' own anchor() uses, but callable with no Interest claim held and no dependency on cells() having
// resolved anything this frame - a dust devil needs no parent cell), gated as a whole by SSVortex::dustGate reading
// this frame's LIVE resolved weather (never birth-time memoised, since "hot/calm/clear/high-sun" is a
// live-conditions gate, not a spawn-time one) - never sharing the funnelled MAX_ACTIVE cap, which is a collar-quad
// render budget dust devils (funnel-less, particle-only) never draw against. No rendering here: this exposes the
// resolved lists and the two debug readouts (collar table, tornado why-not) for the renderer and V-views to read.
class SSVortices : public LLSingleton<SSVortices>
{
    LLSINGLETON(SSVortices);
    ~SSVortices() = default;

public:
    // <SS:Nexii> Judgement call: how far out from the anchor the DUST lattice is enumerated, per the task's own "3km
    // of the anchor" - the design doc states the lattice/epoch shape (small lattice, short epochs) but not a field
    // radius; 3km keeps dust devils a near-field, walkable-distance phenomenon (a fraction of the storm field's
    // 12km) without the design ever having pinned a number.
    static constexpr F32 DUST_FIELD_M = 3000.f;

    // One collar quad's radius and world-Z altitude - SSVortex::collarRadius/collarAltitudeM evaluated at one of
    // COLLARS evenly spaced height fractions (index 0 == contact/ground, COLLARS - 1 == the wall cloud).
    struct Collar
    {
        F32 mRadiusM = 0.f;
        F32 mAltitudeM = 0.f;
    };

    // A live, funnel-having vortex (mesocyclonic/anticyclonic/landspout/waterspout/satellite) or a funnel-less
    // gustnado, already ranked and kept by update(). mKind is the DISPLAY kind (post waterspout relabel); mCandidate
    // and mState are exactly what childVortex/vortexAt returned (mCandidate.mKind is the PRE-relabel kind, so a
    // caller can still tell "this was a landspout the water test relabelled" from "this was born a waterspout" -
    // it never is, mCandidate.mKind is never KIND_WATERSPOUT, only mKind can be). mCollars is only populated
    // (COLLARS non-zero entries) when mHasFunnel; a gustnado carries a zeroed table - it draws no collar quads,
    // only the debris skirt (LLViewerPartSim, seeded from mCandidate.mId, per the design's rendering section - not
    // built here).
    struct LiveVortex
    {
        U64 mParentId = 0;                 // the owning storm cell's ActiveCell::mCandidate.mId
        bool mParentIsHero = false;
        SSVortex::Candidate mCandidate;
        SSVortex::State mState;
        SSVortex::EKind mKind = SSVortex::KIND_NONE;   // display kind, post waterspout relabel
        bool mWasRelabelledWaterspout = false;
        SSVortex::Vec2 mContactGlobal;      // world/global metres, same frame as SSStormCells' cells
        bool mHasFunnel = false;
        F32 mWallCloudZ = 0.f;              // world Z, SSVortex::wallCloudAltitudeM(groundZ, deck base)
        std::array<Collar, SSVortex::COLLARS> mCollars;
    };

    // A live dust devil - parentless, no funnel/collar table (the taxonomy's funnel-less/dust-only kind - see the
    // KIND_GUSTNADO/KIND_DUST_DEVIL noAloft comment in ssvortexcore.h). mIntensity is the LIVE figure
    // (SSVortex::dustLiveIntensity), not the candidate's hashed baseline.
    struct DustVortex
    {
        SSVortex::DustCandidate mCandidate;
        F32 mIntensity = 0.f;
    };

    // <SS:Nexii> The tornado "why not" readout, keyed to SSStormCells' OWN hero (never a second hero concept) - V2's
    // storm "why not" answers "why is there no hero cell"; this answers "why does the hero cell that DOES exist not
    // carry a live tornado-family funnel right now". Empty mFailing iff mAlive.
    struct WhyNotTornado
    {
        bool mHaveHero = false;
        U64 mHeroId = 0;
        SSVortex::EKind mKind = SSVortex::KIND_NONE;   // slot 0's decided kind (KIND_NONE if no gate passed at all)
        bool mAlive = false;                            // a tornado-family kind (meso/anti/landspout/waterspout) is alive now
        F32 mMeso = 0.f;
        F32 mPotential = 0.f;
        bool mSupercell = false;
        bool mTornadoEligible = false;
        bool mAllowTornadoes = false;
        std::vector<std::string> mFailing;
    };

    // Ticked from SSAtmoMagic::idle() right after SSStormCells::update(). FIX 1: mDust is rebuilt every call this
    // runs at all (see the .cpp's own comment) - only active()/the collar tables/whyNot() are a no-op (leave the
    // previous frame's outputs, mValid false) when SSStormCells itself is not valid this frame, the same anchor/
    // track/region gate as before.
    void update();

    // Governs active()/the collar tables inside it/whyNot() only - see update()'s own comment. dustDevils() is
    // valid whenever update() has run at all, independent of this flag.
    bool valid() const { return mValid; }

    // <= SSVortex::MAX_ACTIVE, hero-parent first (SSStormCells::ActiveCell::mIsHero), then ascending parent id, then
    // (an implementer tiebreak the task's ranking does not name, needed for determinism across two candidates on
    // the SAME parent, i.e. its two slots) ascending mCandidate.mId. Stale (previous frame's kept set) when
    // valid() is false - see update()'s own comment.
    const std::vector<LiveVortex>& active() const { return mActive; }

    // FIX 1: fresh every update() call regardless of valid() - a dust devil is parentless and does not depend on
    // SSStormCells having resolved anything this frame. Never stale the way active() can be.
    const std::vector<DustVortex>& dustDevils() const { return mDust; }
    const WhyNotTornado& whyNot() const { return mWhyNot; }

private:
    void buildWhyNot(SSStormCells* cells, U32 seed, F64 now);

    // <SS:Nexii> SCHEDULER fix 12: one storm-cell child, ranked - a per-frame scratch row, kept only in
    // mAliveScratch (never mActive) between the childVortex/vortexAt walk and the ranked keep-loop below it.
    struct Ranked
    {
        U64 mParentId;
        bool mParentIsHero;
        SSVortex::Parent mParent;
        SSVortex::Candidate mCandidate;
        SSVortex::State mState;
    };

    bool mValid = false;
    std::vector<LiveVortex> mActive;
    std::vector<DustVortex> mDust;
    WhyNotTornado mWhyNot;

    // <SS:Nexii> SCHEDULER fix 12: reused across ticks (cleared, not reallocated) rather than a fresh local vector
    // every update() - everything positional is still re-derived from scratch every tick per the class comment,
    // only the BACKING STORAGE persists.
    std::vector<Ranked> mAliveScratch;

    // <SS:Nexii> SCHEDULER fix 9, revised by review NEW-3: claimed (or kept) on this singleton's own update() and
    // released again the moment the applied track's own mAllowSupercells and mAllowTornadoes are both off - see
    // update()'s own comment in the .cpp for the cost-gate reasoning. No longer held unconditionally for the
    // singleton's whole life: this is a stake in SSStormCells::resolveActive() running on this consumer's behalf,
    // not a permanent claim, so it tracks the ONE thing it actually pays for (the storm-cell-children block) rather
    // than outliving the track state that justified it.
    SSStormCells::Interest mStormInterest;
};

#endif
