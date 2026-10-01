/**
 * @file ssheightfog.h
 * @brief Atmo Magic: the height fog layer - a screen-space post pass that
 *        replaces the old whiteout veil.
 *
 *        Not the environment's fog: that is global and fogs interiors. This
 *        is its own screen-space layer, drawn at the very start of
 *        renderFinalize onto the linear HDR screen - before tonemapping, so
 *        it fogs everything the frame drew (alpha surfaces, particles,
 *        clouds) by the opaque depth behind each pixel. A near rain streak
 *        is fogged as if it stood where the wall behind it is; accepted
 *        trade for a screen-space layer. Sums several demand sources per
 *        pixel (ground fog, a precipitation veil, the old squall/drift-band
 *        whiteout, and a coloured mist toward standing liquid), gated per
 *        pixel by the surface field's column test so interiors stay clear,
 *        with a wisp noise so a still bank drifts with the wind and a
 *        Henyey-Greenstein term so it glows toward the sun or moon.
 *
 *        Design: doc/atmo_magic_surface_weather.md section 10.
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

#ifndef SS_HEIGHTFOG_H
#define SS_HEIGHTFOG_H

#include "llrendertarget.h"
#include "llsingleton.h"
#include "v3color.h"

class LLGLSLShader;

class SSHeightFog : public LLSingleton<SSHeightFog>
{
    LLSINGLETON_EMPTY_CTOR(SSHeightFog);

public:
    void idle(F32 dt);

    // The fog pass: depth copy, then one inscatter-plus-transmit fullscreen veil drawn onto
    // mRT->screen (alpha carries transmittance, not a straight alpha lerp - see the .cpp).
    // Call from the very start of renderFinalize, before the hdr/tonemap branch.
    void render();

    void releaseGL();

    F32 intensity() const { return llmax(mSquallPart, llmax(mLiftPart, llmax(mGroundPart, llmax(mPrecipPart, mMistPart)))); }
    F32 squallPart() const { return mSquallPart; }
    F32 liftPart() const { return mLiftPart; }
    F32 groundPart() const { return mGroundPart; }
    F32 precipPart() const { return mPrecipPart; }
    F32 mistPart() const { return mMistPart; }
    F32 falloff() const { return mFalloffM; }

private:
    bool ensureTarget(U32 w, U32 h);

    // Five demand curves, each ramped CPU-side (in fast, out slow) and dialed before they reach
    // the shader - the veil is their sum per pixel, and nothing fast-moving multiplies it on the
    // GPU. Squall and lift keep the old whiteout's 8s/20s ramp; ground, precip and mist use the
    // core's FOG_RAMP_IN_S/FOG_RAMP_OUT_S (20s/45s).
    F32 mSquallPart = 0.f;
    F32 mLiftPart = 0.f;
    F32 mGroundPart = 0.f;
    F32 mPrecipPart = 0.f;
    F32 mMistPart = 0.f;
    F32 mFalloffM = 10.f;       // the squall/drift layer's depth scale: 10 m up to 100 m in a blizzard
    LLColor3 mFogColor{1.f, 1.f, 1.f};   // smoothed toward the sky's horizon colour, luminance-floored

    LLRenderTarget mDepthCopy;  // the screen's depth, staged for the veil shader
};

#endif
