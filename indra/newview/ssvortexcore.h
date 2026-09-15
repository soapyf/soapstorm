/**
 * @file ssvortexcore.h
 * @brief Atmo Magic: vortices - tornado taxonomy, child scheduling on storm cells, funnel geometry, dust devils. Header-only core. CONTRACT.
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

#ifndef SS_VORTEXCORE_H
#define SS_VORTEXCORE_H

// <SS:Nexii> A CORE header (lldefs.h, ssatmonoisecore.h, ssstormcellcore.h, <cmath> only). Design: doc/atmo_magic_storm_dynamics.md section 4 (one renderer, a taxonomy of gates). A vortex is a CHILD of a storm cell (or, for dust devils, of its own small lattice): everything about it is a pure function of (seed, parent id, slot, wall clock) plus the parent's live state the shell already resolves - no persisted state, no dt. Multi-vortex structure is a SHADER-SIDE angular term (radiusModulation), never extra entities. The renderer draws a stack of Z-billboarded collar quads from the wall-cloud altitude to contact, radius per collar from collarRadius; it subdivides before squashScale, draws in the sky forward pass with REAL alpha (never post-deferred additive - a dark funnel that writes additive alpha blooms), never enters mPuffs or the puff budget. The water test for waterspouts and the ground altitude for contact are the shell's (region water height / the wind profile's ground reference). Bodies marked STUB are the implementer's; the invariants are the test author's spec.
#include "lldefs.h"
#include "ssatmonoisecore.h"
#include "ssstormcellcore.h"

#include <cmath>
#include <cstdint>

namespace SSVortex
{
    constexpr S32 MAX_ACTIVE      = 3;      // vortices drawn at once (hero-first, then by parent id)
    constexpr S32 COLLARS         = 14;     // collar quads per funnel
    constexpr S32 SLOTS_PER_CELL  = 2;      // child vortex slots hashed per storm cell (slot 1 is the satellite slot)
    constexpr F32 WALL_CLOUD_FRAC = 0.85f;  // funnel top as a fraction of the deck base altitude above ground (the lowering)
    constexpr F32 MESO_TORNADO_MIN = 0.35f; // parent lifecycle meso needed for a mesocyclonic vortex to exist (7f calibration: was 0.55, which k * |rotation| could only reach at intensity ~1 and rotation ~0.6+; a staged hero at HERO_INTENSITY_MIN 0.7 with the supercell floor rotationTerm >= 0.501 peaks near 0.39)
    constexpr F32 LANDSPOUT_POT_MIN = 0.80f; // parent potential needed for a landspout in the TCU stage
    constexpr F32 SATELLITE_ODDS  = 0.25f;  // share of strong mesocyclonic vortices that also carry a satellite in slot 1
    constexpr F32 GUSTNADO_ODDS   = 0.35f;  // share of mature cells that spin up a gustnado on the outflow ring
    constexpr F32 GUSTNADO_RING_FRAC = 1.3f;// gustnado ring radius as a share of the parent's influence radius
    constexpr F32 TAPER_WEDGE     = 1.6f;   // collarRadius taper exponent for a wedge (wider than tall)
    constexpr F32 TAPER_DRILL     = 0.25f;  // ... for a drill-bit (thin, violent)
    constexpr F32 WIDTH_MAX_M     = 900.f;  // wedge base radius at full intensity
    constexpr F32 WIDTH_MIN_M     = 18.f;   // drill-bit tip radius
    constexpr F32 ROPE_TILT_MAX   = 0.35f;  // rope-out lateral lean as a share of funnel height
    constexpr S32 MULTI_N_MIN     = 2;      // suction vortices
    constexpr S32 MULTI_N_MAX     = 6;
    constexpr F32 MULTI_AMP_MAX   = 0.45f;  // angular radius modulation amplitude at full multi-vortex weight
    // <SS:Nexii> Review NEW-2: the fragment stage's own radius_mask (ssVortexF.glsl) is a smoothstep band this many
    // collar-radius units wide either side of radiusModulation's own boundary - LOCKSTEP with that shader's local
    // RADIUS_EDGE constant, which carries this exact value in a comment rather than reading it (a core header is not
    // reachable from GLSL). FIX 2: cardHalfWidthM below needs TWO of these units of headroom beyond radiusModulation's
    // own upper bound (1 + MULTI_AMP_MAX), not one - the first widens the card out to where the smoothstep band's
    // OWN outer edge sits at radiusModulation's worst case, the second pushes the card edge strictly past that
    // outer edge so the mask has actually reached zero (fragment fully discarded) before the card geometry itself
    // ends, rather than exactly AT it - see cardHalfWidthM's own comment for the hard-edge bug one unit of headroom
    // left in place.
    constexpr F32 MULTI_RADIUS_EDGE_UNITS = 0.05f;
    constexpr F32 DUST_LATTICE_M  = 2000.f; // dust devils: their own small lattice
    constexpr F64 DUST_EPOCH_S    = 300.0;
    constexpr F32 DUST_TEMP_MIN_C = 24.f;   // hot
    constexpr F32 DUST_WIND_MAX_MS = 3.f;   // calm
    constexpr F32 DUST_COVER_MAX  = 0.15f;  // clear
    constexpr F32 DUST_SUN_MIN    = 0.5f;   // sun elevation sine: high sun
    constexpr F32 DUST_WARMTH_SPAN_C = 15.f; // dustLiveIntensity's headroom above DUST_TEMP_MIN_C to reach full scale (judgement call, no design number)

    // <SS:Nexii> Implementation-only constants (not part of the contract's named-constant set above, but every one of
    // them is a fixed value a hashed draw is lerped across, so they live here rather than as magic numbers in the
    // function bodies). TWO_PI is the angle unit for orbit/phase math; the rest are documented at their one call site
    // below via the salt table comment.
    constexpr F32 TWO_PI = 6.283185307179586f;
    constexpr F32 SATELLITE_OFFSET_FRAC = 0.6f;   // the "orbit at ~0.6" the KIND_SATELLITE comment names
    constexpr F32 MESO_OFFSET_MIN = 0.25f;        // 7f F1: a mesocyclonic funnel forms at the rear-flank interface, this share of the parent radius from the centre at least...
    constexpr F32 MESO_OFFSET_MAX = 0.55f;        // ...and at most (design 3c item 3 says "up to a cell radius away"; implemented as this band of the radius and - 7f re-check N3 - for EVERY mesocyclonic/anticyclonic funnel through vortexAt, not only the hero: a field-wide change no non-hero rung pins yet); hashed per candidate (SALT_OFFSET_FRAC). Was 0: the funnel sat under the centre and the hero-contact hook composed a zero offset
    constexpr F32 HERO_FUNNEL_BAND_LO = 0.20f;    // 7f F6: the hero's pinned window may open this early (the meso band's own 0.30 left a fast, long-lived hero arriving at age 0.32-0.41 with its funnel still aloft at the pass)
    constexpr F32 SATELLITE_INTENSITY_MIN = 0.7f; // the "strong mesocyclonic" the SATELLITE_ODDS comment names
    constexpr F32 TAPER_INTENSITY_BIAS = 0.5f;    // how far the taper hash is pulled toward wedge (1) at intensity 1
    constexpr F32 DURATION_MIN_FRAC = 0.05f;      // shortest hashed duration, as a fraction of parent age01
    constexpr F32 MULTI_OMEGA_MIN_RAD_S = 0.3f;   // suction-vortex orbit rate range (magnitude; sign follows rotation)
    constexpr F32 MULTI_OMEGA_MAX_RAD_S = 1.2f;
    constexpr F32 COLLAR_SHRINK_EDGE = 0.06f;     // smoothstep half-width of the condensation edge in collarRadius
    constexpr F32 BASE_ALPHA_SCALE = 0.85f;       // baseAlpha's own scale: a fully live funnel never exceeds 85% coverage
    constexpr F32 FORCED_BIRTH_AGE01    = 0.30f;  // 7d: an authored funnel's fixed window - touchdown spans age01 0.39..0.64, the cue (0.5) inside it
    constexpr F32 FORCED_DURATION_AGE01 = 0.45f;
    constexpr F32 HERO_FUNNEL_LEAD_AGE01     = 0.12f; // 7f: a spontaneous hero's funnel window starts this much before its closest approach...
    constexpr F32 HERO_FUNNEL_DURATION_AGE01 = 0.35f; // ... and lasts this long: touchdown (t01 0.2..0.75) spans closest-0.05 .. closest+0.14, so the pass is on the ground

    // Per-field salts mixed into the chain hash with SSAtmoNoise::combine before hash01, exactly as ssstormcellcore.h
    // does it: every hashed field gets its own salt so no two fields ever read the same random draw. childVortex's
    // chain is combine(combine(combine(seed, idLo), idHi), slot); dustDevil's is the lattice/epoch chain salted with
    // SALT_DUST_LATTICE first so it never collides with a vortex chain over the same raw numbers.
    constexpr U32 SALT_ID_HI          = 0x56580001u; // high 32 bits of mId
    constexpr U32 SALT_GUSTNADO_ODDS  = 0x56580002u; // gustnado spin-up draw, slot 0
    constexpr U32 SALT_SATELLITE_ODDS = 0x56580003u; // satellite draw, slot 1
    constexpr U32 SALT_BIRTH          = 0x56580004u; // mBirthAge01 within the kind's stage band
    constexpr U32 SALT_DURATION       = 0x56580005u; // mDurationAge01 within the remaining band
    constexpr U32 SALT_TAPER          = 0x56580006u; // mTaper, before the intensity bias
    constexpr U32 SALT_OFFSET_ANGLE   = 0x56580007u; // mOffsetAngle, rad
    constexpr U32 SALT_MULTI_PRESENT  = 0x56580008u; // whether the multi-vortex term is active at all
    constexpr U32 SALT_MULTI_N        = 0x56580009u; // mMultiN in [MULTI_N_MIN, MULTI_N_MAX]
    constexpr U32 SALT_MULTI_OMEGA    = 0x5658000Au; // mMultiOmega magnitude
    constexpr U32 SALT_MULTI_PHASE    = 0x5658000Bu; // mMultiPhase, rad
    constexpr U32 SALT_DUST_LATTICE   = 0x5658000Cu; // dust chain domain separator, mixed in first
    constexpr U32 SALT_DUST_ID_HI     = 0x5658000Du; // high 32 bits of a DustCandidate's mId
    constexpr U32 SALT_DUST_BIRTH     = 0x5658000Eu; // mBirthTime offset within the dust epoch
    constexpr U32 SALT_DUST_DURATION  = 0x5658000Fu; // mDurationS in [30, 300]
    constexpr U32 SALT_DUST_JITTER_X  = 0x56580010u; // mOriginXY.x jitter within the dust lattice cell
    constexpr U32 SALT_DUST_JITTER_Y  = 0x56580011u; // mOriginXY.y jitter
    constexpr U32 SALT_OFFSET_FRAC    = 0x56580013u; // 7f F1: a mesocyclonic/anticyclonic funnel's contact offset share, [MESO_OFFSET_MIN, MESO_OFFSET_MAX]
    constexpr U32 SALT_DUST_INTENSITY = 0x56580012u; // hashed baseline mIntensity (the shell scales it further by live temperature at spawn - dustDevil() takes no weather)

    enum EKind : S32
    {
        KIND_NONE = 0,
        KIND_MESOCYCLONIC,   // supercell, mature stage, meso above MESO_TORNADO_MIN
        KIND_LANDSPOUT,      // TCU stage under a fast-growing high-potential cell, no meso
        KIND_WATERSPOUT,     // a landspout/weak-meso whose contact is over water (the SHELL decides water; the core offers the rule)
        KIND_SATELLITE,      // slot 1 of a strong mesocyclonic parent, orbiting the primary
        KIND_ANTICYCLONIC,   // a mesocyclonic vortex on a parent with negative rotation
        KIND_QLCS,           // squall-line member spin-up (phase 7; the enum reserves it)
        KIND_GUSTNADO,       // outflow-edge spin-up, funnel-less (dust only)
        KIND_DUST_DEVIL      // parentless, its own lattice, clear hot calm days
    };

    enum EPhase : S32
    {
        PHASE_ALOFT = 0,     // funnel forming under the wall cloud, no contact
        PHASE_TOUCHDOWN,     // condensation reaches the ground
        PHASE_ROPE           // narrowing, leaning, dying
    };

    struct Vec2
    {
        F32 x = 0.f;
        F32 y = 0.f;
    };

    // The pure child candidate for one parent cell and slot.
    struct Candidate
    {
        U64 mId = 0;              // hash(parentId, slot, seed)
        S32 mSlot = 0;
        EKind mKind = KIND_NONE;  // decided from the PARENT's gate/lifecycle window and hashes (see childVortex)
        F32 mBirthAge01 = 0.f;    // parent age01 at which the vortex forms
        F32 mDurationAge01 = 0.f; // parent-age01 span it lives (converted to seconds with the parent's lifetime by the shell/vortexAt)
        F32 mTaper = 1.f;         // TAPER_DRILL .. TAPER_WEDGE (hashed, biased by parent intensity: strong -> wedge)
        F32 mIntensity = 0.f;     // [0,1] from parent intensity * meso (or potential for landspouts)
        S32 mMultiN = 0;          // 0 = single vortex; else MULTI_N_MIN..MULTI_N_MAX suction vortices
        F32 mMultiOmega = 0.f;    // rad/s orbit rate of the suction vortices (sign follows rotation)
        F32 mMultiPhase = 0.f;    // rad
        F32 mOffsetFrac = 0.f;    // contact offset from the parent centre as a share of the parent radius (satellites orbit at ~0.6)
        F32 mOffsetAngle = 0.f;   // rad, hashed
        F32 mRotSign = 1.f;       // +1 / -1 (KIND_ANTICYCLONIC is -1)
    };

    // The live state at now.
    struct State
    {
        bool mAlive = false;
        EPhase mPhase = PHASE_ALOFT;
        F32 mT01 = 0.f;           // [0,1] through the vortex's own life
        F32 mCondensation = 0.f;  // [0,1] share of the funnel height that is condensed (0 aloft-forming, 1 full touchdown, falling in rope)
        F32 mIntensity = 0.f;     // live intensity (rises, holds, fades)
        F32 mTiltFrac = 0.f;      // rope-out lean, share of height, along mTiltDir
        Vec2 mTiltDir;
        Vec2 mContact;            // world XY of the ground-contact point (parent centre + orbiting offset)
        F32 mMultiWeight = 0.f;   // [0,1] how visible the multi-vortex modulation is (peaks at touchdown)
    };

    // The parent's live figures the shell already has (from SSStormCell::ActiveCell).
    struct Parent
    {
        U64 mId = 0;
        F64 mBirthTime = 0.0;
        F32 mLifetimeS = 0.f;
        F32 mIntensity = 0.f;
        F32 mRotation = 0.f;      // signed
        F32 mPotential = 0.f;
        bool mSupercell = false;
        bool mTornadoEligible = false;
        Vec2 mCentre;             // world XY at now
        F32 mRadiusM = 0.f;
        SSStormCell::Lifecycle mLife; // at now
        bool mAllowTornadoes = false;
        S32 mForcedKind = 0;      // 7d: ActiveCell::mForcedKind - 2 tornado / 3 waterspout / 4 anticyclonic GUARANTEE slot 0's funnel (see childVortex); 0/1 change nothing
        bool mIsHero = false;     // 7f: the resolved hero - when it clears the ordinary meso gates, slot 0's window is PINNED around mClosestAge01 (see childVortex), never hashed
        F32 mClosestAge01 = 0.5f; // 7f: (hero.mClosestTime - birth) / lifetime as the shell computes it from SSStormCells::hero()
    };

    // 7f: whether kind draws a condensation funnel at all, vs a funnel-less spin-up ring (KIND_GUSTNADO) or nothing
    // (KIND_NONE/KIND_DUST_DEVIL/KIND_QLCS/KIND_WATERSPOUT - the last is a slot-0 funnel kind RE-LABELLED by the
    // shell, so it never reaches childVortex's own kind decision as KIND_WATERSPOUT). Exported (not just a local in
    // childVortex) so the shell's 7f hero-contact hook can gate contactOffsetM the same way childVortex's own
    // multi-vortex term does - a GUSTNADO's mOffsetFrac is its RING radius share, not a funnel contact, so this must
    // be an explicit kind check, never "mOffsetFrac != 0". Invariants: true for MESOCYCLONIC/ANTICYCLONIC/LANDSPOUT/
    // SATELLITE, false for every other kind; pure.
    inline bool hasFunnel(EKind kind)
    {
        return kind == KIND_MESOCYCLONIC || kind == KIND_ANTICYCLONIC || kind == KIND_LANDSPOUT || kind == KIND_SATELLITE;
    }

    // 7f: the funnel's contact offset from the parent centre - the ONE formula site for "how far and which way the
    // funnel sits from the cell centre" (design 3c item 3: "up to a cell radius away"). vortexAt (below) calls this
    // with orbit already folded into a copy of mOffsetAngle for satellites; the shell's hero-contact hook calls it
    // directly on slot 0's own candidate (no orbit - slot 0 never orbits) built at the closest-approach age, so the
    // hero's straight-line path is composed to the funnel's CONTACT, not the cell centre (ssstormcellcore.h's
    // HeroContactFn). Invariants: magnitude == mOffsetFrac * parentRadiusM (for a mesocyclonic/anticyclonic funnel that
    // is [MESO_OFFSET_MIN, MESO_OFFSET_MAX] of the radius - 7f F1; the old 0 made the whole hero-contact path a
    // constant zero), zero only for kinds with mOffsetFrac 0 (landspout, waterspout); angle == mOffsetAngle; pure.
    inline Vec2 contactOffsetM(const Candidate& c, F32 parentRadiusM)
    {
        Vec2 v;
        v.x = c.mOffsetFrac * parentRadiusM * std::cos(c.mOffsetAngle);
        v.y = c.mOffsetFrac * parentRadiusM * std::sin(c.mOffsetAngle);
        return v;
    }

    // The child candidate for (parent, slot). Kind decision: slot 0 - mesocyclonic if supercell && tornadoEligible && meso
    // peak >= MESO_TORNADO_MIN (anticyclonic when rotation < 0), else landspout if potential >= LANDSPOUT_POT_MIN &&
    // allowTornadoes (birth in the TCU window [0.02, 0.13]), else gustnado with GUSTNADO_ODDS (mature/decay window), else
    // NONE; slot 1 - satellite only when slot 0 is mesocyclonic with intensity >= 0.7 and the SATELLITE_ODDS hash passes.
    // Waterspout is NOT decided here (the shell tests water at the contact point and re-labels a landspout/weak meso).
    // 7d FORCED: when parent.mForcedKind is 2, 3 or 4 (an authored tornado / waterspout / anticyclonic pin) slot 0 is
    // ALWAYS mesocyclonic (anticyclonic for kind 4 or a negative rotation) regardless of the meso/supercell/eligibility
    // gates, with a FIXED window: birth age01 FORCED_BIRTH_AGE01 (0.30), duration FORCED_DURATION_AGE01 (0.45), so the
    // TOUCHDOWN plateau (t01 0.2..0.75) spans age01 0.39..0.64 and the cue (age01 0.5, sssquallcore.h) is inside it on
    // every client. Before this the funnel was re-hashed from the cell id, so every offset or cue tweak landed on a
    // different (usually empty) window - the authored funnel vanished as the author moved it. Invariants: for forced
    // kinds 2/3/4 slot 0's kind is MESOCYCLONIC or ANTICYCLONIC, birth/duration are the two constants bit-exactly, and
    // vortexAt is alive in PHASE_TOUCHDOWN at age01 0.5; mForcedKind 0 or 1 leaves every rule below unchanged.
    // 7f HERO: when parent.mIsHero and the ORDINARY meso gates pass (supercell, tornado-eligible, meso >= MESO_TORNADO_MIN,
    // allowTornadoes - the gates still decide WHETHER), slot 0's window is pinned, not hashed: birth = clamp(mClosestAge01
    // - HERO_FUNNEL_LEAD_AGE01, HERO_FUNNEL_BAND_LO, 0.80 - HERO_FUNNEL_DURATION_AGE01), duration HERO_FUNNEL_DURATION_AGE01, so the
    // funnel is in TOUCHDOWN as the hero passes the anchor (the user's staging principle: a show that happens offscreen
    // never happened). The window may open as early as HERO_FUNNEL_BAND_LO (0.20), below the meso band's 0.30, so a
    // fast, long-lived hero whose spawn clamp lands it at age 0.32-0.41 still has the funnel down at the pass.
    // Invariants: for a hero passing the gates with mClosestAge01 in [0.32, 0.63], vortexAt at age01 == mClosestAge01 is
    // alive in PHASE_TOUCHDOWN; duration is the constant bit-exactly and birth is the clamped lead; a non-hero or a hero
    // failing a gate is unchanged. STATED LATENT (7f re-check N2): touchdown spans birth+0.07..birth+0.26 of life, so with
    // birth floored at HERO_FUNNEL_BAND_LO the pass is on the ground only while mClosestAge01 >= 0.27; the 30 km spawn cap
    // pushes the closest age below that when |motion| * lifetime > 111 km (anvil wind above ~31 m/s at LIFE_MAX_S), a
    // windy-day case no fixture drives (the ladder tops out at 25 m/s, closest age 0.383). The constant to move if it
    // shows is HERO_FUNNEL_LEAD_AGE01 (smaller) or HERO_FUNNEL_BAND_LO (lower) - never the spawn cap.
    // Invariants: bit-identical for equal inputs; mKind == NONE whenever !allowTornadoes for every tornado kind (gustnado
    // and dust devil are not tornadoes and are unaffected); birth/duration windows inside [0,1] and inside the kind's stage
    // band (mesocyclonic in [0.30, 0.80] EXCEPT a hero's pinned window, which may open at HERO_FUNNEL_BAND_LO 0.20 - see
    // the 7f HERO paragraph; landspout in [0.02, 0.13] with duration <= 0.12, gustnado in [0.35, 0.95]);
    // taper in [TAPER_DRILL, TAPER_WEDGE] and monotone in parent intensity on average over a sample; mMultiN either 0 or in
    // [MULTI_N_MIN, MULTI_N_MAX]; mRotSign == sign(parent rotation) for mesocyclonic kinds.
    inline Candidate childVortex(U32 seed, const Parent& parent, S32 slot)
    {
        using SSAtmoNoise::combine;
        using SSAtmoNoise::hash01;

        Candidate c;
        c.mSlot = slot;

        const U32 idLo = (U32)(parent.mId & 0xffffffffu);
        const U32 idHi = (U32)((parent.mId >> 32) & 0xffffffffu);
        U32 chain = combine(seed, idLo);
        chain = combine(chain, idHi);
        chain = combine(chain, (U32)slot);
        c.mId = ((U64)combine(chain, SALT_ID_HI) << 32) | (U64)chain;

        // "meso" is the parent's LIVE lifecycle meso (ramps/peaks/spins-down over the parent's life), so this gate -
        // and everything hashed below it - can turn on and off as the parent ages; that is the wall-clock dependence
        // the file-top comment promises, carried entirely through Parent.mLife rather than a "now" parameter here.
        const F32 meso = parent.mLife.mMeso;
        const bool forcedTornado = parent.mForcedKind == 2 || parent.mForcedKind == 3 || parent.mForcedKind == 4; // 7d
        const bool mesoOk = forcedTornado || (parent.mSupercell && parent.mTornadoEligible && meso >= MESO_TORNADO_MIN && parent.mAllowTornadoes); // <SS:Nexii> mesocyclonic/anticyclonic/satellite are tornado kinds; allowTornadoes must gate them same as landspoutOk (contract: "mKind == NONE whenever !allowTornadoes for every tornado kind")
        const bool landspoutOk = parent.mPotential >= LANDSPOUT_POT_MIN && parent.mAllowTornadoes;

        EKind kind = KIND_NONE;
        if (slot == 0)
        {
            if (mesoOk)
            {
                kind = (parent.mRotation < 0.f || parent.mForcedKind == 4) ? KIND_ANTICYCLONIC : KIND_MESOCYCLONIC;
            }
            else if (landspoutOk)
            {
                kind = KIND_LANDSPOUT;
            }
            else if (hash01(combine(chain, SALT_GUSTNADO_ODDS)) < GUSTNADO_ODDS)
            {
                kind = KIND_GUSTNADO;
            }
        }
        else if (slot == 1 && mesoOk && parent.mIntensity >= SATELLITE_INTENSITY_MIN
                 && hash01(combine(chain, SALT_SATELLITE_ODDS)) < SATELLITE_ODDS)
        {
            kind = KIND_SATELLITE;
        }
        c.mKind = kind;

        if (kind == KIND_NONE)
        {
            return c;
        }

        // Stage band for the birth/duration hash: exactly the bands the invariants name.
        F32 bandLo = 0.f, bandHi = 0.f, durCap = 1.f;
        switch (kind)
        {
        case KIND_MESOCYCLONIC:
        case KIND_ANTICYCLONIC:
        case KIND_SATELLITE:
            bandLo = 0.30f; bandHi = 0.80f; durCap = 1.f;
            break;
        case KIND_LANDSPOUT:
            bandLo = 0.02f; bandHi = 0.13f; durCap = 0.12f;
            break;
        case KIND_GUSTNADO:
            bandLo = 0.35f; bandHi = 0.95f; durCap = 1.f;
            break;
        default:
            break;
        }

        if (forcedTornado && slot == 0)
        {
            c.mBirthAge01 = FORCED_BIRTH_AGE01;       // 7d: the authored funnel's fixed window - no hash, so no tweak can lose it
            c.mDurationAge01 = FORCED_DURATION_AGE01;
        }
        else if (parent.mIsHero && slot == 0 && (kind == KIND_MESOCYCLONIC || kind == KIND_ANTICYCLONIC))
        {
            // 7f: the hero's funnel window is pinned around its closest approach (the gates above already said yes).
            c.mBirthAge01 = llclamp(parent.mClosestAge01 - HERO_FUNNEL_LEAD_AGE01, HERO_FUNNEL_BAND_LO, bandHi - HERO_FUNNEL_DURATION_AGE01); // 7f F6: may open before the meso band's 0.30
            c.mDurationAge01 = HERO_FUNNEL_DURATION_AGE01;
        }
        else
        {
            c.mBirthAge01 = std::lerp(bandLo, bandHi, hash01(combine(chain, SALT_BIRTH)));
            const F32 remain = llmax(bandHi - c.mBirthAge01, 0.f); // keeps birth + duration inside the stage band
            const F32 durMax = llmin(durCap, remain);
            const F32 durMin = llmin(DURATION_MIN_FRAC, durMax);
            c.mDurationAge01 = std::lerp(durMin, durMax, hash01(combine(chain, SALT_DURATION)));
        }

        // Taper hash pulled toward the wedge end as parent intensity rises, so it is monotone in intensity on average
        // over a sample while any single draw can still land anywhere in [TAPER_DRILL, TAPER_WEDGE].
        const F32 taperHash = hash01(combine(chain, SALT_TAPER));
        const F32 taperT = std::lerp(taperHash, 1.f, llclamp(parent.mIntensity, 0.f, 1.f) * TAPER_INTENSITY_BIAS);
        c.mTaper = std::lerp(TAPER_DRILL, TAPER_WEDGE, llclamp(taperT, 0.f, 1.f));

        const F32 mesoOrPotential = (kind == KIND_LANDSPOUT) ? parent.mPotential : meso;
        c.mIntensity = (kind == KIND_GUSTNADO)
                      ? llclamp(parent.mIntensity, 0.f, 1.f)
                      : llclamp(parent.mIntensity * mesoOrPotential, 0.f, 1.f);

        c.mRotSign = (kind == KIND_MESOCYCLONIC || kind == KIND_ANTICYCLONIC || kind == KIND_SATELLITE)
                   ? ((parent.mRotation < 0.f) ? -1.f : 1.f)
                   : 1.f;

        c.mOffsetAngle = hash01(combine(chain, SALT_OFFSET_ANGLE)) * TWO_PI;
        c.mOffsetFrac = (kind == KIND_SATELLITE) ? SATELLITE_OFFSET_FRAC
                      : (kind == KIND_GUSTNADO) ? GUSTNADO_RING_FRAC
                      : (kind == KIND_MESOCYCLONIC || kind == KIND_ANTICYCLONIC)
                          ? std::lerp(MESO_OFFSET_MIN, MESO_OFFSET_MAX, hash01(combine(chain, SALT_OFFSET_FRAC))) // 7f F1
                      : 0.f;

        // Multi-vortex angular term: only funnels (never the funnel-less gustnado) carry it, present with odds equal
        // to the candidate's own intensity (a stronger vortex is more likely to show suction vortices).
        if (hasFunnel(kind) && hash01(combine(chain, SALT_MULTI_PRESENT)) < c.mIntensity)
        {
            const F32 nT = hash01(combine(chain, SALT_MULTI_N));
            const S32 span = MULTI_N_MAX - MULTI_N_MIN + 1;
            c.mMultiN = llclamp(MULTI_N_MIN + (S32)(nT * (F32)span), MULTI_N_MIN, MULTI_N_MAX);
            const F32 omegaMag = std::lerp(MULTI_OMEGA_MIN_RAD_S, MULTI_OMEGA_MAX_RAD_S, hash01(combine(chain, SALT_MULTI_OMEGA)));
            c.mMultiOmega = omegaMag * c.mRotSign;
            c.mMultiPhase = hash01(combine(chain, SALT_MULTI_PHASE)) * TWO_PI;
        }

        return c;
    }

    // <SS:Nexii> The waterspout RULE the KIND_WATERSPOUT enum comment promises ("the SHELL decides water; the core
    // offers the rule"): true for a candidate the design's taxonomy row ("landspout/weak-meso rule",
    // doc/atmo_magic_storm_dynamics.md section 4) allows to become a waterspout - a landspout outright, or a
    // mesocyclonic/anticyclonic vortex too WEAK to read as a true tornadic funnel over water. "Weak" reuses
    // SATELLITE_INTENSITY_MIN rather than a new constant: that is already the file's own dividing line for "strong
    // mesocyclonic" (see its declaration comment), so a meso/anti candidate below it is, by the same yardstick, not
    // strong - a judgement call, not a design-stated number. The shell tests water at c's/its state's contact point
    // and relabels mKind to KIND_WATERSPOUT only when this returns true AND that test passes; it never relabels a
    // GUSTNADO (funnel-less) or a strong meso/anti (a real tornado over water stays a tornado). Invariants: false for
    // KIND_NONE/KIND_SATELLITE/KIND_GUSTNADO/KIND_DUST_DEVIL/KIND_QLCS/KIND_WATERSPOUT itself; true for every
    // KIND_LANDSPOUT; true for KIND_MESOCYCLONIC/KIND_ANTICYCLONIC iff mIntensity < SATELLITE_INTENSITY_MIN.
    inline bool waterspoutEligible(const Candidate& c)
    {
        if (c.mKind == KIND_LANDSPOUT)
        {
            return true;
        }
        if (c.mKind == KIND_MESOCYCLONIC || c.mKind == KIND_ANTICYCLONIC)
        {
            return c.mIntensity < SATELLITE_INTENSITY_MIN;
        }
        return false;
    }

    // Live state at wall clock now. Life spans parent age01 [mBirthAge01, mBirthAge01 + mDurationAge01] -> t01. Phases:
    // ALOFT for t01 < 0.2 (condensation rising 0 -> 1 across it), TOUCHDOWN for [0.2, 0.75] (condensation 1, intensity
    // plateau), ROPE for > 0.75 (condensation falling to 0 at t01 1, tilt rising to ROPE_TILT_MAX * intensity, intensity
    // fading). Gustnadoes and dust devils have no ALOFT phase (contact from birth) and condensation stays 0 (dust only).
    // Contact = parent centre + offsetFrac * parentRadius * (cos, sin)(offsetAngle + orbit), orbit = satellites only: 2*pi *
    // t01 * 1.5 * rotSign. mMultiWeight = intensity * smoothstep(0.15, 0.35, t01) * (1 - smoothstep(0.7, 0.9, t01)).
    // Invariants: !mAlive outside the life span and every field zero then; all fractions in [0,1]; condensation continuous
    // in t01; contact within parent radius of the centre; bit-identical for equal inputs.
    inline State vortexAt(const Candidate& c, const Parent& parent, F64 now)
    {
        State s;
        if (c.mKind == KIND_NONE || c.mDurationAge01 <= 0.f || parent.mLifetimeS <= 0.f)
        {
            return s;
        }

        const F64 lifeStartS = parent.mBirthTime + (F64)c.mBirthAge01 * (F64)parent.mLifetimeS;
        const F64 lifeEndS = parent.mBirthTime + (F64)(c.mBirthAge01 + c.mDurationAge01) * (F64)parent.mLifetimeS;
        if (now < lifeStartS || now >= lifeEndS)
        {
            return s;
        }

        s.mAlive = true;
        const F32 t01 = llclamp((F32)((now - lifeStartS) / (lifeEndS - lifeStartS)), 0.f, 1.f);
        s.mT01 = t01;

        // Gustnadoes and dust devils skip ALOFT (contact from birth): condShape jumps straight to the TOUCHDOWN
        // plateau instead of ramping 0 -> 1 first. condShape doubles as both the condensation curve and the
        // rise/hold/fade envelope for live intensity - they share the exact same shape by design.
        const bool noAloft = (c.mKind == KIND_GUSTNADO || c.mKind == KIND_DUST_DEVIL);
        F32 condShape;
        if (!noAloft && t01 < 0.2f)
        {
            s.mPhase = PHASE_ALOFT;
            condShape = SSStormCell::smoothstep(0.f, 0.2f, t01);
        }
        else if (t01 < 0.75f)
        {
            s.mPhase = PHASE_TOUCHDOWN;
            condShape = 1.f;
        }
        else
        {
            s.mPhase = PHASE_ROPE;
            condShape = 1.f - SSStormCell::smoothstep(0.75f, 1.f, t01);
        }

        s.mIntensity = llclamp(c.mIntensity * condShape, 0.f, 1.f);
        s.mCondensation = noAloft ? 0.f : condShape; // <SS:Nexii> KIND_GUSTNADO is funnel-less (dust only, per its enum comment) same as KIND_DUST_DEVIL, so it shares noAloft's kind test rather than singling out dust devil

        const F32 ropeRamp = SSStormCell::smoothstep(0.75f, 1.f, t01);
        s.mTiltFrac = (s.mPhase == PHASE_ROPE) ? ROPE_TILT_MAX * s.mIntensity * ropeRamp : 0.f;
        s.mTiltDir.x = std::cos(c.mOffsetAngle);
        s.mTiltDir.y = std::sin(c.mOffsetAngle);

        // 7f: contactOffsetM is the one formula site (see its own comment) - orbit (satellites only) is folded into
        // a copy of c's own mOffsetAngle first since contactOffsetM takes the Candidate's angle, not a bare F32.
        const F32 orbit = (c.mKind == KIND_SATELLITE) ? (TWO_PI * t01 * 1.5f * c.mRotSign) : 0.f;
        Candidate cAtOrbit = c;
        cAtOrbit.mOffsetAngle = c.mOffsetAngle + orbit;
        const Vec2 offset = contactOffsetM(cAtOrbit, parent.mRadiusM);
        s.mContact.x = parent.mCentre.x + offset.x;
        s.mContact.y = parent.mCentre.y + offset.y;

        s.mMultiWeight = s.mIntensity * SSStormCell::smoothstep(0.15f, 0.35f, t01)
                        * (1.f - SSStormCell::smoothstep(0.7f, 0.9f, t01));

        return s;
    }

    // The condensation envelope collarRadius scales its taper term by: a smoothstep that hides collars below the
    // condensation front (the un-condensed shaft reads as 0), but - unlike a plain smoothstep - floored toward
    // `condensation` itself as h01 -> 0 rather than an unconditional 0, expressed as a lerp across the SAME [e0, e1]
    // smoothstep band rather than a one-off bump bolted on afterward (see collarRadius's own comment for why this
    // used to be a post-multiply special case for the h01 == 0 row only - review finding 6/5b). Because the floor is
    // now baked into the envelope itself as a continuous function of h01, there is no seam between the contact row
    // and its neighbour for collarRadius to inherit. Invariants: in [condensation, 1] for every h01 in [0,1]; ==
    // condensation at h01 == 0; == 1 at h01 == 1 and for every h01 at or above the smoothstep's own e1; == 0 at
    // h01 == 0 when condensation == 0 (preserves "condensation 0 -> 0 below the top collar"); monotone
    // non-decreasing in h01; bit-identical for equal inputs.
    inline F32 condensationShrink(F32 h01, F32 condensation)
    {
        const F32 h = llclamp(h01, 0.f, 1.f);
        const F32 cond = llclamp(condensation, 0.f, 1.f);
        const F32 threshold = llclamp(1.f - cond, 0.f, 1.f);
        const F32 e0 = llclamp(threshold - COLLAR_SHRINK_EDGE, 0.f, 1.f);
        const F32 e1 = llclamp(threshold + COLLAR_SHRINK_EDGE, 0.f, 1.f);
        return std::lerp(cond, 1.f, SSStormCell::smoothstep(e0, e1, h));
    }

    // Collar radius at height fraction h01 (0 contact, 1 wall cloud) for a funnel: base = lerp(WIDTH_MIN_M, WIDTH_MAX_M,
    // intensity) at the top, the raw taper term = base * pow(mix(0.08, 1, h01), 1/taper) - a wedge (taper > 1) stays
    // wide down to the ground, a drill (taper < 1) pinches toward 0 - floored at WIDTH_MIN_M at EVERY h01 (not just
    // the contact row), then scaled by condensationShrink (above) for the un-condensed lower shaft.
    // <SS:Nexii> Review 5b: the old code floored ONLY the h01 == 0 row (WIDTH_MIN_M * condensation, replacing
    // whatever the shrink*taper product computed there), so a drill taper's own unfloored curve could already be a
    // fraction of a metre by collar 1 (h01 ~ 1/(COLLARS-1)) while collar 0 jumped straight to the floor - a 48:1
    // step (0.37 m -> 18 m) between neighbouring collars, and collarRadius's own invariants had to carve out an
    // exception ("monotonicity is claimed only above [h01 == 0]") to admit it. Flooring the RAW TAPER TERM itself at
    // every h01, before condensationShrink is applied, removes the cliff: once the taper curve dips below
    // WIDTH_MIN_M it STAYS at WIDTH_MIN_M for every h01 below that point (collar 1 floors the same as collar 0
    // rather than punching through it just above the ground), and condensationShrink's own floor (now continuous in
    // h01, see its own comment) keeps the whole product free of the old seam. Invariants: radius(1, intensity,
    // taper, condensation) == max(base, WIDTH_MIN_M) (condensationShrink(1, *) == 1 always, and base is itself
    // always >= WIDTH_MIN_M since base = lerp(WIDTH_MIN_M, WIDTH_MAX_M, intensity), so this reduces to base - stated
    // as max(...) because that is the literal formula, not because base can be smaller); radius(0, intensity,
    // taper, condensation) == max(rawTaperTerm(0, intensity, taper), WIDTH_MIN_M) * condensation, which reduces to
    // WIDTH_MIN_M * condensation whenever the raw taper term at h01 == 0 is already <= WIDTH_MIN_M (true for every
    // drill-like taper the 48:1 example above exercises) - a wedge whose own raw term at h01 == 0 exceeds
    // WIDTH_MIN_M keeps a wider-than-drill-floor contact collar instead, which is correct (a wide-based funnel's
    // base should not be clamped down to the drill-bit floor); monotone non-decreasing in h01 for every h01 in
    // [0,1] (the floored taper term and condensationShrink are each non-negative and monotone non-decreasing in
    // h01, so their product is too - the old "above h01 == 0 only" carve-out no longer applies); >= 0; condensation
    // 0 -> 0 below the top collar (h01 > 0, inherited from condensationShrink's own invariant); wedge at h01 0.5
    // wider than drill at the same h01 for equal base (unchanged: the floor only raises values that were already
    // below WIDTH_MIN_M, it never lowers one that was above it).
    inline F32 collarRadius(F32 h01, F32 intensity, F32 taper, F32 condensation)
    {
        const F32 h = llclamp(h01, 0.f, 1.f);
        const F32 k = llclamp(intensity, 0.f, 1.f);
        const F32 t = llmax(taper, 1e-4f);
        const F32 base = std::lerp(WIDTH_MIN_M, WIDTH_MAX_M, k);
        const F32 mix = std::lerp(0.08f, 1.f, h);
        const F32 rawTaperTerm = base * std::pow(mix, 1.f / t); // 1/taper: wedge (taper > 1) stays close to base low down, drill (taper < 1) pinches fast
        const F32 flooredTaperTerm = llmax(rawTaperTerm, WIDTH_MIN_M); // never thinner than WIDTH_MIN_M at any height while condensed - see this function's own comment on the 48:1 step this removes

        return llmax(flooredTaperTerm * condensationShrink(h, condensation), 0.f);
    }

    // The funnel-card half-width the multi-vortex RADIUS mask needs (never a brightness/alpha multiply - see this
    // file's own top comment and doc/atmo_magic_storm_dynamics.md section 4): the true collar quad is widened by
    // (1 + MULTI_AMP_MAX) so the fragment stage has geometry to discard/fade against out to radiusModulation's own
    // upper bound, PLUS TWO further MULTI_RADIUS_EDGE_UNITS of headroom (FIX 2, review NEW-2 continued) - one unit
    // is the fragment stage's own smoothstep band half-width (radius_mod +/- MULTI_RADIUS_EDGE_UNITS, ssVortexF.glsl),
    // the second is the MARGIN that keeps the band's outer edge (where the mask actually reaches zero) strictly
    // BEFORE the card's own edge, never exactly at it. Review NEW-2's original fix used only one unit of headroom:
    // at collarRadiusM * (1 + MULTI_AMP_MAX + MULTI_RADIUS_EDGE_UNITS) exactly, a fragment sitting AT the card edge
    // with multiWeight 1 reads radial_units == 1 + MULTI_AMP_MAX + MULTI_RADIUS_EDGE_UNITS too (the mask's own
    // smoothstep upper bound, radius_mod + EDGE, at radius_mod's worst case 1 + MULTI_AMP_MAX) - the mask reaches
    // exactly zero right where the card discards it, not strictly before, so a fragment that grazes the card edge
    // at a shallow view angle can still sample a hair inside the band and show a hard edge instead of the soft fade
    // the mask is meant to be. The second EDGE unit here is that missing margin: == MULTI_RADIUS_EDGE_UNITS *
    // collarRadiusM strictly beyond where the mask reaches zero. Invariants: == collarRadiusM when MULTI_AMP_MAX ==
    // 0 and MULTI_RADIUS_EDGE_UNITS == 0; monotone non-decreasing in collarRadiusM; >= collarRadiusM for
    // collarRadiusM >= 0; == collarRadiusM * (1 + MULTI_AMP_MAX + 2 * MULTI_RADIUS_EDGE_UNITS) exactly.
    inline F32 cardHalfWidthM(F32 collarRadiusM)
    {
        return collarRadiusM * (1.f + MULTI_AMP_MAX + 2.f * MULTI_RADIUS_EDGE_UNITS);
    }

    // <SS:Nexii> The funnel-top altitude WALL_CLOUD_FRAC's own declaration comment names ("funnel top as a fraction
    // of the deck base altitude above ground, the lowering"), spelled out as a function since the shell needs it at
    // more than its declaration site (the collar altitude table below, and the vortex renderer). groundZ/deckBaseZ
    // are the shell's own reads (wind profile ground reference; SSVolCloud::cloudBaseZ) - never computed here.
    // Invariants: == groundZ when deckBaseZ <= groundZ (a collapsed or inverted deck never lowers the wall cloud
    // below ground); rises linearly with deckBaseZ above that; == groundZ + WALL_CLOUD_FRAC * (deckBaseZ - groundZ)
    // otherwise.
    inline F32 wallCloudAltitudeM(F32 groundZ, F32 deckBaseZ)
    {
        return groundZ + WALL_CLOUD_FRAC * llmax(deckBaseZ - groundZ, 0.f);
    }

    // Collar altitude at height fraction h01 (0 contact/ground, 1 wall cloud) - the companion lerp to collarRadius's
    // own h01 convention, so a caller building the COLLARS-row table walks both with the same index. Invariants:
    // collarAltitudeM(g, w, 0) == g; collarAltitudeM(g, w, 1) == w; linear in h01.
    inline F32 collarAltitudeM(F32 groundZ, F32 wallCloudZ, F32 h01)
    {
        return std::lerp(groundZ, wallCloudZ, llclamp(h01, 0.f, 1.f));
    }

    // The rope-out lateral placement offset for a collar vertex at height fraction h01, along State::mTiltDir: the
    // ground contact (h01 == 0) stays anchored, the wall cloud (h01 == 1) leans the full tiltFrac share of the
    // funnel's own true height - PHASE_ROPE's own comment above states this only as "a share of height", not a
    // placement formula; this is that formula, pulled out of ssvortexrender.cpp's render loop per the fork's
    // "numeric formula -> core" rule. Invariants: 0 when h01 == 0 or tiltFrac == 0; linear in each of tiltFrac,
    // h01 and heightM; magnitude <= ROPE_TILT_MAX * heightM for any tiltFrac a live State actually produces
    // (State::mTiltFrac is bounded by ROPE_TILT_MAX * intensity <= ROPE_TILT_MAX, and h01 <= 1).
    inline F32 tiltOffsetM(F32 tiltFrac, F32 h01, F32 heightM)
    {
        return tiltFrac * h01 * heightM;
    }

    // The funnel's overall drawn alpha from its live intensity - ssvortexrender.cpp's own base_alpha, pulled into
    // the core per the fork's "numeric formula -> core" rule. Invariants: 0 at intensity 0; == BASE_ALPHA_SCALE at
    // intensity 1; linear; in [0, BASE_ALPHA_SCALE] for intensity in [0,1].
    inline F32 baseAlpha(F32 intensity)
    {
        return BASE_ALPHA_SCALE * llclamp(intensity, 0.f, 1.f);
    }

    // The lit-rim facing term for one billboard corner, from how much that corner's own "right" axis points toward
    // the sun (rightDotSun, the dot product ssvortexrender.cpp already computes CPU-side per corner - see its own
    // comment on why: the same CPU-structure-term precedent SSVolCloud::Puff::mForm sets). Invariants: in [0,1] for
    // rightDotSun in [-1,1]; facingLight(1) == 1; facingLight(-1) == 0; facingLight(0) == 0.5; monotone
    // non-decreasing in rightDotSun.
    inline F32 facingLight(F32 rightDotSun)
    {
        return 0.5f + 0.5f * llclamp(rightDotSun, -1.f, 1.f);
    }

    // The multi-vortex angular modulation, a SHADER term (GLSL twin): radius(theta, t) = r * (1 + amp * sin(N * theta +
    // omega * t + phase)), amp = MULTI_AMP_MAX * multiWeight. Returns the multiplier. Invariants: 1 when N == 0 or
    // multiWeight == 0; in [1 - MULTI_AMP_MAX, 1 + MULTI_AMP_MAX]; periodic in theta with period 2*pi/N; continuous in t.
    inline F32 radiusModulation(F32 theta, F64 t, S32 n, F32 omega, F32 phase, F32 multiWeight)
    {
        if (n == 0 || multiWeight <= 0.f)
        {
            return 1.f;
        }
        const F32 amp = MULTI_AMP_MAX * llclamp(multiWeight, 0.f, 1.f);
        const F32 ang = (F32)n * theta + omega * (F32)t + phase;
        return 1.f + amp * std::sin(ang);
    }

    // Dust devils: a parentless candidate on the DUST lattice/epoch, gated by weather the shell samples (hot, calm, clear,
    // high sun). Invariants: bit-identical; birth inside its epoch; duration in [30, 300] s; gate false when any of the
    // four conditions fails; intensity in [0,1] rising with temperature above DUST_TEMP_MIN_C.
    struct DustWeather
    {
        F32 mTemperatureC = 0.f;
        F32 mWindSpeedMS = 0.f;
        F32 mCoverage = 0.f;
        F32 mSunElevationSin = 0.f;
    };
    struct DustCandidate
    {
        U64 mId = 0;
        F64 mBirthTime = 0.0;
        F32 mDurationS = 0.f;
        Vec2 mOriginXY;
        F32 mIntensity = 0.f;
    };
    inline DustCandidate dustDevil(U32 seed, S32 lx, S32 ly, S64 epoch)
    {
        using SSAtmoNoise::combine;
        using SSAtmoNoise::hash01;

        DustCandidate d;
        const U32 epochLo = (U32)((U64)epoch & 0xffffffffu);
        const U32 epochHi = (U32)(((U64)epoch >> 32) & 0xffffffffu);
        U32 chain = combine(seed, (U32)lx);
        chain = combine(chain, (U32)ly);
        chain = combine(chain, epochLo);
        chain = combine(chain, epochHi);
        chain = combine(chain, SALT_DUST_LATTICE); // its own small lattice: never the same chain as a storm-cell draw

        d.mId = ((U64)combine(chain, SALT_DUST_ID_HI) << 32) | (U64)chain;

        // hash01 < 1 strictly, so the birth stays inside [epoch * DUST_EPOCH_S, (epoch + 1) * DUST_EPOCH_S).
        d.mBirthTime = (F64)epoch * DUST_EPOCH_S + (F64)hash01(combine(chain, SALT_DUST_BIRTH)) * DUST_EPOCH_S;
        d.mDurationS = std::lerp(30.f, 300.f, hash01(combine(chain, SALT_DUST_DURATION)));

        const F32 jitterM = 0.5f * DUST_LATTICE_M;
        d.mOriginXY.x = ((F32)lx + 0.5f) * DUST_LATTICE_M + (hash01(combine(chain, SALT_DUST_JITTER_X)) * 2.f - 1.f) * jitterM;
        d.mOriginXY.y = ((F32)ly + 0.5f) * DUST_LATTICE_M + (hash01(combine(chain, SALT_DUST_JITTER_Y)) * 2.f - 1.f) * jitterM;

        // Hashed baseline strength; dustDevil() has no weather input, so the shell is the one that scales visible
        // intensity up with live temperature above DUST_TEMP_MIN_C once it has both this candidate and a DustWeather.
        d.mIntensity = hash01(combine(chain, SALT_DUST_INTENSITY));
        return d;
    }
    inline bool dustGate(const DustWeather& w)
    {
        return w.mTemperatureC >= DUST_TEMP_MIN_C
            && w.mWindSpeedMS <= DUST_WIND_MAX_MS
            && w.mCoverage <= DUST_COVER_MAX
            && w.mSunElevationSin >= DUST_SUN_MIN;
    }
    inline S64 dustEpochOf(F64 t)
    {
        return (S64)floor(t / DUST_EPOCH_S);
    }

    // Lattice cells whose centres lie within radiusM of centre (world), on the DUST lattice (pitch DUST_LATTICE_M,
    // not the storm lattice's LATTICE_M) - the same enumeration SSStormCell::enumerateLattice does for its own
    // pitch, respelled here rather than reused because the two lattices are deliberately incommensurate pitches (no
    // shared period, so dust devils never inherit the storm lattice's spacing). Same contract: written as (lx, ly)
    // pairs up to cap, rows then columns ascending, returns the count that WOULD be written.
    inline S32 enumerateDustLattice(const Vec2& centre, F32 radiusM, S32* outLX, S32* outLY, S32 cap)
    {
        if (radiusM < 0.f)
        {
            return 0;
        }
        const F64 cx = centre.x, cy = centre.y, r = radiusM, L = DUST_LATTICE_M;
        const S32 lxMin = (S32)ceil((cx - r) / L - 0.5), lxMax = (S32)floor((cx + r) / L - 0.5);
        const S32 lyMin = (S32)ceil((cy - r) / L - 0.5), lyMax = (S32)floor((cy + r) / L - 0.5);
        const F64 r2 = r * r;
        S32 count = 0;
        for (S32 ly = lyMin; ly <= lyMax; ++ly)
        {
            const F64 dy = ((F64)ly + 0.5) * L - cy;
            for (S32 lx = lxMin; lx <= lxMax; ++lx)
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

    // Whether a DustCandidate is alive at wall clock now - the same [birth, birth+duration) test the storm cell's
    // own age/alive checks make, respelled here since dustDevil() returns only the candidate, no state struct (a
    // dust devil has no phases/lifecycle curve to animate, just a presence window). Invariants: bit-identical;
    // false strictly outside [mBirthTime, mBirthTime + mDurationS).
    inline bool dustAlive(const DustCandidate& d, F64 now)
    {
        return now >= d.mBirthTime && now < d.mBirthTime + (F64)d.mDurationS;
    }

    // <SS:Nexii> Judgement call (the dustDevil() comment anticipates this but leaves the curve to the shell; pulled
    // in here instead so the formula does not live in ss*.cpp per the fork's "numeric formula -> core" rule): scales
    // the hashed baseline intensity up toward 1 as live temperature climbs above the DUST_TEMP_MIN_C gate floor,
    // reaching full scale DUST_WARMTH_SPAN_C above the floor. No design-stated span; 15C chosen as a wide-but-finite
    // heat range (a dust devil right at the gate floor stays at its hashed baseline, one deep into a heat wave reads
    // strong). Invariants: == baselineIntensity at temperatureC <= DUST_TEMP_MIN_C; == 1 at
    // temperatureC >= DUST_TEMP_MIN_C + DUST_WARMTH_SPAN_C; monotone non-decreasing in temperatureC; in [0,1].
    inline F32 dustLiveIntensity(F32 baselineIntensity, F32 temperatureC)
    {
        const F32 warmth = llclamp((temperatureC - DUST_TEMP_MIN_C) / DUST_WARMTH_SPAN_C, 0.f, 1.f);
        return llclamp(std::lerp(llclamp(baselineIntensity, 0.f, 1.f), 1.f, warmth), 0.f, 1.f);
    }

    // <SS:Nexii> FIX 4: Debris skirt spawn parameters - the five per-particle values SSVortexDebrisSource::update
    // (ssvortexrender.cpp) used to compute inline as MIN + SPAN * hash01(...)/mIntensity, pulled into the core per
    // the fork's "numeric formula -> core" rule. Two independent axes, each a genuine [0,1] input the render code
    // already has at the spawn site: h01 is THIS PARTICLE's own hashed jitter draw (a fresh SALT_* hash off the
    // spawn's existing chain, one per field that needs per-particle spread rather than a single shared value - it
    // drives radius share and lifetime, so two particles spawned the same tick still land at different ring radii
    // and burn out at different times); intensity is the vortex's live State::mIntensity, shared by every particle
    // spawned this update (it drives speed and alpha, so every particle from one spawn call shares one outward
    // speed and one base alpha) - count is a new intensity axis, not a behaviour-preservation claim: a weak vortex
    // now throws debris more sparsely than a strong one, where before the spawn RATE was a flat constant.
    constexpr F32 DEBRIS_RADIUS_SHARE_MIN = 0.80f;  // ring radius jitter multiplier, off h01
    constexpr F32 DEBRIS_RADIUS_SHARE_MAX = 1.15f;
    constexpr F32 DEBRIS_SPEED_MIN_MS     = 1.5f;   // outward speed, m/s, off intensity
    constexpr F32 DEBRIS_SPEED_MAX_MS     = 6.5f;
    constexpr F32 DEBRIS_LIFETIME_MIN_S   = 2.0f;   // particle lifetime, s, off h01
    constexpr F32 DEBRIS_LIFETIME_MAX_S   = 4.5f;
    constexpr F32 DEBRIS_COUNT_MIN        = 0.5f;   // spawn-rate multiplier, off intensity
    constexpr F32 DEBRIS_COUNT_MAX        = 2.0f;
    constexpr F32 DEBRIS_ALPHA_MIN        = 0.05f;  // dust colour alpha, off intensity
    constexpr F32 DEBRIS_ALPHA_MAX        = 0.30f;
    // <SS:Nexii> Review 5b: SSVortexDebrisSource::update's own part->mScale.mV[0] = 0.35f + 0.55f * mIntensity
    // (ssvortexrender.cpp) was the one debris numeric formula debrisParams had not yet absorbed - a sixth
    // intensity-only axis, same shape as mSpeedMS/mCount/mAlpha, folded in here as mScale rather than a new
    // function so the render loop keeps reading every per-particle field off one struct.
    constexpr F32 DEBRIS_SCALE_MIN        = 0.35f;  // particle billboard scale (both axes), off intensity
    constexpr F32 DEBRIS_SCALE_MAX        = 0.90f;

    struct DebrisParams
    {
        F32 mRadiusShare = 0.f;
        F32 mSpeedMS = 0.f;
        F32 mLifetimeS = 0.f;
        F32 mCount = 0.f;
        F32 mAlpha = 0.f;
        F32 mScale = 0.f;
    };

    // Invariants: every field is its own MIN..MAX lerp (see the constants above) and so lies within [MIN, MAX];
    // mRadiusShare/mLifetimeS depend only on h01 (== DEBRIS_*_MIN at h01 0, == DEBRIS_*_MAX at h01 1, independent of
    // intensity); mSpeedMS/mCount/mAlpha/mScale depend only on intensity (== DEBRIS_*_MIN at intensity 0, ==
    // DEBRIS_*_MAX at intensity 1, independent of h01); bit-identical for equal inputs.
    inline DebrisParams debrisParams(F32 h01, F32 intensity)
    {
        const F32 h = llclamp(h01, 0.f, 1.f);
        const F32 k = llclamp(intensity, 0.f, 1.f);
        DebrisParams p;
        p.mRadiusShare = std::lerp(DEBRIS_RADIUS_SHARE_MIN, DEBRIS_RADIUS_SHARE_MAX, h);
        p.mSpeedMS = std::lerp(DEBRIS_SPEED_MIN_MS, DEBRIS_SPEED_MAX_MS, k);
        p.mLifetimeS = std::lerp(DEBRIS_LIFETIME_MIN_S, DEBRIS_LIFETIME_MAX_S, h);
        p.mCount = std::lerp(DEBRIS_COUNT_MIN, DEBRIS_COUNT_MAX, k);
        p.mAlpha = std::lerp(DEBRIS_ALPHA_MIN, DEBRIS_ALPHA_MAX, k);
        p.mScale = std::lerp(DEBRIS_SCALE_MIN, DEBRIS_SCALE_MAX, k);
        return p;
    }

    // <SS:Nexii> Review 5b: SSVortexDebrisSource::update's own spawn-rate formula (ssvortexrender.cpp) - a fixed
    // period RATE_BASE divided by the live mCount axis debrisParams already exposes, floored so a near-zero count
    // never divides toward an unbounded period. RATE_BASE itself is the same 0.06s the shell used before this
    // moved; DEBRIS_COUNT_FLOOR is the shell's own inline 0.05f floor, named here.
    constexpr F32 RATE_BASE = 0.06f;           // base spawn period, s, before the mCount scale
    constexpr F32 DEBRIS_COUNT_FLOOR = 0.05f;  // floor on mCount before it divides RATE_BASE

    // Invariants: > 0 for intensity in [0,1]; == RATE_BASE / DEBRIS_COUNT_MAX at intensity 1 (shortest period,
    // busiest skirt); == RATE_BASE / DEBRIS_COUNT_MIN at intensity 0 (longest period); monotone non-increasing in
    // intensity; bit-identical for equal inputs.
    inline F32 debrisSpawnPeriodS(F32 intensity)
    {
        const F32 count = debrisParams(0.f, intensity).mCount; // h01 irrelevant: mCount is intensity-only
        return RATE_BASE / llmax(count, DEBRIS_COUNT_FLOOR);
    }

    // <SS:Nexii> Review 5b: SSVortexDebrisSource::update's own per-particle lift formula (ssvortexrender.cpp),
    // pulled into the core per the fork's "numeric formula -> core" rule - a per-particle draw off the spawn's own
    // hashed jitter (u), same idiom debrisParams's h01-keyed fields use, just not folded into that struct since
    // lift is a velocity-axis term the render loop applies to Z rather than a spawn-shape field.
    constexpr F32 DEBRIS_LIFT_MIN_MS = 0.6f;  // initial upward velocity, m/s
    constexpr F32 DEBRIS_LIFT_MAX_MS = 2.2f;

    // Invariants: == DEBRIS_LIFT_MIN_MS at u == 0; == DEBRIS_LIFT_MAX_MS at u == 1; linear; in
    // [DEBRIS_LIFT_MIN_MS, DEBRIS_LIFT_MAX_MS] for u in [0,1]; bit-identical for equal inputs.
    inline F32 debrisLift(F32 u)
    {
        return std::lerp(DEBRIS_LIFT_MIN_MS, DEBRIS_LIFT_MAX_MS, llclamp(u, 0.f, 1.f));
    }

    // <SS:Nexii> Review 5b: the debris skirt's ring-radius floors (ssvortexrender.cpp) - a touched-down funnel
    // throws its skirt at its own ground-collar radius, floored so even a drill-bit tip still throws a visible
    // ring; a funnel-less kind (gustnado, or - review NEW-6 - a dust devil) has no collar table, so it uses the same
    // flat, generous ring instead. parentRadius is accepted (not currently read) so the signature already carries
    // every physical input a future gustnado-scaled ring would need without another call-site change - the design
    // names no parentRadius-scaled ring today (a judgement call, no design number), so the !hasFunnel branch stays
    // the flat RING_RADIUS_GUSTNADO_M constant regardless of which funnel-less kind is asking.
    constexpr F32 RING_RADIUS_FUNNEL_FLOOR_M = 6.f;   // floor on a funnel's own ground-collar radius
    constexpr F32 RING_RADIUS_GUSTNADO_M = 25.f;      // flat ring for a funnel-less kind (gustnado or dust devil)

    // Invariants: == max(collarRadius0, RING_RADIUS_FUNNEL_FLOOR_M) when hasFunnel; == RING_RADIUS_GUSTNADO_M when
    // !hasFunnel (independent of collarRadius0 and parentRadius); >= RING_RADIUS_FUNNEL_FLOOR_M when hasFunnel; > 0
    // always; bit-identical for equal inputs.
    inline F32 ringRadiusM(F32 collarRadius0, bool hasFunnel, F32 parentRadius)
    {
        (void)parentRadius; // reserved - see this function's own declaration comment
        return hasFunnel ? llmax(collarRadius0, RING_RADIUS_FUNNEL_FLOOR_M) : RING_RADIUS_GUSTNADO_M;
    }

    // Subdivide a vertical span into segments no taller than maxSegM before the caller applies squashScale per vertex (the
    // squash is non-linear along a segment) - capped at capSegments so a pathologically tall span never submits an
    // unbounded vertex count, the same cap SSVolCloud::renderDebug's own line-subdivision carries (ssvolcloud.cpp:
    // llclamp((S32)(span/200.f), 1, 24)) - capSegments defaults to that same 24 so every existing call site keeps
    // its prior behaviour unless it opts into a different cap. Returns the segment count for a span. Invariants:
    // >= 1; <= capSegments; == ceil(span/maxSegM) whenever that is <= capSegments, else == capSegments.
    inline S32 squashSegments(F32 spanM, F32 maxSegM, S32 capSegments = 24)
    {
        const S32 cap = llmax(capSegments, 1);
        if (spanM <= 0.f || maxSegM <= 0.f)
        {
            return 1;
        }
        return llclamp((S32)std::ceil(spanM / maxSegM), 1, cap);
    }
}

#endif
