/**
 * @file ssPrecipLitF.glsl
 * @brief Atmo Magic lit particle fragment shader (SS:Nexii): non-emissive
 *        precipitation (snow, ripples) shaded by probe ambient and the sun
 *        with directional shadow sampling, like other lit alpha objects.
 *        The landing ring is the exception and is shaded as water - scene
 *        refraction, fresnel reflection and a sun glint through its baked
 *        wave normal, the same treatment ssPrecipRainF.glsl gives a drop.
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

    // A mid-grey stand-in for the ground a decal is lying on, for the two places below that have to guess at it.
    const float ASSUMED_ALBEDO = 0.4;

    float shadow = 1.0;
#ifdef HAS_SUN_SHADOW
    // The plane the quad is actually in, not the wave bent out of it: the normal offset the shadow lookup applies is a fix for the geometry's own depth bias and a steep flank of a ripple would throw
    // the sample well off the ground it is lying on.
    shadow = sampleDirectionalShadow(pos.xyz, normalize(vary_normal), frag_tc);
#endif

    // Probe irradiance as local ambient (sky fallback when probes are off). The ring asks for a GLOSSY sample as well: a wave lying on wet ground is mostly a mirror, and the sky and sun sliding across its
    // flanks is what makes the analytic puddle ripples in the normal pass read as water. Those get it for free by writing a tilted normal into the gbuffer and letting the deferred wet pass light it; a
    // forward-drawn particle has to ask for the reflection itself, which is what this and the fresnel below are for. [interaction: ssSurfaceNormalF.glsl item 6 - the same look, drawn the other way round]
    vec3 irradiance = amblit_linear;
    vec3 glossenv = vec3(0);
    vec3 legacyenv = vec3(0);
    sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, frag_tc, pos.xyz, norm, decal_norm ? 0.9 : 0.0, 0.0, true, amblit_linear);

    // Flakes scatter light near-isotropically and a ripple lies flat on the ground, so neither wants a hard lambert term: the sun comes in through a wide wrap, which leaves a billboard about where
    // the old flat 0.6 constant had it while letting a ripple on a surface turned away from the sun, or standing in shadow, actually go dark. A ring with the wave baked into it is the exception: it
    // has a shape now, and a wrap that wide would flatten it straight back out. It takes a narrower one, so the flank turned toward the sun reads against the flank turned away, and a specular on top
    // of that - the crest of a ripple catching the sun in a bright line is most of what says the ground is wet.
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
    float wrap = decal_norm ? 0.25 : 0.7;
    float wrapped = max((dot(norm, light_dir) + wrap) / (1.0 + wrap), 0.0);
    vec3 lit = irradiance + srgb_to_linear(sunlit) * shadow * wrapped * 0.85;

    // A ripple is a film of water lying on ground that has already been through the whole deferred light pass - every point light, every projector, the lot - so rather than trying to reproduce that
    // lighting on a batched particle with no light list of its own, read it back off the surface. The scene map holds lit colour, which is light times albedo, so dividing by a mid-grey guess turns
    // it back into roughly the light arriving there. Taking the larger of the two rather than adding them is what keeps the sun from being counted twice: under open sky the sun term already explains
    // the ground and nothing changes, and it is only where the ground is brighter than sky and sun alone can account for - a lamp, a spotlight - that the ripple picks the difference up. It is a
    // frame behind, since the scene map is copied at the end of the frame. For a coarse "is there more light here than the sky is giving" signal on a decal that is fine; it would not be if this were
    // being used as the ripple's colour.
    bool has_scene = (ss_decal * ss_scene_lit > 0.5);
    if (has_scene)
    {
        vec3 beneath = texture(sceneMap, frag_tc).rgb / ASSUMED_ALBEDO;
        lit = max(lit, beneath);
    }

    vec4 color;
    if (decal_norm)
    {
        // A landing ring is WATER, not paint, and the thing it has to match is the analytic ring the normal pass draws on a puddle a couple of metres nearer the camera: the two are the same event and
        // used to be told apart at a glance, because one of them tilted the surface's normal and the other drew a white circle on top of it. So the ring's body is the ground it is lying on, re-lit
        // through the wave's own normal - the flank turned toward the light comes up, the flank turned away goes down, and where the wave is flat nothing happens at all and the ground shows through
        // untouched. The scene map is that ground; without one (HDR off) the same mid-grey guess under the same sky stands in, which is dimmer and greyer than the white it replaces either way.
        // And it is looked at THROUGH the wave rather than past it: the same screen refraction the falling drops get (ssPrecipRainF.glsl's ss_refract_strength block), last frame's scene pulled sideways
        // by the crest's own slope. The one thing that cannot be copied across is the offset's SCALE. A drop is a billboard whose screen size the renderer fixed, so a flat screen-space offset suits it;
        // a ring is a patch of ground, and the same flat offset would leave the ring at your feet barely bent while smearing one 20 m out across the whole puddle. So the offset is a WORLD displacement -
        // what a film SS_RING_REFRACT_M deep bends a ray by - turned into screen space by the fragment's own depth, the 0.87 folding in the NDC per radian of a 60 degree vertical fov.
        const float SS_RING_REFRACT_M = 0.02;
        vec3 view = normalize(pos);
        vec2 refract_tc = clamp(frag_tc + norm.xy * (SS_RING_REFRACT_M * 0.87 / max(length(pos), 0.25)), vec2(0.001), vec2(0.999));
        vec3 beneath = has_scene ? texture(sceneMap, refract_tc).rgb : srgb_to_linear(vec3(ASSUMED_ALBEDO)) * lit;

        // The re-lighting only ever DARKENS, and the reflection is MIXED in rather than added. That is not a look choice, it is what keeps the loop from running away: the scene map is last frame's scene
        // at this same pixel, this quad drew into it, and alpha blending makes the frame-to-frame gain (1 - a + a * relight) - so any relight above 1 held over a pixel compounds geometrically until the
        // ring blows out white. A film of water darkens the diffuse under it and trades the rest for reflection, which is the same shape as the constraint.
        float wrapped_flat = max((dot(normalize(vary_normal), light_dir) + wrap) / (1.0 + wrap), 0.0);
        float relight = (wrapped_flat > 0.01) ? clamp(wrapped / wrapped_flat, 0.4, 1.0) : 1.0;

        // Water fresnel at 0.02 normal-incidence, the drops' own transmit-head-on/reflect-at-the-edges split: the flanks turned away from the eye go to sky, the flat parts stay all but transparent. The
        // sun's glint along a crest is the only additive term, and it is the one thing in here that cannot feed back on itself.
        float fres = 0.02 + 0.98 * pow(1.0 - clamp(dot(-view, norm), 0.0, 1.0), 5.0);
        float rl = max(dot(reflect(view, norm), light_dir), 0.0);
        vec3 glint = srgb_to_linear(sunlit) * shadow * pow(rl, 64.0) * 0.6;

        color.rgb = mix(beneath * relight, glossenv, fres) + glint;
    }
    else
    {
        color.rgb = srgb_to_linear(tex.rgb * vertex_color.rgb) * lit;
    }
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
