/**
 * @file sswhiteout.h
 * @brief Atmo Magic: the whiteout layer - a local, height-limited fog veil.
 *
 *        Not the environment's fog: that is global and fogs interiors. This is
 *        its own screen-space layer drawn after the atmospherics, driven by the
 *        granular weather state (squall and ground-blizzard lift), gated per
 *        pixel by the surface field's exposure march so interiors stay clear,
 *        and height-limited to the drift band so the fog hugs the ground the
 *        way blowing snow actually does. Composites as an alpha fog lerp over
 *        the scene - the same recipe the haze pass uses - so it never needs to
 *        read the colour it is fogging.
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

#ifndef SS_WHITEOUT_H
#define SS_WHITEOUT_H

#include "llrendertarget.h"
#include "llsingleton.h"
#include "v3color.h"

class LLGLSLShader;

class SSWhiteout : public LLSingleton<SSWhiteout>
{
    LLSINGLETON_EMPTY_CTOR(SSWhiteout);

public:
    void idle(F32 dt);

    // The fog pass: depth copy, alpha-lerped fullscreen veil over the screen.
    // Call from the pool pass, after doAtmospherics, before the weather render.
    void render();

    void clear();

    void releaseGL();

    F32 intensity() const { return llmax(mSquallPart, mLiftPart); }
    F32 squallPart() const { return mSquallPart; }
    F32 liftPart() const { return mLiftPart; }
    F32 falloff() const { return mFalloffM; }

private:
    bool ensureTarget(U32 w, U32 h);

    // The layer's two demand curves, each ramped with the regime's own rates
    // (in fast, out slow) and dialed before they reach the shader - the veil is
    // their sum per pixel, and nothing fast-moving multiplies it on the GPU.
    F32 mSquallPart = 0.f;
    F32 mLiftPart = 0.f;
    F32 mFalloffM = 10.f;       // the layer's depth scale: 10 m in light snow up to 100 m in a blizzard
    LLColor3 mFogColor{1.f, 1.f, 1.f};   // smoothed toward the sky's horizon colour

    LLRenderTarget mDepthCopy;  // the screen's depth, staged for the veil shader
    U32 mTargetW = 0;
    U32 mTargetH = 0;
};

#endif
