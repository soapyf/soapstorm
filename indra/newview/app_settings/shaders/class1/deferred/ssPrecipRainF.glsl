/**
 * @file ssPrecipRainF.glsl
 * @brief Atmo Magic rain particle fragment shader (SS:Nexii): water-like
 *        droplets with screen refraction, probe environment reflection and a
 *        sun/moon specular glint over a fake cylindrical normal.
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

uniform sampler2D diffuseMap;   // splatted drops, alpha = coverage
uniform sampler2D sceneMap;     // last frame's lit scene (SSR buffer)
uniform vec2 screen_res;
// eye-space dominant light direction; must match the vec3 declaration in windlight/atmosphericsFuncs.glsl exactly or the link fails
uniform vec3 lightnorm;
uniform float ss_refract_strength;

// Shape of the water on the sprite and how the light it throws back is distributed over it; see the notes in main()
uniform float ss_drop_bulge;
uniform float ss_drop_core;
uniform float ss_drop_sparkle;

in vec3 vary_position;
in vec3 vary_normal;
in vec3 vary_axis;
in vec4 vertex_color;
in vec2 vary_texcoord0;

vec3 srgb_to_linear(vec3 cs);
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten);
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);
void mirrorClip(vec3 pos);

void main()
{
    mirrorClip(vary_position);

    vec2 tc = vary_texcoord0.xy;
    float splat = texture(diffuseMap, tc).a;
    float final_alpha = splat * vertex_color.a;
    if (final_alpha < 0.004)
    {
        discard;
    }

    vec3 pos = vary_position;
    vec3 view = normalize(pos);

    // The drop's own frame: the way it is falling, the way it faces the eye, and the direction across it.
    vec3 axis = normalize(vary_axis);
    vec3 face = normalize(vary_normal);              // billboard faces the eye
    vec3 across = cross(axis, face);
    float across_len = length(across);
    across = (across_len > 0.001) ? across / across_len : normalize(cross(axis, vec3(0.0, 0.0, 1.0)));

    // Water normal read off the shape in the texture's alpha rather than assumed across the quad. This was a half cylinder wrapped around the quad's long axis, built from the texture coordinates:
    // near enough for the drops tier, where a quad is one splat, and wrong everywhere else. A cluster sprite carries a dozen separate drops and a sheet ninety, and one cylinder spanning the quad
    // gave all of them a single fat drop's worth of shading smeared across the whole card. Even on a lone drop it knew nothing about the taper down the tail, so the head and the tip shaded alike.
    // The alpha the bake writes is coverage, which stands in for how much water is in the way, so it peaks along each splat's spine and falls to nothing at its silhouette - exactly where the rounded
    // side of the drop turns away from us. Its gradient is therefore the slope of that side, per splat, for free. Taken in texture space and scaled back up by the texture's own size it is a
    // derivative per unit of uv, so the same bulge dial means the same roundness at any bake resolution.
    vec2 tsize = vec2(textureSize(diffuseMap, 0));
    vec2 texel = 1.0 / tsize;
    float ax = texture(diffuseMap, tc + vec2(texel.x, 0.0)).a
             - texture(diffuseMap, tc - vec2(texel.x, 0.0)).a;
    float ay = texture(diffuseMap, tc + vec2(0.0, texel.y)).a
             - texture(diffuseMap, tc - vec2(0.0, texel.y)).a;

    // Uphill in coverage is toward the spine, so the surface leans the other way. Capped short of grazing: past that the reflection vector swings wildly across a texel and the edge of every drop
    // crawls with noise.
    vec2 lat = -vec2(ax, ay) * tsize * 0.5 * ss_drop_bulge;
    float lat_len = length(lat);
    if (lat_len > 3.0) lat *= 3.0 / lat_len;

    vec3 norm = normalize(face + across * lat.x + axis * lat.y);

    // The drops are drawn fatter than life - the bake floors every splat at a minimum share of its quad, or the far tiers come out as empty cards - and a highlight spread evenly over that fat
    // footprint is what makes a lit drop read as a white blob instead of as water. Real water carries the light in a thin bright line down the spine with dark water either side of it, so the
    // specular terms below are weighted toward where the coverage is densest. The drop keeps its size and its refraction; only the light it throws back collapses onto the middle of it.
    float core = pow(splat, ss_drop_core);

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    // The real light vector, not vec3(0): the module multiplies its haze glow by dot(light_dir, view_dir), and a zero vector guts the additive airlight the fog call below applies.
    calcAtmosphericVars(pos.xyz, lightnorm.xyz, 1.0, sunlit, amblit, additive, atten);
    vec3 amblit_linear = srgb_to_linear(amblit);

    // Environment reflection through the probe system; the class2 fallback approximates from sky ambient when probes are disabled
    vec3 irradiance = amblit_linear;
    vec3 glossenv = vec3(0);
    vec3 legacyenv = vec3(0);
    vec2 frag_tc = gl_FragCoord.xy / screen_res;
    sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, frag_tc, pos.xyz, norm, 0.9, 1.0, true, amblit_linear);

    // Refraction: pull last frame's scene sideways through the droplet;
    // without an SSR buffer fall back to ambient transmission
    vec3 transmitted = irradiance;
    if (ss_refract_strength > 0.0)
    {
        // ...collapsed toward zero as the view closes on the sun/moon: the offset was dragging the saturated disc sideways into every drop within reach of its silhouette, painting a hard-edged
        // ring of solid white blobs around it - drops drawn fatter than life lens the disc as blobs, not points. Sampling straight through instead makes near-disc drops white-on-white invisible,
        // which is what rain across the sun actually reads as; the sparkle lobes still glitter around it.
        float toward_light = pow(clamp(dot(-view, lightnorm.xyz), 0.0, 1.0), 8.0);
        vec2 refract_tc = clamp(frag_tc + norm.xy * ss_refract_strength * (1.0 - toward_light),
                                vec2(0.001), vec2(0.999));
        transmitted = texture(sceneMap, refract_tc).rgb;
    }

    // Water fresnel: transmit head-on, reflect the environment at the edges;
    // biased reflective so the water look reads at streak scale
    float ndv = clamp(dot(norm, -view), 0.0, 1.0);
    float fres = 0.06 + 0.94 * pow(1.0 - ndv, 2.5);

    // Sun/moon glint plus forward scatter when looking lightward. Two lobes, not one. The broad one is the drop's wet sheen; the tight one is the glint proper, and because it only survives where the
    // normal is within a hair of mirroring the sun it lands on a few drops at a time and moves off them as they fall. That flicker is the sparkle - a wide lobe at the same energy is a uniform sheet
    // of grey shine instead.
    vec3 light_dir = lightnorm.xyz;
    float rl = clamp(dot(reflect(view, norm), light_dir), 0.0, 1.0);
    float spec = pow(rl, 96.0) + pow(rl, 1024.0) * ss_drop_sparkle;

    // Forward scatter: VERY wide and VERY quiet. The old cos^8 x 0.25 cone had a steep shoulder, and the Reinhard compression flattened its bright interior into a plateau - together they drew a
    // hard circle around the sun with lit drops inside and dark ones out. Real backlit rain (see any sunset-shower photo) is a broad gentle brightening with no boundary anywhere: cos^2.5 spreads
    // the gradient across half the sky, and the low gain keeps the compression from ever plateauing. The per-drop sparkle lobes above carry the actual glitter.
    float scatter = pow(clamp(dot(-view, -light_dir), 0.0, 1.0), 2.5) * 0.06;

    vec4 color;
    color.rgb = mix(transmitted, glossenv, fres);

    // Compressed before it is added: sunlit is HDR and near a low sun runs to several times white, so the forward-scatter cone painted every drop around the disc as a saturated blob - a ring of
    // popcorn about the sun (the drops IN FRONT of the disc vanishing white-on-white is natural and stays). Reinhard on the glint alone keeps the sparkle's flicker and the scatter's directionality
    // while capping any single drop short of burnout; away from the light the term is tiny and passes through effectively untouched.
    vec3 glint = srgb_to_linear(sunlit) * (spec * core * 1.5 + scatter) * splat;
    color.rgb += glint / (1.0 + glint);
    color.rgb *= vertex_color.rgb;
    color.a = final_alpha;

    color.rgb = applySkyAndWaterFog(pos, additive, atten, color).rgb;

    frag_color = max(color, vec4(0));
}
