/**
 * @file ssatmoenvasset.h
 * @brief Atmo Magic: the unified environment asset schema.
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

#ifndef SS_ATMOENVASSET_H
#define SS_ATMOENVASSET_H

#include <cmath>

#include "llsd.h"
#include "lluuid.h"

#include <cfloat>
#include <map>
#include <string>
#include <vector>

#include "ssatmoenvkeyframe.h"

class LLSettingsSky;
class LLSettingsWater;

const S32 SS_ATMOENV_VERSION = 1;

const S32 SS_ATMOENV_MIN_TRACKS = 1;
const S32 SS_ATMOENV_MAX_TRACKS = 8;

const F32 SS_ATMOENV_REGION_CEILING = 4096.f;

const F32 SS_ATMOENV_MIN_TRACK_FLOOR = 256.f;

// <SS:Nexii> The water height window the slider dials: a near-surface band that reaches below sea level for sunken seas and up over lifted lakes. Values past the slider's ends show there pinned at the rail.
const F32 SS_ATMOENV_WATER_FLOOR = -50.f;
const F32 SS_ATMOENV_WATER_CEILING = 200.f;

// <SS:Nexii> The water height the spinner will take by hand: sky-themed builds put the ocean way below the platform, so the typed range runs -10 km to +10 km, far past the slider's dial. Values past the slider's ends show there pinned at the rail.
const F32 SS_ATMOENV_WATER_MIN = -10000.f;
const F32 SS_ATMOENV_WATER_MAX = 10000.f;

// <SS:Nexii> The under deck's base height, in the same floor-relative frame: the slider spans a hang-below-the-track window (-400 m) up to the region ceiling, while the spinner takes the same wide hand-typed range the water plane does.
const F32 SS_ATMOENV_UDECK_BASE_FLOOR = -400.f;
const F32 SS_ATMOENV_UDECK_BASE_MIN = -10000.f;
const F32 SS_ATMOENV_UDECK_BASE_MAX = 10000.f;

const S32 SS_ATMOENV_PREVIEW_STEPS = 100;

// <SS:Nexii> EEP's reference disc: the angular diameter a stock sky's disc scale of 1.0 states - the real Sun's apparent size, which is what scale 1.0 was always MEANT to draw. It is the convention a sky's disc scale is read under when an import becomes a body (see SSAtmoEnvPlanetary::translateSettingsSky); what the quad GEOMETRY actually draws at scale 1.0 is the much larger ss_atmoenv_quad_deg below, which is exactly the bug the quad angles exist to fix - the two numbers used to be one, and every body drew ~10x its authored size.
const F32 SS_ATMOENV_REFERENCE_DISC_DEG = 0.53f;

// <SS:Nexii> The angular diameter the WHOLE celestial quad subtends at disc scale 1.0, per slot kind. updateHeavenlyBodyGeometry sizes a quad's half-extent at HEAVENLY_BODY_DIST * HEAVENLY_BODY_FACTOR * disk_radius (llvosky.h: factor 0.1, the sun's disk radius 0.5, the moon's 0.45), so the shell distance cancels out of the angle and scale 1.0 is simply 2*atan(0.1 * r) - 5.72 degrees of sun, 5.15 of moon. EEP never converted its own chain to degrees, which is how the 0.53 reference above ended up describing a quad ten times smaller than the one it draws. Disc art fills the quad (ssCelestialF inscribes the phase-shaded sphere in it), so the drawn body IS the quad: the conversion from an authored angular diameter runs through these, with the 0.53 convention left to the import translation alone.
inline F32 ss_atmoenv_quad_deg(F32 disk_radius)
{
    return 2.f * RAD_TO_DEG * atanf(0.1f * disk_radius);
}
const F32 SS_ATMOENV_SUN_QUAD_DEG  = ss_atmoenv_quad_deg(0.5f);
const F32 SS_ATMOENV_MOON_QUAD_DEG = ss_atmoenv_quad_deg(0.45f);

inline F64 ss_atmoenv_snap_phase(F64 phase)
{
    const F64 steps = (F64)SS_ATMOENV_PREVIEW_STEPS;
    F64 snapped = ll_round(phase * steps) / steps;
    snapped = std::fmod(snapped, 1.0);
    if (snapped < 0.0) snapped += 1.0;
    return snapped;
}

struct SSAtmoEnvWeather
{
    SSAtmoEnvKeyframed<F32> mMoisture{0.f};
    SSAtmoEnvKeyframed<F32> mConvection{0.f};
    SSAtmoEnvKeyframed<F32> mTemperatureC{15.f};

    SSAtmoEnvKeyframed<F32> mWindHeading{0.f};
    SSAtmoEnvKeyframed<F32> mWindSpeed{0.f};

    bool mGustAuto = true;
    SSAtmoEnvKeyframed<F32> mGustDepth{0.f};
    SSAtmoEnvKeyframed<F32> mGustLength{140.f};
    SSAtmoEnvKeyframed<F32> mGustVeer{0.f};

    bool mLightningEnabled = true;
    bool mLightningCharge = true;
    bool mLightningSparks = true;

    bool mLightningAuto = true;
    SSAtmoEnvKeyframed<F32> mLightningIntensity{0.f};

    SSAtmoEnvKeyframed<LLColor3> mLightningColor{LLColor3(0.62f, 0.55f, 1.f)};

    SSAtmoEnvKeyframed<F32> mLightningCoreWhite{0.85f};

    SSAtmoEnvKeyframed<std::string> mPrecipitationOverride{std::string()};

    // <SS:Nexii> Whether anything falls at all. Moisture used to be the sole switch - the sky wet enough to be overcast was the sky that rained - so an overcast, stormy, dry sky was unauthorable. Keyframed, because when it starts and stops is the whole point: flag keyframes HOLD (ss_atmoenv_default_curve<bool>), so the rain starts at the key that turns it on and stops at the key that turns it off - where the author put the marks, not between them. Off suppresses only the precipitation half of the resolve - cloud cover, gloom, wind and lightning stay exactly as authored. [interaction: precipitation]
    SSAtmoEnvKeyframed<bool> mPrecipitationFalls{true};

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvWater
{
    bool mEnabled = false;

    // <SS:Nexii> Metres relative to the owning track's floor (SSAtmoEnvTrack::mFloorZ), so a whole stack - deck, water, ocean floor - rides with the track's vertical position instead of being pinned to world zero: dial a sky build's ocean to -2000 and it stays 2 km below that build wherever the track itself sits. Resolved to world Z wherever it renders.
    SSAtmoEnvKeyframed<F32> mHeight{0.f};

    SSAtmoEnvKeyframed<LLColor3> mFogColor{LLColor3(0.f, 0.24f, 0.34f)};
    SSAtmoEnvKeyframed<F32> mFogDensity{16.f};
    SSAtmoEnvKeyframed<F32> mUnderwaterModifier{0.25f};

    // <SS:Nexii> Whether the fog colour is EMISSIVE - applied exactly as authored, ignoring whatever light the sky has, the fullbright look - or LIT: scaled by the light the applied sky actually has, so a fog colour authored for day darkens to black on a moonless night instead of glowing. Unset and false are the same state - lit, the default; the key only exists in a document once an author has turned the flag on (or keyframed it), so every asset written before the flag existed reads as lit. Animatable like any field here: a glowing sea can switch itself off for the day.
    SSAtmoEnvKeyframed<bool> mFogEmissive{false};

    SSAtmoEnvKeyframed<F32> mFresnelScale{0.4f};
    SSAtmoEnvKeyframed<F32> mFresnelOffset{0.5f};

    SSAtmoEnvKeyframed<LLUUID> mNormalMap{LLUUID::null};
    SSAtmoEnvKeyframed<LLVector2> mLargeWaveSpeed{LLVector2(0.f, -0.2f)};
    SSAtmoEnvKeyframed<LLVector2> mSmallWaveSpeed{LLVector2(0.f, -0.3f)};

    SSAtmoEnvKeyframed<F32> mNormalScaleX{2.f};
    SSAtmoEnvKeyframed<F32> mNormalScaleY{2.f};
    SSAtmoEnvKeyframed<F32> mNormalScaleZ{2.f};

    SSAtmoEnvKeyframed<F32> mRefractionScaleAbove{0.03f};
    SSAtmoEnvKeyframed<F32> mRefractionScaleBelow{0.2f};
    SSAtmoEnvKeyframed<F32> mBlurMultiplier{0.04f};

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);

    // <SS:Nexii> Adopts an EEP water preset wholesale onto this block (the reverse of the applier's live v3->EEP mapping): colour and density, fog modifiers, fresnel, the normal map and its scale, the refraction scales and blur, and both wave directions. Only the block's own fields are written - height and emissive stay as authored, and the caller decides whether to enable the plane.
    void fromSettingsWater(const LLSettingsWater& settings);
};

const S32 SS_ATMOENV_MAX_SUNS = 4;

struct SSAtmoEnvCelestialBody
{
    enum EKind { SUN = 0, PLANET = 1, MOON = 2 };

    EKind mKind = PLANET;
    std::string mName = "Body";

    bool mNameCustom = false;

    S32 mParentIndex = -1;

    F32 mDiameterM = 1.0e7f;
    F32 mMassRelative = 1.f;

    F32 mOrbitalRadius = 1.0e8f;
    F32 mOrbitalInclinationDeg = 0.f;
    F32 mOrbitalPhaseDeg = 0.f;

    F64 mOrbitalPeriodSeconds = 0.0;
    F64 mRotationPeriodSeconds = 0.0;

    F32 mAxialTiltDeg = 0.f;

    F32 mLatitudeDeg = 50.f;

    bool mEmissive = false;

    bool mPhaseShaded = true;

    F64 mSpinPeriodSeconds = 0.0;

    bool mIsHome = false;

    bool mIsLightEmitter = false;

    S32 mBoundPartnerIndex = -1;

    LLUUID mCustomTexture;

    // <SS:Nexii> The transparent margin the disc art carries, as a fraction of the texture's width on EACH side - the stock sun art is glow past a small core, lunar art is full-bleed. The disc the renderer treats as the body is the central 1 - 2*padding of the quad, so the quad overdrawing the authored angular diameter by exactly that factor is what makes a padded texture land its art on the authored size. Zero - full-bleed art, the whole texture is the body - and capped at 0.45 so a disc never shrinks below a tenth of its quad.
    F32 mDiscPadding = 0.f;

    // <SS:Nexii> Runtime-only, never serialized: the diameter this body carries was written by a sky translation (translateSettingsSky) at the EEP QUAD size - the glow-inclusive size a pre-padding sky authored - and the first disc-padding derive must shrink it to the solid visible disc exactly once, then clear the flag. The author's own later edits (spinner, another texture pick) travel with no flag and never rescale behind their back.
    bool mPadPendingTranslation = false;

    bool mHasRing = false;
    F32 mRingInnerRadius = 1.5f;
    F32 mRingOuterRadius = 2.2f;
    LLUUID mRingTexture;

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvPlanetary
{
    std::vector<SSAtmoEnvCelestialBody> mBodies;

    // <SS:Nexii> The two distance dials ARE the perception control: compressing the Sun-Planet and Planet-Moon distances makes the sun and moons loom in the sky without touching any authored size. The Space tab's Disc Perception radios (1x physical truth, 3x what a person feels they saw, 8x game-cinematic) are a preset front-end that writes BOTH dials to 1/N; moving either dial by hand is the custom path and just deselects the radios. The seeding default is the perceptual one - 3x - so a fresh system looms out of the box; 1.0 on both is the authored-distance truth the 1x radio restores.
    F32 mSunPlanetScale = 1.f / 3.f;
    F32 mPlanetMoonScale = 1.f / 3.f;

    S32 homeBodyIndex() const;
    bool setHomeBody(S32 index);

    std::vector<S32> lightEmitterIndices() const;
    bool canSetLightEmitter(S32 index) const;

    S32 addBody(SSAtmoEnvCelestialBody::EKind kind, S32 preferred_parent_index = -1);

    bool removeBody(S32 index);

    S32 effectiveParent(S32 index) const;

    void normalizeSunTopology();

    void autoNameBodies();

    // <SS:Nexii> The stock sun/moon of the standard setup makeDefault() seeds - the EEP-shaped world an author starts from. A body still counts as one while its LOOK is untouched (size, mass, glow character, disc texture, no rings, the kind's place in the topology); where the author has redesigned it, a dropped sky must not quietly overwrite that design. Index into mBodies, or -1 when there is none: an empty or fully custom system offers no body groups.
    S32 standardSunIndex() const;
    S32 standardMoonIndex() const;

    // Translates a fetched EEP sky's disc values onto the standard sun/moon this system still
    // carries. The sky's disc scale is read against EEP's reference disc (SS_ATMOENV_REFERENCE_
    // DISC_DEG - the convention scale 1.0 was authored under, the real Sun's apparent size) and
    // becomes a physical diameter at the body's resolved home-to-body distance; a disc texture
    // the sky does not name at its own stock value replaces the body's. The body then renders
    // through the physical chain (ss_atmoenv_quad_deg), so what arrives from a stock sky is the
    // body its scale STATES - the real Sun for 1.0 - drawn at that body's true apparent size
    // rather than at EEP's oversized glow quad. Only the groups named in `groups` take part; a
    // group with no standard body left silently does nothing.
    void translateSettingsSky(const LLSettingsSky& sky, U32 groups);

    bool setBoundPartner(S32 a, S32 b);
    bool clearBoundPartner(S32 index);

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvCloudField
{
    SSAtmoEnvCloudField();

    // <SS:Nexii> The secondary field a sky-themed build hangs below its platform: disabled by default (no undercloud unless asked for), authored rather than auto-derived, and seeded to a low flat deck the author dials down to wherever the build's floor wants its cloud base.
    static SSAtmoEnvCloudField under();

    // Whether this field renders at all. The primary deck is always on - its empty state is
    // coverage-driven - so only the under deck's flag is ever false.
    bool mEnabled = true;

    bool mAuto = true;

    // <SS:Nexii> Metres above the owning track's floor (SSAtmoEnvTrack::mFloorZ), negative for a deck hung below it - the same floor-relative frame the water plane and the altitude rail's baseline tick live in, so a whole sky build rides its track's vertical position. The resolver adds the floor back for rendering (SSAtmoEnvCloudFieldResolver::resolve).
    SSAtmoEnvKeyframed<F32> mBaseHeightM{800.f};
    SSAtmoEnvKeyframed<F32> mBaseThicknessM{300.f};

    SSAtmoEnvKeyframed<F32> mCoverageScale{1.f};

    SSAtmoEnvKeyframed<LLUUID> mBaseTexture;
    SSAtmoEnvKeyframed<LLUUID> mDetailTexture;

    // <SS:Nexii> The convection noise map. A field-scale greyscale map - the same kind of tileable noise the dome's cloud image is - that gives the deck's response to convection a geography. Sampled per cell in the air frame and run through a ramp (see ssvolcloud's tower and hole windows): where the map runs high the deck rises into cumulonimbus towers that carry the anvil's spread before the convection dial alone would allow it, where it runs low the deck thins into the pockets and gaps between them - down to holes in the sky when the air is dry and stable. Moisture lifts the map's floor back over the holes, so the same map that breaks a dry stable deck turns a moist one into an unbroken overcast nimbostratus sheet. The Noise Scale slider scales how many metres one tile of the map spans. None leaves the deck to the plain cluster noise it has always used.
    SSAtmoEnvKeyframed<LLUUID> mNoiseTexture;

    // <SS:Nexii> The vertical profile ramp: a thin strip texture whose four channels are four curves over the deck's height (v = 0 at the cloud base, 1 at the lid, painted bottom to top), expressing HOW the convection noise map applies vertically. Red is the tower/ramp weight - how much the map counts at this height, ramping toward white near the lid so the top consolidates into the anvil. Green is the carve guard - where the anvil's underside may bite (keep the base band black so the deck keeps its body). Blue is the torn cap band. Alpha is the thick-base fill - a density floor that makes the bottom read solid. None runs the built-in curves the deck has always used.
    SSAtmoEnvKeyframed<LLUUID> mProfileTexture;

    SSAtmoEnvKeyframed<F32> mTextureMix{0.4f};

    SSAtmoEnvKeyframed<F32> mPuffDensity{0.8f};

    SSAtmoEnvKeyframed<F32> mDetailScale{3.f};

    // <SS:Nexii> Metres per tile of the convection noise map, as a multiplier on the viewer's field-scale tile. 1 is the tuned default; smaller packs the map's pockets and towers tighter together, larger spreads them into continent-scale weather.
    SSAtmoEnvKeyframed<F32> mNoiseScale{1.f};

    SSAtmoEnvKeyframed<F32> mDriftRate{1.f};

    SSAtmoEnvKeyframed<F32> mStormDarkening{0.85f};

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// <SS:Nexii> Which clusters of a dropped EEP sky's values an import stamps. A sky arrives as one asset, but its fields read as separate looks - the haze is not the cloud layer is not the light - and an author dropping a sky onto a track they have already tuned usually wants some of it, not all of it. SSFloaterAtmoSkyImport asks which groups before anything is stamped. The four field groups stamp as keyframes (see SSAtmoEnvAtmosphere::addKeyframesFromSky and SSAtmoEnvCloudDome::addKeyframesFromSky). The two body groups are different in kind: a sky states what its sun and moon DISCS look like (angular scale against SS_ATMOENV_REFERENCE_DISC_DEG, plus disc texture), while an Atmo Magic body states a physical diameter and texture - so those groups are offered only when the target track still carries the STANDARD sun/moon bodies makeDefault() seeds (see SSAtmoEnvPlanetary::standardSunIndex), and the stamp is a translation (see SSAtmoEnvPlanetary::translateSettingsSky) rather than a keyframe write.
const U32 SS_SKY_IMPORT_ATMOSPHERE = 0x1; // sky gradient, haze, moisture, multipliers, sky ceiling
const U32 SS_SKY_IMPORT_LIGHTING   = 0x2; // ambient/sunlight colour, gamma, probe ambiance, sun glow
const U32 SS_SKY_IMPORT_CELESTIAL  = 0x4; // star and moon brightness
const U32 SS_SKY_IMPORT_CLOUDS     = 0x8; // the legacy dome layer, whole block
const U32 SS_SKY_IMPORT_SUN        = 0x10; // the standard sun body's size and texture
const U32 SS_SKY_IMPORT_MOON       = 0x20; // the standard moon body's size and texture

const U32 SS_SKY_IMPORT_ALL = SS_SKY_IMPORT_ATMOSPHERE | SS_SKY_IMPORT_LIGHTING
                            | SS_SKY_IMPORT_CELESTIAL | SS_SKY_IMPORT_CLOUDS
                            | SS_SKY_IMPORT_SUN | SS_SKY_IMPORT_MOON;

struct SSAtmoEnvCloudDome
{
    // <SS:Nexii> The dome layer's own ALTITUDE, metres - what a metre of camera travel is worth to the parallax, and the one authority the disc occlusion shares (doc/atmo_magic_cloud_parallax.md). Authored here rather than borrowed from max altitude, which is an atmosphere ceiling dialled for haze and has no business setting where a cloud sits. mAuto hands the number back to the volumetric field's derivation - cirrus-high while the field is empty, merging down onto the deck's mid-height as coverage builds - for anyone who wants dome and deck to agree at the rim without dialling it themselves.
    // <SS:Nexii> ON by default: the dome band and the volumetric deck are two halves of one sky, and the number that keeps them agreeing at the rim is not one an author should have to find. Auto tracks the cirrus level and brings the band down onto the deck's lid as the deck anvils (SSAtmoEnvApplier::cirrusAltitudeMetres) - a new environment gets a band that follows its deck from the first frame, and anyone who wants the band pinned still has one switch to throw.
    bool mAuto = true;
    SSAtmoEnvKeyframed<F32> mHeightM{6000.f};

    SSAtmoEnvKeyframed<LLColor3> mColor{LLColor3(0.4099f, 0.4099f, 0.4099f)};

    SSAtmoEnvKeyframed<F32> mCoverage{0.2699f};
    SSAtmoEnvKeyframed<F32> mScale{0.4199f};
    SSAtmoEnvKeyframed<F32> mVariance{0.f};

    // <SS:Nexii> No authored Scroll Rate: the dome band (cirrus) moves with the WIND, scaled to its own altitude by the boundary-layer wind gradient (SSWindFlowMap::windGradientScale), so there is no dial to keyframe. See the applier.

    SSAtmoEnvKeyframed<F32> mDensityX{1.f};
    SSAtmoEnvKeyframed<F32> mDensityY{0.526f};
    SSAtmoEnvKeyframed<F32> mDensityD{1.f};
    SSAtmoEnvKeyframed<F32> mDetailX{1.f};
    SSAtmoEnvKeyframed<F32> mDetailY{0.526f};
    SSAtmoEnvKeyframed<F32> mDetailD{1.f};

    SSAtmoEnvKeyframed<LLUUID> mNoiseTexture{LLUUID::null};

    // <SS:Nexii> The broad octave's OWN map, for when the cloud noise's blob scale is wrong for the dome band's now huge tile (8 km): a purpose-made large-scale map draws the broad composition - the warps, the base octave and its self-shadow read it - while the fine octave keeps the cloud noise. Null means not in use: every octave reads the cloud noise, exactly as before this existed.
    SSAtmoEnvKeyframed<LLUUID> mLargeNoiseTexture{LLUUID::null};

    static const char* const CLOUD_TEXTURE_LAYERED;
    static const char* const CLOUD_TEXTURE_CUMULONIMBUS;
    static const char* const CLOUD_TEXTURE_ALTOCUMULUS;

    static const char* const BODY_TEXTURE_SUN;
    static const char* const BODY_TEXTURE_MOON;

    void fromSettingsSky(const LLSettingsSky& sky);

    void addKeyframesFromSky(const LLSettingsSky& sky, F64 phase, U32 groups = SS_SKY_IMPORT_ALL);

    void collapseConstantKeyframes();

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvAtmosphere
{
    SSAtmoEnvKeyframed<LLColor3> mAmbientColor{LLColor3(0.25f, 0.25f, 0.25f)};
    SSAtmoEnvKeyframed<LLColor3> mBlueHorizon{LLColor3(0.4954f, 0.4954f, 0.6399f)};
    SSAtmoEnvKeyframed<LLColor3> mBlueDensity{LLColor3(0.2447f, 0.4487f, 0.7599f)};
    SSAtmoEnvKeyframed<LLColor3> mSunlightColor{LLColor3(0.7342f, 0.7815f, 0.8999f)};

    SSAtmoEnvKeyframed<F32> mHazeHorizon{0.19f};
    SSAtmoEnvKeyframed<F32> mHazeDensity{0.7f};

    SSAtmoEnvKeyframed<F32> mSkyMoistureLevel{0.f};
    SSAtmoEnvKeyframed<F32> mSkyDropletRadius{800.f};
    SSAtmoEnvKeyframed<F32> mSkyIceLevel{0.f};

    SSAtmoEnvKeyframed<F32> mDensityMultiplier{0.0001f};
    SSAtmoEnvKeyframed<F32> mDistanceMultiplier{0.8f};
    SSAtmoEnvKeyframed<F32> mMaxAltitude{1605.f};

    SSAtmoEnvKeyframed<F32> mReflectionProbeAmbiance{0.f};
    SSAtmoEnvKeyframed<F32> mSceneGamma{1.f};

    SSAtmoEnvKeyframed<F32> mStarBrightness{250.f};
    SSAtmoEnvKeyframed<F32> mGlowFocus{0.096f};
    SSAtmoEnvKeyframed<F32> mGlowSize{1.75f};

    SSAtmoEnvKeyframed<F32> mMoonBrightness{0.5f};

    // <SS:Nexii> Atmo Magic: the horizon clip. When on, the sky dome is split at the horizon plane and its lower half takes a depth slot one step nearer than the cloud layer (the LL_SHADER_CONST_HORIZON_DEPTH shader const against the clouds' 0.99998 and the celestial discs' 0.99999 - see skyF.glsl), so the sun, moon, stars, planetary bodies and the dome's own cloud layer are hidden by the dome the moment they set instead of glowing through the world below it. A look, not a dial: there is no meaningful "partially clipped", and nothing here follows the day, so this is a plain authored flag rather than a keyframed value - the same call mGustAuto makes.
    bool mHorizonClip = true;

    void fromSettingsSky(const LLSettingsSky& sky);

    void addKeyframesFromSky(const LLSettingsSky& sky, F64 phase, U32 groups = SS_SKY_IMPORT_ALL);

    void collapseConstantKeyframes();

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

struct SSAtmoEnvWeatherInfluence
{
    bool mEnabled = true;

    bool mCloudCoverEnabled = true;
    F32  mCloudCoverStrength = 1.f;

    bool mWindScrollEnabled = true;
    F32  mWindScrollStrength = 1.f;

    // <SS:Nexii> Gates precipitation -> water fog. Was the haze pair: it used to gate moisture -> haze density / distance multiplier as well, and that mapping is retired - the haze lift blew whole scenes out on custom skies (see SSAtmoEnvSkyWeatherModulator::compute), and the row it owned in the Weather Influence floater is now the water-fog row alone.
    bool mWaterFogEnabled = true;
    F32  mWaterFogStrength = 1.f;

    bool mStormDarkeningEnabled = true;
    F32  mStormDarkeningStrength = 1.f;

    bool mColdSkyEnabled = true;
    F32  mColdSkyStrength = 1.f;

    bool mRainbowEnabled = true;
    F32  mRainbowStrength = 1.f;

    // <SS:Nexii> Water-drop optics: the corona's diffraction rings around the light. Liquid only - it needs mist (moisture), droplets lingering in rain's wake, or light drizzle, and freezes out below about -4C. Heavy precipitation hides the disc itself and suppresses it.
    bool mCoronaEnabled = true;
    F32  mCoronaStrength = 1.f;

    // <SS:Nexii> Ice-crystal optics: the 22° halo family from small cirrus platelets, the 46° halo family (large plates/columns), sundogs and the circumzenithal + supralateral arcs from still air that lets plates align. Every drive gates on MOISTURE as well as cold - crystals need something to freeze - so a record-cold dry sky renders exactly as authored with no phantom ice, and the phenomena separate by depth of cold, loft and wind as the weather moves.
    bool mIceHaloEnabled = true;
    F32  mIceHaloStrength = 1.f;

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// Values for SSAtmoEnvTrack::mWeatherSourceDeck.
const S32 SS_ATMOENV_DECK_DERIVED = -1;
const S32 SS_ATMOENV_DECK_MAIN    = 0;
const S32 SS_ATMOENV_DECK_UNDER   = 1;

struct SSAtmoEnvTrack
{
    std::string mName = "Ground";

    // <SS:Nexii> The track's vertical position in the world - the floor of the altitude band it owns, and the reference surface everything placed inside it is authored against: the water plane's height and both decks' base heights are metres relative to this, and the resolver adds it back wherever they render. The ground track sits at 0, where relative and world coincide.
    F32 mFloorZ = 0.f;

    F32 mTransitionBuffer = 15.f;

    F64 mDayLengthSeconds = 4.0 * 60.0 * 60.0;
    F64 mDayOffsetSeconds = 0.0;

    SSAtmoEnvWater    mWater;
    SSAtmoEnvWeather  mWeather;
    SSAtmoEnvPlanetary mPlanetary;

    SSAtmoEnvWeatherInfluence mWeatherInfluence;

    SSAtmoEnvAtmosphere mAtmosphere;
    SSAtmoEnvCloudField mCloudField;
    SSAtmoEnvCloudDome  mCloudDome;

    // <SS:Nexii> The optional under deck: a second volumetric field at the bottom of a sky-themed build - cloud layer below the platform, ocean way below that - whose base height is authored against the track floor, typically dialled negative to hang it below the build. Seeded off, manual, and low: see SSAtmoEnvCloudField::under().
    SSAtmoEnvCloudField mUnderField = SSAtmoEnvCloudField::under();

    // <SS:Nexii> Which deck precipitation falls from. Derived by default - the lowest enabled deck above the reference surface, which resolves to the main deck for a sky build because the under deck hangs below the platform floor. Authored only for the case of wanting weather from the upper deck while a lower one is enabled for looks. Not keyframed: it is a property of the track, not of a moment. See doc/atmo_magic_env_ui.md.
    S32 mWeatherSourceDeck = SS_ATMOENV_DECK_DERIVED;

    F64 currentDayCyclePhase() const;

    LLSD asLLSD() const;
    bool fromLLSD(const LLSD& sd);
};

// <SS:Nexii> World templates. An author arrives with a theme - a coastline, a sky archipelago, a permanent barrage - and a theme is never one setting: a sky world is water kilometres down AND a deck under the landmass AND another above it. Seeding those together is what makes the first screen useful to someone who came to build a place rather than to tune haze. The height seeds (water, both decks, the dome) carry the same frames the fields do: water and deck heights are offsets from the track's floor, so a template dialed for a sky build keeps its shape - ocean below, decks above and below the platform - wherever that track sits. Seeding is one-shot: the template is copied into the track and forgotten, with no live link back. A link would mean drift and reconciliation for no gain, since the whole point is that the author dials the result afterwards. Numbers here are starting points, not authored presets - tune them in place. See doc/atmo_magic_env_ui.md.
struct SSAtmoEnvTemplate
{
    const char* mKey;
    const char* mLabel;

    F64 mDayLengthHours;

    bool mWaterEnabled;
    F32  mWaterHeightM;
    LLColor3 mWaterFogColor;
    F32  mWaterFogDensity;

    F32  mDeckBaseM;
    F32  mDeckThicknessM;
    F32  mDeckCoverage;
    F32  mDeckStormDarkening;

    bool mUnderEnabled;
    F32  mUnderBaseM;
    F32  mUnderThicknessM;

    bool mDomeAuto;
    F32  mDomeHeightM;
    F32  mDomeCoverage;

    F32  mTemperatureC;
    F32  mMoisture;
    F32  mConvection;
    F32  mWindSpeed;

    LLColor3 mBlueHorizon;
    LLColor3 mBlueDensity;
    F32  mHazeDensity;
    F32  mMaxAltitudeM;
};

const std::vector<SSAtmoEnvTemplate>& ssAtmoEnvTemplates();

// Looks a template up by key; null when the key is unknown.
const SSAtmoEnvTemplate* ssAtmoEnvFindTemplate(const std::string& key);

// Everything the template names that is NOT the sky's look: day length, water, both decks, the
// dome's structure (auto, height, coverage) and the weather. The atmosphere columns are the
// template's mood and travel separately - a constant sky here (ssAtmoEnvApplyTemplate), or a
// tint over the seeded stock day cycle there (SSAtmoEnvManager::applyTemplateToTrack).
void ssAtmoEnvApplyTemplateWorld(SSAtmoEnvTrack& track, const SSAtmoEnvTemplate& tmpl);

// Returns false only for an unknown key. Everything the template names is overwritten on the track,
// keyframes included - a seed is destructive by design, which is why the UI confirms first.
bool ssAtmoEnvApplyTemplate(SSAtmoEnvTrack& track, const std::string& key);

struct SSAtmoEnvAsset
{
    std::string mName = "New Atmo Environment";

    std::vector<SSAtmoEnvTrack> mTracks;

    // <SS:Nexii> The environment's own precipitation types, name to serialised SSPrecipPreset. Two tiers exist: the ones shipped with the viewer, which we author, and these, which the region's author derives from them. Held as LLSD rather than SSPrecipPreset so the asset schema does not have to depend on the particle system's headers, and so a type authored by a newer build survives a round trip through an older one instead of being silently dropped. Derived types carry a full copy of their parent, never a reference: a viewer update that retunes stock rain must not silently change a shipped region. Referenced shipped types are copied in here on save for the same reason, which is what makes an environment self-contained and an unresolvable type reference impossible. See doc/atmo_magic_env_ui.md.
    std::map<std::string, LLSD> mPrecipitationTypes;

    static SSAtmoEnvAsset makeDefault();

    bool addTrack();
    bool removeTrack(S32 index);

    F32 trackCeilingZ(S32 index) const;

    S32 sortTracksByAltitude(S32 follow_index = -1);

    std::string nextDefaultTrackName() const;

    bool visibleWaterHeight(F32& out_height) const;

    LLSD asLLSD() const;

    bool fromLLSD(const LLSD& sd, std::string& out_error);
};

// <SS:Nexii> Stages the environment's own precipitation types into the live preset list, so every consumer resolves a type by name without needing to know which tier it came from. Called when an environment is adopted; the manager drops them again on unload.
void ssAtmoEnvStagePrecipTypes(const SSAtmoEnvAsset& asset);

// Copies the definition of every shipped type the asset's keyframes name into the asset itself.
// Run before saving: it is what makes an environment self-contained, so a region opened on a build
// whose shipped set differs still renders the precipitation its author chose.
void ssAtmoEnvEmbedReferencedPrecipTypes(SSAtmoEnvAsset& asset);

#endif
