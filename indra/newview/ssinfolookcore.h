/**
 * @file ssinfolookcore.h
 * @brief Atmo Magic: the info-view LOOK - warm-gray luminance ramp, hemisphere shade, occlusion term, distance fog. Header-only core. CONTRACT.
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

#ifndef SS_INFOLOOKCORE_H
#define SS_INFOLOOKCORE_H

// <SS:Nexii> A CORE header (lldefs.h, <cmath>, <cstdint> only). Design: doc/atmo_magic_phase8_show.md section 6 item 4. These are the FORMULAS the info-view's post-screen pass (app_settings/shaders/class1/deferred/ssInfoLookF.glsl) transliterates; the shader is the transliteration, this file is the definition, and V:\Scratch\atmo\tests\twin_infolook.cpp reads the shader text and asserts the two agree bit for bit over a grid. Nothing here knows about GL, settings or the pipeline: it is a colour pipeline over four scalars (scene luminance, the surface normal's up and key cosines, an occlusion factor, a view distance in metres). The look replaces the old SSAtmoInfoViewDim quad, which only multiplied the world toward black - the user's verdict was "it is just making the world black at max" - with the Cities-Skylines style: a warm-gray world under a bright data overlay, shape carried by ambient occlusion and a soft hemisphere shade rather than by the scene's own albedo, and depth carried by a fog toward a lighter warm tone. ALPHA IS HANDLED BY CONSTRUCTION and this is a property of the CALL SITE, not of these functions: the pass runs over the finished, presented image, so an alpha-blended surface (water, glass, particles, the rain curtains) is already composited into the colour that warmGray() sees, and it takes the occlusion, shade and fog of the OPAQUE surface behind it, because depth and normal at that pixel are the opaque one's - which is exactly what a real fog would do to a pane of glass, and is why this look is specified as "extreme fog" rather than as a per-material shading override. [interaction: SSAtmoInfoView::renderInfoLook] [interaction: ssInfoLookF.glsl]
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSInfoLook
{
    // The ramp's two ends. Dark is a warm brown-gray (r > g > b, so a black scene reads as unlit clay rather than as
    // a hole); light is cream, deliberately short of white so the overlay's own white lines still separate from it.
    constexpr F32 LOOK_DARK_R  = 0.36f;
    constexpr F32 LOOK_DARK_G  = 0.33f;
    constexpr F32 LOOK_DARK_B  = 0.30f;
    constexpr F32 LOOK_LIGHT_R = 0.92f;
    constexpr F32 LOOK_LIGHT_G = 0.89f;
    constexpr F32 LOOK_LIGHT_B = 0.84f;

    // The ramp's exponent, applied to luminance BEFORE the mix. Below 1 it lifts the midtones, which is the whole
    // point: a tonemapped daylight scene sits around 0.2-0.4 luminance and a linear ramp would leave the world down
    // near LOOK_DARK, i.e. the very "everything goes dark" failure this replaces. 0.70 puts luminance 0.25 at ramp
    // position 0.38 and 0.5 at 0.61.
    constexpr F32 LOOK_GAMMA = 0.70f;

    // The hemisphere shade's floor: what a face pointing straight down receives. Not zero - a model-viewer ambient,
    // so downward faces stay readable instead of becoming the black the dim quad used to produce.
    constexpr F32 LOOK_AMBIENT = 0.45f;

    // The fog's knee and its reach, metres of view distance. Start is beyond a building interior, so near geometry
    // keeps its occlusion; end is deck scale, where everything has collapsed into the fog colour.
    constexpr F32 LOOK_FOG_START = 400.f;
    constexpr F32 LOOK_FOG_END   = 6000.f;

    // The fog's colour: lighter than LOOK_LIGHT's midpoint and warmer than the sky, so distance reads as haze rather
    // than as a wash toward white.
    constexpr F32 LOOK_FOG_R = 0.88f;
    constexpr F32 LOOK_FOG_G = 0.85f;
    constexpr F32 LOOK_FOG_B = 0.80f;

    // How much of the computed occlusion is allowed to darken the pixel. 1.0 would let a fully occluded crevice go to
    // black; 0.85 keeps a factor of 0.15 there, which is the "strong ambient occlusion" the reference look has. Note
    // what this does NOT claim: the three darkening terms COMPOUND, so the darkest pixel surfaceLook can produce is
    // LOOK_DARK * LOOK_AMBIENT * (1 - LOOK_AO_STRENGTH) = 0.36 * 0.45 * 0.15, i.e. (0.0243, 0.0223, 0.0202) -
    // measured, printed and pinned by twin_infolook_surface_grid. That is a fully occluded, downward-facing face on a
    // black scene at the camera, so it is rarer than it sounds, but it is not "never dark"; what the look actually
    // guarantees against the old dim quad is that darkness is EARNED by shape (occlusion and orientation) instead of
    // being applied uniformly, and that it is never exactly zero.
    constexpr F32 LOOK_AO_STRENGTH = 0.85f;

    // The elevation the key direction is pinned to, as a unit-vector z BEFORE renormalisation: the call site takes
    // the environment's light direction, flattens it to its azimuth and lifts it back to this. The look is a
    // diagram, not a time of day - a key that followed the real sun would go flat at noon and rake at dusk, and at
    // night there would be none at all. 0.45 is roughly 24 degrees above the horizon: enough that up-facing ground
    // still separates from a wall, little enough that the azimuth is what actually reads.
    constexpr F32 LOOK_KEY_ELEVATION = 0.45f;

    // The flat tone every sky pixel becomes - a touch lighter than the fog so the horizon still separates.
    constexpr F32 LOOK_SKY_R = 0.90f;
    constexpr F32 LOOK_SKY_G = 0.88f;
    constexpr F32 LOOK_SKY_B = 0.84f;

    struct Rgb
    {
        F32 r;
        F32 g;
        F32 b;
    };

    // GLSL's smoothstep, spelled out, so the twin has something to compare against: clamp then the cubic. Undefined in
    // GLSL for edge0 >= edge1; every call here passes LOOK_FOG_START < LOOK_FOG_END, which are constants.
    inline F32 smoothstep01(F32 edge0, F32 edge1, F32 x)
    {
        F32 t = (x - edge0) / (edge1 - edge0);
        t = llclamp(t, 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // GLSL's mix for a scalar, spelled out for the same reason.
    inline F32 mixf(F32 a, F32 b, F32 t)
    {
        return a + (b - a) * t;
    }

    // Rec.709 luminance of the presented colour. The pass runs POST-TONEMAP, so this reads a display-space triple, not
    // a linear one - deliberate: the ramp is a look, and it is authored against what the eye is actually shown.
    inline F32 lum(F32 r, F32 g, F32 b)
    {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    // The warm-gray ramp: the scene's luminance, gamma-lifted, mixed from clay to cream. INVARIANT: monotone
    // non-decreasing per channel in l (LOOK_LIGHT > LOOK_DARK on all three, and pow is monotone on [0,1]);
    // warmGray(0) == LOOK_DARK and warmGray(1) == LOOK_LIGHT exactly; every channel stays inside [dark, light] for
    // l in [0,1] because l is clamped before the power.
    inline Rgb warmGray(F32 l)
    {
        const F32 t = std::pow(llclamp(l, 0.f, 1.f), LOOK_GAMMA);
        Rgb c;
        c.r = mixf(LOOK_DARK_R, LOOK_LIGHT_R, t);
        c.g = mixf(LOOK_DARK_G, LOOK_LIGHT_G, t);
        c.b = mixf(LOOK_DARK_B, LOOK_LIGHT_B, t);
        return c;
    }

    // The soft shade. n_up is the surface normal's cosine against world up, n_key its cosine against the key
    // direction (the sun's azimuth, flattened - a gentle wrap so vertical faces separate from each other rather than
    // a lambert that would reintroduce hard lighting). INVARIANT: the result lies in [LOOK_AMBIENT, 1] for any
    // n_up, n_key in [-1, 1]; it equals LOOK_AMBIENT exactly at n_up == -1 and 1 exactly at n_up == 1, n_key >= 1.
    inline F32 hemiShade(F32 n_up, F32 n_key)
    {
        const F32 hemi = 0.5f + 0.5f * n_up;
        const F32 key  = 0.75f + 0.25f * llmax(n_key, 0.f);
        return LOOK_AMBIENT + (1.f - LOOK_AMBIENT) * hemi * key;
    }

    // How far a pixel has travelled toward the fog colour. INVARIANT: fogMix(LOOK_FOG_START) == 0 exactly,
    // fogMix(LOOK_FOG_END) == 1 exactly, monotone non-decreasing in dist, and clamped outside the knee.
    inline F32 fogMix(F32 dist)
    {
        return smoothstep01(LOOK_FOG_START, LOOK_FOG_END, dist);
    }

    // The occlusion multiplier from a raw visibility factor ao (1 open, 0 fully occluded). INVARIANT:
    // occlusionTerm(1) == 1 exactly, occlusionTerm(0) == 1 - LOOK_AO_STRENGTH, monotone non-decreasing in ao.
    inline F32 occlusionTerm(F32 ao)
    {
        return mixf(1.f, llclamp(ao, 0.f, 1.f), LOOK_AO_STRENGTH);
    }

    // The whole surface pixel in one call, so the shader has ONE expression to transliterate and the twin one thing to
    // grid: ramp the luminance, multiply by shade and occlusion, then mix toward the fog by distance. Sky pixels never
    // reach here - the pass writes LOOK_SKY flat, because a sky has no normal and no occlusion to speak of.
    inline Rgb surfaceLook(F32 l, F32 n_up, F32 n_key, F32 ao, F32 dist)
    {
        const Rgb base = warmGray(l);
        const F32 s    = hemiShade(n_up, n_key) * occlusionTerm(ao);
        const F32 f    = fogMix(dist);
        Rgb c;
        c.r = mixf(base.r * s, LOOK_FOG_R, f);
        c.g = mixf(base.g * s, LOOK_FOG_G, f);
        c.b = mixf(base.b * s, LOOK_FOG_B, f);
        return c;
    }

    // The flat sky tone, as a function so the shader and the twin share one spelling.
    inline Rgb skyLook()
    {
        Rgb c;
        c.r = LOOK_SKY_R;
        c.g = LOOK_SKY_G;
        c.b = LOOK_SKY_B;
        return c;
    }
}

#endif // SS_INFOLOOKCORE_H
