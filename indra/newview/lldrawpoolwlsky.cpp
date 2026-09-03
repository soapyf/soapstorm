/**
 * @file lldrawpoolwlsky.cpp
 * @brief LLDrawPoolWLSky class implementation
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lldrawpoolwlsky.h"

#include "llrendertarget.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "llerror.h"
#include "llface.h"
#include "llimage.h"
#include "llrender.h"
#include "llenvironment.h"
#include "llglslshader.h"
#include "llgl.h"

#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewercamera.h"
#include "pipeline.h"
#include "llsky.h"
#include "llvowlsky.h"
#include "llsettingsvo.h"
#include "llviewercontrol.h"
#include "llagent.h" // <SS:Nexii> for gAgent.getRegion()
#include "ssatmoenvapplier.h" // <SS:Nexii> Atmo Magic celestial billboards
#include "llviewertexturelist.h" // <SS:Nexii> fetching the dome's authored large-scale noise
#include "ssvolcloud.h" // <SS:Nexii> the deck's top, for the dome band's horizon melt

extern bool gCubeSnapshot;

static LLStaticHashedString sCamPosLocal("camPosLocal");
static LLStaticHashedString sCustomAlpha("custom_alpha");
static LLStaticHashedString sRegionOffset("region_offset"); // <SS:Nexii> cloud parallax
static LLStaticHashedString sCloudDrift("ss_cloud_drift"); // <SS:Nexii> wind-driven cloud travel

// <SS:Nexii> The dome band's Scale crossfade pair (cloudsF.glsl, SSAtmoEnvApplier::cloudScaleTo / cloudScaleBlend): the ground mapping tiles the band by the sky's own cloud_scale AND by this partner, and mixes the two endpoint-scale renderings by the eased weight - the pattern is never zoomed, which is what made the old interpolated Scale read as erratic cloud motion. Zero unless the day cycle sits between two Scale keyframes.
static LLStaticHashedString sCloudScaleTo("ss_cloud_scale_to");
static LLStaticHashedString sCloudScaleBlend("ss_cloud_scale_blend");


// <SS:Nexii> Atmo Magic celestial discs - see ssCelestialF.glsl. Every look constant lives in the shader; these are the per-body handles.
static LLStaticHashedString sDiscColor("ss_disc_color");
static LLStaticHashedString sBodyDir("ss_body_dir");
static LLStaticHashedString sSunDir("ss_sun_dir");
static LLStaticHashedString sQuadRight("ss_quad_right");
static LLStaticHashedString sQuadUp("ss_quad_up");
static LLStaticHashedString sSunlight("ss_sunlight");
static LLStaticHashedString sEmissive("ss_emissive");
static LLStaticHashedString sPhaseShaded("ss_phase_shaded");
static LLStaticHashedString sDaylight("ss_daylight");
static LLStaticHashedString sFaceRot("ss_face_rot");
static LLStaticHashedString sDiscFraction("ss_disc_fraction");

// <SS:Nexii> The dome band's virtual ALTITUDE above the CAMERA, metres - the deck-tracking merge (SSAtmoEnvApplier::cloudDomeAltitudeMetres) read against the camera's own height, so the shell the shader intersects stays put while the camera climbs. Both the curved-deck UVs (cloudsF) and the disc occlusion (ssCelestialF, by depth) derive from it - one authority, which is why it is resolved in the applier and only read here.
static LLStaticHashedString sCloudAltM("ss_cloud_alt_m");

// <SS:Nexii> The camera's distance from the home planet's CENTRE, metres (home radius + camera height) - the curvature term for the dome cloud's deck mapping (cloudsF.glsl): the deck is a spherical shell, terminates at its own curved horizon and fades there. Zero falls back to the flat-deck mapping.
static LLStaticHashedString sPlanetOrbit("ss_planet_orbit_m");

// <SS:Nexii> The horizon clip's uniform (SSAtmoEnvAtmosphere::mHorizonClip) - the on/off gate the dome fragment shader tests before it writes the lower half of the dome into its own depth slot (LL_SHADER_CONST_HORIZON_DEPTH, a shader const on both sky programs; see skyF.glsl). The sky's layer stack reads 1.0 stars, 0.999999 stock sun, 0.999991 stock moon, 0.99999 haze dome and Atmo discs, 0.99998 cloud layer - and the clip slot, one step nearer than the clouds, is what the below-horizon half of the dome takes: the discs, the stars, the stock sun and moon and the clouds are all deeper than it, so they fail LEQUAL behind it and the horizon line hides whatever has set. The VALUE lives in the shader const table (gShaderConstsVal in llglslshader.cpp) rather than here, so there is one number to keep honest; this side only ever passes the gate.
static LLStaticHashedString sHorizonClip("ss_horizon_clip");

// <SS:Nexii> The sky dome's below-horizon ray treatment (skyV.glsl): 1 mirrors below-horizon rays instead of stock's -32000 collapse, 0 is exactly stock. Zero unless an ACTIVE Atmo environment is driving the sky, so an enabled-but- idle viewer's plain EEP sky shades below the horizon the way stock always has.
static LLStaticHashedString sHorizonMirror("ss_horizon_mirror");

// <SS:Nexii> The dome cloud band's own depth slot (cloudsV.glsl): 0.99998 when an ACTIVE Atmo environment is driving the sky - the slot orders the band against the Atmo discs - and 0 for stock's untouched projection squash.
static LLStaticHashedString sCloudDepth("ss_cloud_depth");

// <SS:Nexii> The stock atmosphere ray lift (skyV.glsl, cloudsV.glsl): 1 computes the haze ray 50 m above the geometry - stock's legacy fudge, which once paired with the stock sun disc's own legacy 50 m drop (sunDiscV.glsl), glow hotspot and drawn disc wrong together - and 0 computes it at the true direction the Atmo discs draw at (ssCelestialV.glsl carries no offset). 1 unless the Atmo discs own the sky, so the glow lands on the disc exactly when there is an unfudged disc to land on: with the stock discs drawing, the dome keeps the stock pairing.
static LLStaticHashedString sRayLift("ss_ray_lift");

// <SS:Nexii> Weather-driven optics (ssOptics in skyF.glsl): the corona, the 22/46 deg halos and the aligned-plate arcs, all rendered at true angular positions from the light direction and the weather's drive amplitudes. ss_optic_gate is the on/off: 0 unless an ACTIVE Atmo environment is pushing at least one drive, which is what keeps the stock halo_map strip pristine for idle viewers.
static LLStaticHashedString sOpticGate("ss_optic_gate");
static LLStaticHashedString sOpticActive("ss_optic_active");
static LLStaticHashedString sOpticLight("ss_optic_light");
static LLStaticHashedString sOpticCorona("ss_optic_corona");
static LLStaticHashedString sOpticHalo22("ss_optic_halo22");
static LLStaticHashedString sOpticHalo46("ss_optic_halo46");
static LLStaticHashedString sOpticAlign("ss_optic_align");
static LLStaticHashedString sOpticSunCol("ss_optic_sun_col");

// <SS:Nexii> The physical rainbow's gate (SSAtmoRainbow, ss_rainbow in skyF.glsl).
static LLStaticHashedString sRainbowGate("ss_rainbow_gate");

// Whether Atmo Magic should draw the discs at all. Its own shader replaces
// the stock sun and moon ones only while an Atmo environment owns the sky -
// enabled but idle, the stock EEP discs go through untouched stock code.
static bool ss_atmo_discs_active()
{
    // Gated on an ACTIVE Atmo environment, matching the cloud parallax below: an enabled-but-idle
    // viewer falling back to a plain EEP sky must keep the stock discs, the stock alpha-blended
    // compositing and the stock disc shaders, pixel for pixel. (A stock alpha-blended disc REPLACES
    // the sky behind it, and against a hot authored sunset glow that replacement reads darker than
    // the surroundings - which is exactly why the Atmo discs composite additively when the applier
    // IS driving the sky.)
    static LLCachedControl<bool> ss_atmo(gSavedSettings, "SSAtmoEnabled", false);
    return ss_atmo
           && SSAtmoEnvApplier::instance().isActive()
           && gSSCelestialProgram.isComplete();
}

// The quad axes LLVOSky builds for a body at `dir`, rebuilt here because the
// fragment shader has to put the sphere back together in the same frame the
// vertices were laid out in (updateHeavenlyBodyGeometry: right = dir x z,
// then up = right x dir).
static void ss_quad_axes(const LLVector3& dir, LLVector3& out_right, LLVector3& out_up)
{
    out_right = dir % LLVector3::z_axis;
    if (out_right.normalize() < 0.001f) out_right = LLVector3::x_axis;
    out_up = out_right % dir;
    out_up.normalize();
}

// One body's worth of uniforms.
static void ss_bind_disc(const LLColor4& tint, const LLVector3& body_dir,
                         const LLVector3& sun_dir, F32 sunlight,
                         bool emissive, bool phase_shaded,
                         F32 disc_fraction)
{
    LLVector3 right, up;
    ss_quad_axes(body_dir, right, up);

    gSSCelestialProgram.uniform4fv(sDiscColor, 1, tint.mV);
    gSSCelestialProgram.uniform3fv(sBodyDir, 1, body_dir.mV);
    gSSCelestialProgram.uniform3fv(sSunDir, 1, sun_dir.mV);
    gSSCelestialProgram.uniform3fv(sQuadRight, 1, right.mV);
    gSSCelestialProgram.uniform3fv(sQuadUp, 1, up.mV);
    gSSCelestialProgram.uniform1f(sSunlight, sunlight);
    gSSCelestialProgram.uniform1f(sEmissive, emissive ? 1.f : 0.f);
    gSSCelestialProgram.uniform1f(sPhaseShaded, phase_shaded ? 1.f : 0.f);

    // The art's disc as a fraction of the quad - the phase-shaded sphere is inscribed in the
    // DISC, not the quad, so a padded texture needs its normal mapped into the art's central
    // fraction. Always set: an unset GL uniform reads zero, and a zero fraction divides by it.
    gSSCelestialProgram.uniform1f(sDiscFraction, disc_fraction);

    // How far this body's face is turned, relative to the quad it is drawn
    // on: the parallactic angle.
    //
    // The quad is a billboard whose up axis points at the zenith (see
    // ss_quad_axes), so without this the art is pinned to the HORIZON - the
    // maria in the same place on screen at moonrise as at moonset, while
    // the terminator sweeps across them because that is computed from the
    // sun's real direction. Half the face fixed to the ground and half to
    // the sky, which is a worse answer than either alone.
    //
    // A tidally locked body does keep one face turned toward its planet, so
    // the billboard is right about WHICH face. What it cannot know is the
    // roll: a real moon's north points at the celestial pole, not at the
    // observer's zenith, and the angle between those two is nothing at
    // culmination and tens of degrees near rise and set at temperate
    // latitudes. That rotation is why a crescent sits like a bowl low in
    // the sky and tips over as it climbs.
    //
    // Measured about the view direction, from the quad's own up axis to the
    // pole-ward one.
    const LLVector3 pole = SSAtmoEnvApplier::instance().observerPole();
    LLVector3 pole_tangent = pole - body_dir * (pole * body_dir);
    F32 cos_q = 1.f;
    F32 sin_q = 0.f;
    if (pole_tangent.normalize() > 0.001f)
    {
        cos_q = pole_tangent * up;
        sin_q = (up % pole_tangent) * body_dir;
    }
    gSSCelestialProgram.uniform2f(sFaceRot, cos_q, sin_q);

    // How much daylight the OBSERVER is standing in - nothing to do with
    // this body, which is why it is the sun's own elevation rather than
    // anything in the arguments. The shader uses it to fade earthshine out;
    // see SS_EARTHSHINE.
    //
    // From the sky's sun rather than the applier's resolved slot so that it
    // matches the sky actually being drawn even mid-transition, when the two
    // can briefly disagree.
    LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
    const F32 sun_alt = sky ? sky->getSunDirection().mV[VZ] : 0.f;

    // Across the same band twilight happens in: full night by about six
    // degrees below the horizon, full day by about nine above.
    const F32 daylight = llclamp((sun_alt + 0.1f) / 0.25f, 0.f, 1.f);
    gSSCelestialProgram.uniform1f(sDaylight, daylight * daylight * (3.f - 2.f * daylight));


    // No airlight uniform any more. Estimating the haze over a disc from a
    // single sky-wide colour was always going to be wrong somewhere - it
    // was far too dark against a bright daytime sky, leaving the moon
    // crisp and pasted-on - and there is no need to estimate it at all now
    // that the disc is added to the sky the dome has already drawn.
}


static LLGLSLShader* cloud_shader = NULL;
static LLGLSLShader* sky_shader   = NULL;
static LLGLSLShader* sun_shader   = NULL;
static LLGLSLShader* moon_shader  = NULL;

static float sStarTime;

LLDrawPoolWLSky::LLDrawPoolWLSky(void) :
    LLDrawPool(POOL_WL_SKY)
{
}

LLDrawPoolWLSky::~LLDrawPoolWLSky()
{
}

LLViewerTexture *LLDrawPoolWLSky::getDebugTexture()
{
    return NULL;
}

void LLDrawPoolWLSky::beginDeferredPass(S32 pass)
{
    sky_shader = &gDeferredWLSkyProgram;
    cloud_shader = &gDeferredWLCloudProgram;

    sun_shader = &gDeferredWLSunProgram;

    moon_shader = &gDeferredWLMoonProgram;
}

void LLDrawPoolWLSky::endDeferredPass(S32 pass)
{
    sky_shader   = nullptr;
    cloud_shader = nullptr;
    sun_shader   = nullptr;
    moon_shader  = nullptr;

    // clear the depth buffer so haze shaders can use unwritten depth as a mask
    glClear(GL_DEPTH_BUFFER_BIT);
}

void LLDrawPoolWLSky::renderDome(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader * shader,
                                 F32 scale, bool depth_write) const
{
    llassert_always(NULL != shader);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();

    //chop off translation
    if (LLPipeline::sReflectionRender && camPosLocal.mV[2] > 256.f)
    {
        gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], 256.f-camPosLocal.mV[2]*0.5f);
    }
    else
    {
        gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    }


    // the windlight sky dome works most conveniently in a coordinate system
    // where Y is up, so permute our basis vectors accordingly.
    gGL.rotatef(120.f, 1.f / F_SQRT3, 1.f / F_SQRT3, 1.f / F_SQRT3);

    gGL.scalef(scale, scale, scale);

    gGL.translatef(0.f,-camHeightLocal, 0.f);

    // Draw WL Sky
    shader->uniform3f(sCamPosLocal, 0.f, camHeightLocal, 0.f);

    // depth_write rides in only from the haze pass when the horizon clip is on - the lower dome
    // must store its nearer depth slot for the discs, stars and clouds to fail against (see
    // LL_SHADER_CONST_HORIZON_DEPTH). The cloud pass keeps the stock mask-off behaviour.
    gSky.mVOWLSkyp->drawDome(depth_write);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}

extern LLPointer<LLImageGL> gEXRImage;

static bool use_hdri_sky()
{
    static LLCachedControl<F32> hdri_split(gSavedSettings, "RenderHDRISplitScreen", 1.f);
    static LLCachedControl<bool> irradiance_only(gSavedSettings, "RenderHDRIIrradianceOnly", false);

    return gCubeSnapshot && (!irradiance_only || !gPipeline.mReflectionMapManager.isRadiancePass()) ? gEXRImage.notNull() : // always use HDRI for reflection probes when available
        gEXRImage.notNull() ? hdri_split > 0.f : // fallback to EEP sky when split screen is zero
        false; // no HDRI available, always use EEP sky

}

void LLDrawPoolWLSky::renderSkyHazeDeferred(const LLVector3& camPosLocal, F32 camHeightLocal) const
{
    if (!gSky.mVOSkyp)
    {
        return;
    }

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    if (gPipeline.canUseWindLightShaders() && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY))
    {
        if (use_hdri_sky())
        {
            sky_shader = &gEnvironmentMapProgram;
            sky_shader->bind();
            S32 idx = sky_shader->enableTexture(LLShaderMgr::ENVIRONMENT_MAP);
            if (idx > -1)
            {
                gGL.getTexUnit(idx)->bind(gEXRImage);
            }

            static LLCachedControl<F32> hdri_exposure(gSavedSettings, "RenderHDRIExposure", 0.0f);
            static LLCachedControl<F32> hdri_rotation(gSavedSettings, "RenderHDRIRotation", 0.f);
            static LLCachedControl<F32> hdri_split(gSavedSettings, "RenderHDRISplitScreen", 1.f);
            static LLStaticHashedString hdri_split_screen("hdri_split_screen");

            LLMatrix3 rot;
            rot.setRot(0.f, hdri_rotation*DEG_TO_RAD, 0.f);

            sky_shader->uniform1f(LLShaderMgr::SKY_HDR_SCALE, powf(2.f, hdri_exposure));
            sky_shader->uniformMatrix3fv(LLShaderMgr::DEFERRED_ENV_MAT, 1, GL_FALSE, (F32*) rot.mMatrix);
            sky_shader->uniform1f(hdri_split_screen, gCubeSnapshot ? 1.f : hdri_split);
        }
        else
        {
            sky_shader->bind();
        }

        LLGLSPipelineDepthTestSkyBox sky(true, true);

        sky_shader->uniform1i(LLShaderMgr::CUBE_SNAPSHOT, gCubeSnapshot ? 1 : 0);

        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        LLViewerTexture* rainbow_tex = gSky.mVOSkyp->getRainbowTex();
        LLViewerTexture* halo_tex  = gSky.mVOSkyp->getHaloTex();

        sky_shader->bindTexture(LLShaderMgr::RAINBOW_MAP, rainbow_tex);
        sky_shader->bindTexture(LLShaderMgr::HALO_MAP,  halo_tex);

        F32 moisture_level  = (float)psky->getSkyMoistureLevel();
        F32 droplet_radius  = (float)psky->getSkyDropletRadius();
        F32 ice_level       = (float)psky->getSkyIceLevel();

        // hobble halos and rainbows when there's no light source to generate them
        if (!psky->getIsSunUp() && !psky->getIsMoonUp())
        {
            moisture_level = 0.0f;
            ice_level      = 0.0f;
        }

        sky_shader->uniform1f(LLShaderMgr::MOISTURE_LEVEL, moisture_level);
        sky_shader->uniform1f(LLShaderMgr::DROPLET_RADIUS, droplet_radius);
        sky_shader->uniform1f(LLShaderMgr::ICE_LEVEL, ice_level);

        // <SS:Nexii> The physical rainbow's gate (ss_rainbow in skyF.glsl): the double-bow look with the interior wash cut, the reversed secondary, the red low-sun grade and the white moonbow - versus the stock single strip. A plain bool, not weather-driven: the look is the same correction at every sky.
        static LLCachedControl<bool> s_rainbow_phys(gSavedSettings, "SSAtmoRainbow", true);
        sky_shader->uniform1f(sRainbowGate, s_rainbow_phys ? 1.f : 0.f);

        sky_shader->uniform1f(LLShaderMgr::SUN_MOON_GLOW_FACTOR, psky->getSunMoonGlowFactor());

        sky_shader->uniform1i(LLShaderMgr::SUN_UP_FACTOR, psky->getIsSunUp() ? 1 : 0);

        // <SS:Nexii> The horizon clip: when an active Atmo environment asks for it, the dome fragment stage writes the lower half of the dome into the clip's depth slot (see sHorizonClip above and skyF.glsl) and drawDome is asked to let it - the only draw in this pool that writes depth, because it is the only one that has anything to hide. The uniform is the gate only: 0 leaves the fragment stage's write on the harmless 1.0 path and drawDome keeps the depth mask off, exactly as stock.
        const SSAtmoEnvApplier& atmo_applier = SSAtmoEnvApplier::instance();
        const bool horizon_clip = atmo_applier.isActive() && atmo_applier.horizonClip();
        sky_shader->uniform1f(sHorizonClip, horizon_clip ? 1.f : 0.f);

        // <SS:Nexii> The below-horizon ray mirror rides the same gate: only an active Atmo environment may depart from stock's -32000 collapse.
        sky_shader->uniform1f(sHorizonMirror, atmo_applier.isActive() ? 1.f : 0.f);

        // ...and the ray lift drops only when the Atmo discs own the sky, taking the
        // glow's hotspot with it onto the disc - see sRayLift above.
        sky_shader->uniform1f(sRayLift, ss_atmo_discs_active() ? 0.f : 1.f);

        // <SS:Nexii> Weather-driven optics (ssOptics in skyF.glsl). The halo grows with the DISC, not the disc's centre: while the sun's rise band is live - full strength while the disc is up, easing out through the dusk below the horizon - the optics ramp on the SAME horizon-band share the glow ramps on (ss_sun_rise, SSAtmoEnvApplier::sunRiseFraction) and aim at the sun's true direction, so a low sun's halos burn in from the first sliver above the horizon and fade out through the twilight after it sets, never popping the moment the centre crosses. SUN ONLY for now - the optics are the sun's own, and moonlight optics are not wired up yet, so once the band is spent there are none. The gate is active-env AND the sun's rise band live AND at least one drive speaking: leave it all absent and the stock halo_map strip renders as always.
        const SSAtmoEnvSkyModulation& ssm = atmo_applier.lastModulation();

        float optic_gate = 0.f;
        LLVector3 optic_dir(0.f, 1.f, 0.f);
        // <SS:Nexii> Hoisted out of the active-env block below: the same horizon-band share gates the optics, and it was out of scope there.
        float sun_rise = 0.f;
        if (atmo_applier.isActive())
        {
            sun_rise = atmo_applier.sunRiseFraction();
            if (sun_rise > 0.001f)
            {
                optic_gate = sun_rise;
                // The light-norm permutation toLightNorm() applies (world x,y,z -> ogl y,z,x),
                // inlined here rather than widening that private helper's access; sunSlotDirection
                // is the sun's TRUE direction from the applier, valid through the whole rise band.
                const LLVector3& sun_dir = atmo_applier.sunSlotDirection();
                optic_dir.set(sun_dir.mV[1], sun_dir.mV[2], sun_dir.mV[0]);
            }
        }
        if (optic_gate > 0.001f)
        {
            const F32 max_drive = llmax(ssm.mCorona,
                                        llmax(ssm.mIceHalo, llmax(ssm.mIceHalo46, ssm.mCrystalAlign)));
            optic_gate *= (max_drive > 0.001f) ? 1.f : 0.f;
        }

        sky_shader->uniform1f(sOpticGate, optic_gate);
        sky_shader->uniform1f(sOpticActive, atmo_applier.isActive() ? 1.f : 0.f);
        sky_shader->uniform3fv(sOpticLight, 1, optic_dir.mV);
        sky_shader->uniform1f(sOpticCorona, ssm.mCorona);
        sky_shader->uniform1f(sOpticHalo22, ssm.mIceHalo);
        sky_shader->uniform1f(sOpticHalo46, ssm.mIceHalo46);
        sky_shader->uniform1f(sOpticAlign, ssm.mCrystalAlign);

        // <SS:Nexii> The optics' light colour is produced IN the vertex shader now (skyV.glsl: vary_ss_optic_sun_col) - the dome's own capped-glow sun light along the light's ray, the light the sunset band actually renders with - so the halos, arcs and sundogs keep their sunrise/sunset hue down through the horizon band and below it. The old CPU replica of the uncapped beam (sunSlotLight's 1/max(1e-6, sin(elev)) cosecant) underflowed to zero within a degree of the horizon and fell back to the raw near-white authored sun colour - the white snap. This bind is the vertex shader's MOONLIGHT fallback only (no sun band live): moonlight optics are not wired to their own light yet, so they keep a faint cool-white tint rather than borrowing the sun's (or dumping the moon into it).
        sky_shader->uniform3fv(sOpticSunCol, 1, LLColor3(1.f, 1.f, 1.f).mV);

        /// Render the skydome
        renderDome(origin, camHeightLocal, sky_shader, 0.333f, horizon_clip);

        sky_shader->unbind();
    }
}

void LLDrawPoolWLSky::renderStarsDeferred(const LLVector3& camPosLocal) const
{
    if (!gSky.mVOSkyp || use_hdri_sky())
    {
        return;
    }

    LLGLSPipelineBlendSkyBox gls_sky(true, false);

    gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);

    F32 star_alpha = LLEnvironment::instance().getCurrentSky()->getStarBrightness() / 500.0f;

    // If start_brightness is not set, exit
    if(star_alpha < 0.001f)
    {
        LL_DEBUGS("SKY") << "star_brightness below threshold." << LL_ENDL;
        return;
    }

    gDeferredStarProgram.bind();

    LLViewerTexture* tex_a = gSky.mVOSkyp->getBloomTex();
    LLViewerTexture* tex_b = gSky.mVOSkyp->getBloomTexNext();

    F32 blend_factor = (F32)LLEnvironment::instance().getCurrentSky()->getBlendFactor();

    if (tex_a && (!tex_b || (tex_a == tex_b)))
    {
        // Bind current and next sun textures
        gGL.getTexUnit(0)->bind(tex_a);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
        blend_factor = 0;
    }
    else if (tex_b && !tex_a)
    {
        gGL.getTexUnit(0)->bind(tex_b);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
        blend_factor = 0;
    }
    else if (tex_b != tex_a)
    {
        gGL.getTexUnit(0)->bind(tex_a);
        gGL.getTexUnit(1)->bind(tex_b);
    }

    gGL.pushMatrix();
    gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    gGL.rotatef(gFrameTimeSeconds*0.01f, 0.f, 0.f, 1.f);
    gDeferredStarProgram.uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);

    if (LLPipeline::sReflectionRender)
    {
        star_alpha = 1.0f;
    }
    gDeferredStarProgram.uniform1f(sCustomAlpha, star_alpha);

    sStarTime = (F32)LLFrameTimer::getElapsedSeconds() * 0.5f;

    gDeferredStarProgram.uniform1f(LLShaderMgr::WATER_TIME, sStarTime);

    gSky.mVOWLSkyp->drawStars();

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

    gDeferredStarProgram.unbind();

    gGL.popMatrix();
}

void LLDrawPoolWLSky::renderSkyCloudsDeferred(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader* cloudshader) const
{
    if (use_hdri_sky())
    {
        return;
    }

    if (gPipeline.canUseWindLightShaders() && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_CLOUDS) && gSky.mVOSkyp && gSky.mVOSkyp->getCloudNoiseTex())
    {
        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        LLGLSPipelineBlendSkyBox pipeline(true, true);

        cloudshader->bind();

        LLPointer<LLViewerTexture> cloud_noise      = gSky.mVOSkyp->getCloudNoiseTex();
        LLPointer<LLViewerTexture> cloud_noise_next = gSky.mVOSkyp->getCloudNoiseTexNext();

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

        F32 cloud_variance = psky ? (F32)psky->getCloudVariance() : 0.0f;
        F32 blend_factor   = psky ? (F32)psky->getBlendFactor() : 0.0f;

        // <SS:Nexii> Hoisted above the noise bindings: the Atmo crossfade below needs the gate before the stock pair logic runs. (Was declared with the parallax uniforms further down.)
        const bool atmo_env_active = SSAtmoEnvApplier::instance().isActive();

        if (psky->getCloudScrollRate().isExactlyZero())
        {
            blend_factor = 0.f;
        }

        // if we even have sun disc textures to work with...
        if (cloud_noise || cloud_noise_next)
        {
            if (cloud_noise && (!cloud_noise_next || (cloud_noise == cloud_noise_next)))
            {
                // Bind current and next sun textures
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise, LLTexUnit::TT_TEXTURE);
                blend_factor = 0;
            }
            else if (cloud_noise_next && !cloud_noise)
            {
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise_next, LLTexUnit::TT_TEXTURE);
                blend_factor = 0;
            }
            else if (cloud_noise_next != cloud_noise)
            {
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise, LLTexUnit::TT_TEXTURE);
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP_NEXT, cloud_noise_next, LLTexUnit::TT_TEXTURE);
            }
        }

        // <SS:Nexii> Atmo Magic's dome-noise crossfade. The environment's keyframes name the maps, and mid-fade the applier hands over the pair - the sky's own noise id keeps holding the fade's FROM map, so rebind both channels here and put the eased weight into the stock blend factor. This also survives the stock zero-scroll kill above: Atmo's dome drift is its own uniform, so the coupling that silences a static stock sky's blend says nothing about a fading Atmo pair. Fetches on change and caches, like the large map below.
        if (atmo_env_active)
        {
            LLUUID noise_from, noise_to;
            F32 noise_blend = 0.f;
            if (SSAtmoEnvApplier::instance().cloudNoiseBlend(noise_from, noise_to, noise_blend))
            {
                static LLUUID s_noise_from_id;
                static LLPointer<LLViewerTexture> s_noise_from_tex;
                static LLUUID s_noise_to_id;
                static LLPointer<LLViewerTexture> s_noise_to_tex;
                if (noise_from != s_noise_from_id || s_noise_from_tex.isNull())
                {
                    s_noise_from_id = noise_from;
                    s_noise_from_tex = LLViewerTextureManager::getFetchedTexture(
                        noise_from, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
                    s_noise_from_tex->addTextureStats((F32)MAX_IMAGE_AREA);
                }
                if (noise_to != s_noise_to_id || s_noise_to_tex.isNull())
                {
                    s_noise_to_id = noise_to;
                    s_noise_to_tex = LLViewerTextureManager::getFetchedTexture(
                        noise_to, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
                    s_noise_to_tex->addTextureStats((F32)MAX_IMAGE_AREA);
                }
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, s_noise_from_tex, LLTexUnit::TT_TEXTURE);
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP_NEXT, s_noise_to_tex, LLTexUnit::TT_TEXTURE);
                blend_factor = noise_blend;
            }
        }

        cloudshader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);
        cloudshader->uniform1f(LLShaderMgr::CLOUD_VARIANCE, cloud_variance);
        cloudshader->uniform1f(LLShaderMgr::SUN_MOON_GLOW_FACTOR, psky->getSunMoonGlowFactor());

        // <SS:Nexii> Region-relative cloud parallax (doc/atmo_magic_cloud_parallax.md). Gated on an ACTIVE Atmo environment, not just the compiled-in SS_ATMO define: the master toggle bakes the shader variant, but an enabled-yet-idle viewer falling back to a plain EEP sky must leave it pixel-stock - zeros make both additive terms vanish. (The drift below already self-gates: it is zero unless an Atmo environment is driving the sky.)
        LLViewerRegion* region       = gAgent.getRegion();
        F32             region_width = region ? region->getWidth() : REGION_WIDTH_METERS;
        F32             region_off_x = atmo_env_active ? (camPosLocal.mV[VX] - region_width * 0.5f) : 0.f;
        F32             region_off_y = atmo_env_active ? (camPosLocal.mV[VY] - region_width * 0.5f) : 0.f;
        cloudshader->uniform2f(sRegionOffset, region_off_x, region_off_y);

        // ...and how far the deck itself has travelled on the wind. Zero
        // unless an Atmo Magic environment is driving the sky, which is also
        // the only thing that knows what the wind is doing.
        const LLVector2 drift = SSAtmoEnvApplier::instance().cloudDriftMetres();
        cloudshader->uniform2f(sCloudDrift, drift.mV[0], drift.mV[1]);

        // <SS:Nexii> The band's Scale crossfade (SSAtmoEnvApplier::cloudScaleTo/cloudScaleBlend): the fragment ground mapping samples the band at both endpoint scales and blends the two renderings by the eased weight - the sky's own cloud_scale uniform keeps the FROM endpoint. Zero when no Atmo environment drives the sky, or between equal keyframes, which leaves the shader on its single-sample branch - idle EEP skies are untouched.
        cloudshader->uniform1f(sCloudScaleTo, SSAtmoEnvApplier::instance().cloudScaleTo());
        cloudshader->uniform1f(sCloudScaleBlend, SSAtmoEnvApplier::instance().cloudScaleBlend());

        // <SS:Nexii> The dome band's own depth slot (cloudsV.glsl): 0.99998 when an ACTIVE Atmo environment is driving the sky - it orders the band against the Atmo discs - and 0 for stock's untouched projection squash (the file-scope sCloudDepth).
        static const F32 SS_BAND_DEPTH    = 0.99998f;

        // <SS:Nexii> The deck-mapping gate (cloudsF.glsl): 1 computes the dome cloud UVs from the true view ray's intersection with the band's curved deck - per-band parallax, the deck's own horizon curvature and fade - and 0 keeps the stock dome-mesh texcoords.
        static LLStaticHashedString sCloudPlane("ss_cloud_plane");

        // The stock ray lift, riding the same gate as the discs: the deck's glow
        // hotspot and its disc-neighbourhood restore track the true direction while
        // the Atmo discs draw, and keep stock's +50 m when it does not - see
        // sRayLift above.
        cloudshader->uniform1f(sRayLift, ss_atmo_discs_active() ? 0.f : 1.f);

        // The deck-mapping gate.
        cloudshader->uniform1f(sCloudPlane, atmo_env_active ? 1.f : 0.f);

        // <SS:Nexii> The dome band's authored large-scale noise map, when one is set: the broad composition (warp fields, base octave, self-shadow) reads it, the fine octave keeps the cloud noise. Fetched on change and cached - the applier hands over the id, the pool owns the binding. Gate 0 (idle sky, or no map authored) leaves every octave on the cloud noise, exactly as stock.
        static LLStaticHashedString sNoiseLargeOn("ss_noise_large_on");
        static LLUUID s_large_noise_id;
        static LLPointer<LLViewerFetchedTexture> s_large_noise_tex;
        const LLUUID& large_noise_id = SSAtmoEnvApplier::instance().cloudLargeNoiseId();
        bool large_noise_on = false;
        if (atmo_env_active && large_noise_id.notNull())
        {
            if (large_noise_id != s_large_noise_id || s_large_noise_tex.isNull())
            {
                s_large_noise_id = large_noise_id;
                s_large_noise_tex = LLViewerTextureManager::getFetchedTexture(
                    large_noise_id, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
                s_large_noise_tex->addTextureStats((F32)MAX_IMAGE_AREA);
            }
            cloudshader->bindTexture(LLShaderMgr::SS_NOISE_LARGE_MAP, s_large_noise_tex, LLTexUnit::TT_TEXTURE);
            large_noise_on = true;

            // <SS:Nexii> The large map's own crossfade: mid-fade the applier names a second authored map and the eased weight, bound on the partner channel (reserved name - see llshadermgr). No fade running, the partner sits on the SAME map with weight 0, so the shader's mix is a no-op; and the sky's stock blend factor never reaches this uniform - the pair carries its own weight.
            static LLStaticHashedString sNoiseLargeBlend("ss_noise_large_blend");
            const LLUUID& large_noise_next_id = SSAtmoEnvApplier::instance().cloudLargeNoiseNextId();
            const F32 large_noise_blend = SSAtmoEnvApplier::instance().cloudLargeNoiseBlend();
            if (large_noise_blend > 0.f && large_noise_next_id.notNull()
                && large_noise_next_id != large_noise_id)
            {
                static LLUUID s_large_noise_next_id;
                static LLPointer<LLViewerFetchedTexture> s_large_noise_next_tex;
                if (large_noise_next_id != s_large_noise_next_id || s_large_noise_next_tex.isNull())
                {
                    s_large_noise_next_id = large_noise_next_id;
                    s_large_noise_next_tex = LLViewerTextureManager::getFetchedTexture(
                        large_noise_next_id, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
                    s_large_noise_next_tex->addTextureStats((F32)MAX_IMAGE_AREA);
                }
                cloudshader->bindTexture(LLShaderMgr::SS_NOISE_LARGE_MAP_NEXT, s_large_noise_next_tex, LLTexUnit::TT_TEXTURE);
                cloudshader->uniform1f(sNoiseLargeBlend, large_noise_blend);
            }
            else
            {
                cloudshader->bindTexture(LLShaderMgr::SS_NOISE_LARGE_MAP_NEXT, s_large_noise_tex, LLTexUnit::TT_TEXTURE);
                cloudshader->uniform1f(sNoiseLargeBlend, 0.f);
            }
        }
        cloudshader->uniform1f(sNoiseLargeOn, large_noise_on ? 1.f : 0.f);

        // <SS:Nexii> The scale the dome mesh draws at: 0.3325 of the dome radius when Atmo owns the sky, the stock 0.333 when not - twelve metres of headroom over the haze backdrop, see the note at the old single-pass draw.
        const F32 dome_scale = atmo_env_active ? 0.3325f : 0.333f;

        // <SS:Nexii> The planet the deck curves around: the home body's radius plus the camera's height above the region floor - the camera's orbit. A track with no home body falls back to an Earth-sized default (see the applier), so the deck always curves and terminates at its own rim rather than running flat into the world's horizon line.
        const F32 planet_orbit_m = atmo_env_active
            ? SSAtmoEnvApplier::instance().homePlanetRadiusM() + llmax(camPosLocal.mV[VZ], 0.f)
            : 0.f;
        cloudshader->uniform1f(sPlanetOrbit, planet_orbit_m);

        if (!atmo_env_active)
        {
            // Idle EEP sky: one pass, stock cloud shadow from the live settings, stock texcoords,
            // stock squash. Pixel-stock by construction.
            cloudshader->uniform1f(sCloudDepth, 0.f);
            cloudshader->uniform1f(sCloudAltM, 6000.f);
            renderDome(camPosLocal, camHeightLocal, cloudshader, dome_scale);
        }
        else
        {
            // <SS:Nexii> ONE dome band. Two bands over one noise texture with per-band parallax rates ghost apart the moment the camera moves - the same pattern twice, shifted, a second ghost layer. The band IS the cirrus layer: the Sky Dome's animatable height param, floor-relative, brought down only by convection's anvil ramp (cloudDomeAltitudeMetres) - moisture never moves it. Its density is the live sky's cloud shadow - the dome's authored coverage, its own layer after the deck-coverage lift was removed (no moisture term reaches the dome band) - which also dims the world, so band and world light overcast together.
            SSAtmoEnvApplier& applier = SSAtmoEnvApplier::instance();

            const F32 band_world_height_m = applier.cloudDomeAltitudeMetres();
            cloudshader->uniform1f(sCloudAltM, band_world_height_m - camPosLocal.mV[VZ]);

            // <SS:Nexii> The volumetric deck's perceived edge, as an elevation sine over the camera: the deck's top slab seen at the field's own edge distance - the 9800 m the deck's puffs and base veil dissolve by (ssVolCloudF.glsl). The dome band's horizon melt runs up to this line: past the deck's edge the band has no cloud in front of it, so it fades toward the horizon instead of running the overcast sheet flat into it. Zero - no deck, or the camera at/over its top - leaves the old narrow rim melt.
            static LLStaticHashedString sDeckEdgeSin("ss_deck_edge_sin");
            F32 deck_edge_sin = 0.f;
            SSVolCloud* vol = SSVolCloud::getInstance();
            if (vol && !vol->empty())
            {
                const F32 deck_top_m = vol->cloudTopZ() - camPosLocal.mV[VZ];
                if (deck_top_m > 0.f)
                {
                    static const F32 SS_DECK_EDGE_M = 9800.f;
                    deck_edge_sin = deck_top_m / sqrtf(deck_top_m * deck_top_m
                                                       + SS_DECK_EDGE_M * SS_DECK_EDGE_M);
                }
            }
            cloudshader->uniform1f(sDeckEdgeSin, deck_edge_sin);

            cloudshader->uniform1f(sCloudDepth, SS_BAND_DEPTH);
            renderDome(camPosLocal, camHeightLocal, cloudshader, dome_scale);
        }

        cloudshader->unbind();

        // <SS:Nexii> The volumetric layer used to be drawn here, on top of the dome. It is now a late translucent pass instead - see LLPipeline::renderGeomPostDeferred. Drawn in the sky pass it could only ever be part of the backdrop: everything rendered afterwards, water included, painted straight over it, and it had no scene depth to soften itself against.

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
    }
}

void LLDrawPoolWLSky::renderHeavenlyBodies()
{
    if (!gSky.mVOSkyp || use_hdri_sky()) return;

    LLGLSPipelineBlendSkyBox gls_skybox(true, true); // SL-14113 we need moon to write to depth to clip stars behind

    // <SS:Nexii> Atmo Magic's discs are ADDED to the sky rather than composited over it - the whole atmosphere is in front of a celestial body, so the sky already drawn at those pixels is exactly the airlight over the disc. See the note in ssCelestialF.glsl. Only when Atmo Magic owns the sky: the stock discs are built to be composited and would come out as bright smears added to it.
    const bool ss_additive_discs = ss_atmo_discs_active();
    if (ss_additive_discs)
    {
        gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);
    }

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    // <SS:Nexii> Celestial quads onto a true camera-centred shell. LLVOSky::updateHeavenlyBodyGeometry bakes mCameraPosAgent (the sky drawable's position, i.e. the camera) into the sun and moon face vertices, and this pass then translated by the camera origin as well - so every celestial quad sat at camera + cameraPosAgent + dir * HEAVENLY_BODY_DIST instead of camera + dir * HEAVENLY_BODY_DIST. On the ground the extra term is small enough to pass for correct; at altitude it is not. In a 3000m skybox it threw the moon roughly 3000m out along the camera vector, putting the quad within a few hundred metres of the cloud dome (dome radius 15000 scaled by 0.333 in renderDome, so ~5000m out) - two surfaces at nearly the same depth, which is exactly the moon/cloud z-fighting this fixes. Subtracting the baked offset here rather than removing it from LLVOSky keeps the fix to the draw site: the faces' own vertex data is shared with the reflection and glow paths, which expect it in agent space. Our own billboards below add the same term for the same reason, so all three land on one shell. (gSky.mVOSkyp is non-null here - this function returns early above.)
    const LLVector3 shell_origin = origin - gSky.mVOSkyp->getCameraPosAgent();
    gGL.pushMatrix();
    gGL.translatef(shell_origin.mV[0], shell_origin.mV[1], shell_origin.mV[2]);

    LLFace * face = gSky.mVOSkyp->mFace[LLVOSky::FACE_SUN];

    F32 blend_factor = (F32)LLEnvironment::instance().getCurrentSky()->getBlendFactor();
    bool can_use_vertex_shaders = gPipeline.shadersLoaded();
    bool can_use_windlight_shaders = gPipeline.canUseWindLightShaders();


    if (gSky.mVOSkyp->getSun().getDraw() && face && face->getGeomCount())
    {
        LLPointer<LLViewerTexture> tex_a = face->getTexture(LLRender::DIFFUSE_MAP);
        LLPointer<LLViewerTexture> tex_b = face->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP);

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

        // if we even have sun disc textures to work with...
        if (tex_a || tex_b)
        {
            // <SS:Nexii> Atmo Magic draws its own discs - see ss_atmo_discs_active - so a stock environment goes through the untouched path below and this one never runs for it.
            if (ss_atmo_discs_active())
            {
                SSAtmoEnvApplier& atmo = SSAtmoEnvApplier::instance();

                gSSCelestialProgram.bind();
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gSSCelestialProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP,
                    tex_a ? tex_a : tex_b, LLTexUnit::TT_TEXTURE);

                LLSettingsSky::ptr_t atmo_sky = LLEnvironment::instance().getCurrentSky();
                const LLVector3 body_dir = atmo_sky ? atmo_sky->getSunDirection()
                                                    : LLVector3::z_axis;

                // A star in the sun slot lights itself. A body that is not
                // emissive gets the phase and eclipse treatment instead,
                // which is what a moon standing in as someone's sun should
                // look like.
                // White, NOT getSun().getInterpColor().
                //
                // Neither stock disc shader reads the colour uniform the sky
                // pass sets - sunDiscF writes its texture out verbatim and
                // moonF only scales by moon brightness - so nothing has ever
                // depended on that colour being sensible, and it is not: it
                // comes through as black, which multiplied straight into a
                // shader that DOES read it turned both discs black.
                //
                // The disc's own art carries its colour, and how bright it
                // is comes from the light reaching it. A tint on top would
                // be a third opinion; white leaves the other two alone.
                // The disc's own art carries its colour, and how bright it
                // is comes from the light reaching it. A tint on top would
                // be a third opinion; white leaves the other two alone.
                ss_bind_disc(LLColor4::white,
                             body_dir,
                             atmo.sunSlotSunDirection(),
                             atmo.sunSlotSunlight(),
                             atmo.sunSlotEmissive(),
                             atmo.sunSlotPhaseShaded(),
                             atmo.sunSlotDiscFraction());

                face->renderIndexed();

                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gSSCelestialProgram.unbind();
            }
            // if and only if we have a texture defined, render the sun disc
            else if (can_use_vertex_shaders && can_use_windlight_shaders)
            {
                sun_shader->bind();

                if (tex_a && (!tex_b || (tex_a == tex_b)))
                {
                    // Bind current and next sun textures
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                    blend_factor = 0;
                }
                else if (tex_b && !tex_a)
                {
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                    blend_factor = 0;
                }
                else if (tex_b != tex_a)
                {
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                    sun_shader->bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                }

                LLColor4 color(gSky.mVOSkyp->getSun().getInterpColor());
                sun_shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, color.mV);
                sun_shader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);

                face->renderIndexed();

                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

                sun_shader->unbind();
            }
        }
    }

    face = gSky.mVOSkyp->mFace[LLVOSky::FACE_MOON];

    if (gSky.mVOSkyp->getMoon().getDraw() && face && face->getTexture(LLRender::DIFFUSE_MAP) && face->getGeomCount() && moon_shader)
    {
        LLViewerTexture* tex_a = face->getTexture(LLRender::DIFFUSE_MAP);
        LLViewerTexture* tex_b = face->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP);

        LLColor4 color(gSky.mVOSkyp->getMoon().getInterpColor());

        if (can_use_vertex_shaders && can_use_windlight_shaders && (tex_a || tex_b))
        {
            LLSettingsSky::ptr_t moon_sky = LLEnvironment::instance().getCurrentSky();

            // <SS:Nexii> Atmo Magic's own disc shader, when it owns the sky.
            if (ss_atmo_discs_active() && moon_sky)
            {
                SSAtmoEnvApplier& atmo = SSAtmoEnvApplier::instance();

                gSSCelestialProgram.bind();
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gSSCelestialProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP,
                    tex_a ? tex_a : tex_b, LLTexUnit::TT_TEXTURE);

                // White - see the note on the sun above.
                ss_bind_disc(LLColor4::white, moon_sky->getMoonDirection(),
                             atmo.moonSunDirection(),
                             atmo.moonSlotSunlight(),
                             atmo.moonSlotEmissive(),
                             atmo.moonSlotPhaseShaded(),
                             atmo.moonSlotDiscFraction());

                face->renderIndexed();

                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gSSCelestialProgram.unbind();
            }
            else
            {
            moon_shader->bind();

            if (tex_a && (!tex_b || (tex_a == tex_b)))
            {
                // Bind current and next sun textures
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                //blend_factor = 0;
            }
            else if (tex_b && !tex_a)
            {
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                //blend_factor = 0;
            }
            else if (tex_b != tex_a)
            {
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                //moon_shader->bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
            }

            LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

            F32 moon_brightness = (float)psky->getMoonBrightness();

            moon_shader->uniform1f(LLShaderMgr::MOON_BRIGHTNESS, moon_brightness);
            moon_shader->uniform3fv(LLShaderMgr::MOONLIGHT_COLOR, 1, gSky.mVOSkyp->getMoon().getColor().mV);
            moon_shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, color.mV);
            //moon_shader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);
            moon_shader->uniform3fv(LLShaderMgr::DEFERRED_MOON_DIR, 1, psky->getMoonDirection().mV); // shader: moon_dir

            face->renderIndexed();

            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

            moon_shader->unbind();
            }
        }
    }

    // <SS:Nexii> Atmo Magic: the active track's non-emitter celestial bodies as camera-facing textured quads - the design doc Planetary section's "quad/billboard only for v1". The applier publishes an empty vector whenever it is inactive, so this whole block costs one emptiness check when Atmo Magic is off. Drawn after the sun and moon so their quads (and the moon's star-clipping depth write, SL-14113) always land first. The moon shader is reused wholesale: it is the one shader in this pass whose every uniform is per-body suppliable (moon_dir is just the body's direction), and it buys the same horizon fade, moon-brightness scaling, transparent-texel discard and star-clipping depth layer the moon itself gets - a sun-shader body would sit on the stars' depth layer instead and have them poke through it.
    const std::vector<SSAtmoEnvBillboard>& billboards =
        SSAtmoEnvApplier::instance().celestialBillboards();
    if (!billboards.empty() && moon_shader
        && can_use_vertex_shaders && can_use_windlight_shaders
        && gSSCelestialProgram.isComplete())
    {
        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        // Atmo Magic's own disc shader. The billboards are its own bodies, so
        // unlike the two light slots there is no stock path to fall back to -
        // the moon shader used to stand in here, which meant borrowing its
        // moon-specific horizon fade and brightness and then fighting both.
        gSSCelestialProgram.bind();

        // White, for the same reason the two slots use it - see the note
        // there. The body's art carries its colour and the light reaching it
        // carries its brightness.
        const LLColor4 bb_color(LLColor4::white);

        // Sizing matches LLVOSky::updateHeavenlyBodyGeometry's chain for
        // the moon (dist * factor * disk radius * disc scale, plus its
        // near-horizon enlargement), with the disc scale coming from the
        // same diameter mapping the applier feeds setMoonScale - so a
        // billboard body and the moon at equal angular diameter render at
        // equal size, through their whole arc. The mapping runs against the
        // moon slot's quad angle (SS_ATMOENV_MOON_QUAD_DEG, the angle this
        // chain actually draws at scale 1.0) and inflates by the body's art
        // padding, so the VISIBLE disc lands on the authored diameter.
        const F32 disk_radius = gSky.mVOSkyp->getMoon().getDiskRadius();

        for (const SSAtmoEnvBillboard& body : billboards)
        {
            const LLVector3& dir = body.mDirection;

            // Camera-facing frame: horizon-aligned right, then up within
            // the quad's plane, both perpendicular to the view direction.
            // Near zenith/nadir the horizontal cross degenerates; a
            // body's roll is arbitrary (it is a disc), so any fixed
            // horizontal axis serves there.
            LLVector3 bb_right = dir % LLVector3::z_axis;
            if (bb_right.normalize() < 0.001f)
            {
                bb_right = LLVector3::x_axis;
            }
            LLVector3 bb_up = bb_right % dir;
            bb_up.normalize();

            const F32 enlargm_factor = 1.f - dir.mV[VZ];
            const F32 horiz_enlargement = 1.f + enlargm_factor * 0.3f;
            const F32 vert_enlargement = 1.f + enlargm_factor * 0.2f;
            const F32 half_size =
                SSAtmoEnvApplier::celestialDiscScale(body.mAngularDiameterDeg,
                                                     body.mDiscFraction,
                                                     SS_ATMOENV_MOON_QUAD_DEG)
                * HEAVENLY_BODY_DIST * HEAVENLY_BODY_FACTOR * disk_radius;

            // Land on the SAME shell the sun/moon quads occupy, which
            // means carrying the same mCameraPosAgent term their face
            // vertices carry: this pass now subtracts it once from the
            // whole matrix (see shell_origin above), so adding it here
            // cancels out and every celestial quad ends up exactly
            // HEAVENLY_BODY_DIST from the camera. Dropping it instead
            // would put billboards a whole camera-position vector away
            // from the sun and moon, and they would parallax against
            // both as the camera moved.
            const LLVector3 center = dir * HEAVENLY_BODY_DIST
                + gSky.mVOSkyp->getCameraPosAgent();
            const LLVector3 half_right = (horiz_enlargement * half_size) * bb_right;
            const LLVector3 half_up = (vert_enlargement * half_size) * bb_up;

            // A body without a custom texture still shows as a
            // recognisable disc, chosen by the BODY's kind rather than
            // any slot: sun-kind bodies get the stock sun disc, the rest
            // the stock moon disc. The sun's stand-in is the blank-sun
            // ASSET - GetDefaultSunTextureId() is null, meaning "EEP's
            // built-in sun rendering", which a billboard cannot draw.
            const LLUUID tex_id = body.mTexture.notNull()
                ? body.mTexture
                : body.mIsSun ? LLSettingsSky::GetBlankSunTextureId()
                              : LLSettingsSky::GetDefaultMoonTextureId();
            LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(
                tex_id, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
            if (!tex)
            {
                continue;
            }
            // Keep the fetcher feeding full resolution, the way
            // LLVOSky::updateTextures() does for the sun and moon; while
            // still loading, binding draws whatever placeholder the
            // fetched texture currently holds.
            tex->addTextureStats(static_cast<F32>(MAX_IMAGE_AREA));

            // Everything the disc shader needs about this body: where it is,
            // where its own star is, how much of that star's light reaches
            // it, and whether it lights itself or takes a phase.
            //
            // Brightness is a consequence of those rather than an authored
            // dial - see SSAtmoEnvCelestialBody - and every look constant
            // (emissive gain, earthshine, terminator softness) lives in the
            // shader, so there is no magic number on this side at all.
            ss_bind_disc(bb_color, dir, body.mSunDirection, body.mSunlight,
                         body.mEmissive, body.mPhaseShaded, body.mDiscFraction);
            gSSCelestialProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, tex,
                                            LLTexUnit::TT_TEXTURE);

            gGL.begin(LLRender::TRIANGLE_STRIP);
            gGL.texCoord2f(0.f, 1.f);
            gGL.vertex3fv((center - half_right + half_up).mV);
            gGL.texCoord2f(0.f, 0.f);
            gGL.vertex3fv((center - half_right - half_up).mV);
            gGL.texCoord2f(1.f, 1.f);
            gGL.vertex3fv((center + half_right + half_up).mV);
            gGL.texCoord2f(1.f, 0.f);
            gGL.vertex3fv((center + half_right - half_up).mV);
            gGL.end();
            // Flush while this body's texture and moon_dir are still
            // bound - the next iteration rebinds both.
            gGL.flush();
        }

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gSSCelestialProgram.unbind();
    }

    // <SS:Nexii> Back to ordinary compositing for whatever draws next.
    if (ss_additive_discs)
    {
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }

    gGL.popMatrix();
}

void LLDrawPoolWLSky::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_WL_SKY);
    if (!gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY) || gSky.mVOSkyp.isNull())
    {
        return;
    }

    // TODO: remove gSky.mVOSkyp and fold sun/moon into LLVOWLSky
    gSky.mVOSkyp->updateGeometry(gSky.mVOSkyp->mDrawable);

    const F32 camHeightLocal = LLEnvironment::instance().getCamHeight();

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    if (gPipeline.canUseWindLightShaders())
    {
        renderSkyHazeDeferred(origin, camHeightLocal);
        renderHeavenlyBodies();
        if (!gCubeSnapshot)
        {
            renderStarsDeferred(origin);
        }

        if (!gCubeSnapshot || gPipeline.mReflectionMapManager.isRadiancePass()) // don't draw clouds in irradiance maps to avoid popping
        {
            renderSkyCloudsDeferred(origin, camHeightLocal, cloud_shader);
        }
    }
}



LLViewerTexture* LLDrawPoolWLSky::getTexture()
{
    return NULL;
}

void LLDrawPoolWLSky::resetDrawOrders()
{
}

//static
void LLDrawPoolWLSky::cleanupGL()
{
}

//static
void LLDrawPoolWLSky::restoreGL()
{
}
