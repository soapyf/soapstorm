/**
 * @file sswhiteout.cpp
 * @brief Atmo Magic: the whiteout layer - a local, height-limited fog veil.
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

#include "sswhiteout.h"

#include "ssatmomagic.h"
#include "sssurfacefield.h"
#include "sswindflow.h"

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

// The regime's own ramp rates: a squall arrives over seconds and lifts over
// many more - visibility collapse is fast, recovery is slow (doc/atmo_magic_snow.md 14).
static const F32 WHITEOUT_RAMP_IN  = 8.f;
static const F32 WHITEOUT_RAMP_OUT = 20.f;

void SSWhiteout::idle(F32 dt)
{
    static LLCachedControl<bool> whiteout_on(gSavedSettings, "SSAtmoWhiteout", true);

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    if (!atmo->isEnabled() || !atmo->granularWeather() || !whiteout_on)
    {
        mSquallPart = llmax(0.f, mSquallPart - dt / WHITEOUT_RAMP_OUT);
        mLiftPart = llmax(0.f, mLiftPart - dt / WHITEOUT_RAMP_OUT);
        return;
    }

    // The layer's two demand curves, each ramped HERE - once, slowly - so the
    // shader never multiplies anything fast-moving into the veil. The old shape
    // (a smoothed intensity times fast shader-side factors) arrived at full
    // strength inside a second whenever the weather state jumped.
    const F32 squall_target = atmo->squallFactor() * llclamp(atmo->precipitation(), 0.f, 1.f) * 0.9f;

    const bool storm_regime = atmo->regime() == SSAtmoMagic::ERegime::BLIZZARD ||
                              atmo->regime() == SSAtmoMagic::ERegime::SQUALL;
    const F32 lift_target = storm_regime
        ? atmo->liftAt(LLViewerCamera::getInstance()->getOrigin()) * 0.7f
        : 0.f;

    {
        const F32 tau = (squall_target > mSquallPart) ? WHITEOUT_RAMP_IN : WHITEOUT_RAMP_OUT;
        mSquallPart += (squall_target - mSquallPart) * llclamp(dt / llmax(tau, 0.5f), 0.f, 1.f);

        const F32 tau_l = (lift_target > mLiftPart) ? WHITEOUT_RAMP_IN : WHITEOUT_RAMP_OUT;
        mLiftPart += (lift_target - mLiftPart) * llclamp(dt / llmax(tau_l, 0.5f), 0.f, 1.f);
    }

    // The layer's vertical depth scale: 10 m of blowing-snow haze in light
    // snow, growing to 100 m as the squall and the ground blizzard take hold.
    // Driven by the already-ramped parts, so it never jumps with a gust.
    const F32 layer_intensity = llmax(mSquallPart, mLiftPart);
    mFalloffM = 10.f + 90.f * layer_intensity;

    // The layer's colour is the environment's own horizon colour - what
    // distant geometry already fades into, so the veil reads as that sky's
    // haze rather than as a hardcoded white. Only the density is the weather's.
    // With one floor: at night the horizon is near-black, and a black veil over
    // the snow-lit scene punches black holes in exactly the bright surfaces
    // (observed) - fog over a snowfield is lit BY the snow, so the veil colour
    // never drops below a dark-grey luminance.
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

bool SSWhiteout::ensureTarget(U32 w, U32 h)
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
        LL_WARNS_ONCE("AtmoMagic") << "Whiteout depth staging failed to allocate;"
                                      " no whiteout layer" << LL_ENDL;
        return false;
    }

    // The staging FBO has to be complete, and that verified once: a broken
    // depth stage would hand the veil shader garbage depth, and the driver a
    // reason to flicker.
    mDepthCopy.bindTarget();
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    mDepthCopy.flush();
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        LL_WARNS_ONCE("AtmoMagic") << "Whiteout depth staging incomplete, status 0x"
                                   << std::hex << (U32)status << std::dec
                                   << "; no whiteout layer" << LL_ENDL;
        return false;
    }

    mTargetW = w;
    mTargetH = h;
    return true;
}

// The fog pass. Mirrors doAtmospherics' compositing: stage the screen's depth,
// then one alpha-blended fullscreen veil over the screen - dst becomes
// fogColor*alpha + scene*(1-alpha) through the blend func, so the pass never
// reads the colour it is fogging.
void SSWhiteout::render()
{
    if (gCubeSnapshot) return;
    if (LLPipeline::sImpostorRender || LLPipeline::sShadowRender) return;
    if (intensity() <= 0.004f) return;

    // The dial gates the whole machinery, not just the colour: at zero the
    // depth staging and the fullscreen draw do not run at all.
    static LLCachedControl<F32> strength_gate(gSavedSettings, "SSAtmoWhiteoutStrength", 1.f);
    if (llclamp((F32)strength_gate, 0.f, 2.f) <= 0.f) return;

    static LLCachedControl<bool> layer_enabled(gSavedSettings, "SSAtmoWhiteout", true);
    if (!(bool)layer_enabled) return;

    if (!gSSWhiteoutProgram.isComplete()) return;
    if (!SSSurfaceField::getInstance()->hasWindow()) return;

    SSAtmoMagic* atmo = SSAtmoMagic::getInstance();

    LLRenderTarget& screen = gPipeline.mRT->screen;
    const U32 w = screen.getWidth();
    const U32 h = screen.getHeight();
    if (w == 0 || h == 0) return;
    if (!ensureTarget(w, h)) return;

    LL_PROFILE_GPU_ZONE("atmo whiteout");

    // 1. Stage the depth the veil shader marches against.
    {
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

        LLRenderTarget& depth_src_target = gPipeline.mRT->deferredScreen;

        screen.flush();
        mDepthCopy.bindTarget();
        gCopyDepthProgram.bind();

        S32 diff_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DIFFUSE_MAP);
        S32 depth_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);

        gGL.getTexUnit(diff_map)->bind(&screen);
        gGL.getTexUnit(depth_map)->bind(&depth_src_target, true);

        gGL.setColorMask(false, false);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        // Unbind what the copy bound: screen's colour texture must not linger
        // on a unit while screen is the draw framebuffer - the veil program
        // declares no diffuse sampler to overwrite it, and a lingering binding
        // is a feedback loop (observed: flickering black and a frozen frame
        // the moment the layer first drew).
        gGL.getTexUnit(diff_map)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(depth_map)->unbind(LLTexUnit::TT_TEXTURE);

        mDepthCopy.flush();
        screen.bindTarget();
    }

    // 2. The veil: the exact haze recipe - inscatter in rgb (added), the
    // scene multiplied by the source alpha (transmittance). Screen alpha is
    // attenuated the same way the haze attenuates it; this pass introduces no
    // new semantics there.
    LLGLEnable blend(GL_BLEND);
    gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_SOURCE_ALPHA, LLRender::BF_ZERO, LLRender::BF_SOURCE_ALPHA);
    gGL.setColorMask(true, true);

    gPipeline.bindDeferredShader(gSSWhiteoutProgram, nullptr, &mDepthCopy);

    // The surface field window - the exposure march and the band's surface
    // heights both read it, exactly as the wet and snow passes bind it.
    const S32 field_channel = gSSWhiteoutProgram.mActiveTextureChannels;
    SSSurfaceField::getInstance()->bindForShader(gSSWhiteoutProgram, field_channel);

    static LLStaticHashedString inv_view("ssFieldInvView");
    static LLStaticHashedString wo_color("ssWhiteoutColor");
    static LLStaticHashedString wo_squall("ssWhiteoutSquall");
    static LLStaticHashedString wo_lift("ssWhiteoutLift");
    static LLStaticHashedString wo_band("ssWhiteoutBand");
    static LLStaticHashedString wo_range("ssWhiteoutRange");
    static LLStaticHashedString wo_falloff("ssWhiteoutFalloff");
    static LLStaticHashedString wo_groundz("ssWhiteoutGroundZ");
    static LLStaticHashedString wo_waterz("ssWhiteoutWaterZ");
    static LLStaticHashedString wo_skyveil("ssWhiteoutSkyVeil");
    static LLStaticHashedString wo_debug("ssWhiteoutDebug");

    const glm::mat4 inv = glm::inverse(get_current_modelview());
    gSSWhiteoutProgram.uniformMatrix4fv(inv_view, 1, GL_FALSE, glm::value_ptr(inv));

    static LLCachedControl<F32> strength(gSavedSettings, "SSAtmoWhiteoutStrength", 1.f);
    static LLCachedControl<F32> band(gSavedSettings, "SSAtmoWhiteoutBand", 2.5f);
    static LLCachedControl<F32> range(gSavedSettings, "SSAtmoWhiteoutRange", 48.f);
    static LLCachedControl<F32> falloff_mult(gSavedSettings, "SSAtmoWhiteoutFalloff", 1.f);

    const F32 dial = llclamp((F32)strength, 0.f, 2.f);

    gSSWhiteoutProgram.uniform3fv(wo_color, 1, mFogColor.mV);
    gSSWhiteoutProgram.uniform1f(wo_squall, mSquallPart * dial);
    gSSWhiteoutProgram.uniform1f(wo_lift, mLiftPart * dial);
    gSSWhiteoutProgram.uniform1f(wo_band, llmax((F32)band, 0.5f));
    gSSWhiteoutProgram.uniform1f(wo_range, llmax((F32)range, 4.f));
    gSSWhiteoutProgram.uniform1f(wo_falloff, llmax(mFalloffM * (F32)falloff_mult, 4.f));
    gSSWhiteoutProgram.uniform1f(wo_groundz, atmo->groundZero());

    // The water plane, and what looking up shows: the veil at the camera's own
    // column, faded by the same bottom-to-top falloff, zeroed when the camera
    // is sheltered.
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLViewerRegion* cam_region = LLWorld::getInstance()->getRegionFromPosAgent(cam);
    const F32 water_z = cam_region ? cam_region->getWaterHeight() : SSAtmoMagic::voidWaterHeight();
    gSSWhiteoutProgram.uniform1f(wo_waterz, water_z);

    F32 column_top = 0.f;
    const bool cam_column = SSWindFlowMap::getInstance()->surfaceAt(cam, column_top);
    const bool cam_outdoors = !(cam_column && (column_top - cam.mV[VZ] > 0.75f));
    const F32 cam_above = llmax(cam.mV[VZ] - atmo->groundZero(), 0.f);
    const F32 sky_veil = cam_outdoors
        ? mSquallPart * expf(-cam_above / llmax(mFalloffM, 4.f))
        : 0.f;
    gSSWhiteoutProgram.uniform1f(wo_skyveil, sky_veil);

    static LLCachedControl<S32> debug_view(gSavedSettings, "SSAtmoWhiteoutDebug", 0);
    gSSWhiteoutProgram.uniform1f(wo_debug, (F32)llclamp((S32)debug_view, 0, 3));

    {
        LLGLDepthTest depth(GL_FALSE);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    gPipeline.unbindDeferredShader(gSSWhiteoutProgram);

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}

void SSWhiteout::clear()
{
    mSquallPart = 0.f;
    mLiftPart = 0.f;
}

void SSWhiteout::releaseGL()
{
    mDepthCopy.release();
    mTargetW = 0;
    mTargetH = 0;
}
