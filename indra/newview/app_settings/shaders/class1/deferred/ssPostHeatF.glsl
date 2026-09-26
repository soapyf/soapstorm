/**
 * @file class1/deferred/ssPostHeatF.glsl
 * @brief Atmo Magic: the heat-shimmer post pass - a mirage ripple over distant, low-screen pixels.
 *
 *        One fullscreen draw, src into dst (SSScreenFXPost::renderHeat): reconstruct each pixel's linear
 *        view-space distance from depthMap the way cofF.glsl does, weight the ripple up with distance (no
 *        distortion inside SS_MIRAGE_NEAR_M, full weight past SS_MIRAGE_FULL_M) and down toward the top of
 *        the screen (a crude horizon proxy - the hot ground is low on screen, the sky above it is not), then
 *        offset the diffuseRect sample by a two-octave procedural noise scrolled by the wind. No texture
 *        lookups beyond diffuseRect and depthMap; the noise is a small hash, not a texture.
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;

uniform mat4 inv_proj;
uniform vec2 screen_res;

uniform float ssHeatStrength;
uniform float ssHeatTime;
uniform vec2  ssHeatWind;
uniform float ssHeatAspect;

in vec2 vary_fragcoord;

// <SS:Nexii> LOCKSTEP ssscreenfxcore.h SSScreenFX::MIRAGE_NEAR_M / MIRAGE_FULL_M - the depth band the ripple weight ramps across.
const float SS_MIRAGE_NEAR_M = 6.0;
const float SS_MIRAGE_FULL_M = 60.0;

// A cheap 2D hash, presentation only (never state): no two calls in this pass need to agree with any other system.
float ssHeatHash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Bilinear value noise over the hash above, smoothed with the standard quintic-free Hermite blend; returns roughly -1..1 so the offset it drives can push either side of a pixel.
float ssHeatValueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);

    float a = ssHeatHash(i);
    float b = ssHeatHash(i + vec2(1.0, 0.0));
    float c = ssHeatHash(i + vec2(0.0, 1.0));
    float d = ssHeatHash(i + vec2(1.0, 1.0));

    vec2 u = f * f * (3.0 - 2.0 * f);
    float n = mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
    return n * 2.0 - 1.0;
}

void main()
{
    vec2 uv = vary_fragcoord.xy;

    // Linear view-space distance from the staged depth, the same reconstruction cofF.glsl uses.
    float z = texture(depthMap, uv).r;
    z = z * 2.0 - 1.0;
    vec4 ndc = vec4(0.0, 0.0, z, 1.0);
    vec4 p = inv_proj * ndc;
    float d = -(p.z / p.w);

    float weight = smoothstep(SS_MIRAGE_NEAR_M, SS_MIRAGE_FULL_M, d)
                 * (1.0 - smoothstep(0.35, 0.85, uv.y));

    if (weight <= 0.0)
    {
        frag_color = vec4(texture(diffuseRect, uv).rgb, 1.0);
        return;
    }

    // Aspect-corrected sample coordinate so the noise cells stay square regardless of the window's shape.
    vec2 np = vec2(uv.x * ssHeatAspect, uv.y);

    float n1 = ssHeatValueNoise(np * vec2(6.0, 3.0)  + ssHeatWind * ssHeatTime * 0.05);
    float n2 = ssHeatValueNoise(np * vec2(12.0, 6.0) - ssHeatWind * ssHeatTime * 0.08);
    float noise = n1 * 0.6 + n2 * 0.4;

    vec2 offset = vec2(0.35, 1.0) * (noise * weight * ssHeatStrength * 0.004);

    vec3 rgb = texture(diffuseRect, clamp(uv + offset, 0.0, 1.0)).rgb;
    frag_color = vec4(rgb, 1.0);
}
