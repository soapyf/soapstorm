/**
 * @file ssheightfog.cpp
 * @brief Atmo Magic: the height fog layer - a screen-space post pass that
 *        replaces the old whiteout veil.
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

#include "ssheightfog.h"

#include "ssatmomagic.h"
#include "ssscreenfxcore.h"
#include "sssurfacefield.h"
#include "sswindflow.h"
#include "ssworldfield.h"

#include "llappviewer.h"
#include "llenvironment.h"
#include "llfasttimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llworld.h"
#include "pipeline.h"

#include <sstream>

extern bool gCubeSnapshot;

// The squall/drift-band source's own ramp rates, unchanged from the whiteout: a squall arrives
// over seconds and lifts over many more - visibility collapse is fast, recovery is slow
// (doc/atmo_magic_snow.md 14). Ground/precip/mist use the core's FOG_RAMP_IN_S/FOG_RAMP_OUT_S.
static const F32 WHITEOUT_RAMP_IN  = 8.f;
static const F32 WHITEOUT_RAMP_OUT = 20.f;

// One frame of the layer's five demand curves and its colour.
void SSHeightFog::idle(F32 dt)
{
    static LLCachedControl<bool> fog_on(gSavedSettings, "SSAtmoHeightFog", true);
    static LLCachedControl<bool> squall_on(gSavedSettings, "SSAtmoWhiteout", false);
    static LLCachedControl<F32> ground_dial(gSavedSettings, "SSAtmoHeightFogGround", 1.f);
    static LLCachedControl<F32> precip_dial(gSavedSettings, "SSAtmoHeightFogPrecip", 1.f);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();
    const bool enabled = fog_on && atmo->isEnabled();

    // The old whiteout's two demand curves: the squall's falling veil and the ground-blizzard's
    // drift-band veil. Gated by granular weather and their own (still-named) dial.
    F32 squall_target = 0.f;
    F32 lift_target = 0.f;
    if (enabled && squall_on && atmo->granularWeather())
    {
        squall_target = atmo->squallFactor() * llclamp(atmo->precipitation(), 0.f, 1.f) * 0.9f;

        const bool storm_regime = atmo->regime() == SSAtmoMagic::ERegime::BLIZZARD ||
                                  atmo->regime() == SSAtmoMagic::ERegime::SQUALL;
        if (storm_regime)
        {
            lift_target = atmo->liftAt(LLViewerCamera::getInstance()->getOrigin()) * 0.7f;
        }
    }

    mSquallPart = SSScreenFX::relax(mSquallPart, squall_target, WHITEOUT_RAMP_IN, WHITEOUT_RAMP_OUT, dt);
    mLiftPart = SSScreenFX::relax(mLiftPart, lift_target, WHITEOUT_RAMP_IN, WHITEOUT_RAMP_OUT, dt);

    // Ground fog (humid, still, dark), the precipitation veil, and the coloured mist toward
    // whatever liquid is standing on the surface field right now.
    F32 ground_target = 0.f;
    F32 precip_target = 0.f;
    F32 mist_target = 0.f;
    if (enabled)
    {
        ground_target = SSScreenFX::groundFogDemand(atmo->humidity(), atmo->windSpeed(), atmo->sunUp()) * (F32)ground_dial;
        precip_target = SSScreenFX::precipVeilDemand(atmo->precipitation(), atmo->preset().isGranular()) * (F32)precip_dial;

        const SSSurfaceState::LiquidLook liquid = SSSurfaceField::getInstance()->liquidLook();
        mist_target = liquid.mOpacity * SSScreenFX::precipVeilDemand(atmo->precipitation(), false);
    }

    mGroundPart = SSScreenFX::relax(mGroundPart, ground_target, SSScreenFX::FOG_RAMP_IN_S, SSScreenFX::FOG_RAMP_OUT_S, dt);
    mPrecipPart = SSScreenFX::relax(mPrecipPart, precip_target, SSScreenFX::FOG_RAMP_IN_S, SSScreenFX::FOG_RAMP_OUT_S, dt);
    mMistPart = SSScreenFX::relax(mMistPart, mist_target, SSScreenFX::FOG_RAMP_IN_S, SSScreenFX::FOG_RAMP_OUT_S, dt);

    // TODO(core): the squall/drift layer's depth scale (10 m of blowing-snow haze growing to
    // 100 m as the squall and the ground blizzard take hold) is a physical relationship, not a
    // unit conversion, and SSScreenFX has no function for it yet - carried over verbatim from
    // the whiteout rather than invented fresh here. Flagged for the core audit, not silently
    // promoted without one.
    const F32 layer_intensity = llmax(mSquallPart, mLiftPart);
    mFalloffM = 10.f + 90.f * layer_intensity;

    // TODO(core): ditto for the horizon-colour smoothing and its luminance floor below - colour
    // math, not a unit conversion, and also not something SSScreenFX exposes. Carried over from
    // the whiteout unchanged.
    //
    // The layer's colour is the environment's own horizon colour - what distant geometry already
    // fades into, so the veil reads as that sky's haze rather than as a hardcoded white. Only the
    // density is the weather's. With one floor: at night the horizon is near-black, and a black
    // veil over a snow-lit scene punches black holes in exactly the bright surfaces (observed) -
    // fog over a snowfield is lit BY the snow, so the veil colour never drops below a dark-grey
    // luminance.
    if (LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky())
    {
        LLColor3 fog = sky->getBlueHorizon();
        fog = LLColor3(fog.mV[0] + 0.2f, fog.mV[1] + 0.2f, fog.mV[2] + 0.2f);

        const F32 lum = llmax(fog.mV[0], llmax(fog.mV[1], fog.mV[2]));
        if (lum < 0.22f)
        {
            const F32 lift = 0.22f / llmax(lum, 0.001f);
            fog = LLColor3(fog.mV[0] * lift, fog.mV[1] * lift, fog.mV[2] * lift);
        }

        const F32 blend = llclamp(dt * 2.f, 0.f, 1.f);
        mFogColor.mV[0] = lerp(mFogColor.mV[0], llclamp(fog.mV[0], 0.f, 1.f), blend);
        mFogColor.mV[1] = lerp(mFogColor.mV[1], llclamp(fog.mV[1], 0.f, 1.f), blend);
        mFogColor.mV[2] = lerp(mFogColor.mV[2], llclamp(fog.mV[2], 0.f, 1.f), blend);
    }
}

bool SSHeightFog::ensureTarget(U32 w, U32 h)
{
    if (mDepthCopy.getWidth() == w && mDepthCopy.getHeight() == h && mDepthCopy.isComplete())
    {
        return true;
    }

    releaseGL();
    // Needs a depth attachment: the copy program writes DEPTH into it (colour mask off), the
    // same staging trick doAtmospherics runs for the haze pass.
    if (!mDepthCopy.allocate(w, h, GL_RGBA, true))
    {
        LL_WARNS_ONCE("AtmoMagic") << "Height fog depth staging failed to allocate;"
                                      " no height fog layer" << LL_ENDL;
        return false;
    }

    // The staging FBO has to be complete, and that verified once: a broken depth stage would
    // hand the veil shader garbage depth, and the driver a reason to flicker.
    mDepthCopy.bindTarget();
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    mDepthCopy.flush();
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        LL_WARNS_ONCE("AtmoMagic") << "Height fog depth staging incomplete, status 0x"
                                   << std::hex << (U32)status << std::dec
                                   << "; no height fog layer" << LL_ENDL;
        return false;
    }

    return true;
}

// The fog pass. Mirrors doAtmospherics' compositing: stage the screen's depth, then one
// inscatter-plus-transmit fullscreen veil over the screen (BF_ONE/BF_SOURCE_ALPHA - dst becomes
// inscatter + scene*transmittance, alpha carrying transmittance, not a straight alpha lerp), so
// the pass never reads the colour it is fogging. Drawn at the very start of renderFinalize, on
// the linear HDR screen, before tonemapping - it fogs everything the frame drew, not just opaque
// geometry.
void SSHeightFog::render()
{
    if (gCubeSnapshot) return;
    if (LLPipeline::sImpostorRender || LLPipeline::sShadowRender) return;

    static LLCachedControl<bool> layer_enabled(gSavedSettings, "SSAtmoHeightFog", true);
    if (!(bool)layer_enabled) return;

    if (intensity() <= 0.004f) return;

    // The strength dial gates the squall/lift machinery specifically (it scales those two
    // sources only), but at zero it should not even stage the depth copy if nothing else needs it.
    static LLCachedControl<F32> strength_gate(gSavedSettings, "SSAtmoWhiteoutStrength", 1.f);
    const F32 dial = llclamp((F32)strength_gate, 0.f, 2.f);
    if (dial <= 0.f && mGroundPart <= 0.004f && mPrecipPart <= 0.004f && mMistPart <= 0.004f) return;

    if (!gSSPostFogProgram.isComplete()) return;
    // <SS:Nexii> ground/precip/mist are weather-only terms and do not read the surface field; only squall and the drift band are column-gated, so the whole layer no longer bails out on a calm humid morning before any window has been built.
    const bool need_field = (mSquallPart > 0.004f) || (mLiftPart * dial > 0.004f);
    if (need_field && !SSSurfaceField::getInstance()->hasWindow()) return;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    LLRenderTarget& screen = gPipeline.mRT->screen;
    const U32 w = screen.getWidth();
    const U32 h = screen.getHeight();
    if (w == 0 || h == 0) return;
    if (!ensureTarget(w, h)) return;

    LL_PROFILE_GPU_ZONE("atmo height fog");

    // 1. Stage the depth the veil shader marches against.
    {
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

        LLRenderTarget& depth_src_target = gPipeline.mRT->deferredScreen;

        // <SS:Nexii> Unlike doAtmospherics (this pattern's source), screen is NOT bound at this call site (renderFinalize's top, right after renderDeferredLighting's own screen_target->flush()) - no screen.flush() here, or LLRenderTarget::flush's sBoundTarget assert fires and the bindTarget below never gets matched, leaving screen bound through the rest of renderFinalize including the final present (B2).
        mDepthCopy.bindTarget();
        gCopyDepthProgram.bind();

        S32 diff_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DIFFUSE_MAP);
        S32 depth_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);

        gGL.getTexUnit(diff_map)->bind(&screen);
        gGL.getTexUnit(depth_map)->bind(&depth_src_target, true);

        gGL.setColorMask(false, false);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        // Unbind what the copy bound: screen's colour texture must not linger on a unit while
        // screen is about to be the draw framebuffer again - the veil program declares no
        // diffuse sampler to overwrite it, and a lingering binding is a feedback loop (the
        // whiteout's own lesson: flickering black and a frozen frame the moment the layer first
        // drew).
        gGL.getTexUnit(diff_map)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(depth_map)->unbind(LLTexUnit::TT_TEXTURE);

        mDepthCopy.flush();
        screen.bindTarget();
    }

    // 2. The veil itself: the exact haze recipe - inscatter in rgb (added), the scene multiplied
    // by the source alpha (transmittance). Screen alpha is the glow mask at this point in
    // renderFinalize; attenuated the same way the haze attenuates it, introducing no new
    // semantics there.
    LLGLEnable blend(GL_BLEND);
    gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_SOURCE_ALPHA, LLRender::BF_ZERO, LLRender::BF_SOURCE_ALPHA);
    gGL.setColorMask(true, true);

    // This is a POST program (registered like gDeferredCoFProgram: SHADER_DEFERRED level,
    // add_common_permutations, isDeferred set for the automatic matrix sync only) - bound
    // directly, not through LLPipeline::bindDeferredShader, so every uniform beyond that
    // automatic sync (inv_proj) is uploaded here by hand.
    gSSPostFogProgram.bind();
    gSSPostFogProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &mDepthCopy, true);
    gSSPostFogProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)w, (GLfloat)h);

    // The surface field window - the same column lookup the exposure march and the wet/snow
    // passes read.
    const S32 field_channel = gSSPostFogProgram.mActiveTextureChannels;
    const bool field_bound = SSSurfaceField::getInstance()->bindForShader(gSSPostFogProgram, field_channel);
    // <SS:Nexii> The cover window rides one channel up: the world field's enclosure spectrum
    // per cell, which grades the march's covered branch. Validity is shared with the field
    // window (same updateWindow fill), so the two binds succeed or fail together; when neither
    // binds, the fetch's out-of-window sentinel answers -1 and the march keeps the old binary
    // covered test.
    if (field_bound)
    {
        SSSurfaceField::getInstance()->bindCoverForShader(gSSPostFogProgram, field_channel + 1);
    }
    if (!field_bound)
    {
        // <SS:Nexii> No window this frame - force ssFieldFetch's out-of-window sentinel so the march falls back to ssFogGroundZ instead of reading a stale or default-zero origin.
        static LLStaticHashedString field_origin("ssFieldOrigin");
        gSSPostFogProgram.uniform4f(field_origin, 1.0e7f, 1.0e7f, 1.f, 1.f);
    }

    static LLStaticHashedString inv_view("ssFieldInvView");
    static LLStaticHashedString fog_color("ssFogColor");
    static LLStaticHashedString fog_suncolor("ssFogSunColor");
    static LLStaticHashedString fog_sundir("ssFogSunDir");
    static LLStaticHashedString fog_ground("ssFogGround");
    static LLStaticHashedString fog_precip("ssFogPrecip");
    static LLStaticHashedString fog_squall("ssFogSquall");
    static LLStaticHashedString fog_squallscale("ssFogSquallScale");
    static LLStaticHashedString fog_lift("ssFogLift");
    static LLStaticHashedString fog_band("ssFogBand");
    static LLStaticHashedString fog_mist("ssFogMist");
    static LLStaticHashedString fog_range("ssFogRange");
    static LLStaticHashedString fog_groundz("ssFogGroundZ");
    static LLStaticHashedString fog_waterz("ssFogWaterZ");
    static LLStaticHashedString fog_skydensity("ssFogSkyDensity");
    static LLStaticHashedString fog_wind("ssFogWind");
    static LLStaticHashedString fog_time("ssFogTime");
    static LLStaticHashedString fog_debug("ssFogDebug");

    const glm::mat4 inv = glm::inverse(get_current_modelview());
    gSSPostFogProgram.uniformMatrix4fv(inv_view, 1, GL_FALSE, glm::value_ptr(inv));

    static LLCachedControl<F32> band(gSavedSettings, "SSAtmoWhiteoutBand", 2.5f);
    static LLCachedControl<F32> range(gSavedSettings, "SSAtmoWhiteoutRange", 48.f);
    static LLCachedControl<F32> falloff_mult(gSavedSettings, "SSAtmoWhiteoutFalloff", 1.f);

    const F32 band_m = llmax((F32)band, 0.5f);
    const F32 range_m = llmax((F32)range, 4.f);
    const F32 squall_scale_m = llmax(mFalloffM * (F32)falloff_mult, 4.f);

    // The sun (or moon) colour and direction, agent space, toward the light - what the fog's
    // Henyey-Greenstein term scatters toward.
    LLColor3 sun_color(1.f, 1.f, 1.f);
    LLVector3 sun_dir(0.f, 0.f, 1.f);
    if (LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky())
    {
        sun_color = sky->getIsSunUp() ? sky->getSunlightColor() : sky->getMoonlightColor();
        sun_dir = sky->getLightDirection();
    }

    gSSPostFogProgram.uniform3fv(fog_color, 1, mFogColor.mV);
    gSSPostFogProgram.uniform3fv(fog_suncolor, 1, sun_color.mV);
    gSSPostFogProgram.uniform3fv(fog_sundir, 1, sun_dir.mV);
    gSSPostFogProgram.uniform1f(fog_ground, mGroundPart);
    gSSPostFogProgram.uniform1f(fog_precip, mPrecipPart);
    gSSPostFogProgram.uniform1f(fog_squall, mSquallPart * dial);
    gSSPostFogProgram.uniform1f(fog_squallscale, squall_scale_m);
    gSSPostFogProgram.uniform1f(fog_lift, mLiftPart * dial);
    gSSPostFogProgram.uniform1f(fog_band, band_m);
    gSSPostFogProgram.uniform1f(fog_mist, mMistPart);
    gSSPostFogProgram.uniform1f(fog_range, range_m);
    gSSPostFogProgram.uniform1f(fog_groundz, atmo->groundZero());

    // The water plane, and what looking up shows: the veil at the camera's own column, faded by
    // the same bottom-to-top falloff, zeroed when the camera is sheltered.
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);
    const F32 water_z = cam_region ? cam_region->getWaterHeight() : SSAtmoMagic::voidWaterHeight();
    gSSPostFogProgram.uniform1f(fog_waterz, water_z);

    F32 column_top = 0.f;
    const bool cam_column = SSWindFlowMap::getInstance()->surfaceAt(cam, column_top);
    const bool cam_outdoors = !(cam_column && (column_top - cam.mV[VZ] > 0.75f));
    // <SS:Nexii> The sky veil rides the enclosure spectrum where the world field answers: a
    // sheltered camera's sky fades with its measured depth behind the openings instead of
    // cutting off at the column-top test, a sealed interior still reads zero. The wind tile's
    // column test stands in when the field has no verdict (off, tile stale, camera inside a
    // band's implied solid), so the fallback veil is the old binary.
    const F32 cam_enclosure = SSWorldField::getInstance()->enclosureAt(cam);
    const F32 sky_open = (cam_enclosure >= 0.f) ? (1.f - cam_enclosure)
                                               : (cam_outdoors ? 1.f : 0.f);
    // <SS:Nexii> Above the field's own stored surface when the camera has a column (matches the shader's ssFogSkyDensity twin, ssPostFogF.glsl), the flat ground otherwise - not always groundZero(), or a rooftop camera reads near-zero sky veil while the marched fog around it stays thick (M5).
    const F32 cam_above = llmax(cam.mV[VZ] - (cam_column ? column_top : atmo->groundZero()), 0.f);
    // <SS:Nexii> fogDensityAt has no mist parameter of its own - the shader's mist_term rides the ground term's own scale height exactly (SS_FOG_GROUND_SCALE_M == FOG_GROUND_SCALE_M, ssPostFogF.glsl:68-69), so folding mMistPart into the ground argument here reproduces it exactly instead of leaving the sky veil mist-blind while the marched ground fog is not.
    const F32 sky_density = SSScreenFX::fogDensityAt(cam_above, mGroundPart + mMistPart, mPrecipPart, mSquallPart * dial, squall_scale_m, mLiftPart * dial, band_m)
                          * sky_open;
    gSSPostFogProgram.uniform1f(fog_skydensity, sky_density);

    const LLVector3 wind = SSWindFlowMap::getInstance()->sample(cam);
    gSSPostFogProgram.uniform3fv(fog_wind, 1, wind.mV);
    gSSPostFogProgram.uniform1f(fog_time, (F32)gFrameTimeSeconds);

    static LLCachedControl<S32> debug_view(gSavedSettings, "SSAtmoHeightFogDebug", 0);
    gSSPostFogProgram.uniform1f(fog_debug, (F32)llclamp((S32)debug_view, 0, 2));

    {
        LLGLDepthTest depth(GL_FALSE);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    // <SS:Nexii> unbind() does not unbind textures - the depth copy would otherwise stay bound on this unit while mDepthCopy becomes the draw FBO again next frame (L11, weaker version of the guard at :232-238 above).
    gSSPostFogProgram.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
    gSSPostFogProgram.unbind();

    // <SS:Nexii> Matches the bindTarget() above (B2) - screen must be flushed before renderFinalize's later passes bind/flush their own targets, or the present at the end of renderFinalize draws into screen instead of the back buffer.
    screen.flush();

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

void SSHeightFog::releaseGL()
{
    mDepthCopy.release();
}
