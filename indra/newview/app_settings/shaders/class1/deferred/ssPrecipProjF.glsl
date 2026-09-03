/**
 * @file ssPrecipProjF.glsl
 * @brief Atmo Magic projector fragment shader (SS:Nexii): one projected
 *        spotlight's contribution to precipitation, added over the particles
 *        that have already been drawn.
 *
 * Precipitation is batched into a single buffer spanning the whole visible
 * scene, so it has no per-object light list the way an ordinary alpha
 * drawable does and cannot run the forward light loop. A projector is drawn
 * as its own additive pass over that buffer instead: one pass per light, the
 * same shape of thing the deferred pipeline does for opaque geometry, except
 * that here the surface being lit is the drop rather than the gbuffer.
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

// The projector's own uniforms - proj_mat, proj_n, proj_range, proj_lod, proj_focus, proj_ambiance, projectionMap, color and size - are all declared by deferredUtil.glsl, which is attached because
// this shader asks for reflection probes. Only the light centre is ours: the deferred spot pass carries it as a varying off the light volume it draws, and there is no light volume here, so it comes
// in already in view space.
uniform vec3 center;
uniform float falloff;

// How much light the drops throw back, and how forward-biased that scatter is. The gain is small by design: see the note in main().
uniform float ss_scatter_gain;
uniform float ss_scatter_aniso;

// Shape of the water on the sprite and how the beam's light is distributed over it; the same dials the rain shader shades daylight with, so a drop keeps one surface whatever is lighting it
uniform float ss_drop_bulge;
uniform float ss_drop_core;
uniform float ss_drop_sparkle;

// 1 for the surface-aligned ripples, which are not scatterers hanging in the beam but wet ground being lit by it, and 1 again when the ring art bound is the generated one, whose colour channels
// carry the wave's own normal
uniform float ss_decal;
uniform float ss_decal_normals;

in vec3 vary_position;
in vec3 vary_normal;
in vec3 vary_axis;
in vec4 vertex_color;
in vec2 vary_texcoord0;

bool clipProjectedLightVars(vec3 light_center, vec3 pos, out float dist, out float l_dist, out vec3 lv, out vec4 proj_tc);
vec3 getProjectedLightDiffuseColor(float light_distance, vec2 projected_uv);
float calcLegacyDistanceAttenuation(float distance, float falloff);
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

    vec3 lv;
    vec4 proj_tc;
    float dist, l_dist;
    if (clipProjectedLightVars(center, pos, dist, l_dist, lv, proj_tc))
    {
        discard;   // outside the light's radius
    }

    float dist_atten = calcLegacyDistanceAttenuation(dist, falloff);
    if (dist_atten <= 0.0)
    {
        discard;
    }

    // Outside the projected frustum there is no beam to be lit by. The ambiance term the deferred pass applies around the cone is deliberately left off: it exists to keep a projector from cutting a
    // hard edge across a wall, and on precipitation it would just raise a flat haze over every drop in radius whether it was in the beam or not.
    if (proj_tc.z < 0.0 || proj_tc.x < 0.0 || proj_tc.x > 1.0
                        || proj_tc.y < 0.0 || proj_tc.y > 1.0)
    {
        discard;
    }

    vec3 dlit = getProjectedLightDiffuseColor(l_dist, proj_tc.xy);

    vec3 v = normalize(pos);               // eye -> drop
    vec3 to_light = normalize(lv);         // drop -> light

    // A ripple is the one thing in this buffer that is not a drop. It lies on the ground with the ground's own normal, so a beam crossing it lights it the way it lights the concrete around it - by
    // how squarely the water faces the light - and not by how much of the beam it redirects on the way through. Run through the scatterer maths it came out glowing from the side and dimmest looked
    // straight down at, which is backwards for a puddle. The generated ring carries the crest's shape in its colour channels, so the highlight runs along the wave rather than washing the whole disc.
    if (ss_decal > 0.5)
    {
        vec3 dnorm = normalize(vary_normal);
        if (ss_decal_normals > 0.5)
        {
            vec3 T = normalize(vary_axis - dnorm * dot(dnorm, vary_axis));
            vec3 B = cross(dnorm, T);
            vec3 n_ts = texture(diffuseMap, tc).rgb * 2.0 - 1.0;
            dnorm = normalize(T * n_ts.x + B * n_ts.y + dnorm * n_ts.z);
        }

        float nl = max(dot(dnorm, to_light), 0.0);
        float rl = max(dot(reflect(v, dnorm), to_light), 0.0);
        vec3 wet = dlit * dist_atten * (nl + pow(rl, 64.0) * 1.5);

        frag_color = max(vec4(wet * vertex_color.rgb, final_alpha), vec4(0));
        return;
    }

    // A drop in a beam is not a surface being shaded, it is a scatterer, and the two do not look remotely alike. Lighting it as a diffuse surface - beam colour times the sprite's own texture, which
    // is white with the shape carried in the alpha - turns every drop into an opaque white blob and throws away the refraction the art is built around. What is wanted is the small amount of the beam
    // the drop redirects toward the eye, added to the water look rather than painted over it. Henyey-Greenstein, the standard phase function for this: 1.0 at g = 0 for isotropic, sharply
    // forward-peaked as g rises. Water drops scatter strongly forward, so looking into a beam lights the rain up and looking across it barely does - which is what makes a spotlight in rain read as a
    // beam and not as a glowing fog. At the default g that is about a twenty-five to one ratio between the two.
    float cos_fwd = dot(v, -to_light);     // beam travel vs view

    float g = clamp(ss_scatter_aniso, 0.0, 0.95);
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cos_fwd, 0.0001);
    float phase = min((1.0 - g2) / pow(denom, 1.5), 24.0);

    // Glint off the water itself, on the same surface the rain shader shades daylight with: read off the gradient of the sprite's coverage, per splat, rather than off one cylinder wrapped around the
    // whole quad. See the long note in ssPrecipRainF.glsl - the two have to agree or a drop changes shape the moment a spotlight reaches it.
    vec3 axis = normalize(vary_axis);
    vec3 face = normalize(vary_normal);
    vec3 across = cross(axis, face);
    float across_len = length(across);
    across = (across_len > 0.001) ? across / across_len : normalize(cross(axis, vec3(0.0, 0.0, 1.0)));

    vec2 tsize = vec2(textureSize(diffuseMap, 0));
    vec2 texel = 1.0 / tsize;
    float ax = texture(diffuseMap, tc + vec2(texel.x, 0.0)).a
             - texture(diffuseMap, tc - vec2(texel.x, 0.0)).a;
    float ay = texture(diffuseMap, tc + vec2(0.0, texel.y)).a
             - texture(diffuseMap, tc - vec2(0.0, texel.y)).a;

    vec2 lat = -vec2(ax, ay) * tsize * 0.5 * ss_drop_bulge;
    float lat_len = length(lat);
    if (lat_len > 3.0) lat *= 3.0 / lat_len;

    vec3 norm = normalize(face + across * lat.x + axis * lat.y);

    // Broad sheen and tight glint, as in daylight. Under a beam the tight lobe is doing most of the work: it is what picks a handful of drops out of the rain as points of light and leaves the rest
    // of them dark, which is the whole of what makes a lit downpour read as rain rather than as smoke.
    float rl = max(dot(reflect(v, norm), to_light), 0.0);
    float glint = pow(rl, 48.0) + pow(rl, 1024.0) * ss_drop_sparkle;

    // The drops are drawn well fatter than life so that they read on screen at all. That is fine while the sky is lighting them, because the sky lights the whole sprite about equally and a fat drop
    // just looks like a near drop; it stops being fine the moment a beam is on them. Scatter spread evenly over the fat footprint gives a lit drop the one thing water never looks like - an even
    // bright slab - and the thicker the art, the worse it gets, which is exactly backwards from what the thickness was for. So the beam's light is weighted toward the densest part of the coverage
    // instead of laid flat across it. The wash keeps a tenth of its strength out at the silhouette and full strength down the spine, and the glint, which is already narrow, is confined harder still.
    // A thick drop then lights up as a bright line inside a dark body - the streak - rather than as a filled white shape, and the art keeps the size it was given for legibility without paying for it
    // under every lamp in the scene.
    float core = pow(splat, ss_drop_core);

    // Tinted by the beam and by the drop's own tint, never by the sprite's white texture: the texture's job here is coverage, which the alpha in the blend already applies.
    vec3 lit = dlit * dist_atten * ss_scatter_gain
             * (phase * mix(0.1, 1.0, core) + glint * core * 4.0);

    vec4 color;
    color.rgb = lit * vertex_color.rgb;
    color.a = final_alpha;

    frag_color = max(color, vec4(0));
}
