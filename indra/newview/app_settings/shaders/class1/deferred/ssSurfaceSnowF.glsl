/**
 * @file class1/deferred/ssSurfaceSnowF.glsl
 * @brief Atmo Magic snow surfaces. A screen space pass over the gbuffer that
 *        lifts the albedo of anything the field says is holding settled snow,
 *        toward the drift colour the weather leaves behind, and adds the
 *        stable per-cell glint that makes fresh snow read as grains rather
 *        than paint.
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

// <SS:Nexii> Atmo Magic snow surfaces

out vec4 frag_color;

in vec2 vary_fragcoord;

// The albedo attachment, declared here as the wetness pass declares the specular one -
// gbufferUtil is not attached to this program family, so the sampler is ours to name.
uniform sampler2D diffuseRect;

// Agent space from view space. The field is anchored to the world; everything the gbuffer hands back is relative to the eye.
uniform mat4 ssFieldInvView;

// The field lattice, for the cell size. Declared here as well as in ssSurfaceFieldF.glsl - same program, same uniform, one value.
uniform vec4 ssFieldOrigin;

// Master scale on the whole pass - a user who wants no snow surface pays for none of it.
uniform float ssSnowStrength;

// The settled depth, metres, that reads as fully snow-covered. The field's own repose gate keeps
// depths honest; this is only where full white starts.
uniform float ssSnowDepthFull;

// How hard the stable glints bite, 0 to 1.
uniform float ssSnowSparkle;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);
vec4 ssFieldAt(vec3 p_agent, vec3 n_agent);
float ssFieldHash(vec2 p);

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 col = texture(diffuseRect, tc);

    float depth = getDepth(tc);

    // decodeNormal() reconstructs xyz from the octahedral encoding but never assigns w - the flag
    // channel comes along for the ride in the same texture but is not part of what that function
    // decodes (the same trap the wetness pass documented first).
    vec4 raw = getNormRaw(tc);
    float flag = raw.w;
    vec4 norm = decodeNormal(raw);

    // Sky, stars, the sun disc, HDRI - none are surfaces, none hold snow
    if (GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_HDRI) ||
        GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        frag_color = col;
        return;
    }

    vec4 pos_view = getPositionWithDepth(tc, depth);
    vec3 p = (ssFieldInvView * vec4(pos_view.xyz, 1.0)).xyz;
    vec3 n = normalize(mat3(ssFieldInvView) * norm.xyz);

    vec4 field = ssFieldAt(p, n);

    // The field hands back "no answer" (negative exposure) for anything standing above its
    // column - avatars, furniture - and for cells it does not know. Neither holds snow.
    float coverage = 0.0;
    if (field.w >= 0.0 && ssSnowDepthFull > 0.001)
    {
        coverage = clamp(field.y / ssSnowDepthFull, 0.0, 1.0)
                 * clamp(field.w, 0.0, 1.0)
                 * clamp(ssSnowStrength, 0.0, 1.0);
    }

    if (coverage <= 0.004)
    {
        frag_color = col;
        return;
    }

    // The stable glint. A hash in agent space, not screen space, so the pattern does not swim as
    // the camera moves - the same world-anchored trick the puddle mask uses. Thresholded hard,
    // so a few cells per metre glint and the rest do not: that discontinuity is what reads
    // as grains rather than as a brightness ramp. Sun-facing bias comes from the shading normal's
    // upward component - fresh snow glints most when you look across it toward the light, and
    // vertical faces hold none of it.
    float cell = max(ssFieldOrigin.z, 0.05);
    float h = ssFieldHash(floor(p.xy / cell) + floor(p.z * 2.0));
    float glint = step(0.94, h) * clamp(n.z, 0.0, 1.0) * ssSnowSparkle;

    // Fresh drift is near-white with a cold cast; the underlying albedo still modulates a
    // quarter of the result, so a dark roof under 3cm of snow reads as snowy dark-grey rather
    // than as paper, and the relief of the surface survives the lift.
    float shade = 0.72 + 0.28 * clamp(dot(col.rgb, vec3(0.333)), 0.0, 1.0);
    vec3 snow_col = vec3(0.86, 0.88, 0.93) * shade;

    vec3 out_col = mix(col.rgb, snow_col, pow(coverage, 1.25));

    // The glints ride ON TOP of the mix, not inside it - at a dusting the mix is nearly
    // the untouched albedo and glints folded into snow_col would be invisible exactly when the
    // first snow starts catching light. A dusting sparkles; full cover sparkles over white.
    out_col += vec3(glint * 0.30) * smoothstep(0.02, 0.25, coverage);

    frag_color = vec4(out_col, col.a);
}

