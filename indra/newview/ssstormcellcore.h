/**
 * @file ssstormcellcore.h
 * @brief Atmo Magic: deterministic storm cells - lattice schedule, hero flyby, lifecycle. Header-only core. CONTRACT.
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

#ifndef SS_STORMCELLCORE_H
#define SS_STORMCELLCORE_H

// <SS:Nexii> A CORE header (lldefs.h, ssatmonoisecore.h, <cmath> only; NO llmath.h). Design: doc/atmo_magic_storm_dynamics.md sections 2, 5, 6. Storm cells are ENTITIES RECOMPUTED FROM SCRATCH: every function is a pure function of (shared seed, wall-clock time, lattice/world position, the weather the SHELL evaluated at the candidate's birth time) - no persisted sim state, no dt, no camera, no frame counter. Positions are WORLD frame, never the drifted air frame. The shell (SSStormCells) owns anything that needs a weather cube, a region, or a setting: it enumerates lattice cells with this core, asks the core for each candidate, evaluates the cube at the candidate's birth time, gates it here, ranks survivors by hashed id (never by camera distance), and composes the hero flyby against the weather domain's anchor. Bodies marked STUB are the implementer's; the invariants in each comment are the test author's spec.
#include "lldefs.h"
#include "ssatmonoisecore.h"

#include <algorithm> // <SS:Nexii> S6: std::sort only, for resolveActive's hashed-id rank - explicitly allowed in a core by the phase-2b brief
#include <cmath>
#include <cstdint>

namespace SSStormCell
{
    // ---- Constants. LOCKSTEP ones mirror values owned elsewhere. ----
    // <SS:Nexii> S7 density calibration (2026-09-05, scenario_storm_density.cpp): probe = 40 anchors on a 90km grid
    // (>> the 12km/FIELD_M field diameter, so anchors sample near-independent noise) x 24 hourly instants = 960
    // anchor-hours per weather profile, enumerating the lattice and epoch range exactly as SSStormCells::updateNow
    // does (candidate/alive/gate, no hero logic). Measured with the constants below (seed 0x5EED1337):
    //   calm    m=.30 c=.30 s=.20 -> anyCell%=  0.0  tornado%=  0.0  meanCells= 0.00
    //   design  m=.80 c=.70 s=.60 -> anyCell%= 96.8  tornado%= 50.8  meanCells= 4.75
    //   severe  m=.90 c=.90 s=.80 -> anyCell%=100.0  tornado%=100.0  meanCells=23.74
    //   extreme m=1.0 c=1.0 s=.90 -> anyCell%=100.0  tornado%=100.0  meanCells=38.49
    // Targets met: calm (0 always), design (any%>=70, tornado%>=45, mean in [2,5]), severe's tornado%>=85.
    // Targets NOT met: severe's mean in [4,8] (measured 23.74) and extreme's mean <=10 (measured 38.49). Root
    // cause, not a tuning miss: gateScore is potential*convection*moisture*shearTerm, so severe/extreme differ from
    // design ONLY through convection*moisture (0.81 and 1.0 vs design's 0.56, a <=1.79x spread), while the fbm2-
    // driven potential/shearTerm distribution's tail is steep enough that pinning design's mean to a low single
    // digit forces the SPAWN_THRESHOLD cutoff far enough into that tail that the SAME cutoff scaled down by 0.81 or
    // 1.0 (severe/extreme's easier bar) still admits 5-8x more candidates. Swept SPAWN_THRESHOLD over its full
    // permitted (0.2, 0.4] range (gate_monotone_in_convection_and_moisture pins that bound) crossed with
    // SHEAR_SCORE_BASE over [0, 1]: no point satisfies design's window and severe's simultaneously - the closest
    // severe gets while design stays in [2,5] is a mean of ~22. Fixing this needs a lever outside the four listed
    // (e.g. reshaping mPotential's own distribution, or a non-multiplicative moisture/convection combination),
    // which the S7 brief does not authorize; reported here rather than silently left at a worse compromise.
    // FIELD_SCALE moved 0.31 -> 1.20 to decorrelate the ~54 lattice cells inside one 12km field (was ~2.6 fbm units
    // across the whole field, now ~10): this does not change any profile's MEAN (fbm2's marginal distribution is
    // scale-invariant; verified by holding SPAWN_THRESHOLD/SHEAR_SCORE_BASE fixed and varying only FIELD_SCALE), but
    // it fixes the all-or-nothing lumpiness that was failing design's anyCell%/tornado% floors and destabilising the
    // 40-anchor sample mean (measured at FIELD_SCALE=0.31 with the same threshold: design any%=69.3, tornado%=38.9,
    // mean=5.16 - all three off-target from sampling variance alone, not from a different expected population).
    // gate_score_formula_twin (stormcellcore.cpp) pins the score shape SHEAR_SCORE_BASE + (1 - SHEAR_SCORE_BASE)*shear
    // as its own reference formula, parameterised on SHEAR_SCORE_BASE rather than hardcoding the original 0.5+0.5*shear
    // constant; it passes 68/68 against this file's current SHEAR_SCORE_BASE=0.
    constexpr F32 CELL_M            = 260.f;   // LOCKSTEP ssvolcloud.cpp CELL_M - only so tests can prove the lattice is incommensurate with it
    constexpr F32 LATTICE_M         = 2900.f;  // spawn lattice pitch: not a whole multiple of CELL_M, the 2080m noise tile, 880 or 1400 (nor they of it); shortest shared period with any of them is 37700m, beyond a visible field, and folding it into the deck's 800800m wrap span passes WRAP_MAX_M so lcmSpanM drops it (xsection_wind_storm pins both)
    constexpr F64 EPOCH_S           = 1800.0;  // one birth candidate per (lattice cell, epoch)
    constexpr F32 LIFE_MIN_S        = 1200.f;  // 20 min
    constexpr F32 LIFE_MAX_S        = 4200.f;  // 70 min; one-shot, never cycles
    constexpr F32 JITTER_FRAC       = 0.35f;   // birth position jitter as a share of LATTICE_M, per axis
    constexpr F32 RADIUS_MIN_M      = 900.f;   // influence radius at maturity for intensity 0
    constexpr F32 RADIUS_MAX_M      = 2200.f;  // ... for intensity 1
    constexpr F32 SPAWN_THRESHOLD   = 0.245f;  // gate: potential * convection * moisture * (SHEAR_SCORE_BASE + (1 - SHEAR_SCORE_BASE) * shear) must clear this
    constexpr F32 INTENSITY_FULL_SCORE = 0.65f; // 7f calibration: a gate score at or above this is a full-intensity storm (was 1.0 - a typical severe score of 0.3-0.5 mapped to intensity 0.0-0.15, so every storm was weak and no meso gate could pass)
    constexpr F32 HERO_INTENSITY_MIN = 0.7f;   // 7f: the composed hero's intensity floor - the one staged cell is a strong one (the show principle); background cells keep the gate's own figure
    constexpr F32 SHEAR_SCORE_BASE  = 0.0f;    // gate score shear term floor at mShearNoise == 0; the term is 1 at mShearNoise == 1 regardless (kept from the original 0.5+0.5*shear shape)
    constexpr F32 SUPERCELL_ROT_MIN = 0.501f;  // |rotation| * shear must clear this for a supercell (and Allow Supercells)
    constexpr F32 ANTICYCLONIC_ODDS = 0.03f;   // share of candidates whose rotation sign flips
    constexpr F32 DEVIATION_MIN_DEG = 20.f;    // supercell motion deviates right (clockwise) of the anvil-level wind by this much...
    constexpr F32 DEVIATION_MAX_DEG = 30.f;    // ... to this much, scaled by |rotation|; sign follows the rotation sign
    constexpr F32 STORM_SPEED_FRAC  = 0.75f;   // supercells move slower than the anvil-level flow
    constexpr F32 CELL_SPEED_FRAC   = 0.85f;   // ordinary cells too, a little less so
    constexpr F32 HERO_SPAWN_MIN_M  = 200.f;   // 7f: the hero's spawn distance up the motion vector is now DERIVED (speed * closest-approach lead) and clamped to this range
    constexpr F32 HERO_SPAWN_MAX_M  = 30000.f; // 7f calibration: a 13.5 m/s storm with a 4200 s life needs 28 km to reach the anchor at mid-life; the 2 km cap made every hero arrive two minutes old (age01 0.07 over 2400 draws) and a 12 km cap still left the long-lived half arriving at 0.21, before the meso window opens. The hero is cull-exempt, so it may be born outside the 12 km field and travel in
    constexpr F32 HERO_PASS_MIN_M   = 0.f;     // 7f (user): the closest approach ranges 0..2 km, skewed toward 0 - the show passes over or beside the region, not always far away
    constexpr F32 HERO_PASS_MAX_M   = 2000.f;
    constexpr F32 HERO_PASS_SKEW    = 2.74f;   // pass = HERO_PASS_MAX_M * u^HERO_PASS_SKEW: median 300 m (0.15^(1/2.74) = 0.5); quartiles 45 m and 909 m (a one-parameter power law fixes the median, the quartiles follow)
    constexpr F32 HERO_CLOSEST_AGE01 = 0.5f;   // the closest approach happens at this age (mature) unless a spawn clamp bites: a fast wind arrives earlier (30 km cap), a near-calm one (< 0.1 m/s) later
    constexpr F32 LIFE_ENUM_MARGIN  = 1.05f;   // 8a F6: resolveActive enumerates epochs back to LIFE_MAX_S * this - covers a line member's +LINE_JITTER_AGE01 (4%) lifetime
    constexpr F32 HERO_MIN_LIFE_S   = 1500.f;  // a hero must live long enough to reach and pass the anchor; shorter candidates are not hero-eligible

    struct Vec2
    {
        F32 x = 0.f;   // east, metres or m/s
        F32 y = 0.f;   // north
    };

    // Everything about a candidate that does not need the weather cube: a pure function of (seed, lx, ly, epoch).
    struct Candidate
    {
        U64 mId = 0;             // hash of (seed, lx, ly, epoch); the ONLY ranking key the shell may use
        S32 mLX = 0, mLY = 0;    // lattice cell
        S64 mEpoch = 0;
        F64 mBirthTime = 0.0;    // wall-clock seconds: mEpoch * EPOCH_S + hashed offset in [0, EPOCH_S)
        F32 mLifetimeS = 0.f;    // hashed in [LIFE_MIN_S, LIFE_MAX_S]
        Vec2 mOriginXY;          // world: lattice cell centre + hashed jitter within +-JITTER_FRAC * LATTICE_M per axis (hero cells override this, see composeHero)
        F32 mPotential = 0.f;    // P in [0,1]: 0.5 + 0.5 * fbm2 over the lattice (SSAtmoNoise, seed ^ salt), so neighbours correlate into storm clusters
        F32 mRotation = 0.f;     // Omega in [-1,1]: magnitude hashed, sign + except for the ANTICYCLONIC_ODDS share
        F32 mShearNoise = 0.f;   // SH in [0,1]: a slow independent fbm2 channel, its own salt
        F32 mPassSide = 1.f;     // +1 or -1: which side of the anchor the hero passes
        F32 mSpawnFrac = 0.f;    // [0,1] into [HERO_SPAWN_MIN_M, HERO_SPAWN_MAX_M]
        F32 mPassFrac = 0.f;     // [0,1] into [HERO_PASS_MIN_M, HERO_PASS_MAX_M]
    };

    // What the shell evaluated from the weather cube AT mBirthTime (never at "now"), plus the per-track checkboxes.
    struct WeatherAtBirth
    {
        F32 mMoisture = 0.f;
        F32 mConvection = 0.f;
        F32 mShearStrength = 0.f;  // the wind profile's S at birth
        bool mAllowSupercells = false;
        bool mAllowTornadoes = false;
    };

    struct Gate
    {
        bool mSpawn = false;
        F32 mIntensity = 0.f;         // [0,1]: how far over the threshold, eased
        bool mSupercell = false;
        bool mTornadoEligible = false; // supercell && Allow Tornadoes && lifetime >= HERO_MIN_LIFE_S
    };

    enum EStage : S32
    {
        STAGE_TCU = 0,      // [0.00, 0.15) towering cumulus
        STAGE_MATURING,     // [0.15, 0.30)
        STAGE_MATURE,       // [0.30, 0.55)
        STAGE_ANVIL,        // [0.55, 0.80)
        STAGE_DECAY         // [0.80, 1.00]
    };

    struct Lifecycle
    {
        EStage mStage = STAGE_TCU;
        F32 mTowerBoost = 0.f;  // [0,1] local tower/consolidation push, bell over the life, peaks in MATURE
        F32 mAnvil = 0.f;       // [0,1] rises through MATURING, full through ANVIL, fades in DECAY
        F32 mMeso = 0.f;        // [0,1] rotation strength for a supercell: ramps in MATURING, peaks MATURE, spins down in DECAY; 0 for non-supercells
        F32 mOvershoot = 0.f;   // [0,1] overshooting-top weight, MATURE only
        F32 mMammatus = 0.f;    // [0,1] ANVIL and early DECAY
        F32 mRadiusFrac = 0.f;  // [0,1] share of radiusAt's maturity radius currently reached
    };

    struct Hero
    {
        Vec2 mOrigin;          // world birth position
        Vec2 mMotion;          // m/s
        Vec2 mClosest;         // the point of closest approach to the anchor
        F32 mClosestDistM = 0.f;
        F64 mClosestTime = 0.0; // wall clock when the centre passes mClosest
    };

    // <SS:Nexii> S6: one resolved cell, everything resolveActive derives for it at "now" - the shell's own ActiveCell
    // (ssstormcells.h) wraps this with the shell-only bookkeeping (nothing here needs an id column: mCandidate.mId
    // IS the ranking key). mOrigin/mMotion are the lattice birth values EXCEPT for the hero cell, whose entry
    // resolveActive overwrites with composeHero's origin/motion (mCandidate stays the untouched lattice draw, so
    // mCandidate.mOriginXY is still readable as "the lattice cell", separately from mOrigin, "where this cell
    // actually is"). SCHEDULER (doc/atmo_magic_storm_dynamics.md sections 5-6): mLineId is the squall line's id
    // (SSSquall::LineEvent::mLineId) when this cell was emitted as a line member INSTEAD of a discrete lattice draw
    // for its epoch, else 0 - 0 is never a real line id (SALT_LINE_ID_HI's high word is combined into it, see
    // sssquallcore.h) so a plain equality test tells a line member from an ordinary cell. mIsForced is true for the
    // (at most one) candidate the shell pinned from an authored/forced override (SSSquall::forcedCandidate); a
    // forced cell is never also a line member.
    struct ActiveCell
    {
        Candidate mCandidate;
        WeatherAtBirth mWeather;
        Gate mGate;
        Lifecycle mLifecycle;
        Vec2 mOrigin;
        Vec2 mMotion;
        Vec2 mCentre;
        F32 mAge01 = 0.f;
        F32 mRadiusM = 0.f;
        bool mIsHero = false;
        U64 mLineId = 0;
        bool mIsForced = false;
        S32 mForcedKind = 0;      // 7d: the authored override kind when mIsForced (SSSquall::ForcedOverride::mKind: 1 supercell, 2 tornado, 3 waterspout, 4 anticyclonic), else 0 - the vortex core reads it to guarantee the funnel
    };

    // <SS:Nexii> SCHEDULER: the epoch-level line decision resolveActive needs, generic over whatever emits it
    // (sssquallcore.h's SSSquall::lineEvent/lineMember today) so this core never includes sssquallcore.h - that
    // header already includes THIS one (Candidate, LATTICE_M, ...), and a core never includes a core that includes
    // it (no cycles). mMembers[0..mMemberCount) are the line's own candidates (already gated the normal way by the
    // caller), mOrigin/mDirection/mHalfLengthM describe the segment discrete candidates are tested against
    // (distanceToSegment below), mSuppressRadiusM is how close a discrete draw must be to that segment before the
    // line replaces it. mMemberCount is clamped to LINE_MEMBERS_CAP by the caller (the shell); a caller emitting
    // more members than the cap silently loses the excess, same discipline as every other fixed buffer in this file.
    constexpr S32 LINE_MEMBERS_CAP = 8; // >= SSSquall::LINE_MEMBERS_MAX (7); a core constant only because this file cannot see the squall one
    struct LineDesc
    {
        bool mIsLine = false;
        U64 mLineId = 0;
        Vec2 mOrigin;
        Vec2 mDirection;
        Vec2 mWindAnvil;          // 7b F2: the ONE anvil-wind sample (at the epoch's phase) every member's motion derives from - never the per-member birth-phase wind, or the line's direction/origin and its members' travel disagree whenever the wind veers inside an epoch
        F32 mHalfLengthM = 0.f;
        F32 mSuppressRadiusM = 0.f;
        S32 mMemberCount = 0;
        Candidate mMembers[LINE_MEMBERS_CAP];
        bool mForced = false;         // 8a: an AUTHORED line (SSSquall::forcedLine) - the shell's band fill prefers it over any hashed line (audit F2)
        bool mHasFloor = false;       // 8a F1: when true, every member's live weather is raised through applyWeatherFloor(mWeatherFloor) before gate() - the same floor an authored pin gets; without it the generator's deliberately dry pre-cue sky gated every member out and the authored wall never spawned
        WeatherAtBirth mWeatherFloor;
    };

    // <SS:Nexii> SCHEDULER: the (at most one) authored/forced candidate the shell resolved this frame from the
    // track's override keyframes (SSSquall::forcedCandidate), generic for the same reason LineDesc is. mCandidate
    // is the pinned candidate (birth/lifetime/rotation/etc already overwritten to guaranteed-active constants by
    // forcedCandidate); mWeatherFloor is applied to the live weather read at its birth time via applyWeatherFloor,
    // BEFORE gate() - never in place of the live read: the floor raises moisture/convection/shear to its own values
    // and GRANTS the Allow-Supercells/Tornadoes flags (an authored override is the author's explicit word over the
    // Allow checkboxes - 7c NEW-3); what it does NOT override is the Weather Influence master enable, which the shell
    // gates the whole forced block on. mPreferHero is the shell's translation of "kind == tornado": when
    // true and this candidate spawns, resolveActive makes it the hero regardless of the ascending-id search that
    // picks the hero for every other epoch (a forced tornado must hit its cue, not lose out to an earlier id).
    struct ForcedDesc
    {
        bool mPinned = false;
        Candidate mCandidate;
        WeatherAtBirth mWeatherFloor;
        bool mPreferHero = false;
        Vec2 mOffsetM;            // 7b F1: the authored track-floor-relative offset - composeForced puts the storm's centre at anchor + this at mCueTime
        F64 mCueTime = 0.0;       // 7b F1: the cue's wall-clock instant (already converted by the shell with the SAME phase map it read the cue through, F3)
        S32 mKind = 0;            // 7d: the authored kind, copied onto ActiveCell::mForcedKind so SSVortex::childVortex can pin the funnel
    };

    // <SS:Nexii> SCHEDULER: default no-op hooks so every existing caller that does not pass a LineFn/ForcedFn to
    // resolveActive (the shipped shell before this change, every scenario_*_two_clients.cpp test) still compiles
    // unchanged - NoLineFn returns an all-default LineDesc (mIsLine false, mMemberCount 0) and NoForced an
    // all-default ForcedDesc (mPinned false), so the new code paths below are dead weight, not a behaviour change,
    // for any caller that omits them.
    struct NoLineFn
    {
        LineDesc operator()(S64) const { return LineDesc(); }
    };
    struct NoForced
    {
        ForcedDesc operator()() const { return ForcedDesc(); }
    };
    // 7f: the hero-contact hook - given the hero's ActiveCell (pre-composition) and the closest-approach age, the
    // FUNNEL contact offset from the cell centre at that age (the shell computes SSVortex::childVortex slot 0's
    // offsetFrac * radiusAt(age) * (cos, sin)(offsetAngle); this core cannot see the vortex core). Default: zero.
    struct NoHeroContact
    {
        Vec2 operator()(const ActiveCell&, F32) const { return Vec2(); }
    };

    // Point-to-segment distance in the plane: the segment is [origin - dir*halfLenM, origin + dir*halfLenM] with dir
    // assumed unit length. Invariants: 0 when p lies on the segment; equals the perpendicular distance to the
    // infinite line when the foot falls inside the segment; equals the distance to the nearer endpoint outside it;
    // symmetric under dir -> -dir; never negative.
    inline F32 distanceToSegment(const Vec2& p, const Vec2& origin, const Vec2& dir, F32 halfLenM)
    {
        const F32 dx = p.x - origin.x;
        const F32 dy = p.y - origin.y;
        const F32 t = llclamp(dx * dir.x + dy * dir.y, -halfLenM, halfLenM);
        const F32 ex = origin.x + dir.x * t - p.x;
        const F32 ey = origin.y + dir.y * t - p.y;
        return std::sqrt(ex * ex + ey * ey);
    }

    // Component-wise floor: raises each scalar in w to at least floor's value and ORs the two Allow flags - never
    // lowers anything the live cube already granted. Invariants: floor left at WeatherAtBirth() (all zero/false)
    // returns w unchanged; result >= w and >= floor on every scalar field; result's Allow flags are true whenever
    // either input's is; idempotent (applyWeatherFloor(applyWeatherFloor(w,f),f) == applyWeatherFloor(w,f)).
    inline WeatherAtBirth applyWeatherFloor(const WeatherAtBirth& w, const WeatherAtBirth& floor)
    {
        WeatherAtBirth r;
        r.mMoisture = llmax(w.mMoisture, floor.mMoisture);
        r.mConvection = llmax(w.mConvection, floor.mConvection);
        r.mShearStrength = llmax(w.mShearStrength, floor.mShearStrength);
        r.mAllowSupercells = w.mAllowSupercells || floor.mAllowSupercells;
        r.mAllowTornadoes = w.mAllowTornadoes || floor.mAllowTornadoes;
        return r;
    }

    // floor(t / EPOCH_S) as a signed integer. Invariants: epochOf(EPOCH_S * k) == k for integer k; epochOf(t) is
    // non-decreasing in t; a candidate's birth always satisfies epochOf(mBirthTime) == mEpoch.
    inline S64 epochOf(F64 t)
    {
        return (S64)floor(t / EPOCH_S);
    }

    // GLSL-semantics smoothstep, local so this core needs nothing beyond lldefs.h: 0 for x <= e0, 1 for x >= e1, monotone.
    inline F32 smoothstep(F32 e0, F32 e1, F32 x)
    {
        if (e1 <= e0)
        {
            return (x <= e0) ? 0.f : 1.f;
        }
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // One-shot bell: 0 at or below lo, rises to 1 at mid, back to 0 at or above hi; continuous.
    inline F32 hump(F32 x, F32 lo, F32 mid, F32 hi)
    {
        return (x <= mid) ? smoothstep(lo, mid, x) : 1.f - smoothstep(mid, hi, x);
    }

    // Per-field salts mixed into the candidate's (seed, lx, ly, epoch) chain hash with SSAtmoNoise::combine before hash01.
    // Every hashed field has its own salt so no two fields ever read the same random draw.
    constexpr U32 SALT_ID_HI      = 0x53430001u; // high 32 bits of mId (the low 32 are the raw chain hash)
    constexpr U32 SALT_BIRTH      = 0x53430002u; // birth offset within the epoch, [0, EPOCH_S)
    constexpr U32 SALT_LIFE       = 0x53430003u; // lifetime, [LIFE_MIN_S, LIFE_MAX_S]
    constexpr U32 SALT_JITTER_X   = 0x53430004u; // origin jitter east, +-JITTER_FRAC * LATTICE_M
    constexpr U32 SALT_JITTER_Y   = 0x53430005u; // origin jitter north
    constexpr U32 SALT_ROT_MAG    = 0x53430006u; // |rotation|, [0,1]
    constexpr U32 SALT_ROT_SIGN   = 0x53430007u; // rotation sign: negative when the draw < ANTICYCLONIC_ODDS
    constexpr U32 SALT_PASS_SIDE  = 0x53430008u; // hero pass side: -1 when the draw < 0.5, else +1
    constexpr U32 SALT_SPAWN_FRAC = 0x53430009u; // hero spawn distance fraction
    constexpr U32 SALT_PASS_FRAC  = 0x5343000Au; // hero closest-approach fraction
    constexpr U32 SALT_POTENTIAL  = 0x5343000Bu; // fbm2 seed (combined with the shared seed, not the chain) for mPotential
    constexpr U32 SALT_SHEAR      = 0x5343000Cu; // fbm2 seed for mShearNoise
    constexpr F32 FIELD_SCALE     = 1.20f;       // fbm2 units per lattice cell for mPotential/mShearNoise: at 0.31 a 12km field spans only ~2.6 fbm units (highly correlated, lumpy all-or-nothing anchor-hours); at 1.20 it spans ~10 units, decorrelating the ~54 cells enough that anyCell%/tornado% converge and the 40-anchor sample mean stops drifting with sampling luck (measured: design meanCells 5.16->4.75, any% 69.3->96.8, tornado% 38.9->50.8, see scenario_storm_density.cpp)
    constexpr F64 POTENTIAL_DRIFT = 0.05;        // fbm2 units the potential field scrolls per epoch (slow evolution, no period)
    constexpr F64 SHEAR_DRIFT     = 0.02;        // ... and the slower shear channel

    // The pure candidate for one lattice cell and epoch. Uses SSAtmoNoise::combine/hash01 on (seed, lx, ly, epoch) with
    // distinct salts per field, and SSAtmoNoise::fbm2 over (lx, ly) at a slow scale for mPotential and mShearNoise so
    // neighbouring cells correlate. Invariants: bit-identical for equal inputs; mId differs across any change of lx, ly,
    // epoch or seed (sampled); mBirthTime in [epoch*EPOCH_S, (epoch+1)*EPOCH_S); mLifetimeS in [LIFE_MIN_S, LIFE_MAX_S];
    // mOriginXY within +-JITTER_FRAC*LATTICE_M of the cell centre ((lx+0.5)*LATTICE_M, (ly+0.5)*LATTICE_M); mPotential,
    // mShearNoise in [0,1]; mRotation in [-1,1] with negative sign on roughly ANTICYCLONIC_ODDS of a large sample (2-4%);
    // mPassSide is exactly +1 or -1, both occurring; mSpawnFrac, mPassFrac in [0,1].
    inline Candidate candidate(U32 seed, S32 lx, S32 ly, S64 epoch)
    {
        using SSAtmoNoise::combine;
        using SSAtmoNoise::hash01;
        Candidate c;
        c.mLX = lx;
        c.mLY = ly;
        c.mEpoch = epoch;

        // Chain hash over (seed, lx, ly, epoch low, epoch high); every per-cell draw is hash01(combine(chain, salt)).
        const U32 epochLo = (U32)((U64)epoch & 0xffffffffu);
        const U32 epochHi = (U32)(((U64)epoch >> 32) & 0xffffffffu);
        U32 chain = combine(seed, (U32)lx);
        chain = combine(chain, (U32)ly);
        chain = combine(chain, epochLo);
        chain = combine(chain, epochHi);
        c.mId = ((U64)combine(chain, SALT_ID_HI) << 32) | (U64)chain;

        // hash01 < 1 strictly, so the birth stays inside [epoch * EPOCH_S, (epoch + 1) * EPOCH_S).
        c.mBirthTime = (F64)epoch * EPOCH_S + (F64)hash01(combine(chain, SALT_BIRTH)) * EPOCH_S;
        c.mLifetimeS = std::lerp(LIFE_MIN_S, LIFE_MAX_S, hash01(combine(chain, SALT_LIFE)));

        const F32 jitterM = JITTER_FRAC * LATTICE_M;
        c.mOriginXY.x = ((F32)lx + 0.5f) * LATTICE_M + (hash01(combine(chain, SALT_JITTER_X)) * 2.f - 1.f) * jitterM;
        c.mOriginXY.y = ((F32)ly + 0.5f) * LATTICE_M + (hash01(combine(chain, SALT_JITTER_Y)) * 2.f - 1.f) * jitterM;

        // Correlated fields: fbm2 over the lattice at FIELD_SCALE, scrolled slowly per epoch. Coordinates are formed in F64
        // and rounded once so a large epoch count does not eat the per-cell resolution.
        const F64 e = (F64)epoch;
        const F32 px = (F32)((F64)lx * FIELD_SCALE + 0.5 + e * POTENTIAL_DRIFT);
        const F32 py = (F32)((F64)ly * FIELD_SCALE + 0.5 - e * POTENTIAL_DRIFT * 0.7);
        c.mPotential = llclamp(0.5f + 0.5f * SSAtmoNoise::fbm2(px, py, combine(seed, SALT_POTENTIAL)), 0.f, 1.f);
        const F32 sx = (F32)((F64)lx * FIELD_SCALE + 7.3 - e * SHEAR_DRIFT);
        const F32 sy = (F32)((F64)ly * FIELD_SCALE - 3.1 + e * SHEAR_DRIFT * 0.6);
        c.mShearNoise = llclamp(0.5f + 0.5f * SSAtmoNoise::fbm2(sx, sy, combine(seed, SALT_SHEAR)), 0.f, 1.f);

        const F32 rotMag = hash01(combine(chain, SALT_ROT_MAG));
        c.mRotation = (hash01(combine(chain, SALT_ROT_SIGN)) < ANTICYCLONIC_ODDS) ? -rotMag : rotMag;
        c.mPassSide = (hash01(combine(chain, SALT_PASS_SIDE)) < 0.5f) ? -1.f : 1.f;
        c.mSpawnFrac = hash01(combine(chain, SALT_SPAWN_FRAC));
        c.mPassFrac = hash01(combine(chain, SALT_PASS_FRAC));
        return c;
    }

    // Whether the candidate is alive at now, and its normalised age. Invariants: alive iff mBirthTime <= now <
    // mBirthTime + mLifetimeS; age01 in [0,1] while alive, clamped outside.
    inline bool alive(const Candidate& c, F64 now)
    {
        return now >= c.mBirthTime && now < c.mBirthTime + (F64)c.mLifetimeS;
    }
    inline F32 age01(const Candidate& c, F64 now)
    {
        if (c.mLifetimeS <= 0.f)
        {
            return (now < c.mBirthTime) ? 0.f : 1.f;
        }
        return llclamp((F32)((now - c.mBirthTime) / (F64)c.mLifetimeS), 0.f, 1.f);
    }

    // The weather gate, evaluated by the shell with the cube sampled at the candidate's BIRTH time.
    //   score = mPotential * convection * moisture * (SHEAR_SCORE_BASE + (1 - SHEAR_SCORE_BASE) * mShearNoise)
    //   mSpawn = score >= SPAWN_THRESHOLD; mIntensity = smoothstep(SPAWN_THRESHOLD, 1.0, score)
    //   mSupercell = mSpawn && allowSupercells && |mRotation| * shearStrength >= SUPERCELL_ROT_MIN
    //   mTornadoEligible = mSupercell && allowTornadoes && mLifetimeS >= HERO_MIN_LIFE_S
    // Invariants: zero convection or zero moisture never spawns; a candidate with potential 1 under (1,1,1) spawns with
    // intensity 1; mSupercell false when allowSupercells is false regardless of rotation; mTornadoEligible implies
    // mSupercell; monotone in convection and in moisture.
    // The gate's two scalar terms, named so the shell's "why not" readout reads the SAME numbers gate() decides on
    // (doc/atmo_magic_debug_views.md V2) instead of respelling the formula: gateScore is the spawn score above,
    // rotationTerm is |mRotation| * shearStrength. Both pure; gate() is defined in terms of them.
    inline F32 gateScore(const Candidate& c, const WeatherAtBirth& w)
    {
        const F32 convection = llclamp(w.mConvection, 0.f, 1.f);
        const F32 moisture = llclamp(w.mMoisture, 0.f, 1.f);
        return c.mPotential * convection * moisture * (SHEAR_SCORE_BASE + (1.f - SHEAR_SCORE_BASE) * c.mShearNoise);
    }
    inline F32 rotationTerm(const Candidate& c, const WeatherAtBirth& w)
    {
        return std::fabs(c.mRotation) * w.mShearStrength;
    }
    inline Gate gate(const Candidate& c, const WeatherAtBirth& w)
    {
        Gate g;
        const F32 score = gateScore(c, w);
        g.mSpawn = score >= SPAWN_THRESHOLD;
        g.mIntensity = g.mSpawn ? smoothstep(SPAWN_THRESHOLD, INTENSITY_FULL_SCORE, score) : 0.f; // 7f calibration, see INTENSITY_FULL_SCORE
        g.mSupercell = g.mSpawn && w.mAllowSupercells && rotationTerm(c, w) >= SUPERCELL_ROT_MIN;
        g.mTornadoEligible = g.mSupercell && w.mAllowTornadoes && c.mLifetimeS >= HERO_MIN_LIFE_S;
        return g;
    }

    // The one-shot lifecycle at age01 (bell curves, no cycling; every field 0 at age01 <= 0 and at age01 >= 1 except
    // mStage). Stage bands exactly as the enum documents. Invariants: mTowerBoost peaks in [0.30, 0.55] and is
    // continuous; mAnvil is 0 before 0.15, >= 0.9 somewhere in [0.55, 0.80], 0 at 1; mMeso == 0 when !supercell,
    // else 0 before 0.15 and continuous; mOvershoot > 0 only inside [0.30, 0.55]; mMammatus > 0 only inside
    // [0.55, 0.90]; mRadiusFrac non-decreasing until 0.55 then non-increasing; all fields scale with intensity (0 -> 0).
    inline Lifecycle lifecycle(F32 age01, F32 intensity, F32 rotation, bool supercell)
    {
        Lifecycle l;
        const F32 a = llclamp(age01, 0.f, 1.f);
        const F32 k = llclamp(intensity, 0.f, 1.f);
        l.mStage = (a < 0.15f) ? STAGE_TCU
                 : (a < 0.30f) ? STAGE_MATURING
                 : (a < 0.55f) ? STAGE_MATURE
                 : (a < 0.80f) ? STAGE_ANVIL
                 : STAGE_DECAY;
        l.mTowerBoost = k * hump(a, 0.f, 0.42f, 1.f);                                        // bell over the life, peak in MATURE
        l.mAnvil      = k * smoothstep(0.15f, 0.55f, a) * (1.f - smoothstep(0.80f, 1.f, a)); // rises MATURING, 1 through ANVIL, fades DECAY
        l.mMeso       = supercell ? k * llclamp(std::fabs(rotation), 0.f, 1.f)
                                    * smoothstep(0.15f, 0.35f, a) * (1.f - smoothstep(0.70f, 1.f, a)) : 0.f;
        l.mOvershoot  = k * hump(a, 0.30f, 0.425f, 0.55f);
        l.mMammatus   = k * hump(a, 0.55f, 0.725f, 0.90f);
        l.mRadiusFrac = k * ((a <= 0.55f) ? smoothstep(0.f, 0.55f, a) : 1.f - smoothstep(0.55f, 1.f, a));
        return l;
    }

    // Influence radius at age01 for an intensity: lerp(RADIUS_MIN_M, RADIUS_MAX_M, intensity) * lifecycle radius fraction.
    // Invariants: 0 at age01 <= 0; <= RADIUS_MAX_M always; monotone in intensity.
    inline F32 radiusAt(F32 age01, F32 intensity)
    {
        const F32 k = llclamp(intensity, 0.f, 1.f);
        // Lifecycle at intensity 1 so the radius fraction is not scaled by intensity twice.
        return std::lerp(RADIUS_MIN_M, RADIUS_MAX_M, k) * lifecycle(age01, 1.f, 0.f, false).mRadiusFrac;
    }

    // Storm motion from the ANVIL-LEVEL wind vector at birth (the shell samples SSWindProfile::windAt(anvilZ) at the
    // birth phase): supercells move at STORM_SPEED_FRAC of it, rotated by lerp(DEVIATION_MIN_DEG, DEVIATION_MAX_DEG,
    // |rotation|) CLOCKWISE (to the right, northern-hemisphere convention: in this frame heading grows clockwise, so
    // rotate the vector clockwise, i.e. (x, y) -> (x cos a + y sin a, -x sin a + y cos a) for a > 0) when rotation > 0
    // and anticlockwise when rotation < 0; ordinary cells move at CELL_SPEED_FRAC with no deviation. Invariants:
    // |motion| == frac * |wind| within 1e-4 relative; a supercell with rotation +1 under a north wind (0, v) moves
    // with a positive x component (east of north) and a rotation -1 one with negative x; zero wind gives zero motion.
    inline Vec2 stormMotion(const Vec2& windAnvil, F32 rotation, bool supercell)
    {
        Vec2 m;
        if (!supercell)
        {
            m.x = windAnvil.x * CELL_SPEED_FRAC;
            m.y = windAnvil.y * CELL_SPEED_FRAC;
            return m;
        }
        constexpr F32 kDegToRad = 0.017453292519943295f;
        const F32 rotMag = llclamp(std::fabs(rotation), 0.f, 1.f);
        const F32 devDeg = std::lerp(DEVIATION_MIN_DEG, DEVIATION_MAX_DEG, rotMag);
        const F32 a = ((rotation < 0.f) ? -devDeg : devDeg) * kDegToRad; // a > 0 rotates clockwise (to the right)
        const F32 ca = std::cos(a);
        const F32 sa = std::sin(a);
        m.x = (windAnvil.x * ca + windAnvil.y * sa) * STORM_SPEED_FRAC;
        m.y = (-windAnvil.x * sa + windAnvil.y * ca) * STORM_SPEED_FRAC;
        return m;
    }

    // 7f HERO COMPOSITION (user, 2026-09-06: "0 m..2 km, skewed toward 0, 50% within ~300 m; and stop dying far upwind").
    // The hero's straight-line path is composed so that its FUNNEL CONTACT point (cell centre + contactOffsetM, the
    // slot-0 vortex's offset at the closest-approach age as the shell computes it through SSVortex; zero when unknown)
    // passes the anchor at pass = HERO_PASS_MAX_M * mPassFrac^HERO_PASS_SKEW metres along the motion's right-hand
    // perpendicular (side from mPassSide), at the age HERO_CLOSEST_AGE01 of the hero's life: closestTime = birth +
    // HERO_CLOSEST_AGE01 * lifetime, spawn = clamp(|motion| * (closestTime - birth), HERO_SPAWN_MIN_M, HERO_SPAWN_MAX_M)
    // (a slow wind spawns the hero nearer so it still arrives mature; when the clamp bites, closestTime is recomputed
    // from the clamped spawn so the two stay consistent), origin = closest - contactOffset - unitMotion * spawn.
    // Invariants: mClosestDistM == pass within 1e-2 and in [0, HERO_PASS_MAX_M]; over 10000 uniform mPassFrac values the
    // median pass is within 30 m of 300 m; centreAt(origin, motion, birth, closestTime) + contactOffset == mClosest
    // within F32 reach (0.25 m at a 256 km anchor, where one ulp is 0.03 m); closestTime - birth == HERO_CLOSEST_AGE01 *
    // lifetime while neither spawn clamp bites, EARLIER when the high clamp (HERO_SPAWN_MAX_M) bites (fast wind, long
    // life) and LATER - possibly past death - when the low clamp (HERO_SPAWN_MIN_M) bites (speed below ~0.1 m/s);
    // dot(origin + contactOffset - anchor, motion) <= 0 (upwind); |motion| == 0 degenerates to origin = closest -
    // contactOffset + spawn metres north with closestTime = birth (no NaN); pure; bit-identical for equal inputs.
    // 7f: the closest-approach age composeHero will actually produce for this candidate at this speed - HERO_CLOSEST_AGE01
    // unless the spawn clamp bites (spawn = clamp(speed * HERO_CLOSEST_AGE01 * lifetime, HERO_SPAWN_MIN_M, HERO_SPAWN_MAX_M),
    // closest = birth + spawn / speed). The shell's hero-contact hook and the vortex Parent must both use THIS age, never
    // the nominal constant (lesson 9). Invariants: == (composeHero(...).mClosestTime - birth) / lifetime within 1e-6;
    // <= HERO_CLOSEST_AGE01 whenever the low spawn clamp does not bite (it can exceed it, up to 1, at speeds below
    // ~0.1 m/s where the 200 m minimum spawn takes longer than half a life); 0 for zero speed; pure.
    inline F32 heroClosestAge01(const Candidate& c, F32 speed)
    {
        const F32 life = llmax(c.mLifetimeS, 1e-6f);
        if (speed < 1e-6f) return 0.f;
        const F64 lead = (F64)HERO_CLOSEST_AGE01 * (F64)life;
        const F32 spawn = llclamp((F32)((F64)speed * lead), HERO_SPAWN_MIN_M, HERO_SPAWN_MAX_M);
        return llclamp((F32)((F64)(spawn / speed) / (F64)life), 0.f, 1.f);
    }

    inline Hero composeHero(const Candidate& c, const Vec2& anchor, const Vec2& stormMotion, const Vec2& contactOffsetM = Vec2())
    {
        Hero h;
        h.mMotion = stormMotion;
        const F32 u = llclamp(c.mPassFrac, 0.f, 1.f);
        const F32 pass = HERO_PASS_MAX_M * std::pow(u, HERO_PASS_SKEW);
        const F32 side = (c.mPassSide < 0.f) ? -1.f : 1.f;
        const F32 speed = std::sqrt(stormMotion.x * stormMotion.x + stormMotion.y * stormMotion.y);
        const F64 lead = (F64)HERO_CLOSEST_AGE01 * (F64)llmax(c.mLifetimeS, 0.f);
        if (speed < 1e-6f)
        {
            // Degenerate: no motion direction, so no perpendicular. Park the hero HERO_SPAWN_MIN_M north of the anchor; it
            // never approaches, so the closest point is where it sits and the closest time is birth.
            h.mClosest = anchor;
            h.mOrigin.x = anchor.x - contactOffsetM.x;
            h.mOrigin.y = anchor.y - contactOffsetM.y + HERO_SPAWN_MIN_M;
            h.mClosestDistM = 0.f;
            h.mClosestTime = c.mBirthTime;
            return h;
        }
        const F32 dx = stormMotion.x / speed;
        const F32 dy = stormMotion.y / speed;
        const F32 px = dy;   // right-hand perpendicular of the motion: (my, -mx) normalised
        const F32 py = -dx;
        F32 spawn = llclamp((F32)((F64)speed * lead), HERO_SPAWN_MIN_M, HERO_SPAWN_MAX_M);
        h.mClosest.x = anchor.x + px * pass * side;
        h.mClosest.y = anchor.y + py * pass * side;
        h.mOrigin.x = h.mClosest.x - contactOffsetM.x - dx * spawn;
        h.mOrigin.y = h.mClosest.y - contactOffsetM.y - dy * spawn;
        h.mClosestDistM = pass;
        h.mClosestTime = c.mBirthTime + (F64)(spawn / speed); // == birth + lead unless the spawn clamp bit
        return h;
    }

    // 7b F1 (doc/atmo_magic_storm_dynamics.md section 6 layer 3): the AUTHORED composition. Where composeHero stages a
    // flyby past the anchor from hashed fracs, composeForced places the storm so that its centre at cueTime is exactly
    // anchor + offsetM: origin = (anchor + offsetM) - motion * (cueTime - birth), motion unchanged. mClosest is that point,
    // mClosestTime the cue, mClosestDistM = |offsetM|. Invariants: centreAt(origin, motion, birth, cueTime) == anchor +
    // offsetM within 1e-2 m for |motion| <= 60 m/s and lead <= LIFE_MAX_S; with zero motion origin == anchor + offsetM;
    // pure; the offset is honoured for EVERY forced kind, not only the hero.
    inline Hero composeForced(const Candidate& c, const Vec2& anchor, const Vec2& offsetM, F64 cueTime, const Vec2& stormMotion)
    {
        Hero h;
        h.mMotion = stormMotion;
        h.mClosest.x = anchor.x + offsetM.x;
        h.mClosest.y = anchor.y + offsetM.y;
        const F64 lead = cueTime - c.mBirthTime;
        h.mOrigin.x = (F32)((F64)h.mClosest.x - (F64)stormMotion.x * lead);
        h.mOrigin.y = (F32)((F64)h.mClosest.y - (F64)stormMotion.y * lead);
        h.mClosestDistM = std::sqrt(offsetM.x * offsetM.x + offsetM.y * offsetM.y);
        h.mClosestTime = cueTime;
        return h;
    }

    // 7b F5: the birth-memo key. A forced pin shares its lattice twin's mId but carries a REWRITTEN birth time, and the
    // shell's memo lives across a 2 s bucket, so keying on mId alone let whichever variant memoised first serve the other
    // for up to 2 s - per client. Key on (id, birth quantised to 1 ms) instead. Invariants: pure; equal for equal (mId,
    // mBirthTime); differs whenever mBirthTime differs by >= 1 ms (for the same id) or mId differs (same birth).
    inline U64 memoKey(const Candidate& c)
    {
        const U64 birthMs = (U64)(S64)std::llround(c.mBirthTime * 1000.0);
        return c.mId ^ (birthMs * 0x9E3779B97F4A7C15ull);
    }

    // Closed-form centre: origin + motion * (now - birth). Invariants: equals origin at now == birth; linear in now;
    // bit-identical for equal inputs.
    inline Vec2 centreAt(const Vec2& origin, const Vec2& motion, F64 birth, F64 now)
    {
        const F32 age = (F32)(now - birth);
        Vec2 p;
        p.x = origin.x + motion.x * age;
        p.y = origin.y + motion.y * age;
        return p;
    }

    // Radial influence of a cell at pos: 1 - smoothstep(radius * 0.4, radius, dist) (edges spelled e0 < e1, never the degenerate step) - 1 inside 40% of the radius, 0 at
    // the radius and beyond, monotone non-increasing in dist; 0 everywhere when radius <= 0.
    inline F32 influence(const Vec2& centre, F32 radius, const Vec2& pos)
    {
        if (radius <= 0.f)
        {
            return 0.f;
        }
        const F32 dx = pos.x - centre.x;
        const F32 dy = pos.y - centre.y;
        const F32 dist = std::sqrt(dx * dx + dy * dy);
        return 1.f - smoothstep(radius * 0.4f, radius, dist); // the reversed-edge smoothstep, spelled so e0 < e1
    }

    // Wall-clock buckets for any time-discretised effect (the sslightning idiom; NEVER a frame counter): bucket index
    // floor(now / period) and a crossfade weight that rises 0 -> 1 over the last blendS seconds of each bucket so the
    // next bucket's value can be blended in without a step. Invariants: bucketBlend in [0,1]; 0 right after a bucket
    // boundary (when blendS < period); reaches 1 at the boundary; continuous in now.
    inline S64 bucket(F64 now, F64 period)
    {
        if (period <= 0.0)
        {
            return 0;
        }
        return (S64)floor(now / period);
    }
    inline F32 bucketBlend(F64 now, F64 period, F64 blendS)
    {
        if (period <= 0.0 || blendS <= 0.0)
        {
            return 0.f;
        }
        const F64 blend = llmin(blendS, period);
        const F64 phase = now - floor(now / period) * period;           // [0, period)
        const F64 t = llclamp((phase - (period - blend)) / blend, 0.0, 1.0);
        return (F32)(t * t * (3.0 - 2.0 * t));
    }

    // <SS:Nexii> Phase 8 section 2 (one clock, user 2026-09-06): previewPhaseAt/previewWallTimeAt REMOVED - the
    // preview no longer substitutes a second day-phase map; it substitutes the CLOCK the one map (SSDayCycle::
    // phaseAt/wallTimeAtPhase, ssdaycyclecore.h, called with offset 0 since cycle time already has the track's day
    // offset subtracted) is fed. See SSStormCells::update()'s mCycleRefS/mWallRefS latch (ssstormcells.cpp) for the
    // shell-side substitution and doc/atmo_magic_phase8_show.md section 2 for the design. Superseded 7b F3/7d.

    // Lattice cells whose centres lie within radiusM of centre (world), written as (lx, ly) pairs up to cap; returns the
    // count that WOULD be written (may exceed cap, in which case only cap pairs are). Deterministic order: rows then
    // columns ascending. Invariants: every returned cell centre is within radiusM; every lattice cell within radiusM is
    // returned (when cap allows); order independent of anything but the inputs.
    inline S32 enumerateLattice(const Vec2& centre, F32 radiusM, S32* outLX, S32* outLY, S32 cap)
    {
        if (radiusM < 0.f)
        {
            return 0;
        }
        // Cell centre (lx + 0.5) * LATTICE_M lies in [c - r, c + r] iff lx in [(c - r) / L - 0.5, (c + r) / L - 0.5].
        const F64 cx = centre.x, cy = centre.y, r = radiusM, L = LATTICE_M;
        const S32 lxMin = (S32)ceil((cx - r) / L - 0.5), lxMax = (S32)floor((cx + r) / L - 0.5);
        const S32 lyMin = (S32)ceil((cy - r) / L - 0.5), lyMax = (S32)floor((cy + r) / L - 0.5);
        const F64 r2 = r * r;
        S32 count = 0;
        for (S32 ly = lyMin; ly <= lyMax; ++ly)          // rows ascending...
        {
            const F64 dy = ((F64)ly + 0.5) * L - cy;
            for (S32 lx = lxMin; lx <= lxMax; ++lx)      // ... then columns ascending
            {
                const F64 dx = ((F64)lx + 0.5) * L - cx;
                if (dx * dx + dy * dy > r2)
                {
                    continue;
                }
                if (count < cap && outLX && outLY)
                {
                    outLX[count] = lx;
                    outLY[count] = ly;
                }
                ++count;
            }
        }
        return count;
    }

    // <SS:Nexii> S13 (phase-2b re-audit fixup 3): the no-op default for resolveActive's DiagFn template parameter, so
    // every existing caller (the shipped shell, scenario_storm_two_clients.cpp) that does not pass one still compiles
    // unchanged - a templated operator() swallows whatever (Candidate, WeatherAtBirth, Gate) resolveActive hands it.
    struct NoDiag
    {
        template <class... Args>
        void operator()(Args&&...) const {}
    };

    // <SS:Nexii> S6 (phase-2b audit): the WHOLE storm-cell pipeline, moved out of the shell so nothing but these two
    // hooks - weatherAtBirth(candidate) -> WeatherAtBirth, anvilWindAtBirth(candidate) -> Vec2, both pure functions
    // of a Candidate, never of "now" - can differ between two callers (the shipped shell today, a determinism-twin
    // harness tomorrow). Does exactly what SSStormCells::update() did: enumerate the lattice within radiusM of
    // anchor, walk epochs [epochOf(now - LIFE_MAX_S), epochOf(now)] oldest first, build each candidate, skip the
    // dead, gate the alive ones on weatherAtBirth(candidate), keep the spawned (written to out in enumeration order,
    // capped at `cap` - candidates beyond cap are dropped, not overflowed; size cap well above the S7 density
    // calibration's worst case), sort the survivors by mCandidate.mId (the only ranking key the contract allows -
    // never camera distance), take the smallest-id tornado-eligible survivor as the hero, compose its flyby with
    // composeHero(anchor) and overwrite THAT entry's mOrigin/mMotion (its mCandidate is untouched, so the lattice
    // draw is still readable), resolve every survivor's closed-form mAge01/mCentre/mLifecycle/mRadiusM at now, then
    // drop every NON-hero survivor whose mCentre has drifted beyond radiusM + RADIUS_MAX_M of anchor (compacted in
    // place, hero index remapped through the compaction). Returns the number of entries written to out (<= cap).
    // *heroIndexOut is that survivor's post-compaction index when a hero was composed, else -1 (both pointers may
    // be null if the caller does not want hero detail); *heroOut is the composed flyby geometry. The hero's own
    // stormMotion was already computed with mGate.mSupercell == true (mTornadoEligible implies mSupercell), so
    // composeHero is handed that ActiveCell's OWN mMotion rather than recomputing stormMotion a second time - the
    // same pure function applied to the same arguments gives the same vector, this just does not call it twice.
    // Nothing here reads a camera, a frame counter, a dt, or the eased SSAtmoMagic::mWind.
    // <SS:Nexii> S13 (phase-2b re-audit fixup 3): `diag`, called as diag(candidate, weatherAtBirth, gate) for EVERY
    // alive candidate in this function's own enumerate/epoch loop, right after the gate is computed and before the
    // spawn check/cull - the ONLY traversal of candidates in the pipeline. Lets SSStormCells::update() build its
    // WhyNot readout (best-scoring alive candidate, alive/spawned/supercell/tornado-eligible tallies) from THIS
    // walk instead of re-deriving enumerate/epoch/alive/gate by hand beside this call. Defaults to NoDiag so callers
    // that do not care (scenario_storm_two_clients.cpp) compile unchanged.
    // <SS:Nexii> SCHEDULER (doc/atmo_magic_storm_dynamics.md sections 5-6): `lineAtEpoch(epoch) -> LineDesc` is
    // called once per epoch, right before that epoch's lattice loop. When it returns mIsLine, the line's own
    // mMembers[0..mMemberCount) are gated and emitted FIRST (same alive/weatherAtBirth/gate/diag/spawn treatment as
    // an ordinary candidate, tagged with mLineId), and any of that epoch's DISCRETE lattice candidates within
    // mSuppressRadiusM of the line segment [mOrigin +- mDirection*mHalfLengthM] (distanceToSegment) are skipped -
    // the line replaces them, it does not add to them; STATED LATENT (8a audit): an authored line's members are emitted
    // inside their own epoch's block, after earlier epochs' discrete draws, so under cap pressure they can be truncated
    // where the authored pin (emitted first) cannot - at ACTIVE_CAP 512 against a severe-day population of 20-40 this
    // is unreachable; if the cap ever tightens, emit the forced line's members before the epoch loop; discrete candidates elsewhere in the same epoch are
    // untouched. `forced()` is called ONCE per resolveActive call (not per epoch - an authored cue is a single
    // pinned candidate, not an epoch-keyed lattice draw): when it returns mPinned, that ONE candidate is gated with
    // applyWeatherFloor(weatherAtBirth(c), mWeatherFloor) in place of the raw weatherAtBirth(c) - the floor can
    // only raise the odds and grant the Allow flags (an authored sky IS a supported sky; the master enable still
    // gates it in the shell, 7c NEW-3) - tagged mIsForced, and its
    // (seed, lx, ly, epoch) hash id is excluded from the discrete lattice loop wherever that exact epoch is
    // enumerated (candidate() is a pure function of those four, so forcedCandidate's internal pinned lattice draw
    // and the discrete loop's draw for the same cell/epoch share the SAME id - comparing ids is enough, no lx/ly/
    // epoch bookkeeping needed). Both hooks default to no-ops (NoLineFn/NoForced) so every existing caller (the
    // shipped shell before this change, every scenario_*_two_clients.cpp test) compiles and behaves unchanged.
    template <class WeatherFn, class WindFn, class DiagFn = NoDiag, class LineFn = NoLineFn, class ForcedFn = NoForced, class HeroContactFn = NoHeroContact>
    inline S32 resolveActive(U32 seed, F64 now, const Vec2& anchor, F32 radiusM,
                              WeatherFn weatherAtBirth, WindFn anvilWindAtBirth,
                              ActiveCell* out, S32 cap, Hero* heroOut, S32* heroIndexOut,
                              DiagFn diag = DiagFn(), LineFn lineAtEpoch = LineFn(), ForcedFn forced = ForcedFn(),
                              HeroContactFn heroContact = HeroContactFn())
    {
        if (heroOut) *heroOut = Hero();
        if (heroIndexOut) *heroIndexOut = -1;
        if (!out || cap <= 0)
        {
            return 0;
        }

        // Sized for the shell's own FIELD_M (12km ~= 110 cells, per SSStormCells::update's own comment); a caller
        // enumerating a much larger radiusM silently truncates here, the same contract enumerateLattice itself has.
        constexpr S32 kLatticeEnumCap = 256;
        S32 lx[kLatticeEnumCap];
        S32 ly[kLatticeEnumCap];
        S32 latticeCount = enumerateLattice(anchor, radiusM, lx, ly, kLatticeEnumCap);
        if (latticeCount > kLatticeEnumCap) latticeCount = kLatticeEnumCap;

        // 8a audit F6: reach back LIFE_MAX_S plus the line members' +4% lifetime jitter (LIFE_ENUM_MARGIN), or an
        // authored line (lifetime exactly LIFE_MAX_S) loses its positive-jitter members' last ~170 s for ~9% of cues.
        const S64 epochFirst = epochOf(now - (F64)LIFE_MAX_S * (F64)LIFE_ENUM_MARGIN);
        const S64 epochLast = epochOf(now);

        const ForcedDesc fd = forced();
        // 7b F7: the pin exists for the scheduler only while it is alive; a dormant override (kind held all day, cue
        // hours away) must not delete its lattice twin from the background population.
        const bool forcedAlive = fd.mPinned && alive(fd.mCandidate, now);

        // SCHEDULER: the one gate/emit sequence every candidate source below (forced pin, line member, discrete
        // lattice draw) runs through, so the three call sites cannot diverge on what "alive, gated, spawned" means -
        // takes an already-resolved weather reading (floored for the forced pin, raw for the other two) and the
        // anvil wind the motion derives from (7b F2: a line member's is the LINE's one sample, a discrete draw's is
        // its own birth-phase read). Callers check alive(c, now) themselves BEFORE calling this.
        S32 n = 0;
        auto tryEmit = [&](const Candidate& c, const WeatherAtBirth& w, const Vec2& windAnvil, U64 lineId, bool isForced)
        {
            if (n >= cap)
            {
                return;
            }
            const Gate g = gate(c, w);
            diag(c, w, g);
            if (!g.mSpawn)
            {
                return;
            }
            ActiveCell& cell = out[n];
            cell = ActiveCell();
            cell.mCandidate = c;
            cell.mWeather = w;
            cell.mGate = g;
            cell.mOrigin = c.mOriginXY;
            // 8a ladder finding: a LINE member rides IN the line - its motion is the line's own (rotation 0, no supercell
            // deviation), so the embedded supercell keeps its KIND (the vortex gate reads mGate/mRotation) but not the
            // 20-30 degree walk-out that carried it 3.8 km off the wall by the cue. Discrete cells deviate as before.
            cell.mMotion = (lineId != 0) ? stormMotion(windAnvil, 0.f, false) : stormMotion(windAnvil, c.mRotation, g.mSupercell);
            cell.mLineId = lineId;
            cell.mIsForced = isForced;
            cell.mForcedKind = isForced ? fd.mKind : 0;
            ++n;
        };

        // 7b F8: the forced pin is emitted FIRST, so cap pressure (lines add up to LINE_MEMBERS_CAP members per epoch)
        // can never silently drop the one authored event.
        if (forcedAlive)
        {
            tryEmit(fd.mCandidate, applyWeatherFloor(weatherAtBirth(fd.mCandidate), fd.mWeatherFloor),
                    anvilWindAtBirth(fd.mCandidate), 0, true);
        }

        for (S64 epoch = epochFirst; epoch <= epochLast && n < cap; ++epoch)
        {
            const LineDesc line = lineAtEpoch(epoch);
            const S32 memberCount = line.mIsLine ? llmin(line.mMemberCount, LINE_MEMBERS_CAP) : 0;

            for (S32 mi = 0; mi < memberCount && n < cap; ++mi)
            {
                const Candidate c = line.mMembers[mi];
                if (!alive(c, now))
                {
                    continue;
                }
                // 8a F1: an authored line's members get its weather floor (like the authored pin); a hashed line's do not.
                const WeatherAtBirth wm = line.mHasFloor ? applyWeatherFloor(weatherAtBirth(c), line.mWeatherFloor) : weatherAtBirth(c);
                tryEmit(c, wm, line.mWindAnvil, line.mLineId, false);
            }

            for (S32 i = 0; i < latticeCount && n < cap; ++i)
            {
                const Candidate c = candidate(seed, lx[i], ly[i], epoch);
                if (line.mIsLine
                    && distanceToSegment(c.mOriginXY, line.mOrigin, line.mDirection, line.mHalfLengthM) <= line.mSuppressRadiusM)
                {
                    continue; // this epoch's line already emitted members in place of nearby discrete candidates
                }
                if (forcedAlive && c.mId == fd.mCandidate.mId)
                {
                    continue; // the live forced pin (emitted first, above) replaces this exact lattice draw
                }
                if (!alive(c, now))
                {
                    continue;
                }
                tryEmit(c, weatherAtBirth(c), anvilWindAtBirth(c), 0, false);
            }
        }

        std::sort(out, out + n, [](const ActiveCell& a, const ActiveCell& b)
                  { return a.mCandidate.mId < b.mCandidate.mId; });

        // 7b F1: EVERY forced pin (any kind) is placed by composeForced so its centre at the cue is anchor + offset;
        // when the kind is tornado (fd.mPreferHero) it is also the hero, ahead of the ordinary ascending-id search -
        // an authored cue is not lost to an earlier-id background candidate. Its lifetime (2 * FORCED_LEAD_S, see
        // sssquallcore.h's static_assert) clears HERO_MIN_LIFE_S, so no eligibility bypass is needed.
        S32 heroIndex = -1;
        Hero forcedHero;
        bool heroIsForced = false;
        // 7f F11: while any squall-line member is alive the LINE is the show - the spontaneous hero search below is
        // skipped (a composed hero would otherwise be dragged to the anchor beside the wall, into the very band the
        // line's suppression keeps clear). An authored (forced) hero still wins: the author asked for both.
        bool lineAlive = false;
        for (S32 i = 0; i < n; ++i) { if (out[i].mLineId != 0) { lineAlive = true; break; } }
        for (S32 i = 0; i < n; ++i)
        {
            if (!out[i].mIsForced)
            {
                continue;
            }
            const Hero h = composeForced(out[i].mCandidate, anchor, fd.mOffsetM, fd.mCueTime, out[i].mMotion);
            out[i].mOrigin = h.mOrigin;
            out[i].mMotion = h.mMotion;
            if (fd.mPreferHero && heroIndex < 0)
            {
                heroIndex = i;
                forcedHero = h;
                heroIsForced = true;
            }
        }
        if (heroIndex < 0 && !lineAlive)
        {
            for (S32 i = 0; i < n; ++i)
            {
                // 7c NEW-1: a forced pin of any non-tornado kind gates tornado-eligible by construction (rotation +-1,
                // shear 1, life 1600 s) and would otherwise win this search and have composeHero overwrite its
                // composeForced placement (measured: 5.5% of seeds put an authored supercell 9.8 km off its offset).
                // An authored non-tornado is never the hero.
                // 7f: a squall-line member is never the hero either - its embedded supercell is tornado-eligible by
                // construction, but composing it as the hero would tear it out of the line it exists to form (the line
                // is its own staging; scenario_squall_line pins the members' rigid spacing).
                if (!out[i].mGate.mTornadoEligible || out[i].mIsForced || out[i].mLineId != 0)
                {
                    continue;
                }
                heroIndex = i;
                break;
            }
        }
        if (heroIndex >= 0)
        {
            // 7f calibration: the staged hero is a strong storm by construction - its intensity is floored to
            // HERO_INTENSITY_MIN FIRST, before the contact hook, the composition, the lifecycle/radius pass below and
            // the vortex Parent read it, so every consumer sees ONE intensity (lesson 9). mGate.mSpawn/mSupercell/
            // mTornadoEligible are untouched - the floor raises the show, never the decision.
            if (!heroIsForced)
            {
                out[heroIndex].mGate.mIntensity = llmax(out[heroIndex].mGate.mIntensity, HERO_INTENSITY_MIN);
            }
            // 7f: the hook receives the ACTUAL closest age (after the spawn clamp), the same figure the shell's vortex
            // Parent derives from mClosestTime afterwards - never the nominal HERO_CLOSEST_AGE01.
            const Vec2& heroMotion = out[heroIndex].mMotion;
            const F32 heroSpeed = std::sqrt(heroMotion.x * heroMotion.x + heroMotion.y * heroMotion.y);
            const F32 closestAge = heroClosestAge01(out[heroIndex].mCandidate, heroSpeed);
            const Hero h = heroIsForced ? forcedHero
                                        : composeHero(out[heroIndex].mCandidate, anchor, heroMotion,
                                                      heroContact(out[heroIndex], closestAge)); // 7f: the FUNNEL passes close
            out[heroIndex].mOrigin = h.mOrigin;
            out[heroIndex].mMotion = h.mMotion;
            out[heroIndex].mIsHero = true;
            if (heroOut) *heroOut = h;
        }

        // 7b (review #1): the hero and every forced pin are exempt from the anchor cull - the hero is exempt BECAUSE
        // it may be composed up to HERO_SPAWN_MAX_M (30 km) outside the field and must survive the cull to arrive
        // (the exemption is load-bearing: without it, culling on distance would throw the hero away before it ever
        // gets close enough to pass the anchor), a forced pin because it is authored (its offset may sit anywhere the
        // author put it, and its drift before/after the cue is the author's business, not the cull's).
        const F32 cullM = radiusM + RADIUS_MAX_M;
        S32 kept = 0;
        for (S32 i = 0; i < n; ++i)
        {
            ActiveCell cell = out[i];
            cell.mAge01 = age01(cell.mCandidate, now);
            cell.mCentre = centreAt(cell.mOrigin, cell.mMotion, cell.mCandidate.mBirthTime, now);
            cell.mLifecycle = lifecycle(cell.mAge01, cell.mGate.mIntensity, cell.mCandidate.mRotation, cell.mGate.mSupercell);
            cell.mRadiusM = radiusAt(cell.mAge01, cell.mGate.mIntensity);

            const F32 dx = cell.mCentre.x - anchor.x;
            const F32 dy = cell.mCentre.y - anchor.y;
            if (!cell.mIsHero && !cell.mIsForced && dx * dx + dy * dy > cullM * cullM)
            {
                continue;
            }
            out[kept] = cell;
            if (i == heroIndex)
            {
                heroIndex = kept;
            }
            ++kept;
        }

        if (heroIndexOut) *heroIndexOut = heroIndex;
        return kept;
    }
}

#endif
