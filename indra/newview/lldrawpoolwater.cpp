/**
 * @file lldrawpoolwater.cpp
 * @brief LLDrawPoolWater class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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
#include "llfeaturemanager.h"
#include "lldrawpoolwater.h"

#include "ssatmoenvapplier.h" // <SS:Nexii> light size for the glitter path
#include "sswater.h" // <SS:Nexii> the stock-versus-Atmo water plane family gate

#include "llviewercontrol.h"
#include "lldir.h"
#include "llerror.h"
#include "m3math.h"
#include "llrender.h"

#include "llagent.h"        // for gAgent for getRegion for getWaterHeight
#include "llcubemap.h"
#include "lldrawable.h"
#include "llface.h"
#include "llsky.h"
#include "llviewertexturelist.h"
#include "llviewerregion.h"
#include "llvowater.h"
#include "llworld.h"
#include "pipeline.h"
#include "llviewershadermgr.h"
#include "llenvironment.h"
#include "llsettingssky.h"
#include "llsettingswater.h"

bool LLDrawPoolWater::sSkipScreenCopy = false;
bool LLDrawPoolWater::sNeedsReflectionUpdate = true;
bool LLDrawPoolWater::sNeedsDistortionUpdate = true;
F32 LLDrawPoolWater::sWaterFogEnd = 0.f;

extern bool gCubeSnapshot;

// <SS:Nexii> Angular radius of whichever body is lighting the water.
static LLStaticHashedString sLightAngularRadius("ss_light_angular_radius");
static LLStaticHashedString sMoonlit("ss_moonlit");
static LLStaticHashedString sSunUp("ss_sun_up");

// ...and how far the distant sky mirror takes over from the wavy probe tap.
static LLStaticHashedString sSkyReflect("ss_sky_reflect");

LLDrawPoolWater::LLDrawPoolWater() : LLFacePool(POOL_WATER)
{
    // <FS:Zi> Render speedup for water parameters
    gSavedSettings.getControl("RenderWaterMipNormal")->getCommitSignal()->connect(boost::bind(&LLDrawPoolWater::onRenderWaterMipNormalChanged, this));
    onRenderWaterMipNormalChanged();
    // </FS:Zi>
}

LLDrawPoolWater::~LLDrawPoolWater()
{
}

void LLDrawPoolWater::setTransparentTextures(const LLUUID& transparentTextureId, const LLUUID& nextTransparentTextureId)
{
    LLSettingsWater::ptr_t pwater = LLEnvironment::instance().getCurrentWater();
    mWaterImagep[0] = LLViewerTextureManager::getFetchedTexture(!transparentTextureId.isNull() ? transparentTextureId : pwater->GetDefaultTransparentTextureAssetId());
    mWaterImagep[1] = LLViewerTextureManager::getFetchedTexture(!nextTransparentTextureId.isNull() ? nextTransparentTextureId : (!transparentTextureId.isNull() ? transparentTextureId : pwater->GetDefaultTransparentTextureAssetId()));
    mWaterImagep[0]->addTextureStats(1024.f*1024.f);
    mWaterImagep[1]->addTextureStats(1024.f*1024.f);
}

void LLDrawPoolWater::setOpaqueTexture(const LLUUID& opaqueTextureId)
{
    LLSettingsWater::ptr_t pwater = LLEnvironment::instance().getCurrentWater();
    mOpaqueWaterImagep = LLViewerTextureManager::getFetchedTexture(opaqueTextureId);
    mOpaqueWaterImagep->addTextureStats(1024.f*1024.f);
}

void LLDrawPoolWater::setNormalMaps(const LLUUID& normalMapId, const LLUUID& nextNormalMapId)
{
    LLSettingsWater::ptr_t pwater = LLEnvironment::instance().getCurrentWater();
    mWaterNormp[0] = LLViewerTextureManager::getFetchedTexture(!normalMapId.isNull() ? normalMapId : pwater->GetDefaultWaterNormalAssetId());
    mWaterNormp[1] = LLViewerTextureManager::getFetchedTexture(!nextNormalMapId.isNull() ? nextNormalMapId : (!normalMapId.isNull() ? normalMapId : pwater->GetDefaultWaterNormalAssetId()));
    mWaterNormp[0]->addTextureStats(1024.f*1024.f);
    mWaterNormp[1]->addTextureStats(1024.f*1024.f);
}

void LLDrawPoolWater::prerender()
{
    mShaderLevel = LLCubeMap::sUseCubeMaps ? LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_WATER) : 0;
}

S32 LLDrawPoolWater::getNumPostDeferredPasses()
{
    // <SS:Nexii> Water renders at every camera height. This used to gate on the camera's height above the water plane (1024m dating from when that was the skybox top, then MAX_FAR_CLIP once the SS planes went in) on the theory that the surface stops being visible that far up - but the far-field squash pulls the surface into the far disc at any altitude, so the ocean below a sky build is exactly what the pass exists to draw. Faces still frustum-cull individually; an empty pass costs one fullscreen depth copy.
    return 1;
}

void LLDrawPoolWater::beginPostDeferredPass(S32 pass)
{
    LL_PROFILE_GPU_ZONE("water beginPostDeferredPass")
    gGL.setColorMask(true, true);

    if (LLPipeline::sRenderTransparentWater)
    {
        // copy framebuffer contents so far to a texture to be used for
        // reflections and refractions
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

        LLRenderTarget& src = gPipeline.mRT->screen;
        LLRenderTarget& depth_src = gPipeline.mRT->deferredScreen;
        LLRenderTarget& dst = gPipeline.mWaterDis;

        dst.bindTarget();
        gCopyDepthProgram.bind();

        S32 diff_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DIFFUSE_MAP);
        S32 depth_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);

        gGL.getTexUnit(diff_map)->bind(&src);
        gGL.getTexUnit(depth_map)->bind(&depth_src, true);

        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        dst.flush();
    }
}

void LLDrawPoolWater::renderPostDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LLGLDisable blend(GL_BLEND);

    gGL.setColorMask(true, true);

    LLColor3 light_diffuse(0, 0, 0);

    LLEnvironment& environment = LLEnvironment::instance();
    LLSettingsWater::ptr_t pwater = environment.getCurrentWater();
    LLSettingsSky::ptr_t   psky   = environment.getCurrentSky();
    // <FS:Beq> FIRE-34590 - Bugsplat Crash typically in startup state, due to null water.
    if (!pwater || !psky)
    {
        LL_WARNS() << "LLDrawPoolWater::renderPostDeferred: water or sky settings not available" << LL_ENDL;
        return;
    }
    // </FS:Beq>
    LLVector3              light_dir       = environment.getLightDirection();
    bool                   sun_up          = environment.getIsSunUp();
    bool                   moon_up         = environment.getIsMoonUp();
    // <FS:Zi> Render speedup for water parameters
    //bool                   has_normal_mips = gSavedSettings.getBOOL("RenderWaterMipNormal");
    bool                   has_normal_mips = mRenderWaterMipNormal;
    bool                   underwater      = LLViewerCamera::getInstance()->cameraUnderWater();
    // LLColor4               fog_color       = LLColor4(pwater->getWaterFogColor(), 0.f); // <FS:Beq/> set but unused
    // LLColor3               fog_color_linear = linearColor3(fog_color); // <FS:Beq/> set but unused

    if (sun_up)
    {
        light_diffuse += psky->getSunlightColor();
    }
    // moonlight is several orders of magnitude less bright than sunlight,
    // so only use this color when the moon alone is showing
    else if (moon_up)
    {
        // <SS:Nexii> getMoonlightColor() is literally getSunlightColor() - "moon and sun share light color" (llsettingssky.cpp) - so this used to put the SUN's hue on the water at night, which is most of why a moon's glitter path read as a second sun. getMoonDiffuse() is the derived one: the same light through the atmosphere's transmittance and scaled by the moon's own brightness, which now carries its phase. Then shifted toward the scotopic blue EEP already keeps for moon ambient (moonlight_b in calculateLightSettings). Night vision is cool and desaturated; leaving the glitter warm is the tell that it is really sunlight in disguise. Magnitude is set below regardless, so this is purely about hue.
        static const LLColor3 SS_SCOTOPIC(0.66f, 0.66f, 1.2f);
        static const F32 SS_SCOTOPIC_MIX = 0.5f;

        LLColor3 moon_hue = psky->getMoonDiffuse();
        light_diffuse += lerp(moon_hue, SS_SCOTOPIC * moon_hue.length(), SS_SCOTOPIC_MIX);
    }

    // Apply magic numbers translating light direction into intensities
    light_dir.normalize();
    F32 ground_proj_sq = light_dir.mV[0] * light_dir.mV[0] + light_dir.mV[1] * light_dir.mV[1];
    if (0.f < light_diffuse.normalize())  // Normalizing a color? Puzzling...
    {
        light_diffuse *= (1.5f + (6.f * ground_proj_sq));
    }

    // <SS:Nexii> Moonlight is not sunlight. The normalize() above throws the light colour's MAGNITUDE away and the line after it scales whatever is left to a fixed intensity - so the glitter path off the moon came out exactly as strong as the sun's, differing only in tint. On a dark sea that reads as a second sun: the one thing a moon's reflection should not look like. Scaled here rather than by fixing the normalize, because the code above is upstream's (its own comment says as much) and everything else downstream is tuned against the intensity it produces. The sky's own moon brightness drives it, so an author who has turned the moon up gets a stronger glade and one who has turned it off gets none.
    if (!sun_up && moon_up)
    {
        // The moon's brightness now carries its PHASE as well as its
        // authored level (see SSAtmoEnvApplier::applySky), so this scales
        // with it: a crescent lays down a fainter path than a full moon,
        // and a new moon lays down none. No floor for that reason - a floor
        // would put a glitter path under a moon that is not there.
        static const F32 MOON_GLINT_SPAN = 0.45f;  // a full moon, nothing like the sun
        const F32 moon_bright = llclamp(psky->getMoonBrightness(), 0.f, 1.f);
        light_diffuse *= MOON_GLINT_SPAN * moon_bright;
    }

    LLTexUnit::eTextureFilterOptions filter_mode = has_normal_mips ? LLTexUnit::TFO_ANISOTROPIC : LLTexUnit::TFO_POINT;

    // NOTE: unused. Kept as upstream wrote it - like fog_color above, this
    // local is computed and never read, so dimming it for the moon (as an
    // earlier pass here did) achieved nothing but implying that it mattered.
    LLColor4      specular(sun_up ? psky->getSunlightColor() : psky->getMoonlightColor());
    F32           phase_time = (F32) LLFrameTimer::getElapsedSeconds() * 0.5f;
    LLGLSLShader *shader     = nullptr;

    // One pass, one of two shaders.  Void water and region water share state.
    // There isn't a good reason anymore to really have void water run in a separate pass.
    // It also just introduced a bunch of weird state consistency stuff that we really don't need.
    // Not to mention, re-binding the the same shader and state for that shader is kind of wasteful.
    // - Geenz 2025-02-11
    // select shader
    if (underwater)
    {
        shader = &gUnderWaterProgram;
    }
    else
    {
        shader = &gWaterProgram;
    }

    gPipeline.bindDeferredShader(*shader, nullptr, &gPipeline.mWaterDis);

    LLViewerTexture* tex_a = mWaterNormp[0];
    LLViewerTexture* tex_b = mWaterNormp[1];

    F32 blend_factor = (F32)pwater->getBlendFactor();

    if (tex_a && (!tex_b || (tex_a == tex_b)))
    {
        tex_a->setFilteringOption(filter_mode);
        shader->bindTexture(LLViewerShaderMgr::BUMP_MAP, tex_a);
        blend_factor = 0; // only one tex provided, no blending
    }
    else if (tex_b && !tex_a)
    {
        tex_b->setFilteringOption(filter_mode);
        shader->bindTexture(LLViewerShaderMgr::BUMP_MAP, tex_b);
        blend_factor = 0; // only one tex provided, no blending
    }
    else if (tex_b != tex_a)
    {
        tex_a->setFilteringOption(filter_mode);
        tex_b->setFilteringOption(filter_mode);
        shader->bindTexture(LLViewerShaderMgr::BUMP_MAP, tex_a);
        shader->bindTexture(LLViewerShaderMgr::BUMP_MAP2, tex_b);
    }

    shader->bindTexture(LLShaderMgr::WATER_EXCLUSIONTEX, &gPipeline.mWaterExclusionMask);

    shader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);

    F32      fog_density = pwater->getModifiedWaterFogDensity(underwater);

    shader->bindTexture(LLShaderMgr::WATER_SCREENTEX, &gPipeline.mWaterDis);
    // <FS:Beq> set but unused "fog_color"
    // if (mShaderLevel == 1)
    // {
    //     fog_color.mV[VALPHA] = (F32)(log(fog_density) / log(2));
    // }
    // </FS:Beq>

    F32 water_height = environment.getWaterHeight();
    F32 camera_height = LLViewerCamera::getInstance()->getOrigin().mV[2];
    shader->uniform1f(LLShaderMgr::WATER_WATERHEIGHT, camera_height - water_height);
    shader->uniform1f(LLShaderMgr::WATER_TIME, phase_time);
    shader->uniform3fv(LLShaderMgr::WATER_EYEVEC, 1, LLViewerCamera::getInstance()->getOrigin().mV);

    // <SS:Nexii> Atmo water far-field squash (ss_squash: knee, cap, ring reach - see waterV.glsl). Keyed on the family swap: stock water never wears Atmo geometry, so it gets zeros - passthrough - and never inherits a stale band from a previous Atmo frame.
    {
        static LLStaticHashedString ss_squash("ss_squash");
        if (SSWaterWorld::atmoWaterLive())
        {
            SSWaterWorld* water_world = SSWaterWorld::getInstance();
            shader->uniform3f(ss_squash, water_world->squashKnee(), water_world->squashCap(),
                              water_world->squashReach());
        }
        else
        {
            shader->uniform3f(ss_squash, 0.f, 0.f, 0.f);
        }
    }

    shader->uniform3fv(LLShaderMgr::WATER_SPECULAR, 1, light_diffuse.mV);

    shader->uniform2fv(LLShaderMgr::WATER_WAVE_DIR1, 1, pwater->getWave1Dir().mV);
    shader->uniform2fv(LLShaderMgr::WATER_WAVE_DIR2, 1, pwater->getWave2Dir().mV);

    shader->uniform3fv(LLShaderMgr::WATER_LIGHT_DIR, 1, light_dir.mV);

    // <SS:Nexii> How big the light in the sky actually is. The water shades with a PUNCTUAL light - a point, zero angular size - so the glitter path's spread comes entirely from surface roughness. That is the old single-sun assumption: a body drawn as a two-degree disc and one drawn at half a degree laid down exactly the same reflection, which is what made the water disagree with the sky above it. Handing the shader the angular radius lets it widen the specular lobe to match what is being reflected. Toggleable as a whole: with SSAtmoWaterPunctualLight off, the water's punctual treatment reverts to stock - point-light angular size and no moon punctual - for A/B comparison and taste. The toggle off must hold that contract exactly: a point light has NO angular radius, so the stock fallback (the 0.53 degree sun) only applies while the feature is on and no active Atmo environment is driving the sky.
    static LLCachedControl<bool> ss_punctual(gSavedSettings, "SSAtmoWaterPunctualLight", true);

    F32 light_angular_radius = 0.f;   // a point light - stock's assumption
    if (ss_punctual)
    {
        if (SSAtmoEnvApplier::instance().isActive())
        {
            const F32 diameter_deg = sun_up
                ? SSAtmoEnvApplier::instance().sunSlotAngularDeg()
                : SSAtmoEnvApplier::instance().moonSlotAngularDeg();
            // The VISIBLE disc's diameter, same figure the celestial quads draw - the quad
            // scale inflates by 1/disc_fraction for padded art, but the disc itself does not
            // grow, so the glitter keys on the diameter and stays matched to the quad.
            light_angular_radius = 0.5f * llclamp(diameter_deg, 0.05f, 30.f) * DEG_TO_RAD;
        }
        else
        {
            light_angular_radius = 0.5f * 0.53f * DEG_TO_RAD;   // stock sun, near enough
        }
    }
    shader->uniform1f(sLightAngularRadius, light_angular_radius);

    // ...and what that body is shining with. The PBR water scales its
    // punctual highlight by the atmosphere's SUNLIGHT, which is zero at
    // night, so the moon reflected nothing at all. getMoonDiffuse() is the
    // moon's own light after transmittance and brightness - the same figure
    // the glitter colour above is built from - lifted enough to read as a
    // path on the water rather than a suggestion of one.
    //
    // Gated on an ACTIVE Atmo environment like the angular radius above: a
    // stock EEP sky's moon lights the water exactly as stock computed it.
    static const F32 SS_MOON_PUNCTUAL_GAIN = 3.0f;
    LLColor3 moonlit = (ss_punctual && SSAtmoEnvApplier::instance().isActive())
        ? psky->getMoonDiffuse() * SS_MOON_PUNCTUAL_GAIN
        : LLColor3(0.f, 0.f, 0.f);
    shader->uniform3fv(sMoonlit, 1, moonlit.mV);
    shader->uniform1f(sSunUp, sun_up ? 1.f : 0.f);

    // <SS:Nexii> The distant sky mirror's strength (see waterF.glsl): far water taps the sky probe straight along the mirror angle so the sky's structure reads in the reflection instead of averaging away in the wave normals. 0 is stock water, kept for A/B comparison and fallback.
    static LLCachedControl<F32> ss_sky_reflect(gSavedSettings, "SSWaterSkyReflect", 1.f);
    shader->uniform1f(sSkyReflect, llclamp(ss_sky_reflect(), 0.f, 1.f));

    shader->uniform3fv(LLShaderMgr::WATER_NORM_SCALE, 1, pwater->getNormalScale().mV);
    shader->uniform1f(LLShaderMgr::WATER_FRESNEL_SCALE, pwater->getFresnelScale());
    shader->uniform1f(LLShaderMgr::WATER_FRESNEL_OFFSET, pwater->getFresnelOffset());
    shader->uniform1f(LLShaderMgr::WATER_BLUR_MULTIPLIER, fmaxf(0, pwater->getBlurMultiplier()) * 2);

    static LLStaticHashedString s_exposure("exposure");
    static LLStaticHashedString tonemap_mix("tonemap_mix");
    static LLStaticHashedString tonemap_type("tonemap_type");

    static LLCachedControl<F32> exposure(gSavedSettings, "RenderExposure", 1.f);

    F32 e = llclamp(exposure(), 0.5f, 4.f);

    static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);

    shader->uniform1f(s_exposure, e);
    static LLCachedControl<U32> tonemap_type_setting(gSavedSettings, "RenderTonemapType", 0U);
    shader->uniform1i(tonemap_type, tonemap_type_setting);
    shader->uniform1f(tonemap_mix, psky->getTonemapMix(should_auto_adjust()));

    F32 sunAngle = llmax(0.f, light_dir.mV[1]);
    F32 scaledAngle = 1.f - sunAngle;

    shader->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up ? 1 : 0);

    // <SS:Nexii> Atmo Magic: the sun's horizon-band share - the water's atmospheric lighting ramps its sun glow on the twilight band (full while the disc is up, easing out through the dusk below the horizon) instead of snapping at centre-rise.
    shader->uniform1f(LLShaderMgr::SS_SUN_RISE, SSAtmoEnvApplier::instance().sunRiseFraction());

    // ...and the sun's true direction while the rise band is live - see
    // SSAtmoEnvApplier::sunSlotDirection.
    shader->uniform3fv(LLShaderMgr::SS_SUN_DIR, 1, SSAtmoEnvApplier::instance().sunSlotDirection().mV);

    // SL-15861 This was changed from getRotatedLightNorm() as it was causing
    // lightnorm in shaders\class1\windlight\atmosphericsFuncs.glsl in have inconsistent additive lighting for 180 degrees of the FOV.
    LLVector4 rotated_light_direction = LLEnvironment::instance().getClampedLightNorm();
    shader->uniform3fv(LLViewerShaderMgr::LIGHTNORM, 1, rotated_light_direction.mV);

    shader->uniform3fv(LLShaderMgr::WL_CAMPOSLOCAL, 1, LLViewerCamera::getInstance()->getOrigin().mV);

    if (LLViewerCamera::getInstance()->cameraUnderWater())
    {
        shader->uniform1f(LLShaderMgr::WATER_REFSCALE, pwater->getScaleBelow());
    }
    else
    {
        shader->uniform1f(LLShaderMgr::WATER_REFSCALE, pwater->getScaleAbove());
    }

    LLGLDisable cullface(GL_CULL_FACE);

    // Only push the water planes once.
    // Previously we did this twice: once for void water and one for region water.
    // However, the void water and region water shaders are the same exact shader.
    // They also had the same exact state with the sole exception setting an edge water flag.
    // That flag was not actually used anywhere in the shaders.
    // - Geenz 2025-02-11
    pushWaterPlanes(0);

    // clean up
    gPipeline.unbindDeferredShader(*shader);

    gGL.setColorMask(true, false);
}

void LLDrawPoolWater::pushWaterPlanes(int pass)
{
    LLVOWater* water = nullptr;
    for (LLFace* const& face : mDrawFace)
    {
        water = static_cast<LLVOWater*>(face->getViewerObject());

        // <SS:Nexii> Stock and Atmo water planes coexist in this one pool; exactly one family draws per frame (doc/atmo_magic_water.md).
        if (!SSWaterWorld::drawsThisFrame(water))
        {
            continue;
        }

        face->renderIndexed();

        // Note non-void water being drawn, updates required
        // Previously we had some logic to determine if this pass was also our water edge pass.
        // Now we only have one pass.  Check if we're doing a region water plane or void water plane.
        // - Geenz 2025-02-11
        if (!water->getIsEdgePatch())
        {
            sNeedsReflectionUpdate = true;
            sNeedsDistortionUpdate = true;
        }
    }
}

// <SS:Nexii> The water haze pass (LLPipeline::doWaterHaze) re-pushes this pool's faces through the base loop, which knows nothing of the stock-versus-Atmo family swap - without this gate the hidden family's planes would still paint haze over the live one's.
void LLDrawPoolWater::pushFaceGeometry()
{
    for (LLFace* const& face : mDrawFace)
    {
        if (SSWaterWorld::drawsThisFrame(static_cast<LLVOWater*>(face->getViewerObject())))
        {
            face->renderIndexed();
        }
    }
}

LLViewerTexture *LLDrawPoolWater::getDebugTexture()
{
    return LLViewerTextureManager::getFetchedTexture(IMG_SMOKE);
}

LLColor3 LLDrawPoolWater::getDebugColor() const
{
    return LLColor3(0.f, 1.f, 1.f);
}

// <FS:Zi> Render speedup for water parameters
void LLDrawPoolWater::onRenderWaterMipNormalChanged()
{
    mRenderWaterMipNormal = (bool)gSavedSettings.getBOOL("RenderWaterMipNormal");
}
// </FS:Zi>
