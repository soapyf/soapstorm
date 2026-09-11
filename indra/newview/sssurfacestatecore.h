/**
 * @file sssurfacestatecore.h
 * @brief Atmo Magic: surface weather material state - ice, frost, stain, deposit age, liquid and deposit looks, the wet-material laws the shaders replicate, puddle impact rings. Header-only core. CONTRACT.
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

#ifndef SS_SURFACESTATECORE_H
#define SS_SURFACESTATECORE_H

// <SS:Nexii> A CORE header (lldefs.h, ssatmonoisecore.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_surface_weather.md. The surface field (sssurfacefield.h) already integrates wet, puddle depth and deposit depth per cell; this core adds the STATE those three take on - frozen (ice), hoar frost, the stain a coloured liquid leaves, the age of a deposit - as pure fixed-step transitions, plus the LOOKS (what liquid, what deposit) the passes tint by, and the per-fragment laws the three surface shaders replicate (porosity, wet darkening, drop coverage, deposit thickness, impact rings). Everything here is a pure function of its arguments: no settings, no singletons, no clock but the dt handed in. Fixed-step discipline: the shell calls stepCell in exact TICK quanta of shared time, so every client integrates the same ledger. Bodies marked STUB are the implementer's; the invariants in the comments are the test author's spec. LOCKSTEP constants are duplicated verbatim in the GLSL named beside them; the twin tests read both sources.
#include "lldefs.h"
#include "ssatmonoisecore.h"

#include <cmath>
#include <cstdint>

namespace SSSurfaceState
{
    // ------------------------------------------------------------------ constants (the physics dials; art dials live on the looks)

    constexpr F32 FREEZE_ONSET_C     = 0.f;     // ice starts forming below this
    constexpr F32 FREEZE_FULL_C      = -8.f;    // ... and forms at full rate at or below this
    constexpr F32 ICE_FORM_S         = 240.f;   // seconds for standing water to freeze solid at full rate (ice 0 -> 1)
    constexpr F32 ICE_THAW_C         = 6.f;     // thaw rate is full at or above this many degrees over the onset
    constexpr F32 ICE_THAW_S         = 180.f;   // seconds for solid ice to thaw at full thaw rate
    constexpr F32 ICE_DRY_S          = 30.f;    // seconds for ice to vanish once the water it froze is gone (nothing left to be frozen)
    constexpr F32 ICE_WATER_MIN      = 0.02f;   // wet film below this, with no puddle, counts as "no water": ice decays on ICE_DRY_S

    constexpr F32 FROST_ONSET_C      = -1.f;    // hoar frost starts below this
    constexpr F32 FROST_FULL_C       = -10.f;   // ... full rate at or below
    constexpr F32 FROST_HUMIDITY_MIN = 0.35f;   // no frost in dry air; rate scales linearly from here to 1 at humidity 1
    constexpr F32 FROST_FORM_S       = 900.f;   // seconds to full frost at full rate on a fully exposed cell
    constexpr F32 FROST_SUBLIME_S    = 300.f;   // seconds for full frost to go at full sublimation (warm or sunlit)
    constexpr F32 FROST_UNDER_DEPOSIT_M = 0.005f; // a deposit deeper than this buries the frost: frost -> 0 on FROST_SUBLIME_S
    constexpr F32 FROST_EXPOSURE_MIN = 0.15f;   // sheltered cells (exposure below) grow no frost (radiative cooling needs sky)

    constexpr F32 STAIN_FADE_S       = 3600.f;  // a stain on a dry surface fades over an hour (very slow; the wash below is what really clears it)
    constexpr F32 STAIN_WASH_S       = 600.f;   // clean liquid at wet 1 washes a full stain in ten minutes
    constexpr F32 STAIN_SET_S        = 60.f;    // a staining liquid at wet 1 sets a full stain in a minute

    constexpr F32 AGE_S              = 1800.f;  // a deposit compacts (age 0 -> 1) over half an hour without fresh fall
    constexpr F32 AGE_MELT_C         = 2.f;     // above the onset, warmth ages a deposit toward slush at up to this many times the rate...
    constexpr F32 AGE_MELT_GAIN      = 3.f;     // ... (rate multiplier at AGE_MELT_C or warmer)
    constexpr F32 AGE_WASH_S         = 120.f;   // liquid precipitation at intensity 1 removes a washable deposit (depthFull) in two minutes

    constexpr F32 KIND_MIX_FULL      = 1.f;     // a look mix completes when the new kind has deposited depthFull worth (see advanceMix)

    // Per-fragment laws, LOCKSTEP ssSurfaceStateF.glsl (the twin test reads both sources).
    constexpr F32 WET_DARKEN_POW     = 0.8f;    // albedo exponent gain at wet 1 on a fully porous surface: albedo^(1 + 0.8) (Lagarde's "wet = darker and more saturated")
    constexpr F32 POROSITY_LUM_CUT   = 0.3f;    // bright surfaces read a little less porous
    constexpr F32 POROSITY_VAR_FLOOR = 0.55f;   // a perfectly uniform albedo still gets this much of the variance term
    constexpr F32 DROP_INTENSITY_LO  = 0.02f;   // static drops appear from drizzle...
    constexpr F32 DROP_INTENSITY_HI  = 0.50f;   // ... reach full density here...
    constexpr F32 SHEET_INTENSITY_LO = 0.55f;   // ... and give way to sheet flow from here...
    constexpr F32 SHEET_INTENSITY_HI = 0.95f;   // ... fully at torrential
    constexpr F32 DROP_SIZE_LO       = 0.6f;    // drop radius scale at drizzle
    constexpr F32 DROP_SIZE_HI       = 1.6f;    // drop radius scale at heavy
    constexpr F32 DRIP_INTENSITY_LO  = 0.05f;   // drips on verticals from here...
    constexpr F32 DRIP_INTENSITY_HI  = 0.60f;   // ... to full density here
    constexpr F32 DRIP_SPEED_LO      = 0.3f;    // drip slide speed scale at drizzle
    constexpr F32 DRIP_SPEED_HI      = 1.0f;    // ... at heavy
    constexpr F32 DEPOSIT_SOFTEN_MAX = 0.7f;    // how far a thick deposit rounds the normal toward up (thickness 3x depthFull)
    constexpr F32 DEPOSIT_SOFTEN_DEPTHS = 3.f;  // depthFull multiples at which softening saturates
    constexpr F32 SPARKLE_AGE_LOSS   = 0.7f;    // a fully aged deposit keeps this much less sparkle
    constexpr F32 ICE_ROUGHNESS      = 0.22f;   // frozen puddle roughness (PBR ORM.g target)
    constexpr F32 FROST_ROUGHNESS    = 0.85f;   // frost is matte
    constexpr F32 DEPOSIT_ROUGHNESS  = 0.85f;   // so is a deposit

    // Impact ripple rings (doc section 6), LOCKSTEP ssSurfaceNormalF.glsl. Every time below is measured on the RING'S OWN CLOCK, which the shell runs at ringRate() times the wall clock so the ring
    // and the landing ripple quad it stands in for expand at the same metres per second off the same controls; multiply by 1 / ringRate to get seconds.
    constexpr F32 RING_SPEED_MPS     = 0.55f;   // ring radius growth per unit of the ring's clock
    constexpr F32 RING_WAVELENGTH_M  = 0.045f;  // crest spacing
    constexpr F32 RING_WIDTH_M       = 0.06f;   // gaussian width of the wave packet around the front
    constexpr F32 RING_DAMP_S        = 1.4f;    // amplitude e-folding time
    constexpr F32 RING_AMPLITUDE     = 0.0072f; // height amplitude, metres, at strength 1, t 0
    constexpr F32 RING_LIFE_S        = 2.0f;    // a ring older than this contributes nothing (and is dropped from the buffer)
    constexpr F32 RING_NEAR_M        = 4.f;     // rings are only tracked (and ripple quads suppressed) inside this camera radius
    constexpr S32 RING_MAX           = 24;      // buffer capacity: the newest RING_MAX impacts inside RING_NEAR_M
    constexpr F32 RING_SLOPE_MAX     = 0.6f;    // |ringSlope| ceiling, LOCKSTEP ssSurfaceNormalF.glsl
    constexpr F32 RING_REACH_M       = RING_SPEED_MPS * RING_LIFE_S;   // 1.1 m: how far the front gets before the ring is gone, whatever rate it runs at
    constexpr F32 RING_SPREAD_R0_M   = 0.35f;   // radius up to which the crest keeps full amplitude - a landing quad's own radius, past which it thins as 1/sqrt(r)
    constexpr F32 RING_FADE_FROM     = 0.55f;   // fraction of the life where the terminal taper starts, so nothing is left to cut off at the end
    constexpr F32 RING_QUAD_GROWTH   = 0.85f;   // the fraction of its final radius a landing ripple quad's front crosses over its life (it starts at 0.15 of it)
    constexpr F32 RING_RATE_MIN      = 0.35f;   // clock rate floor and ceiling: the taste controls scale the quad without bound, the ring stays a ring
    constexpr F32 RING_RATE_MAX      = 3.0f;

    // ------------------------------------------------------------------ looks

    struct Rgb
    {
        F32 r = 1.f, g = 1.f, b = 1.f;
    };

    // What the liquid channel (wet, puddle) looks like. Water is the default: clear, no stain, dielectric.
    struct LiquidLook
    {
        Rgb mTint;                // the liquid's own colour where it is opaque
        F32 mOpacity = 0.f;       // 0 clear water (the surface shows through, darkened) .. 1 opaque (ink)
        F32 mStain = 0.f;         // 0 leaves nothing when dry .. 1 sets a full stain (see stepCell)
        F32 mMetal = 0.f;         // 0 dielectric .. 1 liquid metal (mercury): the wet pass pushes metallic, not just roughness
    };

    // What the deposit channel (the field's mSnow depth) looks like. Snow is the default: mTint 0.93/0.95/0.99,
    // mSparkle 0.7, mTranslucency 0.45 (the doc's Snow).
    struct DepositLook
    {
        Rgb mTint{ 0.93f, 0.95f, 0.99f };
        F32 mSparkle = 0.7f;      // 0 (ash) .. 1 (fresh snow) glint strength before age
        F32 mTranslucency = 0.45f; // 0 opaque (sand) .. 1 (snow): how much a thin layer lets the albedo through
        F32 mDepthFull = 0.02f;   // metres at which coverage reads as full
        F32 mWash = 0.f;          // 0 never washed by rain (snow: it melts instead) .. 1 fully washable (ash, dust) at AGE_WASH_S
        F32 mMelts = 1.f;         // 1 melts to wet above freezing (snow) .. 0 never melts (sand, ash); the settle pass's melt scales by this
    };

    // Two looks and a crossfade: the field holds ONE deposit and ONE liquid at a time (the user's rule), and when a new kind starts falling
    // onto the old the look crosses over as the new kind accumulates - not when the preset switches (a switch with nothing yet fallen must
    // change nothing on the ground). mT is 0 (all mA) .. 1 (all mB); at 1 the shell promotes mB to mA and resets.
    template <typename Look>
    struct Mix
    {
        Look mA;
        Look mB;
        F32 mT = 0.f;
    };

    // Advances a mix: gain is what the new kind (mB) added this tick in the channel's own units (metres of deposit, or wet 0..1 gained),
    // present is how much of the channel is present in total, full is the channel's "full" figure (depthFull, or 1 for wet). t rises
    // by gain / max(present, full), clamped to [0,1]. Invariants: gain 0 -> unchanged; monotone non-decreasing in gain; never exceeds 1;
    // a gain of exactly max(present, full) from t 0 reaches 1; pure.
    inline F32 advanceMix(F32 t, F32 gain, F32 present, F32 full)
    {
        return llclamp(t + gain / llmax(llmax(present, full), 1.0e-6f), 0.f, 1.f);   // STUB-OK: reference form
    }

    inline Rgb mixRgb(const Rgb& a, const Rgb& b, F32 t)
    {
        return Rgb{ std::lerp(a.r, b.r, t), std::lerp(a.g, b.g, t), std::lerp(a.b, b.b, t) };
    }

    // The look the shaders receive: the crossfade resolved. Every scalar lerps; the tint lerps per channel. t is clamped.
    inline LiquidLook resolve(const Mix<LiquidLook>& m)
    {
        const F32 t = llclamp(m.mT, 0.f, 1.f);
        LiquidLook out;
        out.mTint = mixRgb(m.mA.mTint, m.mB.mTint, t);
        out.mOpacity = std::lerp(m.mA.mOpacity, m.mB.mOpacity, t);
        out.mStain = std::lerp(m.mA.mStain, m.mB.mStain, t);
        out.mMetal = std::lerp(m.mA.mMetal, m.mB.mMetal, t);
        return out;
    }

    inline DepositLook resolve(const Mix<DepositLook>& m)
    {
        const F32 t = llclamp(m.mT, 0.f, 1.f);
        DepositLook out;
        out.mTint = mixRgb(m.mA.mTint, m.mB.mTint, t);
        out.mSparkle = std::lerp(m.mA.mSparkle, m.mB.mSparkle, t);
        out.mTranslucency = std::lerp(m.mA.mTranslucency, m.mB.mTranslucency, t);
        out.mDepthFull = std::lerp(m.mA.mDepthFull, m.mB.mDepthFull, t);
        out.mWash = std::lerp(m.mA.mWash, m.mB.mWash, t);
        out.mMelts = std::lerp(m.mA.mMelts, m.mB.mMelts, t);
        return out;
    }

    // ------------------------------------------------------------------ per-cell state

    // The four state channels a field cell carries beside wet / puddle / deposit depth. All in [0,1].
    struct CellState
    {
        F32 mIce = 0.f;     // frozen fraction of whatever water the cell holds (a full puddle at ice 1 is a frozen puddle; a wet film at ice 1 is glaze)
        F32 mFrost = 0.f;   // hoar frost cover, independent of precipitation
        F32 mStain = 0.f;   // persistent tint left by a staining liquid
        F32 mAge = 0.f;     // deposit compaction: 0 fresh, 1 old / slushy
    };

    // Everything stepCell reads. The shell fills it per cell from the field, the geometry and the tick's shared scalars.
    struct CellIn
    {
        F32 mWet = 0.f;           // the cell's wet film after this tick's settle pass, 0..1
        F32 mPuddle = 0.f;        // standing depth, metres
        F32 mDeposit = 0.f;       // deposit depth, metres
        F32 mDepositGain = 0.f;   // metres of deposit ADDED this tick by fall (0 when none)
        F32 mWetGain = 0.f;       // wet added this tick (0 when drying), 0..1; read by no core rule - the shell feeds it to advanceMix for the liquid look
        F32 mExposure = 1.f;      // 0 sheltered .. 1 open sky (the geometry's sky view; the shell may pass 1 where it has no answer)
        F32 mTempC = 15.f;        // air temperature this tick
        F32 mHumidity = 0.5f;     // 0..1 relative humidity proxy
        F32 mSunlit = 0.f;        // 0 night / overcast .. 1 full sun on the cell (sun elevation x exposure)
        F32 mLiquidStain = 0.f;   // the stain of the liquid FALLING this tick (the ACTIVE preset's look, 0 when no liquid falls) - NOT the resolved ground look's stain: the ground mix is what is on the surface, the falling liquid is what sets or washes it. Clean rain after an ink storm therefore reaches the wash branch immediately.
        F32 mLiquidIntensity = 0.f; // liquid precipitation intensity this tick, 0..1 (0 while a granular type falls)
        F32 mDepositWash = 0.f;   // the resolved deposit look's mWash
        F32 mDepositFull = 0.02f; // the resolved deposit look's mDepthFull
    };

    // Rate helpers, each a pure ramp in [0,1]. freezeRate: 0 at or above FREEZE_ONSET_C, 1 at or below FREEZE_FULL_C, linear between.
    // thawRate: 0 at or below the onset, 1 at onset + ICE_THAW_C. frostRate: the same shape on the FROST constants, times the humidity
    // ramp (0 at or below FROST_HUMIDITY_MIN, 1 at humidity 1). Invariants: monotone; clamped; pure.
    inline F32 freezeRate(F32 tempC)
    {
        return llclamp((FREEZE_ONSET_C - tempC) / (FREEZE_ONSET_C - FREEZE_FULL_C), 0.f, 1.f);
    }
    inline F32 thawRate(F32 tempC)
    {
        return llclamp((tempC - FREEZE_ONSET_C) / ICE_THAW_C, 0.f, 1.f);
    }
    inline F32 frostRate(F32 tempC, F32 humidity)
    {
        const F32 cold = llclamp((FROST_ONSET_C - tempC) / (FROST_ONSET_C - FROST_FULL_C), 0.f, 1.f);
        const F32 wetAir = llclamp((humidity - FROST_HUMIDITY_MIN) / (1.f - FROST_HUMIDITY_MIN), 0.f, 1.f);
        return cold * wetAir;
    }

    // One fixed-step transition of a cell's state. Rules (each its own invariant, testable in isolation):
    //  ICE  - has water = (mPuddle > 0) or (mWet >= ICE_WATER_MIN). With water and freezeRate > 0: ice += freezeRate * dt / ICE_FORM_S.
    //         With water and thawRate > 0: ice -= thawRate * dt / ICE_THAW_S. Without water: ice -= dt / ICE_DRY_S. Never both grows and
    //         thaws in one step (the rates are mutually exclusive by temperature). Clamped [0,1].
    //  FROST- buried = mDeposit > FROST_UNDER_DEPOSIT_M. If buried: frost -= dt / FROST_SUBLIME_S. Else if frostRate > 0 and
    //         mExposure >= FROST_EXPOSURE_MIN: frost += frostRate * mExposure * dt / FROST_FORM_S. Else (warm or sheltered or dry air):
    //         frost -= max(thawRate(mTempC - FROST_ONSET_C + FREEZE_ONSET_C), mSunlit) * dt / FROST_SUBLIME_S, i.e. warmth or sun
    //         sublimates it and a cold, dark, sheltered cell simply keeps what it has. Clamped [0,1]. A cell with mExposure 0 and
    //         frostRate 1 grows nothing.
    //  STAIN- if mLiquidStain > 0 and mWet > 0: stain += mLiquidStain * mWet * dt / STAIN_SET_S (a staining liquid sets while wet);
    //         else if mWet > 0 (clean liquid): stain -= mWet * dt / STAIN_WASH_S; else stain -= dt / STAIN_FADE_S. Clamped [0,1].
    //         mLiquidStain is the stain of the liquid FALLING this tick (the ACTIVE preset's look, 0 when no liquid falls) - NOT the
    //         resolved ground look's stain: the ground mix is what is on the surface, the falling liquid is what sets or washes it.
    //         Clean rain after an ink storm therefore reaches the wash branch immediately.
    //  AGE  - if mDepositGain > 0: age -= mDepositGain / max(mDeposit, mDepositFull) (fresh fall un-ages in proportion to how much of
    //         the pile is new); else age += (1 + AGE_MELT_GAIN * clamp((mTempC - FREEZE_ONSET_C) / AGE_MELT_C, 0, 1)) * dt / AGE_S.
    //         Clamped [0,1]. mDeposit 0 -> age 0 (no pile, no age). The ONE non-dt-scaled rule: mDepositGain must be the gain
    //         accumulated over the whole fixed quantum (the shell's tick is one quantum), never a per-frame gain.
    // Invariants beyond the rules: all four outputs in [0,1]; identical inputs -> bit-identical outputs (SS_CHECK_BITS); every rule
    // monotone in its driving input (colder -> more ice per step, more humidity -> more frost per step, more mLiquidStain -> more
    // stain per step); dt 0 changes nothing; a step never reads anything but its arguments.
    inline void stepCell(CellState& s, const CellIn& in, F32 dt)
    {
        // <SS:Nexii> ICE: mutually exclusive by temperature (freezeRate>0 xor thawRate>0), so if/else-if is safe and matches "never both".
        const bool hasWater = (in.mPuddle > 0.f) || (in.mWet >= ICE_WATER_MIN);
        const F32 fRate = freezeRate(in.mTempC);
        const F32 tRate = thawRate(in.mTempC);
        if (hasWater)
        {
            if (fRate > 0.f) s.mIce += fRate * dt / ICE_FORM_S;
            else if (tRate > 0.f) s.mIce -= tRate * dt / ICE_THAW_S;
        }
        else
        {
            s.mIce -= dt / ICE_DRY_S;
        }
        s.mIce = llclamp(s.mIce, 0.f, 1.f);

        // <SS:Nexii> FROST: buried check first, then grow-if-cold-and-exposed, else warm/sheltered/dry sublimation using the onset-shifted thaw ramp.
        const bool buried = in.mDeposit > FROST_UNDER_DEPOSIT_M;
        if (buried)
        {
            s.mFrost -= dt / FROST_SUBLIME_S;
        }
        else
        {
            const F32 frRate = frostRate(in.mTempC, in.mHumidity);
            if (frRate > 0.f && in.mExposure >= FROST_EXPOSURE_MIN)
            {
                s.mFrost += frRate * in.mExposure * dt / FROST_FORM_S;
            }
            else
            {
                const F32 sublimate = llmax(thawRate(in.mTempC - FROST_ONSET_C + FREEZE_ONSET_C), in.mSunlit);
                s.mFrost -= sublimate * dt / FROST_SUBLIME_S;
            }
        }
        s.mFrost = llclamp(s.mFrost, 0.f, 1.f);

        // <SS:Nexii> STAIN: staining while wet, washing while wet with clean liquid, fading while dry - in that exclusive order.
        if (in.mLiquidStain > 0.f && in.mWet > 0.f)
        {
            s.mStain += in.mLiquidStain * in.mWet * dt / STAIN_SET_S;
        }
        else if (in.mWet > 0.f)
        {
            s.mStain -= in.mWet * dt / STAIN_WASH_S;
        }
        else
        {
            s.mStain -= dt / STAIN_FADE_S;
        }
        s.mStain = llclamp(s.mStain, 0.f, 1.f);

        // <SS:Nexii> AGE: fresh fall un-ages, else compaction/melt ages it; then the "no pile, no age" override wins regardless of branch.
        F32 age = s.mAge;
        if (in.mDepositGain > 0.f)
        {
            age -= in.mDepositGain / llmax(in.mDeposit, in.mDepositFull);
        }
        else
        {
            const F32 meltGain = llclamp((in.mTempC - FREEZE_ONSET_C) / AGE_MELT_C, 0.f, 1.f);
            age += (1.f + AGE_MELT_GAIN * meltGain) * dt / AGE_S;
        }
        age = llclamp(age, 0.f, 1.f);
        if (in.mDeposit <= 0.f) age = 0.f;
        s.mAge = age;
    }

    // The deposit removal that liquid precipitation does to a washable deposit, in metres this tick:
    // mDepositWash * mLiquidIntensity * mDepositFull * dt / AGE_WASH_S, never more than mDeposit. Invariants: 0 when any of wash,
    // intensity, deposit is 0; monotone in each; <= mDeposit.
    inline F32 washRemoval(const CellIn& in, F32 dt)
    {
        const F32 removal = in.mDepositWash * in.mLiquidIntensity * in.mDepositFull * dt / AGE_WASH_S;
        return llclamp(removal, 0.f, in.mDeposit);
    }

    // ------------------------------------------------------------------ per-fragment laws (LOCKSTEP ssSurfaceStateF.glsl)

    // <SS:Nexii> local smoothstep helper - the GLSL twin uses the same t*t*(3-2t) shape on the clamped ramp; no new public names beyond this. Guarded like SSScreenFX::smoothstep: e1 <= e0 is a degenerate (zero-width or inverted) edge, not a divide-by-zero - the ramp becomes a step at e0.
    inline F32 smoothstep(F32 e0, F32 e1, F32 x)
    {
        if (e1 <= e0)
        {
            return (x <= e0) ? 0.f : 1.f;
        }
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // How porous a surface reads, 0 sealed (glass, polished metal, gloss paint) .. 1 porous (soil, fabric, raw concrete). PBR inputs:
    // roughness01, metallic01, albedo luminance 0..1, albedoVar01 = the local albedo variance measure the shader takes from a 4-tap
    // neighbourhood (0 flat .. 1 very noisy). Formula (twin): pow(rough, 1.5) * (1 - metal) * (POROSITY_VAR_FLOOR + (1 - POROSITY_VAR_FLOOR)
    // * var) * (1 - POROSITY_LUM_CUT * lum), clamped. Invariants: 0 at metal 1; 0 at rough 0; monotone in rough and var; non-increasing
    // in lum and metal; in [0,1].
    inline F32 porosity(F32 rough, F32 metal, F32 lum, F32 var)
    {
        const F32 val = std::pow(rough, 1.5f) * (1.f - metal)
            * (POROSITY_VAR_FLOOR + (1.f - POROSITY_VAR_FLOOR) * var) * (1.f - POROSITY_LUM_CUT * lum);
        return llclamp(val, 0.f, 1.f);
    }

    // The legacy (non-PBR) porosity: from the specular colour's brightness (0 none .. 1 full) and the same variance term:
    // (1 - specBright) * (POROSITY_VAR_FLOOR + (1 - POROSITY_VAR_FLOOR) * var). Invariants: 1 at specBright 0 and var 1; 0 at specBright 1.
    inline F32 porosityLegacy(F32 specBright, F32 var)
    {
        const F32 val = (1.f - specBright) * (POROSITY_VAR_FLOOR + (1.f - POROSITY_VAR_FLOOR) * var);
        return llclamp(val, 0.f, 1.f);
    }

    // The albedo exponent of a wet surface: 1 + WET_DARKEN_POW * wet * porosity. Applied as pow(albedo, exponent) per channel in the
    // albedo pass - darker and more saturated, more so the more porous. Invariants: 1 at wet 0 or porosity 0; monotone; <= 1 + WET_DARKEN_POW.
    inline F32 wetAlbedoExponent(F32 wet, F32 porosity)
    {
        return 1.f + WET_DARKEN_POW * wet * porosity;
    }

    // Rain drop coverage laws from intensity 0..1. Twin of the GLSL.
    struct DropLaw
    {
        F32 mStatic = 0.f;   // density of static drops on up-facing sealed surfaces: smoothstep(DROP_INTENSITY_LO, DROP_INTENSITY_HI, i) * (1 - smoothstep(SHEET_INTENSITY_LO, SHEET_INTENSITY_HI, i))
        F32 mSize = 0.f;     // drop radius scale: lerp(DROP_SIZE_LO, DROP_SIZE_HI, smoothstep(DROP_INTENSITY_LO, DROP_INTENSITY_HI, i))
        F32 mDrip = 0.f;     // density of drips on verticals: smoothstep(DRIP_INTENSITY_LO, DRIP_INTENSITY_HI, i)
        F32 mDripSpeed = 0.f;// drip slide speed scale: lerp(DRIP_SPEED_LO, DRIP_SPEED_HI, mDrip)
        F32 mSheet = 0.f;    // sheet flow on any sloped wet surface: smoothstep(SHEET_INTENSITY_LO, SHEET_INTENSITY_HI, i)
    };
    // Invariants: all 0 at i 0; mStatic peaks strictly inside (DROP_INTENSITY_HI, SHEET_INTENSITY_LO) at 1 and is 0 at i 1; mSheet is
    // 1 at i 1; mDrip and mSheet monotone; mSize in [DROP_SIZE_LO, DROP_SIZE_HI].
    inline DropLaw dropLaw(F32 intensity)
    {
        const F32 dropRamp = smoothstep(DROP_INTENSITY_LO, DROP_INTENSITY_HI, intensity);
        const F32 sheetRamp = smoothstep(SHEET_INTENSITY_LO, SHEET_INTENSITY_HI, intensity);
        DropLaw d;
        d.mStatic = dropRamp * (1.f - sheetRamp);
        d.mSize = std::lerp(DROP_SIZE_LO, DROP_SIZE_HI, dropRamp);
        d.mDrip = smoothstep(DRIP_INTENSITY_LO, DRIP_INTENSITY_HI, intensity);
        d.mDripSpeed = std::lerp(DRIP_SPEED_LO, DRIP_SPEED_HI, d.mDrip);
        d.mSheet = sheetRamp;
        return d;
    }

    // Deposit thickness laws. coverage(depth, depthFull) = smoothstep(0, depthFull, depth) (twin). soften(depth, depthFull) =
    // DEPOSIT_SOFTEN_MAX * clamp(depth / (depthFull * DEPOSIT_SOFTEN_DEPTHS), 0, 1) - how far the normal rounds toward up. sparkle(base, age)
    // = base * (1 - SPARKLE_AGE_LOSS * age). Invariants: coverage 0 at depth 0, 1 at depthFull; soften saturates at DEPOSIT_SOFTEN_MAX;
    // all monotone; sparkle non-increasing in age. depthFull is guarded by max(depthFull, 1e-6) on BOTH sides of the LOCKSTEP pair
    // (this core and ssSurfaceStateF.glsl) - a look with depthFull 0 must read as fully covered at any positive depth, never NaN.
    inline F32 depositCoverage(F32 depth, F32 depthFull)
    {
        return smoothstep(0.f, llmax(depthFull, 1.0e-6f), depth);
    }
    inline F32 depositSoften(F32 depth, F32 depthFull)
    {
        const F32 full = llmax(depthFull, 1.0e-6f);
        return DEPOSIT_SOFTEN_MAX * llclamp(depth / (full * DEPOSIT_SOFTEN_DEPTHS), 0.f, 1.f);
    }
    inline F32 depositSparkle(F32 base, F32 age)
    {
        return base * (1.f - SPARKLE_AGE_LOSS * age);
    }

    // ------------------------------------------------------------------ impact rings (near puddles)

    // One recorded impact: agent-space xy, the surface z, the shared time it landed, its strength 0..1.
    struct Ring
    {
        F32 mX = 0.f, mY = 0.f, mZ = 0.f;
        F32 mBirth = 0.f;
        F32 mStrength = 0.f;
    };

    // The rate the ring's clock runs at, from the landing ripple quad the ring stands in for, so both are driven by the same
    // controls: the quad grows from 0.15 to 1 of quad_radius_m over quad_life_s, so its front crosses RING_QUAD_GROWTH of that
    // radius in that time, and the ring's front covers RING_SPEED_MPS per unit of its own clock - matching the two is a division.
    // The ring's larger extent is taken in TIME, not in speed: it reaches RING_REACH_M rather than a quad's radius, so the same
    // wave at the same metres per second simply takes longer to cross a puddle than to cross a splash. Invariants: 1 when the quad
    // travels exactly RING_SPEED_MPS; monotone in quad_radius_m, falling in quad_life_s; inside [RING_RATE_MIN, RING_RATE_MAX];
    // 1 for a degenerate quad (zero life or radius), which is the "no quad to match" case.
    inline F32 ringRate(F32 quad_radius_m, F32 quad_life_s)
    {
        if (quad_life_s <= 1.0e-4f || quad_radius_m <= 0.f) return 1.f;
        return llclamp(RING_QUAD_GROWTH * quad_radius_m / (quad_life_s * RING_SPEED_MPS), RING_RATE_MIN, RING_RATE_MAX);
    }

    // How much amplitude a crest still carries once it has spread to radius r - the packet's energy is smeared around a circumference
    // that grows with r, so the height goes as 1/sqrt(r) once the ring is bigger than the splash it started as. Invariants: 1 at or
    // inside RING_SPREAD_R0_M, never above 1, monotone falling.
    inline F32 ringSpread(F32 r)
    {
        return std::sqrt(RING_SPREAD_R0_M / llmax(r, RING_SPREAD_R0_M));
    }

    // The terminal taper: the ring is already thin by RING_FADE_FROM of its life and is eased to exactly zero by the end of it, so
    // the buffer can drop it without anything visibly popping out of existence. Invariants: 1 up to RING_FADE_FROM * RING_LIFE_S,
    // 0 at RING_LIFE_S and beyond, monotone falling, and C1 at both ends (it is a smoothstep).
    inline F32 ringTaper(F32 t)
    {
        return 1.f - smoothstep(RING_FADE_FROM * RING_LIFE_S, RING_LIFE_S, t);
    }

    // The ring's height at radius r (metres) and age t (the ring's own clock), twin of the GLSL: front = RING_SPEED_MPS * t; h =
    // RING_AMPLITUDE * strength * exp(-t / RING_DAMP_S) * ringSpread(r) * ringTaper(t) * exp(-((r - front) / RING_WIDTH_M)^2) *
    // sin(2 pi (r - front) / RING_WAVELENGTH_M); 0 for t < 0 or t > RING_LIFE_S. Invariants: 0 outside [0, RING_LIFE_S]; |h| <=
    // RING_AMPLITUDE * strength (spread and taper are both <= 1); decays with t at fixed r - front; reaches 0 continuously at
    // RING_LIFE_S rather than being cut off there; the packet's centre moves outward at RING_SPEED_MPS (argmax over r of
    // |envelope| at t1 > t0 is farther out).
    inline F32 ringHeight(F32 r, F32 t, F32 strength)
    {
        if (t < 0.f || t > RING_LIFE_S) return 0.f;
        constexpr F32 kTwoPi = 6.283185307179586f;   // <SS:Nexii> local; no PI constant in the allowed includes
        const F32 front = RING_SPEED_MPS * t;
        const F32 env = (r - front) / RING_WIDTH_M;
        return RING_AMPLITUDE * strength * std::exp(-t / RING_DAMP_S) * ringSpread(r) * ringTaper(t) * std::exp(-env * env)
            * std::sin(kTwoPi * (r - front) / RING_WAVELENGTH_M);
    }

    // The ring's slope (dh/dr) by central difference with step RING_WAVELENGTH_M / 16 - what the normal pass tilts the normal by,
    // clamped to [-RING_SLOPE_MAX, RING_SLOPE_MAX] so 24 superposed rings cannot shatter a puddle's normal - the pass still clamps
    // the accumulated tilt. Invariants: 0 wherever ringHeight is identically 0 around r; antisymmetric structure is NOT required
    // (the packet is one-sided); |ringSlope| <= RING_SLOPE_MAX always.
    inline F32 ringSlope(F32 r, F32 t, F32 strength)
    {
        const F32 h = RING_WAVELENGTH_M / 16.f;
        const F32 slope = (ringHeight(r + h, t, strength) - ringHeight(r - h, t, strength)) / (2.f * h);
        return llclamp(slope, -RING_SLOPE_MAX, RING_SLOPE_MAX);
    }

    // A fixed-capacity ring buffer of the newest RING_MAX impacts: push overwrites the oldest; expire drops rings older than
    // RING_LIFE_S / rate at time now, compacting the survivors into push (oldest-first) order at [0, survivors) - order matters
    // here, because mNext and every later push() assume index 0 is the oldest survivor. Invariants: count never exceeds RING_MAX;
    // after expire(now, rate) every remaining ring has now - mBirth <= RING_LIFE_S / rate; push of RING_MAX + 1 rings leaves the LAST RING_MAX
    // pushed; a push right after an expire() that dropped nothing still evicts the TRUE oldest ring, not whatever happened to sit
    // at physical index 0 before the wrap.
    struct RingBuffer
    {
        Ring mRings[RING_MAX];
        S32 mCount = 0;
        S32 mNext = 0;   // slot the next push writes

        void push(const Ring& r)
        {
            mRings[mNext] = r;
            mNext = (mNext + 1) % RING_MAX;
            if (mCount < RING_MAX) ++mCount;
        }
        void expire(F32 now, F32 rate = 1.f)
        {
            // <SS:Nexii> births are wall-clock, the life is on the ring's own clock (see ringRate), so a ring is over after RING_LIFE_S / rate SECONDS - a rate of 1 is the old behaviour and is what a caller with no quad to match passes.
            const F32 life = RING_LIFE_S / llmax(rate, RING_RATE_MIN);
            // <SS:Nexii> valid entries occupy [0, mCount) physically, but once the buffer has wrapped (mCount == RING_MAX) that range is NOT in push order - the logical oldest sits at mNext, not at index 0 - so compaction must walk from the logical oldest (start = mNext when full, else 0) and wrap with modulo; the survivors then land at [0, survivors) in push (oldest-first) order, which is what mNext = mCount % RING_MAX and every later push() assume.
            const S32 start = (mCount == RING_MAX) ? mNext : 0;
            S32 w = 0;
            Ring kept[RING_MAX];
            for (S32 k = 0; k < mCount; ++k)
            {
                const S32 i = (start + k) % RING_MAX;
                if (now - mRings[i].mBirth <= life)
                {
                    kept[w++] = mRings[i];
                }
            }
            for (S32 i = 0; i < w; ++i)
            {
                mRings[i] = kept[i];
            }
            mCount = w;
            mNext = mCount % RING_MAX;
        }
    };
}

#endif
