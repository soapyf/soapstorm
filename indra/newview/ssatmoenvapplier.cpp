/**
 * @file ssatmoenvapplier.cpp
 * @brief See ssatmoenvapplier.h.
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

#include "ssatmoenvapplier.h"

#include "ssatmoenvweatherstate.h"

#include "llhudobject.h"
#include "llfontgl.h"
#include "llhudtext.h"

#include "pipeline.h"

#include "llagent.h"
#include "llenvironment.h"
#include "llsky.h"
#include "llvosky.h"
#include "llsettingsvo.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"

#include "ssatmoenvasset.h"
#include "ssatmoenvmanager.h"
#include "ssatmoenvplanetarystate.h"
#include "ssatmoenvtrackstate.h"
#include "ssvolcloud.h" // <SS:Nexii> the auto dome altitude reads the volumetric deck
#include "sswindflow.h" // <SS:Nexii> the wind gradient scales the cirrus drift to its altitude

#include "v3colorutil.h" // <SS:Nexii> componentMult/componentExp, the light handover's attenuation

#include <algorithm>
#include <cmath>

namespace
{
    const F32 SLIDER_SCALE_GLOW_R(20.0f);
    const F32 SLIDER_SCALE_GLOW_B(-5.0f);

    const F32 TELEPORT_JUMP_M(60.f);

    // <SS:Nexii> Disc scale bounds. The floor sits under the real Sun's own scale (0.53 deg / 5.72 deg = 0.093, once the quad's true angles drove the conversion - the old 0.1 floor was fine only while the 10x reference bug was quietly doing the clamping's work) so a correctly authored system is never clamped up; the ceiling keeps a body parked on its home's doorstep from asking for an infinite quad.
    const F32 CELESTIAL_SCALE_MIN(0.01f);
    const F32 CELESTIAL_SCALE_MAX(20.f);

    // The disc art's visible fraction of the quad for a body's padding, floored so a disc never
    // shrinks below a tenth of its quad - the same clamp celestialDiscScale divides by, so the
    // scale math and the disc shader agree on where the art's disc sits.
    const F32 SS_MIN_DISC_FRACTION = 0.1f;
    F32 ss_disc_fraction(F32 disc_padding)
    {
        return llmax(1.f - 2.f * disc_padding, SS_MIN_DISC_FRACTION);
    }

    // <SS:Nexii> The sunrise/sunset twilight band the glow ramps out over once the disc's centre sets: six of the disc's OWN radii below the horizon, so the dusk keeps proportion to whatever sun the sky authors - floored and capped in degrees because the twilight belongs to the ATMOSPHERE, not the disc: a stock-sized sun still gets a real dusk (its six radii are barely a degree and a half) and a colossal one must not paint a quarter-sky twilight.
    const F32 SS_SUN_TWILIGHT_RADII(6.f);
    const F32 SS_SUN_TWILIGHT_MIN_DEG(3.f);
    const F32 SS_SUN_TWILIGHT_MAX_DEG(10.f);

    // Shortest arc taking +X onto a direction - the engine's own sun/moon rotation convention, inverted.
    LLQuaternion quat_from_direction(const LLVector3& dir)
    {
        LLQuaternion quat;
        LLVector3 axis = LLVector3::x_axis % dir;
        if (axis.normalize() < 0.0001f)
        {
            if (dir.mV[VX] < 0.f)
            {
                quat.setAngleAxis(F_PI, LLVector3::z_axis);
            }
            return quat;
        }
        quat.setAngleAxis(acosf(llclamp(LLVector3::x_axis * dir, -1.f, 1.f)), axis);
        return quat;
    }
}

// Singleton shell.
SSAtmoEnvApplier::SSAtmoEnvApplier()
{
}

// <SS:Nexii> The dome band's altitude. The band IS the cirrus layer: it sits at the Sky Dome's ANIMATABLE height param, relative to the owning track's floor - the same convention both decks' base heights use, so an imported day cycle's height keyframes play through it and a sky build's track carries it whole. Moisture never moves it - an earlier derivation merged the band down onto the deck's mid-altitude as the deck's coverage built, which let three hundredths of moisture drag a 6 km cirrus deck down onto a 1 km storm: the cirrus belongs at the cirrus level. The ONLY thing that brings it down is convection: as the deck anvils (the same ramp that flattens the deck's own tops, SSAtmoEnvCloudFieldResolver::mAnvil) the band descends onto the deck's lid, ending ~300 m over the deck's max height - a towering anvil reaches UP and hits the cirrus, never the other way round. What the floater shows in the greyed-out row.
static const F32 SS_CIRRUS_LID_GAP_M = 300.f;
static const F32 SS_ANVIL_ONSET      = 0.6f;
static const F32 SS_ANVIL_FULL       = 0.9f;

F32 SSAtmoEnvApplier::cirrusAltitudeMetres() const
{
    // <SS:Nexii> The seasonal band (SSAtmoCirrusSeason): temperature sets the cirrus layer's home - winter air is dense and squashes the atmosphere down, so the same cirrus that rides ~8km in a summer heatwave hangs at ~5km in -15C cold - with the authored dome height keeping its day-cycle SHAPE: the seasonal altitude is scaled by the authored height over the 6km reference it was authored under, so a deck authored low stays proportionally low. Like the storm deck's base, the band follows the temperature as a cubic centred on the neutral 10C midpoint - flat in the middle so a day's drift barely moves the cirrus, steepest at the seasonal rails. The anvil descent below still brings the band onto the storm deck's lid, exactly as always.
    static const F32 CIRRUS_WINTER_M = 5000.f;
    static const F32 CIRRUS_SUMMER_M = 8000.f;
    static const F32 CIRRUS_KIND_MIN_C = -15.f;
    static const F32 CIRRUS_KIND_MAX_C = 35.f;
    static const F32 CIRRUS_AUTHORED_REFERENCE_M = 6000.f;

    static LLCachedControl<bool> season_setting(gSavedSettings, "SSAtmoCirrusSeason", true);

    const F32 authored = llmax(mCloudDomeHeightM, 1.f);

    F32 base = mTrackFloorZ + authored;
    if (season_setting)
    {
        const F32 mid_c = (CIRRUS_KIND_MIN_C + CIRRUS_KIND_MAX_C) * 0.5f;
        const F32 half_c = (CIRRUS_KIND_MAX_C - CIRRUS_KIND_MIN_C) * 0.5f;
        const F32 mid_m = (CIRRUS_WINTER_M + CIRRUS_SUMMER_M) * 0.5f;
        const F32 t = llclamp((mLastTemperatureC - mid_c) / half_c, -1.f, 1.f);
        const F32 seasonal = mid_m + (CIRRUS_SUMMER_M - mid_m) * t * t * t;
        base = mTrackFloorZ + seasonal * (authored / CIRRUS_AUTHORED_REFERENCE_M);
    }
    const F32 dry = llmax(base, 1.f);

    SSVolCloud* vol = SSVolCloud::getInstance();
    if (!vol || vol->empty()) return dry;

    const F32 anvil = llclamp((mLastConvection - SS_ANVIL_ONSET)
                              / (SS_ANVIL_FULL - SS_ANVIL_ONSET), 0.f, 1.f);
    const F32 lid = llmax(vol->cloudTopZ() + SS_CIRRUS_LID_GAP_M, 300.f);
    return lerp(dry, lid, anvil);
}

// The altitude the shaders actually get, WORLD height.
F32 SSAtmoEnvApplier::cloudDomeAltitudeMetres() const
{
    return cirrusAltitudeMetres();
}

// Per-frame: resolve the primary track, evaluate its keyframes at the phase, and push sky/water/celestial through EEP's ENV_LOCAL slot.
void SSAtmoEnvApplier::apply()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "SSAtmoEnabled", false);

    SSAtmoEnvManager* mgr = SSAtmoEnvManager::getInstance();

    const bool want_active = enabled && mgr->hasAsset() && !mgr->asset().mTracks.empty();

    if (!want_active)
    {
        if (mActive)
        {
            deactivate();
        }
        return;
    }

    if (!mActive)
    {
        activate();
    }
    else if (!LLEnvironment::instance().hasEnvironment(LLEnvironment::ENV_LOCAL))
    {
        install();
    }

    const SSAtmoEnvAsset& asset = mgr->asset();

    const F32 world_z = LLViewerCamera::getInstance()->getOrigin().mV[VZ];
    LLViewerRegion* region = gAgent.getRegion();
    const LLUUID region_id = region ? region->getRegionID() : LLUUID::null;

    const bool teleported = !mPrevWorldZValid
        || region_id != mPrevRegionID
        || llabs(world_z - mPrevWorldZ) > TELEPORT_JUMP_M;

    const SSAtmoEnvTrackBlend blend = SSAtmoEnvTrackResolver::resolve(
        asset, world_z, mPrevWorldZValid ? mPrevWorldZ : world_z, teleported);

    mPrevWorldZ = world_z;
    mPrevWorldZValid = true;
    mPrevRegionID = region_id;

    S32 track_index = blend.mPrimaryTrack;
    if (track_index < 0 || track_index >= static_cast<S32>(asset.mTracks.size()))
    {
        track_index = 0;
    }
    const SSAtmoEnvTrack& track = asset.mTracks[static_cast<size_t>(track_index)];

    // <SS:Nexii> The home body's radius - the curvature authority the dome cloud's deck mapping curves around (cloudsF.glsl, fed by lldrawpoolwlsky). A track with no home body falls back to an Earth-sized default rather than to flat: the deck's own curved horizon - a finite disc terminating at its tangent elevation instead of rows of compressed tiles running into the world's horizon line - is the whole point of the curved mapping, and "no planet authored" should not read as "flat cartoon sky". A track with a home body overrides with its real radius.
    static const F32 SS_DEFAULT_PLANET_RADIUS_M = 5.0e6f;
    mHomePlanetRadiusM = SS_DEFAULT_PLANET_RADIUS_M;
    const S32 home_index = track.mPlanetary.homeBodyIndex();
    if (home_index >= 0 && home_index < static_cast<S32>(track.mPlanetary.mBodies.size()))
    {
        mHomePlanetRadiusM =
            0.5f * track.mPlanetary.mBodies[static_cast<size_t>(home_index)].mDiameterM;
    }

    const F64 phase = mgr->hasPreviewPhaseOverride()
        ? mgr->previewPhaseOverride()
        : track.currentDayCyclePhase();

    const SSAtmoEnvSkyModulation mod = computeModulation(track, phase);

    {
        static LLCachedControl<bool> overlay(gSavedSettings, "SSAtmoPlanetaryDebugOverlay", false);
        if (!overlay && !mDebugLabels.empty())
        {
            releaseDebugLabels();
        }
    }

    applySky(track, phase, mod);
    applyCelestial(track, phase);

    mWaterPlaneOn = track.mWater.mEnabled;
    if (track.mWater.mEnabled)
    {
        applyWater(track, phase, mod);
        setWaterRendering(true);
    }
    else
    {
        applyWaterDefaults();
        setWaterRendering(false);
    }
}

// Angular diameter to EEP's disc scale - see the header comment.
F32 SSAtmoEnvApplier::celestialDiscScale(F32 angular_diameter_deg, F32 disc_fraction, F32 quad_deg)
{
    return llclamp(angular_diameter_deg / (llmax(disc_fraction, SS_MIN_DISC_FRACTION) * quad_deg),
                   CELESTIAL_SCALE_MIN, CELESTIAL_SCALE_MAX);
}

// Kills the celestial debug HUD texts.
void SSAtmoEnvApplier::releaseDebugLabels()
{
    for (LLPointer<LLHUDText>& label : mDebugLabels)
    {
        if (label.notNull()) label->markDead();
    }
    mDebugLabels.clear();
}

// Labels every resolved body on screen - warm for the sun slot, cool for the moon slot, green for the rest.
void SSAtmoEnvApplier::renderCelestialDebug()
{
    if (mDebugMarks.empty())
    {
        releaseDebugLabels();
        return;
    }

    static const F32 LABEL_DIST_M = 24.f;
    static const F32 RAY_DIST_M = 40.f;

    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();

    while ((S32)mDebugLabels.size() < (S32)mDebugMarks.size())
    {
        LLHUDText* text = static_cast<LLHUDText*>(
            LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT));
        if (!text) break;
        text->setFont(LLFontGL::getFontSansSerifSmall());
        text->setZCompare(false);
        text->setDoFade(false);
        text->setVertAlignment(LLHUDText::ALIGN_VERT_CENTER);
        mDebugLabels.push_back(LLPointer<LLHUDText>(text));
    }
    while ((S32)mDebugLabels.size() > (S32)mDebugMarks.size())
    {
        if (mDebugLabels.back().notNull()) mDebugLabels.back()->markDead();
        mDebugLabels.pop_back();
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    LLGLEnable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE);

    gGL.begin(LLRender::LINES);
    for (size_t i = 0; i < mDebugMarks.size(); ++i)
    {
        const DebugMark& mark = mDebugMarks[i];

        LLColor4 colour(0.4f, 1.f, 0.5f, 0.9f);
        if (mark.mIsSunSlot) colour = LLColor4(1.f, 0.85f, 0.3f, 0.9f);
        else if (mark.mIsMoonSlot) colour = LLColor4(0.6f, 0.75f, 1.f, 0.9f);

        gGL.color4fv(colour.mV);
        gGL.vertex3fv(origin.mV);
        gGL.vertex3fv((origin + mark.mDirection * RAY_DIST_M).mV);

        const LLVector3 tip = origin + mark.mDirection * RAY_DIST_M;
        LLVector3 side = mark.mDirection % LLVector3::z_axis;
        if (side.normalize() < 0.001f) side = LLVector3::x_axis;
        const LLVector3 up = side % mark.mDirection;

        gGL.vertex3fv((tip - side * 1.5f).mV);
        gGL.vertex3fv((tip + side * 1.5f).mV);
        gGL.vertex3fv((tip - up * 1.5f).mV);
        gGL.vertex3fv((tip + up * 1.5f).mV);

        if (i < mDebugLabels.size() && mDebugLabels[i].notNull())
        {
            const F32 elev = RAD_TO_DEG * asinf(llclamp(mark.mDirection.mV[VZ], -1.f, 1.f));
            F32 azim = RAD_TO_DEG * atan2f(mark.mDirection.mV[VX], mark.mDirection.mV[VY]);
            if (azim < 0.f) azim += 360.f;

            std::string line = mark.mName;
            if (mark.mIsSunSlot) line += " [sun slot]";
            else if (mark.mIsMoonSlot) line += " [moon slot]";
            line += llformat("\nalt %.1f deg  az %.1f deg", elev, azim);
            line += llformat("\nsize %.2f deg", mark.mAngularDiameterDeg);
            line += mark.mEmissive ? "\nemissive"
                                   : llformat("\nlit %.0f%%", mark.mSunlight * 100.f);
            if (mark.mIsSunSlot || mark.mIsMoonSlot)
            {
                // <SS:Nexii> Which slot owns the scene light right now - the dominant-light handover (applyCelestial) crosses where these swap, not at centre-rise.
                const bool sun_dominant =
                    llmax(mSunSlotLight.mV[0], mSunSlotLight.mV[1], mSunSlotLight.mV[2])
                    >= llmax(mMoonSlotLight.mV[0], mMoonSlotLight.mV[1], mMoonSlotLight.mV[2]);
                if (mark.mIsSunSlot == sun_dominant)
                {
                    line += "\nlight dominant";
                }
            }

            mDebugLabels[i]->setString(line);
            mDebugLabels[i]->setColor(colour);
            mDebugLabels[i]->setPositionAgent(origin + mark.mDirection * LABEL_DIST_M);
        }
    }
    if (gSky.mVOSkyp.notNull())
    {
        struct { const LLHeavenBody* mBody; bool mIsSun; } quads[2] = {
            { &gSky.mVOSkyp->getSun(),  true  },
            { &gSky.mVOSkyp->getMoon(), false }
        };

        gGL.begin(LLRender::LINES);
        for (const auto& quad : quads)
        {
            LLVector3 centre;
            for (S32 c = 0; c < 4; ++c) centre += quad.mBody->corner(c);
            centre *= 0.25f;
            if (centre.normalize() < 0.0001f) continue;

            const LLColor4 colour(1.f, 1.f, 1.f, 0.7f);
            gGL.color4fv(colour.mV);
            gGL.vertex3fv(origin.mV);
            gGL.vertex3fv((origin + centre * (RAY_DIST_M * 0.75f)).mV);

            for (const DebugMark& mark : mDebugMarks)
            {
                if (mark.mIsSunSlot != quad.mIsSun || mark.mIsMoonSlot == quad.mIsSun) continue;

                const F32 quad_elev = RAD_TO_DEG * asinf(llclamp(centre.mV[VZ], -1.f, 1.f));
                const F32 want_elev = RAD_TO_DEG * asinf(llclamp(mark.mDirection.mV[VZ], -1.f, 1.f));
                const F32 apart = RAD_TO_DEG * acosf(llclamp(centre * mark.mDirection, -1.f, 1.f));

                if (apart > 0.25f)
                {
                    LL_INFOS_ONCE("AtmoMagicEnv") << "Celestial debug: "
                        << (quad.mIsSun ? "sun" : "moon") << " quad is drawn "
                        << apart << " deg from where it was placed (quad alt "
                        << quad_elev << " deg, authored alt " << want_elev
                        << " deg) - geometry, not texture" << LL_ENDL;
                }
                break;
            }
        }
        gGL.end();
        gGL.flush();
    }

    if (shader) shader->bind();
}

// Toggles the stock water render types - the own-water-plane rule's bookkeeping.
void SSAtmoEnvApplier::setWaterRendering(bool enabled)
{
    if (!enabled)
    {
        if (!mWaterDerendered && LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_WATER))
        {
            LLPipeline::toggleRenderTypeControl(LLPipeline::RENDER_TYPE_WATER);
            mWaterDerendered = true;
        }
        return;
    }

    if (mWaterDerendered)
    {
        if (!LLPipeline::hasRenderTypeControl(LLPipeline::RENDER_TYPE_WATER))
        {
            LLPipeline::toggleRenderTypeControl(LLPipeline::RENDER_TYPE_WATER);
        }
        mWaterDerendered = false;
    }
}

// Starts owning the environment when an asset is present.
void SSAtmoEnvApplier::activate()
{
    mSky = LLSettingsVOSky::buildDefaultSky();
    mWater = LLSettingsVOWater::buildDefaultWater();

    mDefaultWater = LLSettingsVOWater::buildDefaultWater();

    mGlowG = mSky->getGlow().mV[1];

    install();
    mActive = true;
}

// Pushes our settings objects into EEP's local slot.
void SSAtmoEnvApplier::install()
{
    mSkyCacheValid = false;
    mCelestialCacheValid = false;
    mWaterCacheValid = false;

    LLEnvironment& env = LLEnvironment::instance();
    env.setEnvironment(LLEnvironment::ENV_LOCAL, mSky, mWater);
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
}

// Releases ENV_LOCAL and restores whatever EEP had.
void SSAtmoEnvApplier::deactivate()
{
    LLEnvironment::instance().clearEnvironment(LLEnvironment::ENV_LOCAL);
    LLEnvironment::instance().setSelectedEnvironment(LLEnvironment::ENV_LOCAL);

    setWaterRendering(true);

    mSky.reset();
    mWater.reset();
    mDefaultWater.reset();
    mBillboards.clear();
    mDebugMarks.clear();
    releaseDebugLabels();
    mSkyCacheValid = false;
    mCelestialCacheValid = false;
    mWaterCacheValid = false;
    mPrevWorldZValid = false;
    mActive = false;
}

// Weather cube plus the author's influence settings into this frame's sky transforms, cached for the readouts.
SSAtmoEnvSkyModulation SSAtmoEnvApplier::computeModulation(const SSAtmoEnvTrack& track, F64 phase)
{
    // Sampled before the influence gate, not behind it: the volumetric deck ignores that gate
    // (SSVolCloud::update resolves straight off the weather cube), and the dome band integrates
    // with the DECK - with the deck, not with the influence switch.
    mLastConvection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);
    mLastTemperatureC = llclamp(track.mWeather.mTemperatureC.valueAt(phase), -60.f, 60.f);

    const SSAtmoEnvWeatherInfluence& influence = track.mWeatherInfluence;

    if (!influence.mEnabled)
    {
        mWasPrecipitating = false;
        mSecondsSinceRainStopped = -1.f;
        mLastModulation = SSAtmoEnvSkyModulation();
        return mLastModulation;
    }

    const SSAtmoEnvWeatherState state = SSAtmoEnvWeatherResolver::resolve(track.mWeather, phase);

    const F64 now = LLTimer::getElapsedSeconds();
    const F64 elapsed = (mLastTrailUpdate > 0.0) ? (now - mLastTrailUpdate) : 0.0;
    mLastTrailUpdate = now;

    const bool precipitating = !state.mPrecipitationType.empty()
        && state.mPrecipitationIntensity > 0.f;
    if (precipitating)
    {
        mWasPrecipitating = true;
        mSecondsSinceRainStopped = -1.f;
    }
    else if (mWasPrecipitating)
    {
        mWasPrecipitating = false;
        mSecondsSinceRainStopped = 0.f;
    }
    else if (mSecondsSinceRainStopped >= 0.f)
    {
        mSecondsSinceRainStopped += static_cast<F32>(elapsed);
    }

    SSAtmoEnvSkyWeatherInput in;
    in.mMoisture = llclamp(track.mWeather.mMoisture.valueAt(phase), 0.f, 1.f);
    in.mConvection = llclamp(track.mWeather.mConvection.valueAt(phase), 0.f, 1.f);
    in.mTemperatureC = track.mWeather.mTemperatureC.valueAt(phase);
    in.mWindHeadingDeg = state.mWindHeading;
    in.mWindSpeedMS = state.mWindSpeed;
    in.mMaxAltitudeM = track.mAtmosphere.mMaxAltitude.valueAt(phase);
    in.mCloudScale = track.mCloudDome.mScale.valueAt(phase);
    in.mPrecipitationIntensity = state.mPrecipitationIntensity;
    in.mSecondsSinceRainStopped = mSecondsSinceRainStopped;

    const bool rainbow_possible = influence.mRainbowEnabled
        && influence.mRainbowStrength > 0.f
        && mSecondsSinceRainStopped >= 0.f;
    in.mSunElevationSin = rainbow_possible ? sunElevationSin(track, phase) : 0.f;

    mLastModulation = SSAtmoEnvSkyWeatherModulator::compute(in, influence);

    // <SS:Nexii> The dome/cirrus band drifts with the WIND, scaled to its OWN altitude by the boundary-layer wind gradient - the authored wind is a 10m value, and the cirrus layer sits kilometres up, so the power law (SSWindFlowMap::windGradientScale, clamped at the ~1.5km free-atmosphere top) is exactly the factor that takes the ground wind to where the band is. The reference ground is the water plane or the region's true ground. With no flowmap tile yet it falls back to the SSAtmoWindFlowGradient exponent. The stock cloud_scroll_rate path is left at zero, so this drift is the only thing that moves the band.
    const F32 cirrus_agl = llmax(cirrusAltitudeMetres() - SSWindFlowMap::getInstance()->groundRefZ(), 1.f);
    const F32 drift_scale = SSWindFlowMap::getInstance()->windGradientScale(cirrus_agl);

    const F32 drift_dt = static_cast<F32>(llclamp(elapsed, 0.0, 0.25));
    mCloudDriftM += mLastModulation.mDriftVelocity * drift_scale * drift_dt;

    static const F32 DRIFT_WRAP_M = 1.0e6f;
    mCloudDriftM.mV[0] = fmodf(mCloudDriftM.mV[0], DRIFT_WRAP_M);
    mCloudDriftM.mV[1] = fmodf(mCloudDriftM.mV[1], DRIFT_WRAP_M);

    return mLastModulation;
}

// The lit sun's elevation sine at a phase, for twilight and rainbow gating.
F32 SSAtmoEnvApplier::sunElevationSin(const SSAtmoEnvTrack& track, F64 phase) const
{
    const SSAtmoEnvPlanetary& planetary = track.mPlanetary;
    const S32 home = planetary.homeBodyIndex();
    if (home < 0) return 0.f;

    SSAtmoEnvResolvedBody sun;
    SSAtmoEnvResolvedBody moon;
    SSAtmoEnvPlanetaryResolver::resolveLightRoles(planetary, sun, moon);
    if (sun.mBodyIndex < 0) return 0.f;

    const SSAtmoEnvCelestialBody& home_body = planetary.mBodies[static_cast<size_t>(home)];
    const LLVector3 dir = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
        sun.mDirection, home_body.mAxialTiltDeg, home_body.mLatitudeDeg, phase);
    return dir.mV[VZ];
}

// Write-if-changed walk of every sky parameter: keyframes, then modulation, then setters only where values differ.
void SSAtmoEnvApplier::applySky(const SSAtmoEnvTrack& track, F64 phase,
                                const SSAtmoEnvSkyModulation& mod)
{
    if (!mSky)
    {
        return;
    }

    const SSAtmoEnvAtmosphere& atm = track.mAtmosphere;

    bool dirty = false;
    const bool valid = mSkyCacheValid;
    auto put = [&dirty, valid](auto& cache, const auto& value, auto&& setter)
    {
        if (!valid || !(cache == value))
        {
            cache = value;
            setter(value);
            dirty = true;
        }
    };

    // <SS:Nexii> Authored straight through - the storm's gamma/ambient cuts that used to ride the darkening modulation are retired: they were global and darkened the sky above the deck too. The scene's storm darkening is the deck's ground shadow (moisture-driven through coverage, bound into the deferred sun pass) plus the authored cloud_shadow's ambience job.
    put(mLastAmbient, atm.mAmbientColor.valueAt(phase),
        [this](const LLColor3& v) { mSky->setAmbientColor(v); });
    put(mLastBlueHorizon, atm.mBlueHorizon.valueAt(phase),
        [this](const LLColor3& v) { mSky->setBlueHorizon(v); });
    put(mLastBlueDensity, mod.blueDensity(atm.mBlueDensity.valueAt(phase)),
        [this](const LLColor3& v) { mSky->setBlueDensity(v); });
    put(mLastSunlight, atm.mSunlightColor.valueAt(phase),
        [this](const LLColor3& v) { mSky->setSunlightColor(v); });

    put(mLastHazeHorizon, atm.mHazeHorizon.valueAt(phase),
        [this](F32 v) { mSky->setHazeHorizon(v); });
    put(mLastHazeDensity, atm.mHazeDensity.valueAt(phase),
        [this](F32 v) { mSky->setHazeDensity(v); });
    put(mLastSkyMoisture, mod.skyMoistureLevel(atm.mSkyMoistureLevel.valueAt(phase)),
        [this](F32 v) { mSky->setSkyMoistureLevel(v); });
    put(mLastSkyDroplet, atm.mSkyDropletRadius.valueAt(phase),
        [this](F32 v) { mSky->setSkyDropletRadius(v); });
    put(mLastSkyIce, mod.skyIceLevel(atm.mSkyIceLevel.valueAt(phase)),
        [this](F32 v) { mSky->setSkyIceLevel(v); });
    put(mLastDensityMult, atm.mDensityMultiplier.valueAt(phase),
        [this](F32 v) { mSky->setDensityMultiplier(v); });
    put(mLastDistanceMult, atm.mDistanceMultiplier.valueAt(phase),
        [this](F32 v) { mSky->setDistanceMultiplier(v); });
    put(mLastMaxY, atm.mMaxAltitude.valueAt(phase),
        [this](F32 v) { mSky->setMaxY(v); });
    put(mLastProbeAmbiance, atm.mReflectionProbeAmbiance.valueAt(phase),
        [this](F32 v) { mSky->setReflectionProbeAmbiance(v); });
    put(mLastGamma, atm.mSceneGamma.valueAt(phase),
        [this](F32 v) { mSky->setGamma(v); });
    put(mLastStarBrightness, atm.mStarBrightness.valueAt(phase),
        [this](F32 v) { mSky->setStarBrightness(v); });
    put(mLastMoonBrightness, atm.mMoonBrightness.valueAt(phase) * mMoonSlotBrightness,
        [this](F32 v) { mSky->setMoonBrightness(v); });

    const F32 glow_size = atm.mGlowSize.valueAt(phase);
    const F32 glow_focus = atm.mGlowFocus.valueAt(phase);
    const LLColor3 glow((2.0f - glow_size) * SLIDER_SCALE_GLOW_R,
                        mGlowG,
                        glow_focus * SLIDER_SCALE_GLOW_B);
    put(mLastGlow, glow, [this](const LLColor3& v) { mSky->setGlow(v); });

    const SSAtmoEnvCloudDome& dome = track.mCloudDome;

    // <SS:Nexii> Not put()s - the dome altitude has no LLSettingsSky home to write into. It goes to the cloud and disc shaders straight off this applier, so all that is kept here is the sample: the ANIMATABLE dome height (floor-relative - cirrusAltitudeMetres adds the track's floor back) and the floor itself. The live sky's cloud shadow is the dome band's authored coverage alone (the deck-coverage lift that tracked the layers together is removed - the dome overcast sits as its own layer now), lights the world, and is the ONE density the dome band draws with.
    mCloudDomeAuto = dome.mAuto;
    mCloudDomeHeightM = dome.mHeightM.valueAt(phase);
    mTrackFloorZ = track.mFloorZ;
    mLargeNoiseId = dome.mLargeNoiseTexture.valueAt(phase);

    // <SS:Nexii> The large map's crossfade, only when both ends are authored maps - the gate switches whole octaves between the cloud noise and the large map, so a fade onto or off of None has no honest mix and snaps as it always did.
    mLargeNoiseTo = mLargeNoiseId;
    mLargeNoiseBlend = 0.f;
    {
        LLUUID large_from, large_to;
        F32 large_blend = 0.f;
        if (dome.mLargeNoiseTexture.blendAt(phase, large_from, large_to, large_blend)
            && large_from.notNull() && large_to.notNull())
        {
            mLargeNoiseTo = large_to;
            mLargeNoiseBlend = (large_to != large_from) ? large_blend : 0.f;
        }
    }

    // Same for the horizon clip: no LLSettingsSky home either - the sky pool reads it straight off this applier when it binds the dome shader, and turns it into the lower dome's depth gate (LL_SHADER_CONST_HORIZON_DEPTH in skyF.glsl).
    mHorizonClip = atm.mHorizonClip;

    put(mLastCloudColor, dome.mColor.valueAt(phase),
        [this](const LLColor3& v) { mSky->setCloudColor(v); });
    put(mLastCloudCoverage, mod.cloudCoverage(dome.mCoverage.valueAt(phase)),
        [this](F32 v) { mSky->setCloudShadow(v); });
    // <SS:Nexii> The Scale dial: the live sky's cloud_scale uniform stays on the FROM keyframe's FIXED value through a blend, never valueAt's continuous lerp. A lerped scale fed into ss_plane_base's divisor would zoom the tile continuously mid-fade - the clouds visibly stretch and shrink against the fixed-height band, reading as the layer changing altitude when it has not moved. The TO endpoint and the eased weight ride the cloudScaleTo/ cloudScaleBlend pair instead, and the shader crossfades the two fixed-ratio plates. Off-blend the fallback is valueAt (the single-keyframe/plain value, unchanged).
    {
        F32 scale_from = 0.f, scale_to = 0.f, scale_blend = 0.f;
        const bool fading = dome.mScale.blendAt(phase, scale_from, scale_to, scale_blend)
                         && scale_from > 0.001f && scale_to > 0.001f;
        const F32 live_scale = fading ? scale_from : dome.mScale.valueAt(phase);
        put(mLastCloudScale, live_scale,
            [this](F32 v) { mSky->setCloudScale(v); });
        mCloudScaleTo = fading ? scale_to : dome.mScale.valueAt(phase);
        mCloudScaleBlend = fading ? scale_blend : 0.f;
    }
    // <SS:Nexii> Deliberately unmodulated - the one weather mapping that was removed rather than retuned. Storm darkening used to add the dome a little variance for texture, but EEP's variance erodes (cloudsF.glsl: cloudDensity *= 1 - density_variance^2, saturated wherever four noise samples agree), and on the cirrus layer that erosion has nothing to eat into but the overcast sheet max moisture builds: past convection ~0.8 the saturation spreads and the sheet tears open - gaps, and the disturbed lookup peeling at their edges. The bump predates the volumetric split, when this layer WAS the storm deck; the deck now carries the storm's texture (its churn and flow) and its weight (coverage), so convection leaves the dome alone.
    put(mLastCloudVariance, dome.mVariance.valueAt(phase),
        [this](F32 v) { mSky->setCloudVariance(v); });
    // <SS:Nexii> No authored scroll rate any more - the dome/cirrus band moves with the WIND, scaled to its altitude by the boundary-layer gradient (the drift above). The stock cloud_scroll_rate path is pinned at zero so the band's only motion is that wind drift.
    put(mLastCloudScroll, LLVector2::zero,
        [this](const LLVector2& v) { mSky->setCloudScrollRate(v); });

    const LLColor3 cloud_density(dome.mDensityX.valueAt(phase),
                                 dome.mDensityY.valueAt(phase),
                                 dome.mDensityD.valueAt(phase));
    put(mLastCloudDensity, cloud_density,
        [this](const LLColor3& v) { mSky->setCloudPosDensity1(v); });

    const LLColor3 cloud_detail(dome.mDetailX.valueAt(phase),
                                dome.mDetailY.valueAt(phase),
                                dome.mDetailD.valueAt(phase));
    put(mLastCloudDetail, cloud_detail,
        [this](const LLColor3& v) { mSky->setCloudPosDensity2(v); });

    LLUUID cloud_noise = dome.mNoiseTexture.valueAt(phase);
    if (cloud_noise.isNull())
    {
        cloud_noise = LLSettingsSky::GetDefaultCloudNoiseTextureId();
    }
    put(mLastCloudNoise, cloud_noise,
        [this](const LLUUID& v) { mSky->setCloudNoiseTextureId(v); });

    // <SS:Nexii> The dome noise's crossfade. valueAt holds the fade's FROM keyframe, so the sky's own noise id above keeps the current map while the pair below hands the sky pool both ends of the fade - it rebinds its two noise channels and puts the eased weight into the stock blend factor. Both ids resolve through the default cloud noise so the pair is concrete.
    mDomeNoiseFrom = cloud_noise;
    mDomeNoiseTo = cloud_noise;
    mDomeNoiseBlend = 0.f;
    {
        LLUUID noise_from, noise_to;
        F32 noise_blend = 0.f;
        if (dome.mNoiseTexture.blendAt(phase, noise_from, noise_to, noise_blend))
        {
            if (noise_from.isNull()) noise_from = LLSettingsSky::GetDefaultCloudNoiseTextureId();
            if (noise_to.isNull())   noise_to = LLSettingsSky::GetDefaultCloudNoiseTextureId();
            mDomeNoiseFrom = noise_from;
            mDomeNoiseTo = noise_to;
            mDomeNoiseBlend = (noise_to != noise_from) ? noise_blend : 0.f;
        }
    }

    mSkyCacheValid = true;

    if (dirty)
    {
        mSky->update();
    }
}

// Write-if-changed walk of the water parameters.
void SSAtmoEnvApplier::applyWater(const SSAtmoEnvTrack& track, F64 phase,
                                  const SSAtmoEnvSkyModulation& mod)
{
    if (!mWater || !mDefaultWater)
    {
        return;
    }

    const SSAtmoEnvWater& water = track.mWater;

    bool dirty = false;
    const bool valid = mWaterCacheValid;
    auto put = [&dirty, valid](auto& cache, const auto& value, auto&& setter)
    {
        if (!valid || !(cache == value))
        {
            cache = value;
            setter(value);
            dirty = true;
        }
    };

    // <SS:Nexii> The fog's light. The underwater fog's own in-scatter term (waterFogF.glsl) carries no light of its own - the colour is added to every underwater pixel exactly as authored - so a fog colour authored for day burns at full strength through the night: the sea lit from below that a moonless sky shows. Unless the track authors the fog as EMISSIVE (mFogEmissive - the deliberate fullbright look), the applied colour is scaled by the luminance of whichever body is currently the sky's light - the sun's attenuated diffuse by day, the moon's own diffuse by night, near zero with the moon down - so the fog tracks the same light the rest of the scene gets from this sky. The cosecant crush makes the handover continuous: a setting sun's diffuse is already black at centre-set, where the moon's 0.001 brightness floor takes over. SSAtmoWaterFogLit reverts the whole behaviour to the constant authored fog for A/B comparison and fallback.
    static LLCachedControl<bool> ss_fog_lit(gSavedSettings, "SSAtmoWaterFogLit", true);
    LLColor3 fog_color = water.mFogColor.valueAt(phase);
    if (ss_fog_lit && !water.mFogEmissive.valueAt(phase))
    {
        const LLColor3 fog_light = mSky->getIsSunUp() ? mSky->getSunDiffuse() : mSky->getMoonDiffuse();
        const F32 fog_lum = llclamp(0.2126f * fog_light.mV[0]
                                  + 0.7152f * fog_light.mV[1]
                                  + 0.0722f * fog_light.mV[2], 0.f, 1.f);
        fog_color *= fog_lum;
    }
    put(mLastFogColor, fog_color,
        [this](const LLColor3& v) { mWater->setWaterFogColor(v); });
    put(mLastFogDensity, water.mFogDensity.valueAt(phase),
        [this](F32 v) { mWater->setWaterFogDensity(v); });
    put(mLastFogMod, mod.waterFogModifier(water.mUnderwaterModifier.valueAt(phase)),
        [this](F32 v) { mWater->setFogMod(v); });
    put(mLastFresnelScale, water.mFresnelScale.valueAt(phase),
        [this](F32 v) { mWater->setFresnelScale(v); });
    put(mLastFresnelOffset, water.mFresnelOffset.valueAt(phase),
        [this](F32 v) { mWater->setFresnelOffset(v); });

    LLUUID normal_map = water.mNormalMap.valueAt(phase);
    if (normal_map.isNull())
    {
        normal_map = mDefaultWater->getNormalMapID();
    }
    put(mLastNormalMap, normal_map,
        [this](const LLUUID& v) { mWater->setNormalMapID(v); });

    // <SS:Nexii> The normal map's crossfade. valueAt holds the fade's FROM keyframe, so the put above keeps the current map at the fade's start; the partner and the eased weight ride the stock next-channel plumbing (setNextNormalMapID -> updateSettings -> the pool's two bump bindings; the weight itself the pool reads live at bind time). Both ids resolve through the default water's normal map so the pair is concrete, and a fade between two keyframes that resolve to the same map is skipped.
    LLUUID normal_next = normal_map;
    F32 normal_blend = 0.f;
    {
        LLUUID normal_from, normal_to;
        F32 blend = 0.f;
        if (water.mNormalMap.blendAt(phase, normal_from, normal_to, blend))
        {
            if (normal_from.isNull()) normal_from = mDefaultWater->getNormalMapID();
            if (normal_to.isNull())   normal_to = mDefaultWater->getNormalMapID();
            normal_next = normal_to;
            normal_blend = (normal_to != normal_from) ? blend : 0.f;
        }
    }

    bool blend_dirty = false;
    if (!mWaterCacheValid || !(mLastNormalMapNext == normal_next))
    {
        mLastNormalMapNext = normal_next;
        mWater->setNextNormalMapID(normal_next);
        blend_dirty = true;
    }
    if (!mWaterCacheValid || llabs(mLastNormalBlend - normal_blend) > 1.0e-4f)
    {
        mLastNormalBlend = normal_blend;
        mWater->setBlendWeight(normal_blend);
    }

    const LLVector3 normal_scale(water.mNormalScaleX.valueAt(phase),
                                 water.mNormalScaleY.valueAt(phase),
                                 water.mNormalScaleZ.valueAt(phase));
    put(mLastNormalScale, normal_scale,
        [this](const LLVector3& v) { mWater->setNormalScale(v); });

    put(mLastWave1, water.mLargeWaveSpeed.valueAt(phase),
        [this](const LLVector2& v) { mWater->setWave1Dir(v); });
    put(mLastWave2, water.mSmallWaveSpeed.valueAt(phase),
        [this](const LLVector2& v) { mWater->setWave2Dir(v); });

    put(mLastScaleAbove, water.mRefractionScaleAbove.valueAt(phase),
        [this](F32 v) { mWater->setScaleAbove(v); });
    put(mLastScaleBelow, water.mRefractionScaleBelow.valueAt(phase),
        [this](F32 v) { mWater->setScaleBelow(v); });
    put(mLastBlur, water.mBlurMultiplier.valueAt(phase),
        [this](F32 v) { mWater->setBlurMultiplier(v); });

    mWaterCacheValid = true;

    if (dirty || blend_dirty)
    {
        mWater->update();
    }
}

// Write-if-changed walk against the pristine default water, for tracks with no water plane.
void SSAtmoEnvApplier::applyWaterDefaults()
{
    if (!mWater || !mDefaultWater)
    {
        return;
    }

    bool dirty = false;
    const bool valid = mWaterCacheValid;
    auto put = [&dirty, valid](auto& cache, const auto& value, auto&& setter)
    {
        if (!valid || !(cache == value))
        {
            cache = value;
            setter(value);
            dirty = true;
        }
    };

    put(mLastFogColor, mDefaultWater->getWaterFogColor(),
        [this](const LLColor3& v) { mWater->setWaterFogColor(v); });
    put(mLastFogDensity, mDefaultWater->getWaterFogDensity(),
        [this](F32 v) { mWater->setWaterFogDensity(v); });
    put(mLastFogMod, mDefaultWater->getFogMod(),
        [this](F32 v) { mWater->setFogMod(v); });
    put(mLastFresnelScale, mDefaultWater->getFresnelScale(),
        [this](F32 v) { mWater->setFresnelScale(v); });
    put(mLastFresnelOffset, mDefaultWater->getFresnelOffset(),
        [this](F32 v) { mWater->setFresnelOffset(v); });
    put(mLastNormalMap, mDefaultWater->getNormalMapID(),
        [this](const LLUUID& v) { mWater->setNormalMapID(v); });

    // <SS:Nexii> The defaults walk carries no crossfade: park the partner on the default map and the weight at zero, so a track that just lost its water plane cannot leave a fade behind.
    if (!mWaterCacheValid || !(mLastNormalMapNext == mDefaultWater->getNormalMapID()))
    {
        mLastNormalMapNext = mDefaultWater->getNormalMapID();
        mWater->setNextNormalMapID(mDefaultWater->getNormalMapID());
        dirty = true;
    }
    if (!mWaterCacheValid || mLastNormalBlend != 0.f)
    {
        mLastNormalBlend = 0.f;
        mWater->setBlendWeight(0.f);
    }
    put(mLastNormalScale, mDefaultWater->getNormalScale(),
        [this](const LLVector3& v) { mWater->setNormalScale(v); });
    put(mLastWave1, mDefaultWater->getWave1Dir(),
        [this](const LLVector2& v) { mWater->setWave1Dir(v); });
    put(mLastWave2, mDefaultWater->getWave2Dir(),
        [this](const LLVector2& v) { mWater->setWave2Dir(v); });
    put(mLastScaleAbove, mDefaultWater->getScaleAbove(),
        [this](F32 v) { mWater->setScaleAbove(v); });
    put(mLastScaleBelow, mDefaultWater->getScaleBelow(),
        [this](F32 v) { mWater->setScaleBelow(v); });
    put(mLastBlur, mDefaultWater->getBlurMultiplier(),
        [this](F32 v) { mWater->setBlurMultiplier(v); });

    mWaterCacheValid = true;

    if (dirty)
    {
        mWater->update();
    }
}

// Places every body: the two EEP light slots by the resolver's rules, billboards for the rest, and the sky poked to rebuild when discs move.
void SSAtmoEnvApplier::applyCelestial(const SSAtmoEnvTrack& track, F64 phase)
{
    if (!mSky)
    {
        return;
    }

    const SSAtmoEnvPlanetary& planetary = track.mPlanetary;

    const S32 home_index = planetary.homeBodyIndex();
    std::vector<S32> emitters;
    if (home_index >= 0)
    {
        emitters = planetary.lightEmitterIndices();
    }
    // <SS:Nexii> The dominant-light handover only means something when there is a light to dominate - no emitters leaves the stock single-lightnorm switch in place (lightSlotsValid).
    mLightSlotsValid = !emitters.empty();

    const F32 tilt_deg = (home_index >= 0)
        ? planetary.mBodies[static_cast<size_t>(home_index)].mAxialTiltDeg
        : 0.f;
    const F32 lat_deg = (home_index >= 0)
        ? planetary.mBodies[static_cast<size_t>(home_index)].mLatitudeDeg
        : 0.f;

    {
        const F32 lat = lat_deg * DEG_TO_RAD;
        mObserverPole.setVec(0.f, cosf(lat), sinf(lat));
    }

    const std::vector<SSAtmoEnvResolvedBody> sky_bodies = (home_index >= 0)
        ? SSAtmoEnvPlanetaryResolver::resolveSky(planetary)
        : std::vector<SSAtmoEnvResolvedBody>();

    const std::vector<LLVector3> world_pos = (home_index >= 0)
        ? SSAtmoEnvPlanetaryResolver::resolveWorldPositions(planetary)
        : std::vector<LLVector3>();

    mMoonSlotBrightness = 1.f;

    S32 moon_slot_body = -1;

    S32 debug_slot_sun = -1;
    S32 debug_slot_moon = -1;

    S32 sun_slot_body = -1;

    mSunSlotEmissive = false;
    mMoonSlotEmissive = false;
    mSunSlotPhaseShaded = true;
    mMoonSlotPhaseShaded = true;
    mSunSlotSunDir = LLVector3::z_axis;
    mSunSlotSunlight = 1.f;
    mMoonSlotSunlight = 1.f;
    mSunSlotDiscFraction = 1.f;
    mMoonSlotDiscFraction = 1.f;
    mSunSlotAngularDeg = 0.53f;
    mMoonSlotAngularDeg = 0.53f;
    mSunRiseFraction = 0.f;
    mSunSlotDir = LLVector3::z_axis;

    F32 ss_sun_phys_radius = 0.f;
    F32 ss_sun_phys_dist = 0.f;

    LLVector3 sun_dir = -LLVector3::z_axis;
    LLVector3 moon_dir = -LLVector3::z_axis;
    F32 sun_scale = 1.f;
    F32 moon_scale = 1.f;
    // <SS:Nexii> EEP's default sun id is null, which means "no disc" - the stock pool only draws a sun face that has a texture (see lldrawpoolwlsky's tex_a/tex_b gate). So the stand-in here is the blank-sun disc ASSET, the same drawable default the billboards use.
    LLUUID sun_texture = LLSettingsSky::GetBlankSunTextureId();
    LLUUID moon_texture = LLSettingsSky::GetDefaultMoonTextureId();

    if (!emitters.empty())
    {
        SSAtmoEnvResolvedBody sun_resolved;
        SSAtmoEnvResolvedBody moon_resolved;
        SSAtmoEnvPlanetaryResolver::resolveLightRoles(planetary, sky_bodies,
                                                      sun_resolved, moon_resolved);
        const S32 sun_body = sun_resolved.mBodyIndex;
        const S32 moon_body = moon_resolved.mBodyIndex;
        moon_slot_body = moon_body;

        // <SS:Nexii> The null-texture fallback follows the BODY's kind, not the slot it landed in: a textureless SUN-kind body shows a sun disc in either slot, anything else the stock moon disc. Both stand-ins are real assets - EEP's own default sun id is null and a null id would drop the disc entirely (a null custom texture means "the stock disc", not "no disc").
        auto fallbackFor = [&planetary](S32 body_index) -> LLUUID
        {
            const bool is_sun_kind = planetary.mBodies[static_cast<size_t>(body_index)].mKind
                == SSAtmoEnvCelestialBody::SUN;
            return is_sun_kind ? LLSettingsSky::GetBlankSunTextureId()
                               : LLSettingsSky::GetDefaultMoonTextureId();
        };

        if (sun_body >= 0)
        {
            const SSAtmoEnvCelestialBody& body =
                planetary.mBodies[static_cast<size_t>(sun_body)];
            debug_slot_sun = sun_body;
            mSunSlotEmissive = body.mEmissive;
            mSunSlotPhaseShaded = body.mPhaseShaded;
            sun_slot_body = sun_body;
            sun_dir = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
                sun_resolved.mDirection, tilt_deg, lat_deg, phase);
            sun_scale = celestialDiscScale(sun_resolved.mAngularDiameterDeg,
                                           ss_disc_fraction(body.mDiscPadding),
                                           SS_ATMOENV_SUN_QUAD_DEG);
            mSunSlotDiscFraction = ss_disc_fraction(body.mDiscPadding);
            mSunSlotAngularDeg = sun_resolved.mAngularDiameterDeg;

            ss_sun_phys_radius = body.mDiameterM * 0.5f;
            ss_sun_phys_dist  = sun_resolved.mDistance / llmax(planetary.mSunPlanetScale, 0.001f);
            sun_texture = body.mCustomTexture.notNull()
                ? body.mCustomTexture : fallbackFor(sun_body);
        }
        if (moon_body >= 0)
        {
            const SSAtmoEnvCelestialBody& body =
                planetary.mBodies[static_cast<size_t>(moon_body)];
            debug_slot_moon = moon_body;
            mMoonSlotEmissive = body.mEmissive;
            mMoonSlotPhaseShaded = body.mPhaseShaded;
            moon_dir = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
                moon_resolved.mDirection, tilt_deg, lat_deg, phase);
            moon_scale = celestialDiscScale(moon_resolved.mAngularDiameterDeg,
                                            ss_disc_fraction(body.mDiscPadding),
                                            SS_ATMOENV_MOON_QUAD_DEG);
            mMoonSlotDiscFraction = ss_disc_fraction(body.mDiscPadding);
            mMoonSlotAngularDeg = moon_resolved.mAngularDiameterDeg;
            moon_texture = body.mCustomTexture.notNull()
                ? body.mCustomTexture : fallbackFor(moon_body);
        }
    }

    S32 lamp = -1;
    for (size_t i = 0; i < planetary.mBodies.size(); ++i)
    {
        if (planetary.mBodies[i].mKind != SSAtmoEnvCelestialBody::SUN) continue;
        if (lamp < 0 || planetary.mBodies[i].mDiameterM > planetary.mBodies[(size_t)lamp].mDiameterM)
        {
            lamp = (S32)i;
        }
    }

    auto illuminate = [&](S32 body_index, LLVector3& out_dir, F32& out_light)
    {
        out_dir = LLVector3::z_axis;
        out_light = 1.f;
        if (lamp < 0 || home_index < 0 || body_index < 0) return;
        if (world_pos.size() <= (size_t)llmax(lamp, llmax(body_index, home_index))) return;

        const LLVector3 body = world_pos[(size_t)body_index];
        const LLVector3 to_sun_world = world_pos[(size_t)lamp] - body;

        out_dir = to_sun_world;
        if (out_dir.normalize() < 0.0001f) { out_dir = LLVector3::z_axis; return; }
        out_dir = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
            out_dir, tilt_deg, lat_deg, phase);

        const LLVector3 home = world_pos[(size_t)home_index];
        const LLVector3 home_to_body = body - home;
        const F32 behind = home_to_body * (-out_dir);
        if (behind <= 0.f) return;

        const LLVector3 perp = home_to_body - (-out_dir) * behind;
        const F32 miss = perp.magVec();
        const F32 home_r = planetary.mBodies[(size_t)home_index].mDiameterM * 0.5f;
        if (home_r <= 0.f) return;

        const F32 star_r = planetary.mBodies[(size_t)lamp].mDiameterM * 0.5f;
        const F32 star_dist = (world_pos[(size_t)lamp] - home).magVec();
        const F32 spread = (star_dist > 1.f) ? (star_r / star_dist) * behind : 0.f;

        const F32 umbra = llmax(home_r - spread, 0.f);
        const F32 penumbra = home_r + spread;
        if (miss >= penumbra) return;

        static const F32 ECLIPSE_FLOOR = 0.05f;

        const F32 across = (penumbra > umbra) ? ((miss - umbra) / (penumbra - umbra)) : 1.f;
        out_light = ECLIPSE_FLOOR + (1.f - ECLIPSE_FLOOR) * llclamp(across, 0.f, 1.f);
    };

    if (sun_slot_body >= 0)
    {
        LLVector3 slot_sun_dir;
        F32 slot_light = 1.f;
        illuminate(sun_slot_body, slot_sun_dir, slot_light);
        mSunSlotSunDir = slot_sun_dir;
        mSunSlotSunlight = slot_light;
    }

    mMoonSunDir.setZero();
    if (moon_slot_body >= 0)
    {
        LLVector3 moon_sun_dir;
        F32 moon_light = 1.f;
        illuminate(moon_slot_body, moon_sun_dir, moon_light);

        const SSAtmoEnvCelestialBody& slot_body =
            planetary.mBodies[static_cast<size_t>(moon_slot_body)];
        if (slot_body.mPhaseShaded && !slot_body.mEmissive)
        {
            const F32 lit = 0.5f + 0.5f * (moon_sun_dir * -moon_dir);
            mMoonSlotBrightness = llclamp(lit, 0.f, 1.f) * moon_light;
            mMoonSunDir = moon_sun_dir;
            mMoonSlotSunlight = moon_light;
        }
    }

    // <SS:Nexii> Every resolved body becomes a billboard - no angular-size floor. An early cut dropped bodies smaller than ~1-2 pixels against the advertent "a subpixel quad shimmers", but a realistically-scaled system - the authoring norm, per the defaults - puts every planet save the home's moon below that band (Jupiter is 0.014 deg), so the floor silently hid whole systems from both the sky AND the body-location debug overlay. There is no need for it: celestialDiscScale's CELESTIAL_SCALE_MIN floor already keeps the quad at a stable visible size, and a far planet at the floored size is a dim dot that behaves, whereas dropping it turned the body into nothing at all.
    mBillboards.clear();
    for (const SSAtmoEnvResolvedBody& body : sky_bodies)
    {
        if (std::find(emitters.begin(), emitters.end(), body.mBodyIndex) != emitters.end())
        {
            continue;
        }
        SSAtmoEnvBillboard billboard;
        billboard.mDirection = SSAtmoEnvPlanetaryResolver::resolveObserverDirection(
            body.mDirection, tilt_deg, lat_deg, phase);
        billboard.mAngularDiameterDeg = body.mAngularDiameterDeg;
        const SSAtmoEnvCelestialBody& authored =
            planetary.mBodies[static_cast<size_t>(body.mBodyIndex)];
        billboard.mTexture = authored.mCustomTexture;
        billboard.mIsSun = (authored.mKind == SSAtmoEnvCelestialBody::SUN);
        billboard.mBodyIndex = body.mBodyIndex;
        billboard.mEmissive = authored.mEmissive;
        billboard.mPhaseShaded = authored.mPhaseShaded;
        billboard.mDiscFraction = ss_disc_fraction(authored.mDiscPadding);
        illuminate(body.mBodyIndex, billboard.mSunDirection, billboard.mSunlight);
        mBillboards.push_back(billboard);
    }

    mDebugMarks.clear();
    if (home_index >= 0)
    {
        auto add_mark = [&](S32 body_index, const LLVector3& dir, F32 diameter,
                            F32 sunlight, bool is_sun_slot, bool is_moon_slot)
        {
            if (body_index < 0) return;
            const SSAtmoEnvCelestialBody& b = planetary.mBodies[static_cast<size_t>(body_index)];

            DebugMark mark;
            mark.mName = b.mName.empty() ? llformat("body %d", body_index) : b.mName;
            mark.mDirection = dir;
            mark.mAngularDiameterDeg = diameter;
            mark.mSunlight = sunlight;
            mark.mEmissive = b.mEmissive;
            mark.mIsSunSlot = is_sun_slot;
            mark.mIsMoonSlot = is_moon_slot;
            mDebugMarks.push_back(mark);
        };

        if (debug_slot_sun >= 0)
        {
            add_mark(debug_slot_sun, sun_dir, mSunSlotAngularDeg, 1.f, true, false);
        }
        if (debug_slot_moon >= 0)
        {
            add_mark(debug_slot_moon, moon_dir, mMoonSlotAngularDeg,
                     mMoonSlotBrightness, false, true);
        }
        for (const SSAtmoEnvBillboard& bb : mBillboards)
        {
            add_mark(bb.mBodyIndex, bb.mDirection, bb.mAngularDiameterDeg,
                     bb.mSunlight, false, false);
        }
    }

    bool dirty = false;
    const bool valid = mCelestialCacheValid;
    auto put = [&dirty, valid](auto& cache, const auto& value, auto&& setter)
    {
        if (!valid || !(cache == value))
        {
            cache = value;
            setter(value);
            dirty = true;
        }
    };

    bool celestial_moved = false;
    put(mLastSunDir, sun_dir,
        [this, &celestial_moved](const LLVector3& v)
        { mSky->setSunRotation(quat_from_direction(v)); celestial_moved = true; });
    put(mLastMoonDir, moon_dir,
        [this, &celestial_moved](const LLVector3& v)
        { mSky->setMoonRotation(quat_from_direction(v)); celestial_moved = true; });
    put(mLastSunScale, sun_scale,
        [this, &celestial_moved](F32 v) { mSky->setSunScale(v); celestial_moved = true; });
    put(mLastMoonScale, moon_scale,
        [this, &celestial_moved](F32 v) { mSky->setMoonScale(v); celestial_moved = true; });
    put(mLastSunTexture, sun_texture,
        [this](const LLUUID& v) { mSky->setSunTextureId(v); });
    put(mLastMoonTexture, moon_texture,
        [this](const LLUUID& v) { mSky->setMoonTextureId(v); });

    // <SS:Nexii> The sun's horizon-band share, from the RESOLVED direction and disc - see sunRiseFraction. Full the whole time the disc's centre stands at or above the horizon - the condition the authored skies painted against, stock's own glow and sunlight run at their full sun values from centre-rise to centre-set - and easing out over the twilight BELOW it: the disc's light hits the atmosphere long before the disc itself reaches the horizon and keeps lighting it long after, so the ramp runs DOWN from the horizon crossing instead of across the quad's span. Sizing the band across the disc (the first cut) scaled the glow by the risen SHARE of the disc, which halved the sunset exactly at the horizon where the authored skies put it at full strength, and ended it the frame the last sliver slipped under - a sunrise that only exists while the disc does. The fade spans the disc's own radii (SS_SUN_TWILIGHT_RADII, floored and capped in degrees) and is smoothstepped, so both ends land gently: a rising sun carries near-full glow from its first sliver and the dusk's tail settles flat into the night. The half-angle below is the DISC's, not the quad's. sun_scale is the quad scale, inflated by 1/disc_fraction so padded art lands its visible disc on the authored diameter - fed straight through, the quad's half-angle would size the band (and the dome shaders' held airmass) off the transparent margin, stretching every sunset by exactly that factor. Multiplying the fraction back out lands the band on the disc the quads actually draw. <SS:Nexii> The light's size base is the authored sky's sun - its physical (scale-1.0) angular size, never the disc the Disc Perception dials make it draw. The lighting keeps the EEP benchmark whatever the drawn discs do: the twilight band, the airmass floor and the dusk glow all size themselves off the authored sun.
    F32 light_diameter_deg = mSunSlotAngularDeg;
    F32 light_sun_scale = sun_scale;
    if (ss_sun_phys_radius > 0.f && ss_sun_phys_dist > 0.f)
    {
        light_diameter_deg = RAD_TO_DEG * 2.f * atanf(llclamp(ss_sun_phys_radius / ss_sun_phys_dist, 0.f, 1.f));
        light_sun_scale = celestialDiscScale(light_diameter_deg, mSunSlotDiscFraction, SS_ATMOENV_SUN_QUAD_DEG);
    }

    F32 half_tan = light_sun_scale * mSunSlotDiscFraction * HEAVENLY_BODY_FACTOR * 0.5f; // llvosky.cpp's SUN_DISK_RADIUS
    if (gSky.mVOSkyp.notNull())
    {
        half_tan = light_sun_scale * mSunSlotDiscFraction * HEAVENLY_BODY_FACTOR * gSky.mVOSkyp->getSun().getDiskRadius();
    }
    const F32 half_sin = half_tan / sqrtf(1.f + half_tan * half_tan);
    mSunSlotRadius = half_sin;
    if (half_sin > 1e-6f)
    {
        const F32 fade = llmin(llmax(SS_SUN_TWILIGHT_RADII * half_sin,
                                     sinf(SS_SUN_TWILIGHT_MIN_DEG * DEG_TO_RAD)),
                               sinf(SS_SUN_TWILIGHT_MAX_DEG * DEG_TO_RAD));
        const F32 t = llclamp((sun_dir.mV[VZ] + fade) / fade, 0.f, 1.f);
        mSunRiseFraction = t * t * (3.f - 2.f * t);
    }
    else
    {
        mSunRiseFraction = (sun_dir.mV[VZ] > 0.f) ? 1.f : 0.f;
    }

    // ...and the direction itself, for everything that must keep aiming at the SUN through the
    // rise band - see sunSlotDirection.
    mSunSlotDir = sun_dir;

    // <SS:Nexii> The two slots' scene-light contributions, each carried through the atmosphere on its OWN elevation - the same exp(-light_atten * 1/elev) cosecant curve calcAtmosphericVars applies to whichever body lightnorm belongs to (atmosphericsFuncs.glsl), replicated here against the sky values applySky just wrote so the CPU side of the handover cannot drift from the shader's own formula. The shader takes the per-channel max of the two, which makes the scene light the DOMINANT emitter's: the moon keeps the world lit at its own value until the rising sun genuinely outshines it, instead of lightnorm's flip at centre-rise swapping a high moon's mild attenuation for the horizon sun's crushed one and dropping everything to near-black in a frame. Bounded by the brighter single-light value, so the handover can never overexpose, and a lone sun is exactly the stock line - its own contribution, through its own elevation. The slots hold the top-2 light emitters (SSAtmoEnvPlanetaryResolver::resolveLightRoles), so two suns hand over by the same rule: the bigger star holds the light until the other's contribution crosses it. Deliberately unscaled by the moon's authored brightness and phase - stock's scene light never scaled by them either (they drive the disc, the glow and the water), so night stays exactly the stock night.
    if (mLightSlotsValid)
    {
        LLColor3 light_atten = (mLastBlueDensity + LLColor3(mLastHazeDensity * 0.25f))
            * (mLastDensityMult * mLastMaxY);
        // <SS:Nexii> Attenuation is a density product: negative is never physical, and here it is not merely wrong but explosive. A slot below the horizon reads 1/1e-6 for its cosecant, so one negative component drives exp() to +inf and the shader's max() then floods every lit pixel to white. NaN clamps to zero the same way (llmax answers the second argument for a NaN first), so a wrecked sky value degrades to an unattenuated slot, never a white screen.
        light_atten.mV[0] = llmax(light_atten.mV[0], 0.f);
        light_atten.mV[1] = llmax(light_atten.mV[1], 0.f);
        light_atten.mV[2] = llmax(light_atten.mV[2], 0.f);
        auto slot_light = [&light_atten, this](const LLVector3& dir)
        {
            const F32 cosec = 1.f / llmax(1e-6f, dir.mV[VZ]);
            return componentMult(mLastSunlight, componentExp(light_atten * -cosec));
        };
        mSunSlotLight = slot_light(sun_dir);
        mMoonSlotLight = slot_light(moon_dir);
    }
    else
    {
        mSunSlotLight = LLColor3(0.f, 0.f, 0.f);
        mMoonSlotLight = LLColor3(0.f, 0.f, 0.f);
        // No emitters - no disc to hold an airmass for either.
        mSunSlotRadius = 0.f;
    }

    mCelestialCacheValid = true;

    if (dirty)
    {
        mSky->update();
    }

    if (celestial_moved && gSky.mVOSkyp.notNull())
    {
        gSky.mVOSkyp->forceSkyUpdate();
    }
}
