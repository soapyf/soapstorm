/**
 * @file class1/deferred/ssPostFogF.glsl
 * @brief Atmo Magic: the height fog layer - a post-processing screen-space
 *        veil marched through the surface field along the view ray, drawn at
 *        the start of renderFinalize onto the linear HDR screen.
 *
 *        Not the environment's fog (global, fogs interiors) and not surface
 *        tinting (each fragment fogged by its own column - walls stand out,
 *        sheltered doorways next to fogged streets read wrong). The air is
 *        the fog: the view ray is marched from the camera to the fragment,
 *        and at each step the surface field answers whether that point is
 *        outdoors (the column test), how high above its surface it is (each
 *        source's own vertical falloff), and whether it is underwater (no
 *        fog). The transmittance is the product of the steps; a camera
 *        standing inside the layer fogs everything it sees by the air in
 *        front of the lens, and a sheltered surface seen through ten metres
 *        of storm fogs by those ten metres.
 *
 *        This is a POST program: it is bound directly (like gDeferredCoFProgram),
 *        never through LLPipeline::bindDeferredShader, but mFeatures.isDeferred
 *        is set on it (llviewershadermgr.cpp) so llshadermgr.cpp links
 *        deferredUtil.glsl's getDepth/getScreenCoordinate/getPositionWithNDC/
 *        getPositionWithDepth into this program - this file only prototypes
 *        those four, it must not redefine them (L6: duplicate definitions in
 *        two files of the same program is a link error). It still declares
 *        its own depthMap/inv_proj/screen_res, since uniforms are per-file.
 *        Compiled as its OWN translation unit and linked with
 *        postDeferredNoTCV.glsl (the vertex stage), deferredUtil.glsl, and
 *        ssSurfaceFieldF.glsl (ssFieldFetch and friends): anything else
 *        declared in those files is not visible here except through the
 *        prototypes below, so every OTHER uniform this file touches is
 *        declared again in this file too.
 *
 *        The sky has no endpoint: its veil integrates the camera-height
 *        density along the ray's climb through the layer, so the horizon
 *        collapses into the fog while steep-up rays leave it - exactly the
 *        whiteout's own sky branch. Composited by the call site through an
 *        inscatter-plus-transmit blend (blendFunc ONE / SOURCE_ALPHA, ZERO /
 *        SOURCE_ALPHA): rgb = in-scattered fog light (the horizon colour plus
 *        a Henyey-Greenstein scatter toward the sun or moon), alpha = the
 *        scene's transmittance - which is also the screen's glow mask at this
 *        point in the frame, so a thick bank dims bloom behind it too.
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

// <SS:Nexii> Atmo Magic height fog (replaces the whiteout)

// <SS:Nexii> The ray march's fixed step count. The product of the steps is exact in the limit (exp(-s*dx) raised to N is exp(-s*N*dx), the same extinction whatever N), so the count only prices how faithfully the steps catch the density's variation along the ray - the vertical falloffs and the covered/outdoors flips. 32 samples a depth range up to the far clip well and stays cheap as a full-screen pass. Kept a shader-side constant: there is no dial's worth of tuning in it.
const int SS_FOG_STEPS = 32;

// <SS:Nexii> LOCKSTEP SSScreenFX::FOG_GROUND_SCALE_M / FOG_PRECIP_SCALE_M (ssscreenfxcore.h) - the ground fog and precipitation veil's own scale heights. Also doubles as the coloured mist's scale height (the mist rides the ground term's falloff, not its own).
const float SS_FOG_GROUND_SCALE_M = 6.0;
const float SS_FOG_PRECIP_SCALE_M = 60.0;

// <SS:Nexii> The scattering phase's asymmetry (forward-scattering, g > 0 - fog glows toward the light, not away from it) and its overall share of the in-scatter, both shader-side constants: this is presentation shaping, not a demand curve, so it has no CPU-side twin.
const float SS_FOG_HG_G = 0.55;
const float SS_FOG_SUN_GLOW = 0.35;
const float SS_FOG_PI = 3.14159265359;

out vec4 frag_color;

in vec2 vary_fragcoord;

// This program is not bound through LLPipeline::bindDeferredShader, so nothing here is assumed
// present - every uniform the fragment stage reads is declared in THIS file.
uniform sampler2D depthMap;
uniform mat4 inv_proj;
uniform vec2 screen_res;

// Agent space from view space. The field is anchored to the world; everything the depth buffer hands back is relative to the eye.
uniform mat4 ssFieldInvView;

// The layer's colour: the environment's horizon colour, smoothed CPU-side, so the veil reads as the sky the env author built rather than as a hardcoded white.
uniform vec3 ssFogColor;

// The sun's (or moon's, whichever is up) colour and direction, agent space, pointing TOWARD the light - what the Henyey-Greenstein term below scatters toward.
uniform vec3 ssFogSunColor;
uniform vec3 ssFogSunDir;

// The five demand sources, already ramped CPU-side (in fast, out slow) and already dialed: ground fog, the precipitation veil, the old squall whiteout, the old ground-blizzard drift band, and the coloured mist toward whatever liquid is standing on the field.
uniform float ssFogGround;
uniform float ssFogPrecip;
uniform float ssFogSquall;
uniform float ssFogLift;
uniform float ssFogMist;

// The squall source's own depth scale, metres - 10 in light snow growing to 100 in a blizzard (set CPU-side from the ramped intensity). The drift band's height above the stored surface, metres.
uniform float ssFogSquallScale;
uniform float ssFogBand;

// The extinction range, metres - the visibility inside a full-strength layer.
uniform float ssFogRange;

// The ground reference for columns the field knows nothing about - the void ocean and everything past the stitched window.
uniform float ssFogGroundZ;

// The water plane: samples below it are underwater, and fog does not reach there.
uniform float ssFogWaterZ;

// The layer's total density AT THE CAMERA'S OWN COLUMN - the core's fogDensityAt (ground+precip+squall+lift only, no mist, no wisp) evaluated CPU-side at the camera's height above its surface. Sky rays integrate this along their climb through the layer, exactly as the whiteout's sky branch did with its single squall density.
uniform float ssFogSkyDensity;

// The wind at the camera, agent m/s - what the wisp noise drifts with.
uniform vec3 ssFogWind;

uniform float ssFogTime;

// Diagnosis: 1 shows the fog amount as grayscale, 2 the march's midpoint density (times five, so partial densities read); both write alpha 0 so the haze blend shows the debug colour alone rather than replacing wholesale.
uniform float ssFogDebug;

vec4 ssFieldFetch(vec2 xy_agent);

// <SS:Nexii> The cover window's fetch (ssSurfaceFieldF.glsl): the world field's enclosure
// spectrum per cell, 0 outdoors to 1 sealed interior, -1 where there is no verdict.
float ssFieldFetchCover(vec2 xy_agent);

// <SS:Nexii> mFeatures.isDeferred pulls deferredUtil.glsl into this program's link (llshadermgr.cpp), which already defines these four - prototype only, do not redefine (L6).
float getDepth(vec2 pos_screen);
vec2 getScreenCoordinate(vec2 screenpos);
vec3 getPositionWithNDC(vec3 ndc);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

// A small hash-based 3D value noise, written from scratch for this pass - presentation only (the wisp drift), never state, so a float hash of a vec3 via fract(sin(dot)) is acceptable here.
float ssFogHash3(vec3 p)
{
    return fract(sin(dot(p, vec3(12.9898, 78.233, 45.164))) * 43758.5453);
}

float ssFogValueNoise3(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = ssFogHash3(i + vec3(0.0, 0.0, 0.0));
    float n100 = ssFogHash3(i + vec3(1.0, 0.0, 0.0));
    float n010 = ssFogHash3(i + vec3(0.0, 1.0, 0.0));
    float n110 = ssFogHash3(i + vec3(1.0, 1.0, 0.0));
    float n001 = ssFogHash3(i + vec3(0.0, 0.0, 1.0));
    float n101 = ssFogHash3(i + vec3(1.0, 0.0, 1.0));
    float n011 = ssFogHash3(i + vec3(0.0, 1.0, 1.0));
    float n111 = ssFogHash3(i + vec3(1.0, 1.0, 1.0));

    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);

    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);

    return mix(nxy0, nxy1, f.z);
}

// Henyey-Greenstein phase, unnormalised share folded into SS_FOG_SUN_GLOW at the call site - shapes how the in-scatter concentrates toward the light rather than answering "how much light", which the demand sources already own.
float ssFogHG(float cos_theta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * SS_FOG_PI * pow(max(1.0 + g2 - 2.0 * g * cos_theta, 1.0e-4), 1.5));
}

// The fog density at one point of the ray: zero under cover (the stored column top more than
// 0.75 m above the point - the footstep picker's indoor test), else the LOCKSTEP twin of
// SSScreenFX::fogDensityAt's four terms (ground, precipitation, squall, drift band) PLUS a
// fifth term this core function does not have - the coloured mist, added outside the LOCKSTEP
// shape (audit R10: the twin pins the four-term call; the mist is this shader's own addition on
// top of it) - with the ground and mist terms together multiplied by a WISP term so a still fog
// bank drifts with the wind rather than sitting as a flat lerp. The water gate is applied by the
// caller (the march loop), not here, so this function answers "how thick is the air" and nothing
// about where the water is.
float ssFogDensityAt(vec3 q)
{
    vec4 here = ssFieldFetch(q.xy);

    float surface_z = ssFogGroundZ;
    float openness = 1.0;

    if (here.x > -1.0e5)
    {
        if (here.x - q.z > 0.75)
        {
            // <SS:Nexii> COVERED, but by how much: the world field's enclosure
            // spectrum, stitched per cell into the cover window, grades the air
            // a cover holds. An open-sided porch or a cave mouth fades the fog
            // back in toward its openings at the depth the flood measured
            // behind them; a sealed room stays at the old binary zero. The
            // height reference under a cover is the flat ground reference -
            // the window stores only the cover's own top, so there is nothing
            // better to measure the pool against. -1, the no-verdict sentinel
            // (world field off, tile stale, sub-band solid), reads as fully
            // enclosed, so without a world field answer the march is
            // bit-identical to what it was.
            float cover = ssFieldFetchCover(q.xy);
            openness = (cover > -0.5) ? 1.0 - clamp(cover, 0.0, 1.0) : 0.0;
            if (openness < 0.001) return 0.0;
        }
        else
        {
            surface_z = here.x;
        }
    }

    float h = max(q.z - surface_z, 0.0);

    // LOCKSTEP SSScreenFX::fogDensityAt(h, ground, precip, squall, squallScaleM, lift, bandM) (ssscreenfxcore.h) - ground, precip, squall and the drift band, unchanged from the core's four-term shape.
    float ground_term = ssFogGround * exp(-h / SS_FOG_GROUND_SCALE_M);
    float precip_term = ssFogPrecip * exp(-h / SS_FOG_PRECIP_SCALE_M);
    float squall_term = ssFogSquall * exp(-h / max(ssFogSquallScale, 4.0));
    float band_term   = (h < ssFogBand) ? ssFogLift : 0.0;

    // The fifth term, outside the LOCKSTEP call: the coloured mist rides the ground term's own scale height.
    float mist_term = ssFogMist * exp(-h / SS_FOG_GROUND_SCALE_M);

    // The wisp: a still fog bank should drift, not just fade in and out uniformly. Only the ground and mist sources wisp - the precipitation veil and the squall/drift band are driven weather already in visible motion, and wisping them on top reads as noise rather than as air moving.
    float wisp = 0.75 + 0.25 * ssFogValueNoise3(q * 0.18 - ssFogWind * ssFogTime * 0.18);

    return ((ground_term + mist_term) * wisp + precip_term + squall_term + band_term) * openness;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    float depth = getDepth(tc);

    // The sky has no surface and no endpoint: its veil is the camera-height density integrated
    // along the view ray's climb through the layer. Looking down a street at the horizon, the
    // ray stays inside the layer and the sky collapses into the fog; looking steeply up, the ray
    // leaves it fast. No G-buffer is bound to this post program, so sky detection is by depth
    // alone.
    if (depth >= 0.99995)
    {
        vec4 far_pos = getPositionWithDepth(tc, 1.0);
        vec3 ray = normalize(far_pos.xyz);
        // <SS:Nexii> The climb needs the ray's AGENT-space up component (ray.z is view space, where the camera looks down -Z, so it is negative for every visible pixel and would clamp to 0.15 always).
        vec3 view_dir_agent = normalize((ssFieldInvView * vec4(ray, 0.0)).xyz);
        float through = min(ssFogSquallScale / max(view_dir_agent.z, 0.15), ssFogSquallScale * 4.0);
        float veil = 1.0 - exp(-ssFogSkyDensity * through * (3.0 / max(ssFogRange, 4.0)));

        float phase = ssFogHG(dot(view_dir_agent, ssFogSunDir), SS_FOG_HG_G);

        // <SS:Nexii> Same debug readout as the marched branch below (L13) - without this the sky pixels would show the composited veil instead of the grayscale, disagreeing with the ground under the same debug mode.
        if (ssFogDebug > 0.5)
        {
            frag_color = vec4(vec3(veil), 0.0);
            return;
        }

        vec3 inscatter = ssFogColor * veil + ssFogSunColor * veil * SS_FOG_SUN_GLOW * phase;
        frag_color = vec4(inscatter, 1.0 - veil);
        return;
    }

    vec4 pos_view = getPositionWithDepth(tc, depth);
    vec3 end_agent = (ssFieldInvView * vec4(pos_view.xyz, 1.0)).xyz;
    vec3 cam_agent = (ssFieldInvView * vec4(0.0, 0.0, 0.0, 1.0)).xyz;

    float dist = length(end_agent - cam_agent);
    vec3 view_dir_agent = dist > 1.0e-5 ? (end_agent - cam_agent) / dist : vec3(0.0, 0.0, 1.0);

    // March the ray. Midpoint sampling of a fixed step count - the density field is smooth (a
    // solved flow's vertical falloffs and a stitched heightfield), so midpoint is plenty at this
    // step size - with the start offset dithered by a per-pixel screen hash so the fixed phase
    // does not band across smooth density gradients.
    float dither = fract(sin(dot(tc * screen_res, vec2(12.9898, 78.233))) * 43758.5453);

    float transmittance = 1.0;
    float mid_density = 0.0;

    for (int i = 0; i < SS_FOG_STEPS; ++i)
    {
        float t = (float(i) + dither) / float(SS_FOG_STEPS);
        vec3 q = mix(cam_agent, end_agent, t);

        // Fog stops at the water surface. The boundary is a fade straddling the plane, not a
        // cut - the reconstructed depth wobbles centimetres frame to frame, and a hard test there
        // z-fights (observed in the whiteout).
        float water_gate = smoothstep(ssFogWaterZ - 0.10, ssFogWaterZ + 0.15, q.z);

        float s = ssFogDensityAt(q) * water_gate;
        if (i == SS_FOG_STEPS / 2) mid_density = s;

        // Extinction coefficient: a full-strength layer collapses visibility to roughly ssFogRange metres.
        transmittance *= exp(-s * (dist / float(SS_FOG_STEPS)) * (3.0 / max(ssFogRange, 4.0)));
    }

    float fog = 1.0 - transmittance;

    // Diagnosis: alpha 0 lets the haze blend show the debug colour alone (dst_rgb = src_rgb*1 +
    // dst_rgb*0) while zeroing the destination's glow, rather than the whiteout's old
    // wholesale-replace (alpha 1, which read the same on this blend func by a different route).
    if (ssFogDebug > 0.5)
    {
        if (ssFogDebug < 1.5)
        {
            frag_color = vec4(vec3(fog), 0.0);                 // fog amount
        }
        else
        {
            frag_color = vec4(vec3(mid_density * 5.0), 0.0);   // midpoint density x5
        }
        return;
    }

    // The call site's blend is inscatter + transmit (rgb added, the scene multiplied by alpha) -
    // rgb carries the in-scattered fog light (the horizon colour plus a Henyey-Greenstein
    // scatter toward the sun or moon), alpha the scene's transmittance.
    float phase = ssFogHG(dot(view_dir_agent, ssFogSunDir), SS_FOG_HG_G);
    vec3 inscatter = ssFogColor * fog + ssFogSunColor * fog * SS_FOG_SUN_GLOW * phase;

    frag_color = vec4(inscatter, transmittance);
}
