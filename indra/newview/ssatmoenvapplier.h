/**
 * @file ssatmoenvapplier.h
 * @brief Atmo Magic: applies the environment asset to the live sky and water.
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

#ifndef SS_ATMOENVAPPLIER_H
#define SS_ATMOENVAPPLIER_H

#include "llsingleton.h"
#include "ssatmoenvskymodulator.h"
#include "sswindprofilecore.h" // <SS:Nexii> the altitude wind profile core
#include "llsettingssky.h"
#include "llsettingswater.h"
#include "llpointer.h"
#include "llhudtext.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3math.h"
#include "v3color.h"

#include <vector>

struct SSAtmoEnvTrack;

struct SSAtmoEnvBillboard
{
    S32 mBodyIndex = -1;

    LLVector3 mDirection;
    F32 mAngularDiameterDeg = 0.f;
    LLUUID mTexture;
    bool mIsSun = false;

    LLVector3 mSunDirection;

    F32 mSunlight = 1.f;

    bool mEmissive = false;
    bool mPhaseShaded = true;

    // <SS:Nexii> The disc art's visible fraction of the quad, 1 - 2*padding of the authored body - publish it resolved so the draw pool binds it straight into the disc shader.
    F32 mDiscFraction = 1.f;
};

class SSAtmoEnvApplier : public LLSingleton<SSAtmoEnvApplier>
{
    LLSINGLETON(SSAtmoEnvApplier);
    ~SSAtmoEnvApplier() = default;

public:
    void apply();

    // Kills the celestial debug HUD texts (also GL teardown, via SSAtmoMagic::shutdownGL).
    void releaseDebugLabels();

    const SSAtmoEnvSkyModulation& lastModulation() const { return mLastModulation; }

    // <SS:Nexii> The ONE drift accumulator, integrated at the primary deck's BASE altitude (doc/atmo_magic_wind_profile.md section 4): the cell gate, the shadow bake and precipNoiseAt all read this frame unchanged. Wrapped on a lattice-aligned span (a multiple of the deck's cell and noise tile), never fmodf(1e6), so the wrap leaves every pattern where it was.
    const LLVector2& cloudDriftMetres() const { return mCloudDriftM; }
    // <SS:Nexii> The lattice-aligned span the accumulator wraps on (metres, 0 before the first apply) and how many times either axis has wrapped this session - the V7 sync console's drift row (SSAtmoSyncConsole), so two clients can see whether a divergence is the accumulator or a wrap.
    F32 driftWrapSpanM() const { return mDriftWrapSpanM; }
    U32 driftWrapCount() const { return mDriftWrapCount; }

    // <SS:Nexii> The CURVE-RESOLVED drift velocity at the deck base, m/s (mLastModulation.mDriftVelocity scaled by the PURE profile's speedScale at base_agl - NEW-3 fix 2026-09-05: windProfileAt(track, phase), never the live mWindProfile, whose mAnvilAglM reads a per-client setting through cirrusAltitudeMetres()) - the same vector cloudDriftMetres() integrates every frame, published pre-integration so the hero's storm-local frame shift (SSDeckFrame::HeroFrame::driftVel, doc/atmo_magic_storm_dynamics.md section 3) can read "the drift velocity" without a second, framerate-dependent derivative of the accumulator. Never the eased SSAtmoMagic::mWind. Zero while inactive.
    const LLVector2& driftVelocityMetresPerSec() const { return mDriftVelocityMS; }

    // <SS:Nexii> The dome/cirrus band's frame: base drift plus the BOUNDED shear offset O(z) of the band's own current altitude relative to the deck base (SSWindProfile::shearOffset, capped) - the design's dome seam, so the deck lid and the cirrus band can never slide apart without limit. Equals cloudDriftMetres() only for S=0 with the deck above the boundary layer; the auto shear floors S at 0.15, so a real calm sky carries a small, static, bounded lean instead.
    LLVector2 cirrusDriftMetres() const;

    // <SS:Nexii> The altitude wind profile at the applied phase (doc/atmo_magic_wind_profile.md section 3): windAt(world_z) is the drift VECTOR (x east, y north, m/s) the air moves at that height, built from the CURVE-RESOLVED wind and the cube's shear - never the eased SSAtmoMagic::mWind, which is framerate-dependent and so may never position world content. windProfile() hands out the parameters themselves for callers that sample the core directly (the V1 debug view, the storm scheduler).
    LLVector2 windAt(F32 world_z) const;
    const SSWindProfile::Params& windProfile() const { return mWindProfile; }

    // <SS:Nexii> The profile Params a track carries at an ARBITRARY phase, resolved purely from the cube (SSAtmoEnvWeatherResolver::resolve) with no live state read - what the storm scheduler samples at a cell's BIRTH phase (SSStormCells), where the live mWindProfile would be the wrong instant. Heading, speed, S and veer are the same resolver fields the live profile is built from and the exponent is the same core default; the anvil AGL differs on purpose: the live one reads the dome band's current altitude (seasonal setting + the deck's live lid), which is per-client, so this one takes the authored dome height keyframe at the phase, floored a cell above the boundary-layer top exactly like the live one. Inputs: the track's cube and dome-height keyframes, the phase. Nothing else.
    static SSWindProfile::Params windProfileAt(const SSAtmoEnvTrack& track, F64 phase);

    // <SS:Nexii> Which of the asset's tracks apply() drove this frame (-1 while inactive) and the phase it applied it at - the weather domain the storm scheduler shares with the sky (SSStormCells). The track choice follows the camera's altitude band exactly as the sky does; the phase carries the editor's preview override when one is set.
    S32 appliedTrackIndex() const { return mActive ? mAppliedTrack : -1; }
    F64 appliedPhase() const { return mAppliedPhase; }
    F32 windProfileGroundZ() const { return mTrackFloorZ; }
    F32 windProfileBaseZ() const { return mDriftBaseZ; }
    // <SS:Nexii> SCHEDULER fix 4: the SAME deterministic resolve's coverage (SSAtmoEnvCloudFieldResolver::resolve,
    // cached alongside mDriftBaseZ above) - the resolved weather-deck coverage for consumers (SSVortices'
    // dustGate) that must never read SSVolCloud's LIVE deck, which can still be last frame's build depending on
    // idle() tick order.
    F32 windProfileFieldCoverage() const { return mDriftCoverage; }

    // <SS:Nexii> Public forwarder to the private sunElevationSin(track, phase) - twilight/rainbow gating's own read
    // stays internal, but SSVortices needs the same pure (track, phase) figure for dustGate's "high sun" input, at
    // whatever track/phase the caller is asking about (its own applied-phase read for storm cells, mirrored here for
    // dust devils' live weather - see ssvortices.cpp).
    F32 sunElevationSinAt(const SSAtmoEnvTrack& track, F64 phase) const { return sunElevationSin(track, phase); }

    // <SS:Nexii> The dome band's altitude, resolved per call rather than cached with the rest of the sky walk - it reads the volumetric field's LIVE geometry, which moves between applies. The band IS the cirrus layer: the Sky Dome's animatable height param relative to the owning track's floor, brought down only by convection's anvil ramp (doc/atmo_magic_cloud_parallax.md). cloudDomeAltitudeMetres and cirrusAltitudeMetres are the same number - the pool and the floater's greyed-out dome row just read it by their own names.
    F32 cloudDomeAltitudeMetres() const;
    F32 cirrusAltitudeMetres() const;

    // <SS:Nexii> Atmo Magic altitude haze (doc/atmo_magic_surface_weather.md section 15, sshazecore.h): 1/H sampled
    // from the AUTHORED dome height keyframe (SSAtmoEnvCloudDome::mHeightM at the applied phase, the same sample
    // applySky already takes into mCloudDomeHeightM) and mHazeThinFrac at phase, via SSHaze::invHeight - never
    // cirrusAltitudeMetres(), whose own comment above warns it reads the live volumetric deck and the per-client
    // auto flag, which two clients would then disagree about. hazeCamHeightM() is the camera's OWN altitude above
    // the applied track's floor (mTrackFloorZ) - presentation-only and per-client by nature (it is where THIS
    // viewer's eye happens to be), so unlike every other published figure here it never feeds world state; it only
    // shapes what this client's own view looks like. Both zero while the applier is not driving, exactly like
    // sunRiseFraction() above.
    F32 hazeInvHeight() const { return mActive ? mHazeInvHeight : 0.f; }
    F32 hazeCamHeightM() const { return mActive ? mHazeCamHeightM : 0.f; }

    // <SS:Nexii> The world-up axis (world Z), expressed in the shader's VIEW/eye space, for the haze falloff to
    // read the fragment's real altitude off a view-space rel_pos - rel_pos.y alone is the camera's OWN up axis and
    // only equals world altitude when the camera is exactly level (see the ss_haze_up_view note in
    // atmosphericsFuncs.glsl). Same camera-axis derivation SSVolCloud::bindGroundShadow already uses for
    // ss_cshadow_r/u/f. Zero (an inert dot product) while the applier is not driving - like hazeInvHeight() it is
    // never the actual driver either way, since ss_haze_inv_height is also zero then.
    LLVector3 hazeUpView() const { return mActive ? mHazeUpView : LLVector3::zero; }

    // <SS:Nexii> The home planet's radius, metres, from the applied track's planetary system - the curvature authority the dome cloud's deck mapping curves around (cloudsF.glsl via lldrawpoolwlsky). Zero when the track carries no home body, which leaves the shader on its flat-deck fallback.
    F32 homePlanetRadiusM() const { return mHomePlanetRadiusM; }

    // <SS:Nexii> The dome's authored large-scale noise map, sampled at the applied phase - the broad octave's art when one is set (null: every octave reads the cloud noise). No LLSettingsSky home; the sky pool fetches and binds it straight off this id.
    const LLUUID& cloudLargeNoiseId() const { return mLargeNoiseId; }

    // <SS:Nexii> The dome cloud noise's crossfade, sampled at the applied phase: mid-fade the pair names both maps (each resolved through the default cloud noise) and out_blend is the eased weight; false when no fade runs, leaving the sky pool on its stock single binding.
    bool cloudNoiseBlend(LLUUID& out_from, LLUUID& out_to, F32& out_blend) const
    {
        out_from = mDomeNoiseFrom;
        out_to = mDomeNoiseTo;
        out_blend = mDomeNoiseBlend;
        return out_blend > 0.f;
    }

    // <SS:Nexii> The dome's large-scale map crossfade - see mLargeNoiseTo. Valid only while cloudLargeNoiseId() names an authored map (the gate stays on through the fade).
    const LLUUID& cloudLargeNoiseNextId() const { return mLargeNoiseTo; }
    F32 cloudLargeNoiseBlend() const { return mLargeNoiseBlend; }

    // <SS:Nexii> The dome band's Scale crossfade, sampled at the applied phase: mid-fade the TO keyframe's authored scale and the eased weight. The sky's own cloud_scale (setCloudScale in applySky) keeps holding the fade's FROM value - valueAt and blendAt agree about which keyframe that is - so the sky pool only binds this half of the pair (ss_cloud_scale_to and ss_cloud_scale_blend in cloudsF.glsl). The shader tiles the band by BOTH endpoints and crossfades the two renderings: the pattern is never zoomed, because an interpolated divisor is exactly the "clouds moving when they shouldn't" this re-add exists to stop - mid-fade it drags every feature sideways as the pivot point moves. Off-fade, to equals the live value and the blend is 0 - the single-sample rail. An authored scale of 0.25 is the anchor: it tiles exactly as the render did before the dial was re-added (SS_SCALE_ANCHOR in cloudsF).
    F32 cloudScaleTo() const { return mCloudScaleTo; }
    F32 cloudScaleBlend() const { return mCloudScaleBlend; }

    // <SS:Nexii> Whether the sky dome's lower half takes the nearer depth slot that clips what it draws over at the horizon (SSAtmoEnvAtmosphere::mHorizonClip). Sampled at the applied phase like the dome altitude; the sky pool reads it when it binds the dome shader.
    bool horizonClip() const { return mHorizonClip; }

    const LLVector3& moonSunDirection() const { return mMoonSunDir; }

    // <SS:Nexii> The two light slots' scene-light contributions after their OWN atmospheric attenuation, and whether they mean anything (an active environment with light-emitting bodies - see applyCelestial). Zero and invalid while inactive, which leaves the shaders on stock's single-lightnorm switch. atmosphericsFuncs.glsl takes the per-channel max of the two as the scene light - the dominant-light handover: the light is always whichever emitter is currently brighter, so the moon hands over to the rising sun exactly where their light crosses, never at the lightnorm flip, and never dimmer than either one.
    bool lightSlotsValid() const { return mActive && mLightSlotsValid; }
    LLColor3 sunSlotLight() const { return lightSlotsValid() ? mSunSlotLight : LLColor3(0.f, 0.f, 0.f); }
    LLColor3 moonSlotLight() const { return lightSlotsValid() ? mMoonSlotLight : LLColor3(0.f, 0.f, 0.f); }

    bool sunSlotEmissive() const { return mSunSlotEmissive; }
    bool moonSlotEmissive() const { return mMoonSlotEmissive; }
    bool sunSlotPhaseShaded() const { return mSunSlotPhaseShaded; }
    bool moonSlotPhaseShaded() const { return mMoonSlotPhaseShaded; }
    const LLVector3& sunSlotSunDirection() const { return mSunSlotSunDir; }
    F32 sunSlotSunlight() const { return mSunSlotSunlight; }
    F32 moonSlotSunlight() const { return mMoonSlotSunlight; }
    F32 sunSlotDiscFraction() const { return mSunSlotDiscFraction; }
    F32 moonSlotDiscFraction() const { return mMoonSlotDiscFraction; }

    F32 sunSlotAngularDeg() const { return mSunSlotAngularDeg; }
    F32 moonSlotAngularDeg() const { return mMoonSlotAngularDeg; }

    // <SS:Nexii> The sun's horizon-band share: 1 the whole time the disc's centre stands at or above the horizon, easing smoothly to 0 across the twilight band BELOW it (six disc radii, floored at 3 degrees and capped at 10 - see the computation in applyCelestial). The sky dome, the dome clouds and the atmospheric module ramp their sun glow and haze on this instead of snapping the whole sunrise/sunset horizon on the moment the disc's centre crosses zero (skyV.glsl, cloudsV.glsl, atmosphericsFuncs.glsl), and because the band runs DOWN from the horizon rather than across the disc's span, the glow burns at full authored strength while the sun approaches and sits on the horizon - a sunrise starts well before the disc reaches it, the light hitting the atmosphere first - and carries on through the dusk after it sets. Zero unless an ACTIVE environment is driving the sky, so a plain EEP sky keeps stock's step.
    F32 sunRiseFraction() const { return mActive ? mSunRiseFraction : 0.f; }

    // <SS:Nexii> The sun slot's TRUE direction, sampled at the applied phase - the disc itself, however far below the horizon it currently sits. The shaders need it because lightnorm - the shared light direction - SWITCHES to the moon the moment the sun's centre sets (LLSettingsSky::getLightDirection), and stock never noticed: its glow was zeroed and its disc culled below the horizon, so nothing was left looking at the sun to jump. Atmo's ramps keep the glow, the horizon band and the clouds' disc-neighbourhood body alive through the rise band, and every one of those has to keep aiming at the SUN or it leaps to the moon's azimuth at centre-set. Meaningful while sunRiseFraction() is positive - the whole band, dusk included; the shader ramps gate on that.
    const LLVector3& sunSlotDirection() const { return mSunSlotDir; }

    // <SS:Nexii> The sun slot disc's half-angle as a direction-z sine - the unit the horizon band is sized in (the twilight fade is measured in these radii). The dome shaders hold the sun's airmass at this while the rise band is live (sunRiseFraction > 0, which now spans the whole dusk below the horizon too), so the horizon line keeps the just-cleared light path through rise AND afterglow instead of collapsing to the infinite-airmass clamp. Zero while inactive.
    F32 sunSlotRadius() const { return mActive ? mSunSlotRadius : 0.f; }

    const LLVector3& observerPole() const { return mObserverPole; }

    void renderCelestialDebug();

    bool isActive() const { return mActive; }

    // <SS:Nexii> The track the applier resolved this frame - the landscape world's lifecycle
    // hub. -1 while inactive. Published the same way the billboard list is: the resolver's
    // active-track decision is expensive to recompute and the landscape world must follow the
    // exact same cut the sky does.
    S32 primaryTrackIndex() const { return mActive ? mPrimaryTrackIndex : -1; }
    // </SS:Nexii>

    bool waterPlaneOn() const { return mWaterPlaneOn; }

    const std::vector<SSAtmoEnvBillboard>& celestialBillboards() const { return mBillboards; }

    // <SS:Nexii> Angular diameter to EEP's disc scale, for the slot kind whose scale-1.0 quad subtends quad_deg (SS_ATMOENV_SUN_QUAD_DEG / SS_ATMOENV_MOON_QUAD_DEG - see ss_atmoenv_quad_deg in ssatmoenvasset.h). disc_fraction is the art's visible fraction of the quad (1 = full-bleed, ss_disc_fraction of the body's padding): the quad must overdraw the authored angle by 1/disc_fraction so the visible disc lands on it. Perception - how much the discs LOOM - is not an input here: it lives in the track's distance dials, which shrink the resolved distances the authored angle was measured across.
    static F32 celestialDiscScale(F32 angular_diameter_deg, F32 disc_fraction, F32 quad_deg);

private:
    void activate();
    void deactivate();
    void install();

    void applySky(const SSAtmoEnvTrack& track, F64 phase,
                  const SSAtmoEnvSkyModulation& mod);
    void applyWater(const SSAtmoEnvTrack& track, F64 phase,
                    const SSAtmoEnvSkyModulation& mod);

    SSAtmoEnvSkyModulation computeModulation(const SSAtmoEnvTrack& track, F64 phase);

    F32 sunElevationSin(const SSAtmoEnvTrack& track, F64 phase) const;

    bool mWasPrecipitating = false;
    F32 mSecondsSinceRainStopped = -1.f;
    F64 mLastTrailUpdate = 0.0;

    SSAtmoEnvSkyModulation mLastModulation;
    // <SS:Nexii> F64 ACCUMULATOR FIX (2026-09-05, doc/atmo_magic_wind_profile.md section 4): the running sum is
    // kept in F64 internally - a per-frame F32 sum of v*dt diverges 233.75 m over one hour (30fps vs 144fps, same
    // windParams; measured by V:\Scratch\atmo\tests\scenario_drift_f64.cpp, f64_accumulator_collapses_framerate_
    // divergence_to_millimetres); the SAME sum, SAME hour, in F64 measures 0.000000000 m at float-printf precision -
    // below millimetres, not merely reduced to them. mCloudDriftM stays the F32 CACHE of this pair, refreshed every apply immediately after the F64 integrate+wrap
    // - the only value uniforms/consumers (cloudDriftMetres(), cirrusDriftMetres()) ever see, per the design's own
    // "F32 only at the uniform/consumer boundary" rule.
    F64 mCloudDriftXD = 0.0;
    F64 mCloudDriftYD = 0.0;
    LLVector2 mCloudDriftM;

    // <SS:Nexii> The profile the drift integrates against, rebuilt every apply from the resolved weather state, and the world Z of the deck base the accumulator ran at (the live primary deck's base, else the authored one). mDriftBlend is the Wind Scroll influence's share of the resolved wind - the same factor already inside mDriftVelocity - so the dome seam's shear offset switches off with the drift it rides on.
    SSWindProfile::Params mWindProfile;
    F32 mDriftBaseZ = 0.f;
    F32 mDriftCoverage = 0.f;
    F32 mDriftBlend = 0.f;
    F32 mDriftWrapSpanM = 0.f;
    U32 mDriftWrapCount = 0;
    // <SS:Nexii> The curve-resolved drift velocity at base, m/s - see driftVelocityMetresPerSec(). Set alongside drift_scale/mDriftBlend, before the accumulator's dt integration, so it is the instantaneous rate the accumulator is running at this apply, not a finite difference of mCloudDriftM (which would carry the wrap discontinuity).
    LLVector2 mDriftVelocityMS;
    S32 mAppliedTrack = -1;
    F64 mAppliedPhase = 0.0;

    // <SS:Nexii> The dome band's authored height and its auto flag, sampled at the applied phase - the ANIMATABLE Sky Dome height keyframes, metres relative to the owning track's floor (cirrusAltitudeMetres adds the floor back). The auto flag no longer substitutes an altitude: the height param always rules.
    bool mCloudDomeAuto = false;
    F32 mCloudDomeHeightM = 6000.f;

    // <SS:Nexii> The altitude haze pair, sampled at the applied phase - see hazeInvHeight/hazeCamHeightM.
    F32 mHazeInvHeight = 0.f;
    F32 mHazeCamHeightM = 0.f;
    LLVector3 mHazeUpView = LLVector3::zero;

    // <SS:Nexii> The applied track's floor, convection and temperature - the cirrus altitude is floor-relative, its anvil ramp rides the convection, and the seasonal band rides the temperature (SSAtmoCirrusSeason) - plus the home planet's radius, metres.
    F32 mTrackFloorZ = 0.f;
    F32 mLastConvection = 0.f;
    F32 mLastTemperatureC = 15.f;
    F32 mHomePlanetRadiusM = 0.f;

    // <SS:Nexii> The dome's authored large-scale noise id, sampled at the applied phase - see cloudLargeNoiseId. cloudLargeNoiseNextId and cloudLargeNoiseBlend are its crossfade: live while the day cycle fades between two authored large maps (both ends authored - a fade onto or off of None snaps, since the gate switches whole octaves between maps), otherwise the next id is the current one and the blend is 0.
    LLUUID mLargeNoiseId;
    LLUUID mLargeNoiseTo;
    F32 mLargeNoiseBlend = 0.f;

    // <SS:Nexii> The dome cloud noise's crossfade, sampled at the applied phase - see cloudNoiseBlend. Both ids are resolved through the default cloud noise, so the pair the sky pool binds is always concrete; the sky's own noise id keeps holding the fade's FROM map.
    LLUUID mDomeNoiseFrom;
    LLUUID mDomeNoiseTo;
    F32 mDomeNoiseBlend = 0.f;

    // <SS:Nexii> The dome band's Scale crossfade, sampled at the applied phase - see cloudScaleTo. The FROM endpoint lives in the sky's own cloud_scale (mLastCloudScale above), so only these two ride the pair. The blend stays 0 unless a fade is actually between two Scale keyframes - the shader's single-sample rail needs no extra branching at all.
    F32 mCloudScaleTo = 0.f;
    F32 mCloudScaleBlend = 0.f;

    // <SS:Nexii> The horizon clip, sampled at the applied phase - see horizonClip.
    bool mHorizonClip = true;

    F32 mMoonSlotBrightness = 1.f;

    LLVector3 mMoonSunDir;
    bool mSunSlotEmissive = false;
    bool mMoonSlotEmissive = false;
    bool mSunSlotPhaseShaded = true;
    bool mMoonSlotPhaseShaded = true;
    LLVector3 mSunSlotSunDir;
    F32 mSunSlotSunlight = 1.f;
    F32 mMoonSlotSunlight = 1.f;

    // <SS:Nexii> The slot bodies' disc art fraction (1 - 2*padding) - the disc shader normalises its phase-shaded sphere to the art's disc, not the quad. One unless the slot's body pads.
    F32 mSunSlotDiscFraction = 1.f;
    F32 mMoonSlotDiscFraction = 1.f;

    // <SS:Nexii> The light slots' post-attenuation contributions and their validity flag - see lightSlotsValid. Computed at the end of applyCelestial, against the sky values applySky just wrote, so the CPU side of the handover cannot drift from the shader's own formula.
    LLColor3 mSunSlotLight{0.f, 0.f, 0.f};
    LLColor3 mMoonSlotLight{0.f, 0.f, 0.f};
    bool mLightSlotsValid = false;

    LLVector3 mObserverPole = LLVector3::z_axis;
    F32 mSunSlotAngularDeg = 0.53f;
    F32 mMoonSlotAngularDeg = 0.53f;

    // <SS:Nexii> The sun's horizon-band share, sampled at the applied phase - see sunRiseFraction.
    F32 mSunRiseFraction = 0.f;

    // <SS:Nexii> The sun slot's true direction, sampled at the applied phase - see sunSlotDirection.
    LLVector3 mSunSlotDir = LLVector3::z_axis;

    // <SS:Nexii> The sun slot disc's half-angle as a direction-z sine - the same sizing chain updateHeavenlyBodyGeometry lays the disc out with, the unit the horizon band is sized in, and the airmass floor the sky dome and dome clouds hold their sun term at while the rise band is live (see sunSlotRadius). Sampled with the rise fraction.
    F32 mSunSlotRadius = 0.f;

    std::vector<LLPointer<class LLHUDText> > mDebugLabels;

    struct DebugMark
    {
        std::string mName;
        LLVector3 mDirection;
        F32 mAngularDiameterDeg = 0.f;
        F32 mSunlight = 1.f;
        bool mEmissive = false;
        bool mIsSunSlot = false;
        bool mIsMoonSlot = false;
    };
    std::vector<DebugMark> mDebugMarks;
    void applyWaterDefaults();

    void setWaterRendering(bool enabled);
    bool mWaterDerendered = false;
    bool mWaterPlaneOn = false;

    // <SS:Nexii> The resolved primary track of the most recent apply() - published via
    // primaryTrackIndex() to the landscape world. Kept alongside mActive so an inactive
    // applier reads -1 without extra state machines.
    S32 mPrimaryTrackIndex = -1;
    // </SS:Nexii>

    void applyCelestial(const SSAtmoEnvTrack& track, F64 phase);

    bool mActive = false;

    LLSettingsSky::ptr_t   mSky;
    LLSettingsWater::ptr_t mWater;

    LLSettingsWater::ptr_t mDefaultWater;

    F32 mGlowG = 0.f;

    F32    mPrevWorldZ = 0.f;
    bool   mPrevWorldZValid = false;
    LLUUID mPrevRegionID;

    bool mSkyCacheValid = false;
    LLColor3 mLastAmbient;
    LLColor3 mLastBlueHorizon;
    LLColor3 mLastBlueDensity;
    LLColor3 mLastSunlight;
    F32 mLastHazeHorizon = 0.f;
    F32 mLastHazeDensity = 0.f;
    F32 mLastSkyMoisture = 0.f;
    F32 mLastSkyDroplet = 0.f;
    F32 mLastSkyIce = 0.f;
    F32 mLastProbeAmbiance = 0.f;
    F32 mLastDensityMult = 0.f;
    F32 mLastDistanceMult = 0.f;
    F32 mLastMaxY = 0.f;
    F32 mLastGamma = 0.f;
    F32 mLastStarBrightness = 0.f;
    F32 mLastMoonBrightness = 0.f;
    LLColor3 mLastGlow;

    LLColor3 mLastCloudColor;
    F32 mLastCloudCoverage = 0.f;
    F32 mLastCloudScale = 0.f;
    F32 mLastCloudVariance = 0.f;
    LLVector2 mLastCloudScroll;
    LLColor3 mLastCloudDensity;
    LLColor3 mLastCloudDetail;
    LLUUID mLastCloudNoise;

    bool mCelestialCacheValid = false;
    LLVector3 mLastSunDir;
    LLVector3 mLastMoonDir;
    F32 mLastSunScale = 0.f;
    F32 mLastMoonScale = 0.f;
    LLUUID mLastSunTexture;
    LLUUID mLastMoonTexture;

    std::vector<SSAtmoEnvBillboard> mBillboards;

    bool mWaterCacheValid = false;
    LLColor3 mLastFogColor;
    F32 mLastFogDensity = 0.f;
    F32 mLastFogMod = 0.f;
    F32 mLastFresnelScale = 0.f;
    F32 mLastFresnelOffset = 0.f;
    LLUUID mLastNormalMap;
    // <SS:Nexii> The normal map's crossfade state - the partner id and the last weight pushed through the water settings (see applyWater). The pool reads the weight live at bind time; only the partner id rides update().
    LLUUID mLastNormalMapNext;
    F32 mLastNormalBlend = 0.f;
    LLVector3 mLastNormalScale;
    LLVector2 mLastWave1;
    LLVector2 mLastWave2;
    F32 mLastScaleAbove = 0.f;
    F32 mLastScaleBelow = 0.f;
    F32 mLastBlur = 0.f;
};

#endif
