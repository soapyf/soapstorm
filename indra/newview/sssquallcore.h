/**
 * @file sssquallcore.h
 * @brief Atmo Magic: organised modes and authored intent on the storm lattice - squall-line spawn template and forced storm overrides. Header-only core. CONTRACT.
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

#ifndef SS_SQUALLCORE_H
#define SS_SQUALLCORE_H

// <SS:Nexii> A CORE header (lldefs.h, ssatmonoisecore.h, ssstormcellcore.h, <cmath> only). Design: doc/atmo_magic_storm_dynamics.md sections 5 (squall line as a SPAWN TEMPLATE) and 6 (formation control, layer 3: authored / forced). Nothing here is a new field or a new entity type: a squall line is a rare lattice-epoch event that EMITS ordinary SSStormCell candidates along a line sharing one birth and one motion, and a forced storm is an ordinary candidate whose birth parameters are PINNED to guaranteed-active constants at the lattice cell nearest an authored offset. Both stay pure functions of (seed, epoch, anchor, authored keyframes) so every client composes the same line and hits the same cue. Bodies marked STUB are the implementer's; the invariants are the test author's spec.
#include "lldefs.h"
#include "ssatmonoisecore.h"
#include "ssstormcellcore.h"

#include <cmath>
#include <cstdint>

namespace SSSquall
{
    constexpr F32 LINE_ODDS         = 0.18f;   // share of qualifying epochs (severe weather) whose lattice event is a LINE instead of discrete cells (7d: 0.06 was one line per ~8 h of severe weather - unobservable; 0.18 is one per ~2.8 h)
    constexpr F32 SEVERE_CONSOLIDATION_MIN = 0.5f; // 7c NEW-5: an epoch is severe (may host a line) when SSWindProfile::consolidation(moisture, convection) at its phase >= this - the shell and the V2 view read THIS, never a literal
    constexpr F32 LINE_SPACING_M    = 2500.f;  // member spacing along the line
    constexpr S32 LINE_MEMBERS_MAX  = 7;       // members materialised (the local advancing segment of a line "hundreds of km long")
    constexpr F32 LINE_LENGTH_M     = LINE_SPACING_M * (F32)(LINE_MEMBERS_MAX - 1);
    constexpr F32 LINE_SPAWN_MIN_M  = 1500.f;  // a line's origin is composed this far up its motion vector from the anchor (its own range - never the hero's, whose 7f cap is 30 km)
    constexpr F32 LINE_SPAWN_MAX_M  = 2000.f;
    constexpr F32 LINE_JITTER_AGE01 = 0.04f;   // per-member lifecycle jitter so the line does not look synchronised
    constexpr F32 LINE_SUPPRESS_M   = LINE_SPACING_M; // 7b F6: a discrete lattice draw within this perpendicular distance of the line segment is replaced by the line; farther draws in the same epoch survive (the field is NOT emptied - with the old whole-field radius every discrete cell died on a line epoch)
    constexpr F32 QLCS_JUNCTION_FRAC = 0.5f;   // QLCS spin-ups sit at inter-member junctions on the leading edge
    // <SS:Nexii> 8g-1: the AUTHORED weather floor (forced pin and forced line). It is the authored severity, not a spawn
    // aid: with potential 1 and shear noise 1 a line member's gate score is moisture x convection, and the floor's product
    // must clear INTENSITY_FULL_SCORE so the wall is a full-intensity storm by construction. The first cut floored at
    // 0.5/0.5 = 0.25 against SPAWN_THRESHOLD 0.245: every member spawned at intensity 0.0005 (V2 read I 0.00), and since
    // lifecycle(), the line band's strength and the shafts' drive all scale by intensity, the authored wall arrived and
    // did nothing. applyWeatherFloor only raises, so a live sky already stormier than this keeps its own figures.
    constexpr F32 AUTHORED_FLOOR_MOISTURE   = 0.85f;
    constexpr F32 AUTHORED_FLOOR_CONVECTION = 0.85f;
    static_assert(AUTHORED_FLOOR_MOISTURE * AUTHORED_FLOOR_CONVECTION >= SSStormCell::INTENSITY_FULL_SCORE,
                  "an authored storm is a full-intensity storm: the floor's score must reach INTENSITY_FULL_SCORE");
    constexpr F64 FORCED_LEAD_S     = 800.0;   // a forced storm's birth is placed this long before its authored cue; lifetime = 2 * lead so age01 == 0.5 (mature) at the cue
    static_assert(2.0 * FORCED_LEAD_S >= (F64)SSStormCell::HERO_MIN_LIFE_S, "a forced tornado must be hero-eligible on its own lifetime - no eligibility bypass");
    static_assert(2.0 * FORCED_LEAD_S <= (F64)SSStormCell::LIFE_MAX_S, "forced lifetime inside the ordinary life range");

    struct Vec2
    {
        F32 x = 0.f;
        F32 y = 0.f;
    };

    // 7b F12: the ONE degenerate fallback for a zero motion vector - unit north - shared by lineEvent and qlcsJunction.
    // Invariants: unit length; equals v / |v| when |v| >= 1e-6; (0, 1) otherwise.
    inline Vec2 unitOrNorth(const Vec2& v)
    {
        const F32 speed = std::sqrt(v.x * v.x + v.y * v.y);
        Vec2 u;
        if (speed < 1e-6f) { u.x = 0.f; u.y = 1.f; return u; }
        u.x = v.x / speed;
        u.y = v.y / speed;
        return u;
    }

    // Per-field salts mixed into chain hashes with SSAtmoNoise::combine before hash01, own 0x5351xxxx table so nothing here ever reads a draw already spent by SSStormCell's 0x5343xxxx table.
    constexpr U32 SALT_LINE_ODDS      = 0x53510001u; // is this epoch's anchor-cell event a line
    constexpr U32 SALT_LINE_ID_HI     = 0x53510002u; // high 32 bits of mLineId
    constexpr U32 SALT_LINE_BIRTH     = 0x53510003u; // line birth offset within the epoch, [0, EPOCH_S)
    constexpr U32 SALT_LINE_LIFE      = 0x53510004u; // line lifetime, [LIFE_MIN_S, LIFE_MAX_S]
    constexpr U32 SALT_LINE_SPAWN     = 0x53510005u; // line origin's spawn-distance fraction (hero-style placement)
    constexpr U32 SALT_LINE_SUPERCELL = 0x53510006u; // which member index (if any) is the embedded supercell
    constexpr U32 SALT_MEMBER_ID_HI   = 0x53510007u; // high 32 bits of a member candidate's mId
    constexpr U32 SALT_MEMBER_JITTER  = 0x53510008u; // per-member lifecycle jitter sign/magnitude, hash of (lineId, i)

    // A line event for (anchor lattice, epoch): whether this epoch's event is a line, and its geometry.
    struct LineEvent
    {
        bool mIsLine = false;
        U64 mLineId = 0;
        Vec2 mOrigin;          // world: the line's centre, composed like the hero (up the motion vector, passing the anchor)
        Vec2 mDirection;       // unit, along the line (perpendicular to its motion)
        Vec2 mMotion;          // m/s, the line's advance = mean anvil wind * CELL_SPEED_FRAC (no supercell deviation)
        Vec2 mWindAnvil;       // 7b F2: the anvil wind this line was decided with - the shell hands it to LineDesc::mWindAnvil so EVERY member's stormMotion derives from this one sample (lesson 21), never from a per-member birth-phase read
        F64 mBirthTime = 0.0;
        F32 mLifetimeS = 0.f;
        S32 mSupercellSlot = -1; // which member index may be a supercell (-1 none)
        bool mForced = false;    // 8a: composed by forcedLine (an authored squall) - carried to LineDesc::mForced
        bool mHasFloor = false;  // 8a F1: forcedLine sets the same weather floor forcedCandidate gives an authored pin (AUTHORED_FLOOR_MOISTURE/CONVECTION, shear 1, both Allow flags), applied to every member before gate() - without it the generator's dry pre-cue sky gated the whole wall out
        SSStormCell::WeatherAtBirth mWeatherFloor;
    };

    // The line decision for an epoch, hashed from (seed, epoch, anchor lattice cell). Invariants: pure; mIsLine true for
    // roughly LINE_ODDS of epochs over 5000 samples (within 0.02) when severe is true, never when severe is false;
    // mDirection is unit and perpendicular to mMotion (dot within 1e-4); |mMotion| == |windAnvil| * CELL_SPEED_FRAC within
    // 1e-4; mSupercellSlot in [-1, LINE_MEMBERS_MAX); birth inside the epoch; lifetime in [LIFE_MIN_S, LIFE_MAX_S].
    inline LineEvent lineEvent(U32 seed, S64 epoch, const Vec2& anchor, const Vec2& windAnvil, bool severe)
    {
        using SSAtmoNoise::combine;
        using SSAtmoNoise::hash01;
        LineEvent e;
        if (!severe)
        {
            return e; // never a line on a non-severe epoch
        }

        // Chain hash over (seed, anchor lattice cell, epoch), mirroring SSStormCell::candidate's chain shape.
        const S32 lx = (S32)std::floor(anchor.x / SSStormCell::LATTICE_M);
        const S32 ly = (S32)std::floor(anchor.y / SSStormCell::LATTICE_M);
        const U32 epochLo = (U32)((U64)epoch & 0xffffffffu);
        const U32 epochHi = (U32)(((U64)epoch >> 32) & 0xffffffffu);
        U32 chain = combine(seed, (U32)lx);
        chain = combine(chain, (U32)ly);
        chain = combine(chain, epochLo);
        chain = combine(chain, epochHi);

        if (hash01(combine(chain, SALT_LINE_ODDS)) >= LINE_ODDS)
        {
            return e; // this epoch's event is discrete cells, not a line
        }
        e.mIsLine = true;
        e.mLineId = ((U64)combine(chain, SALT_LINE_ID_HI) << 32) | (U64)chain;

        e.mWindAnvil = windAnvil;
        e.mMotion.x = windAnvil.x * SSStormCell::CELL_SPEED_FRAC;
        e.mMotion.y = windAnvil.y * SSStormCell::CELL_SPEED_FRAC;
        const Vec2 unit = unitOrNorth(e.mMotion);
        const F32 dx = unit.x;
        const F32 dy = unit.y;
        e.mDirection.x = dy;   // right-hand perpendicular of the motion, unit length
        e.mDirection.y = -dx;

        e.mBirthTime = (F64)epoch * SSStormCell::EPOCH_S + (F64)hash01(combine(chain, SALT_LINE_BIRTH)) * SSStormCell::EPOCH_S;
        e.mLifetimeS = std::lerp(SSStormCell::LIFE_MIN_S, SSStormCell::LIFE_MAX_S, hash01(combine(chain, SALT_LINE_LIFE)));

        // Composed like the hero once was: origin sits LINE_SPAWN_MIN_M..LINE_SPAWN_MAX_M up the motion vector from the anchor so the line advances across it over its life.
        const F32 spawn = std::lerp(LINE_SPAWN_MIN_M, LINE_SPAWN_MAX_M, hash01(combine(chain, SALT_LINE_SPAWN))); // 7f: the line's OWN range - the hero's spawn cap grew to 30 km for its mid-life arrival law, which would put a line's members outside the field
        e.mOrigin.x = anchor.x - dx * spawn;
        e.mOrigin.y = anchor.y - dy * spawn;

        S32 slot = (S32)(hash01(combine(chain, SALT_LINE_SUPERCELL)) * (F32)LINE_MEMBERS_MAX);
        e.mSupercellSlot = llclamp(slot, 0, LINE_MEMBERS_MAX - 1);

        return e;
    }

    // The i-th member of a line as an ordinary storm candidate: origin = line origin + direction * (i - (N-1)/2) * spacing,
    // shared birth and motion (the caller reads them from the event), lifetime jittered by +-LINE_JITTER_AGE01 * lifetime via
    // a hash of (lineId, i), potential 1 (lines are severe by construction), rotation only on the supercell slot (else 0),
    // mId = combine(lineId, i). Invariants: members strictly LINE_SPACING_M apart along mDirection; identical birth/motion
    // across members; jitter within the bound; exactly one member has non-zero rotation when mSupercellSlot >= 0; ids unique.
    inline SSStormCell::Candidate lineMember(const LineEvent& e, S32 i)
    {
        using SSAtmoNoise::combine;
        using SSAtmoNoise::hash01;
        SSStormCell::Candidate c;
        if (!e.mIsLine)
        {
            return c;
        }

        const U32 lineLo = (U32)(e.mLineId & 0xffffffffu);
        const U32 lineHi = (U32)((e.mLineId >> 32) & 0xffffffffu);
        U32 chain = combine(lineLo, lineHi);
        chain = combine(chain, (U32)i);
        c.mId = ((U64)combine(chain, SALT_MEMBER_ID_HI) << 32) | (U64)chain;

        c.mBirthTime = e.mBirthTime;
        c.mEpoch = SSStormCell::epochOf(e.mBirthTime);

        const F32 offset = ((F32)i - (F32)(LINE_MEMBERS_MAX - 1) * 0.5f) * LINE_SPACING_M;
        c.mOriginXY.x = e.mOrigin.x + e.mDirection.x * offset;
        c.mOriginXY.y = e.mOrigin.y + e.mDirection.y * offset;

        const F32 jitterSign = hash01(combine(chain, SALT_MEMBER_JITTER)) * 2.f - 1.f;
        c.mLifetimeS = e.mLifetimeS * (1.f + jitterSign * LINE_JITTER_AGE01);

        c.mPotential = 1.f;                                  // lines are severe by construction
        c.mRotation = (i == e.mSupercellSlot) ? 1.f : 0.f;    // rotation only on the hash-picked supercell slot
        c.mShearNoise = 1.f;
        c.mSpawnFrac = 0.5f;
        c.mPassFrac = 0.5f;
        return c;
    }

    // Where a QLCS spin-up may form: the junction between members i and i+1 on the LEADING edge (offset along mMotion by
    // QLCS_JUNCTION_FRAC * spacing). Invariants: midway between the two members along mDirection; ahead of the line along
    // mMotion; pure.
    inline Vec2 qlcsJunction(const LineEvent& e, S32 i)
    {
        const SSStormCell::Candidate a = lineMember(e, i);
        const SSStormCell::Candidate b = lineMember(e, i + 1);
        const Vec2 unit = unitOrNorth(e.mMotion);
        const F32 mx = unit.x;
        const F32 my = unit.y;
        Vec2 j;
        j.x = 0.5f * (a.mOriginXY.x + b.mOriginXY.x) + mx * (QLCS_JUNCTION_FRAC * LINE_SPACING_M);
        j.y = 0.5f * (a.mOriginXY.y + b.mOriginXY.y) + my * (QLCS_JUNCTION_FRAC * LINE_SPACING_M);
        return j;
    }

    // 7b F9 (lesson 12): the junction ADVECTED to time t - qlcsJunction is a birth-frame position, the members the
    // view draws are advected by mMotion * (t - birth); a marker in the birth frame beside members in the advected
    // frame separates by |motion| * age. Invariants: equals qlcsJunction at t == mBirthTime; the difference from it is
    // exactly mMotion * (t - mBirthTime) within 1e-3; pure.
    inline Vec2 qlcsJunctionAt(const LineEvent& e, S32 i, F64 t)
    {
        Vec2 j = qlcsJunction(e, i);
        const F64 age = t - e.mBirthTime;
        j.x = (F32)((F64)j.x + (F64)e.mMotion.x * age);
        j.y = (F32)((F64)j.y + (F64)e.mMotion.y * age);
        return j;
    }

    // AUTHORED / FORCED: the cube carries keyframed overrides (kind string, cue phase, track-floor-relative offset). The shell
    // resolves them to this struct at the current phase; the core pins the lattice cell nearest (anchor + offset) for the
    // epoch containing (cueTime - FORCED_LEAD_S): that cell's candidate gets potential 1, rotation +1 (or -1 for an
    // authored anticyclonic), shear 1, birth = cueTime - FORCED_LEAD_S, lifetime 2 * FORCED_LEAD_S (>= HERO_MIN_LIFE_S by
    // static_assert, so a forced tornado is hero-eligible on its own terms). WHERE it is is not decided here: the
    // scheduler places every forced pin with SSStormCell::composeForced so that its centre at cueTime == anchor +
    // offset (7b F1 - the old "hero fracs so the flyby passes the anchor" claim was false: composeHero overwrote the
    // origin and the offset never placed anything). Invariants: pure in (seed, override, anchor); the pinned candidate
    // is alive at cueTime and mature (age01 == 0.5 within 1e-6) at cueTime; gate() with any weather >= 0 spawns it
    // (potential 1 with a moisture/convection floor of 0.5 applied by the caller); kind "none" returns pinned == false.
    // The shell converts the authored cue phase to mCueTime with the SAME map it read the override through -
    // SSStormCells::wallTimeAtPhase, ssstormcells.cpp, a one-line forward to SSDayCycle::wallTimeAtPhase(phase,
    // nearTau, dayLen, 0) applied to CYCLE time (tau), never a real/preview pair of maps: phase 8 section 2 (user
    // 2026-09-06) has the preview substitute the CLOCK feeding this map (a latched tau_ref/wall_ref), not the map
    // itself, so mCueTime comes out as a tau exactly like every other candidate's mBirthTime - superseding 7b F3/
    // lesson 24's SSStormCell::previewWallTimeAt, which is removed. The three override curves (phase, offset x/y)
    // are HOLD like the kind string, so the cue is piecewise constant in time and cannot slide (7b F4).
    struct ForcedOverride
    {
        bool mActive = false;
        S32 mKind = 0;          // 0 none, 1 supercell, 2 tornado (hero), 3 waterspout-preferred, 4 anticyclonic
        F64 mCueTime = 0.0;     // wall clock of the cue (the shell converts the authored phase to wall time)
        Vec2 mOffsetM;          // track-floor-relative XY offset from the anchor
    };
    struct Pinned
    {
        bool mPinned = false;
        SSStormCell::Candidate mCandidate;
        SSStormCell::WeatherAtBirth mWeatherFloor; // the floor the caller applies before gate()
    };
    inline Pinned forcedCandidate(U32 seed, const ForcedOverride& o, const Vec2& anchor)
    {
        Pinned p;
        if (!o.mActive || o.mKind == 0)
        {
            return p; // kind "none": mPinned stays false
        }

        // Nearest lattice cell to (anchor + offset): the cell whose centre (lx+0.5, ly+0.5)*L is closest to the point.
        const F32 L = SSStormCell::LATTICE_M;
        const S32 lx = (S32)std::round((anchor.x + o.mOffsetM.x) / L - 0.5f);
        const S32 ly = (S32)std::round((anchor.y + o.mOffsetM.y) / L - 0.5f);
        const S64 epoch = SSStormCell::epochOf(o.mCueTime - FORCED_LEAD_S);

        SSStormCell::Candidate c = SSStormCell::candidate(seed, lx, ly, epoch);
        c.mPotential = 1.f;
        c.mRotation = (o.mKind == 4) ? -1.f : 1.f; // anticyclonic override flips the sign, else cyclonic
        c.mShearNoise = 1.f;
        c.mBirthTime = o.mCueTime - FORCED_LEAD_S;
        c.mLifetimeS = (F32)(2.0 * FORCED_LEAD_S); // age01 == 0.5 at the cue; >= HERO_MIN_LIFE_S by the static_assert above
        c.mSpawnFrac = 0.5f; // unused by composeForced (kept neutral so a diagnostic composeHero read stays plausible)
        c.mPassFrac = 0.5f;

        p.mPinned = true;
        p.mCandidate = c;
        p.mWeatherFloor.mMoisture = AUTHORED_FLOOR_MOISTURE;     // 8g-1: the authored severity, see the constants
        p.mWeatherFloor.mConvection = AUTHORED_FLOOR_CONVECTION;
        p.mWeatherFloor.mShearStrength = 1.f;
        p.mWeatherFloor.mAllowSupercells = true; // 7c NEW-3: an authored override outranks the Allow checkboxes (applyWeatherFloor ORs them);
        p.mWeatherFloor.mAllowTornadoes = true;  // the Weather Influence MASTER enable is not overridden - the shell gates the forced block on it
        return p;
    }

    // WEATHERGEN BIAS (authoring-time): given a day's rolled convection/moisture/shear curve maxima, the bias a "severe day"
    // roll applies: raise the peaks toward (0.9, 0.85, 0.8) over a window of PHASE width centred on the rolled peak. Pure so
    // the authoring UI can preview it. Invariants: bias 0 returns the inputs; bias 1 reaches the targets within 1e-3;
    // monotone in bias; never exceeds 1.
    struct DayPeaks { F32 mMoisture = 0.f; F32 mConvection = 0.f; F32 mShear = 0.f; };
    inline DayPeaks severeDayBias(const DayPeaks& rolled, F32 bias)
    {
        const F32 b = llclamp(bias, 0.f, 1.f);
        DayPeaks d;
        d.mMoisture = std::lerp(rolled.mMoisture, 0.9f, b);
        d.mConvection = std::lerp(rolled.mConvection, 0.85f, b);
        d.mShear = std::lerp(rolled.mShear, 0.8f, b);
        return d;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // 8a AUTHORED SQUALL (doc/atmo_magic_phase8_show.md section 3): ForcedOverride::mKind KIND_SQUALL means "a squall line
    // whose leading edge crosses anchor + offset at the cue". The shell's line hook returns forcedLine's event for the ONE
    // epoch containing (cueTime - FORCED_LINE_LEAD_S), replacing that epoch's hashed decision (the members then run through
    // the ordinary member/gate path, so the line is deterministic and previewable exactly like a forced cell). A generated
    // squall line IS the weather change: the generator writes this override and lays the day's curves so precipitation
    // steps up AT the cue (onsetValue below) - clear sky, the wall arrives, hours of rain.
    constexpr S32 KIND_SQUALL           = 5;
    constexpr F64 FORCED_LINE_LEAD_S    = 1200.0;  // born this long before the cue; with a LIFE_MAX_S life the wall arrives young (age01 ~0.29) and matures over the region
    constexpr F32 LINE_BAND_M           = 1800.f;  // deck coupling: the wall's half-thickness either side of the segment
    constexpr F32 LINE_SHELF_M          = 2500.f;  // deck coupling: the gust-front shelf's reach AHEAD of the segment along its motion
    constexpr F32 LINE_ANVIL_FRAC       = 0.8f;    // the wall's anvil weight as a share of its tower boost (a line's anvil is a sheet, not a plume)
    constexpr U32 SALT_FORCED_LINE_ID   = 0x53510009u; // forcedLine's mLineId chain (seed, KIND_SQUALL, epoch of birth)
    constexpr U32 SALT_FORCED_LINE_SLOT = 0x5351000Au; // forcedLine's supercell slot

    // The authored line event for an active KIND_SQUALL override: motion = windAnvil * CELL_SPEED_FRAC (unit north when
    // degenerate, via unitOrNorth), direction its right-hand perpendicular, birth = cueTime - FORCED_LINE_LEAD_S, lifetime
    // LIFE_MAX_S, origin = (anchor + offset) - motion * FORCED_LINE_LEAD_S so the segment's CENTRE is at anchor + offset at
    // the cue, mLineId from combine(seed, KIND_SQUALL, epoch of birth) through SALT_FORCED_LINE_ID (stable while the cue
    // stays inside one epoch), mSupercellSlot hashed in [0, LINE_MEMBERS_MAX), mWindAnvil = windAnvil. Invariants: mIsLine
    // false unless o.mActive && o.mKind == KIND_SQUALL; origin + motion * (cueTime - birth) == anchor + offset within F32
    // reach (0.25 m at a 256 km anchor, where one ulp is 0.03 m - the 7f contact tolerance, same reason: mOrigin is an F32
    // at a grid-global coordinate, so no spelling of this composition reaches 1e-2 m there) for |motion| <= 60 m/s;
    // direction unit and perpendicular to motion (dot within 1e-4); lineMember(e, i) for every i
    // is alive at the cue (age01 == FORCED_LINE_LEAD_S / LIFE_MAX_S within the member jitter); pure; bit-identical.
    inline LineEvent forcedLine(U32 seed, const ForcedOverride& o, const Vec2& anchor, const Vec2& windAnvil)
    {
        using SSAtmoNoise::combine;
        using SSAtmoNoise::hash01;
        LineEvent e;
        if (!o.mActive || o.mKind != KIND_SQUALL)
        {
            return e; // not an active squall override
        }
        e.mForced = true;
        e.mHasFloor = true;
        e.mWeatherFloor.mMoisture = AUTHORED_FLOOR_MOISTURE;     // 8g-1: full intensity by construction, not merely a spawn
        e.mWeatherFloor.mConvection = AUTHORED_FLOOR_CONVECTION;
        e.mWeatherFloor.mShearStrength = 1.f;
        e.mWeatherFloor.mAllowSupercells = true;
        e.mWeatherFloor.mAllowTornadoes = true;

        e.mIsLine = true;
        e.mWindAnvil = windAnvil;
        e.mMotion.x = windAnvil.x * SSStormCell::CELL_SPEED_FRAC;
        e.mMotion.y = windAnvil.y * SSStormCell::CELL_SPEED_FRAC;
        const Vec2 unit = unitOrNorth(e.mMotion);
        e.mDirection.x = unit.y;   // right-hand perpendicular of the motion, unit length - same spelling as lineEvent
        e.mDirection.y = -unit.x;

        e.mBirthTime = o.mCueTime - FORCED_LINE_LEAD_S;
        e.mLifetimeS = SSStormCell::LIFE_MAX_S;

        const S64 epoch = SSStormCell::epochOf(e.mBirthTime);
        const U32 epochLo = (U32)((U64)epoch & 0xffffffffu);
        const U32 epochHi = (U32)(((U64)epoch >> 32) & 0xffffffffu);
        U32 chain = combine(seed, (U32)KIND_SQUALL);
        chain = combine(chain, epochLo);
        chain = combine(chain, epochHi);
        e.mLineId = ((U64)combine(chain, SALT_FORCED_LINE_ID) << 32) | (U64)chain;

        // Origin composed like composeForced: the segment centre at the cue is exactly anchor + offset.
        const F32 centreX = anchor.x + o.mOffsetM.x;
        const F32 centreY = anchor.y + o.mOffsetM.y;
        e.mOrigin.x = centreX - e.mMotion.x * (F32)FORCED_LINE_LEAD_S;
        e.mOrigin.y = centreY - e.mMotion.y * (F32)FORCED_LINE_LEAD_S;

        const S32 slot = (S32)(hash01(combine(chain, SALT_FORCED_LINE_SLOT)) * (F32)LINE_MEMBERS_MAX);
        e.mSupercellSlot = llclamp(slot, 0, LINE_MEMBERS_MAX - 1);

        return e;
    }

    // AUTHORED ONSET (authoring time, the generator lays keyframes from it): a curve that sits at `before` until
    // cuePhase - rampPhase, rises (smoothstep) to `peak` at cuePhase, holds for holdPhase, then falls (smoothstep) back to
    // `before` over taperPhase. Phases are day fractions and wrap mod 1 (a cue at 0.98 with a 0.1 hold spans midnight).
    // Invariants: == before well before the ramp and well after the taper; == peak at cuePhase and throughout the hold;
    // monotone on the ramp and on the taper; continuous; pure; onsetValue(p, o, v, v) == v for every p.
    struct Onset
    {
        F32 cuePhase = 0.5f;
        F32 rampPhase = 0.02f;   // ONSET_RAMP_PHASE: ~5 min of a 4 h day - a squall's onset is abrupt
        F32 holdPhase = 0.15f;   // hours of rain
        F32 taperPhase = 0.10f;
    };
    constexpr F32 ONSET_RAMP_PHASE = 0.02f;
    inline F32 onsetValue(F32 phase, const Onset& o, F32 before, F32 peak)
    {
        F32 d = phase - o.cuePhase;
        d -= std::floor(d + 0.5f); // signed distance from the cue, wrapped onto the circle into [-0.5, 0.5)
        if (d <= 0.f)
        {
            return std::lerp(before, peak, SSStormCell::smoothstep(-o.rampPhase, 0.f, d));
        }
        if (d <= o.holdPhase)
        {
            return peak;
        }
        return std::lerp(peak, before, SSStormCell::smoothstep(o.holdPhase, o.holdPhase + o.taperPhase, d));
    }
}

#endif
