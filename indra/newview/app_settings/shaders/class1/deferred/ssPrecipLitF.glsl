/**
 * @file ssPrecipLitF.glsl
 * @brief Atmo Magic lit particle fragment shader (SS:Nexii): non-emissive
 *        precipitation (snow, ripples) shaded by probe ambient and the sun
 *        with directional shadow sampling, like other lit alpha objects.
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

uniform sampler2D diffuseMap;   // splatted particles, alpha = coverage
uniform sampler2D sceneMap;     // last frame's lit scene (SSR buffer)
uniform vec2 screen_res;

// 1 for surface-aligned ripples, 0 for everything airborne; and 0 when there is no scene map to read, which is the case with HDR off
uniform float ss_decal;
uniform float ss_scene_lit;

// 1 when the bound decal art is the generated ripple, whose colour channels carry the wave's tangent-space normal rather than a tint. A developer-set ripple texture is ordinary art and clears this.
uniform float ss_decal_normals;

// 1 for the granular family (drift, snow cascades): near the camera the alpha
// becomes a screen-door stipple - fragments kept at full brightness, the rest
// discarded - so a cascade of grains never reads as a blended liquid sheet.
// The stipple crossfades back to plain blending by ~24 m, where stipple
// aliasing would outshout the coverage it is faking.
uniform float ss_granular;

// Also declared by shadowUtil, which is only attached when shadows are on;
// these are separate compilation units, so the duplicate is fine and this
// keeps the sun direction available in the no-shadow build too.
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int sun_up_factor;

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

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

void main()
{
    mirrorClip(vary_position);

    vec4 tex = texture(diffuseMap, vary_texcoord0.xy);
    float final_alpha = tex.a * vertex_color.a;
    if (final_alpha < 0.004)
    {
        discard;
    }

    vec3 pos = vary_position;
    // Billboards come in with a viewer-facing normal, which is what the legacy particle path assumes; ripples come in with the normal of the surface they are lying on, so they take the sun and
    // sample the shadow map the way that surface does
    vec3 norm = normalize(vary_normal);

    // A ripple is not flat. The generated ring bakes the crest's own shape into its colour channels, so where that art is what is bound the ring is bent out of the plane it is lying in and takes the
    // sun along its flanks: the side of the wave facing the light comes up bright and the far side falls away, which is the difference between water standing off the ground and a circle painted on
    // it. The quad's tangent is the axis the renderer built it along, so the frame here is the one the bake was drawn in.
    bool decal_norm = (ss_decal * ss_decal_normals > 0.5);
    if (decal_norm)
    {
        vec3 T = normalize(vary_axis - norm * dot(norm, vary_axis));
        vec3 B = cross(norm, T);
        vec3 n_ts = tex.rgb * 2.0 - 1.0;
        norm = normalize(T * n_ts.x + B * n_ts.y + norm * n_ts.z);
    }

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVars(pos.xyz, vec3(0), 1.0, sunlit, amblit, additive, atten);
    vec3 amblit_linear = srgb_to_linear(amblit);

    vec2 frag_tc = gl_FragCoord.xy / screen_res;

    float shadow = 1.0;
#ifdef HAS_SUN_SHADOW
    // The plane the quad is actually in, not the wave bent out of it: the normal offset the shadow lookup applies is a fix for the geometry's own depth bias and a steep flank of a ripple would throw
    // the sample well off the ground it is lying on.
    shadow = sampleDirectionalShadow(pos.xyz, normalize(vary_normal), frag_tc);
#endif

    // Probe irradiance as local ambient (sky fallback when probes are off)
    vec3 irradiance = amblit_linear;
    vec3 glossenv = vec3(0);
    vec3 legacyenv = vec3(0);
    sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, frag_tc, pos.xyz, norm, 0.0, 0.0, true, amblit_linear);

    // Flakes scatter light near-isotropically and a ripple lies flat on the ground, so neither wants a hard lambert term: the sun comes in through a wide wrap, which leaves a billboard about where
    // the old flat 0.6 constant had it while letting a ripple on a surface turned away from the sun, or standing in shadow, actually go dark. A ring with the wave baked into it is the exception: it
    // has a shape now, and a wrap that wide would flatten it straight back out. It takes a narrower one, so the flank turned toward the sun reads against the flank turned away, and a specular on top
    // of that - the crest of a ripple catching the sun in a bright line is most of what says the ground is wet.
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
    float wrap = decal_norm ? 0.25 : 0.7;
    float wrapped = max((dot(norm, light_dir) + wrap) / (1.0 + wrap), 0.0);
    vec3 lit = irradiance + srgb_to_linear(sunlit) * shadow * wrapped * 0.85;

    vec3 sheen = vec3(0);
    if (decal_norm)
    {
        vec3 view = normalize(pos);
        float rl = max(dot(reflect(view, norm), light_dir), 0.0);
        sheen = srgb_to_linear(sunlit) * shadow * pow(rl, 64.0) * 0.6;
    }

    // A ripple is a film of water lying on ground that has already been through the whole deferred light pass - every point light, every projector, the lot - so rather than trying to reproduce that
    // lighting on a batched particle with no light list of its own, read it back off the surface. The scene map holds lit colour, which is light times albedo, so dividing by a mid-grey guess turns
    // it back into roughly the light arriving there. Taking the larger of the two rather than adding them is what keeps the sun from being counted twice: under open sky the sun term already explains
    // the ground and nothing changes, and it is only where the ground is brighter than sky and sun alone can account for - a lamp, a spotlight - that the ripple picks the difference up. It is a
    // frame behind, since the scene map is copied at the end of the frame. For a coarse "is there more light here than the sky is giving" signal on a decal that is fine; it would not be if this were
    // being used as the ripple's colour.
    if (ss_decal * ss_scene_lit > 0.5)
    {
        const float ASSUMED_ALBEDO = 0.4;
        vec3 beneath = texture(sceneMap, frag_tc).rgb / ASSUMED_ALBEDO;
        lit = max(lit, beneath);
    }

    // The ring's colour channels are its shape, not its colour, so it is tinted by the particle alone
    vec3 albedo = decal_norm ? vertex_color.rgb : (tex.rgb * vertex_color.rgb);

    vec4 color;
    color.rgb = srgb_to_linear(albedo) * lit + sheen;
    color.a = final_alpha;

    color.rgb = applySkyAndWaterFog(pos, additive, atten, color).rgb;

    // The granular screen-door. A stable per-pixel hash (not animated - the
    // quad moves across a fixed pattern, which reads as grains passing), kept
    // fraction scales with the fragment's own alpha and with proximity, and
    // kept fragments write at full weight.
    if (ss_granular > 0.5)
    {
        float dist = length(vary_position);
        float dither = clamp((24.0 - dist) / 12.0, 0.0, 1.0);
        if (dither > 0.001)
        {
            float n = fract(sin(dot(floor(gl_FragCoord.xy), vec2(12.9898, 78.233))) * 43758.5453);
            if (n > color.a * dither)
            {
                discard;
            }
            color.a = mix(color.a, 1.0, dither);
        }
    }

    frag_color = max(color, vec4(0));
}
