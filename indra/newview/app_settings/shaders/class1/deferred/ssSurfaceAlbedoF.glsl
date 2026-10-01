/**
 * @file class1/deferred/ssSurfaceAlbedoF.glsl
 * @brief Atmo Magic surface weather albedo. A screen space pass over the
 *        gbuffer's diffuse attachment that darkens and tints wet and stained
 *        surfaces, tints and cracks ice, whitens frost, and lays down the
 *        deposit palette (snow, sand, dust, ash) as a layer sitting ON the
 *        surface rather than a lift toward one hardcoded white. Was
 *        ssSurfaceSnowF.glsl; renamed with the widened job (doc section 3).
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

// <SS:Nexii> Atmo Magic surface albedo (was ssSurfaceSnowF.glsl). Design: doc/atmo_magic_surface_weather.md sections 3-4, 7-8. AUDIT (finding 3): this is its own GLSL compilation unit - diffuseRect/specularRect and every ssSurfaceStateF.glsl uniform/function this pass reads have to be re-declared/prototyped HERE too, matching uniform declarations across units are merged at link but nothing else is shared. [interaction: ssSurfaceWetF.glsl read for ssWetFlattenCosZero/Full, ssWetPuddleDepthFull, ssPuddleMaskAmt/ScaleM/Anchor and the n_geo_world dFdx/dFdy derivation, kept identical so the wet and albedo passes agree about which fragments are level and which count as a full puddle - finding 10].

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform mat4 ssFieldInvView;
uniform vec4 ssFieldOrigin;
uniform sampler2D diffuseRect;
uniform sampler2D specularRect;

// The resolved looks and toggles (ssSurfaceStateF.glsl declares the same names - re-declared here, own compilation unit).
uniform vec4 ssLiquidLook;     // rgb tint, a opacity
uniform vec4 ssDepositLook;    // rgb tint, a sparkle (base, before age)
uniform vec4 ssDepositLook2;   // x translucency, y depthFull (metres), z wash, w melts
uniform float ssSurfaceDebug;
uniform float ssSurfaceIceOn;
uniform float ssSurfaceFrostOn;
uniform float ssSurfaceFrostStrength;

// LOCKSTEP-adjacent art constant re-declared per unit like the rest (doc section 8).
const float SS_DRIFT_CARRY = 0.8;

// The legacy "snow surfaces" master toggle/scale (doc section 8, audit finding 11) - the deposit LAYER's own strength dial, not a gate on the whole pass; wet darkening/ice/frost run regardless of it.
uniform float ssSnowStrength;

// Same names, same values as ssSurfaceWetF.glsl - the puddle-full depth, the flatten taper's cosines, and the shoreline carve. Declared again here (a different program) so this pass answers "how full a puddle" and "how level" exactly as the wet pass does for the same fragment (audit finding 10).
uniform float ssWetPuddleDepthFull;
uniform float ssWetFlattenCosZero;
uniform float ssWetFlattenCosFull;
uniform float ssPuddleMaskAmt;      // 0 disables the carve, 1 full
uniform vec2 ssPuddleMaskAnchor;    // agent-space origin of the lattice

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);
vec4 ssFieldAt(vec3 p_agent, vec3 n_agent);
vec4 ssFieldFetch(vec2 xy_agent);
float ssFieldHash(vec2 p);

// <SS:Nexii> ssSurfaceStateF.glsl, linked ahead of this file - prototypes only (finding 3). [interaction: ssSurfaceStateF.glsl]
vec4 ssFieldFetchState(vec2 xy_agent);
float ssSurfaceLevel(vec3 n_geo_world, float cosZero, float cosFull);
float ssPorosityAt(vec2 tc, vec4 albedo, vec4 spec, bool is_pbr);
float ssDepositCoverage(float depth, float depthFull);
float ssWetAlbedoExponent(float wet, float porosity);
float ssWorldNoise(vec2 p, float pitchM);
float ssFieldWindCarry(vec2 xy_agent);
float ssDepositMask(float coverage, float up, vec3 p);
float ssSparkleCell(vec3 p, float pitchM, float fraction);
float ssDepositSparkle(float base, float age);
float ssPuddleMaskNoise(vec2 m);

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 col = texture(diffuseRect, tc);
    vec4 spec = texture(specularRect, tc);

    float depth = getDepth(tc);
    vec4 pos_view = getPositionWithDepth(tc, depth);

    // <SS:Nexii> AUDIT (finding 7): hoisted ahead of the sky/HDRI return below - dFdx/dFdy of view position must run before any fragment in the quad could have taken a different control-flow path. This used to sit after that return, which is undefined per the GLSL spec.
    vec3 n_geo_view = cross(dFdx(pos_view.xyz), dFdy(pos_view.xyz));
    if (dot(n_geo_view, -pos_view.xyz) < 0.0) n_geo_view = -n_geo_view;
    vec3 n_geo_world = normalize(mat3(ssFieldInvView) * n_geo_view);

    // decodeNormal() reconstructs xyz from the octahedral encoding but never assigns w - the flag channel comes along for the ride in the same texture but is not part of what that function decodes.
    vec4 raw = getNormRaw(tc);
    float flag = raw.w;
    vec4 norm = decodeNormal(raw);

    // Sky, stars, the sun disc, HDRI - none are surfaces, none hold weather
    if (GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_HDRI) ||
        GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        frag_color = col;
        return;
    }

    vec3 p = (ssFieldInvView * vec4(pos_view.xyz, 1.0)).xyz;
    vec3 n = normalize(mat3(ssFieldInvView) * norm.xyz);

    vec4 field = ssFieldAt(p, n);

    // The field hands back a negative exposure for anything it has no answer for (outside the window, or standing above its own column) - neither holds weather.
    if (field.w < 0.0)
    {
        frag_color = col;
        return;
    }

    vec4 state = ssFieldFetchState(p.xy);   // x ice, y frost, z stain, w age

    float wet = field.x * field.w;          // no avatar fold here - the albedo pass has no per-avatar model, stated in the doc
    float puddle = clamp(field.z / ssWetPuddleDepthFull, 0.0, 1.0);
    float level = ssSurfaceLevel(n_geo_world, ssWetFlattenCosZero, ssWetFlattenCosFull);

    // <SS:Nexii> AUDIT (finding 10): the same height-above-column fade and shoreline carve ssSurfaceWetF.glsl applies to its own level/puddle, so the two passes agree where the puddle actually is rather than the wet pass's shore-carved edge and the albedo pass's uncarved metre-cell circle disagreeing (worst with an opaque liquid like Ink Rain). [interaction: ssSurfaceWetF.glsl]
    vec4 field_here = ssFieldFetch(p.xy);
    float cell = ssFieldOrigin.z;
    level *= 1.0 - smoothstep(min(cell * 0.5, 0.20), min(cell * 1.5, 0.45), p.z - field_here.x);
    if (ssPuddleMaskAmt > 0.0 && puddle > 0.004)
    {
        float mask_v = ssPuddleMaskNoise(p.xy - ssPuddleMaskAnchor);
        float shore = smoothstep(0.47, 0.56, mask_v);
        puddle *= mix(1.0, shore, ssPuddleMaskAmt);
    }

    bool is_pbr = GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_PBR);
    float porosity = ssPorosityAt(tc, col, spec, is_pbr);

    // The base deposit coverage from depth alone - reused by frost (buries it) and by the deposit layer below (which additionally gates on exposure/wind carry).
    float depBase = ssDepositCoverage(field.y, ssDepositLook2.y);

    // Diagnostic: paint the requested channel as grayscale and stop, before any of the look chain touches col. 1 ice, 2 frost, 3 stain, 4 age, 5 porosity.
    if (ssSurfaceDebug > 0.5)
    {
        int mode = int(ssSurfaceDebug + 0.5);
        float dbg = 0.0;
        if (mode == 1) dbg = state.x;
        else if (mode == 2) dbg = state.y;
        else if (mode == 3) dbg = state.z;
        else if (mode == 4) dbg = state.w;
        else if (mode == 5) dbg = porosity;
        frag_color = vec4(vec3(dbg), col.a);
        return;
    }

    // (a) Wet darkening (Lagarde: darker AND more saturated, more so the more porous). Ice on the water eases this off - a glaze is not absorbed the way a puddle soaking into gravel is.
    float wetForDarken = wet * (1.0 - state.x * 0.6);
    col.rgb = pow(col.rgb, vec3(ssWetAlbedoExponent(wetForDarken, porosity)));

    // (b) Liquid tint, by the look's opacity, over whichever of plain wet or a level puddle is greater.
    col.rgb = mix(col.rgb, ssLiquidLook.rgb, ssLiquidLook.a * max(wet, puddle * level));

    // (c) Stain - the colour of the liquid that set it (state.z), not necessarily the one falling now. With one liquid at a time this is the current look; stated limitation (doc section 9).
    col.rgb = mix(col.rgb, ssLiquidLook.rgb * 0.9, state.z * 0.85);

    // (d) Puddle floor - a slight darkening, yielding to the liquid tint's own opacity (an opaque puddle has no "floor" to darken).
    col.rgb *= 1.0 - 0.12 * puddle * level * (1.0 - ssLiquidLook.a);

    // (e) Ice - frozen puddle or glaze: pale blue-white lift, a world-hashed cell-edge crack pattern, a sparse glint.
    if (ssSurfaceIceOn > 0.5)
    {
        float ice = state.x * max(puddle * level, wet * 0.5);
        if (ice > 0.001)
        {
            const float latticeM = 0.35;
            vec2 cellUV = p.xy / latticeM;
            vec2 cellF = fract(cellUV);
            vec2 edgeDist = min(cellF, 1.0 - cellF);
            float crack = (1.0 - smoothstep(0.0, 0.04, min(edgeDist.x, edgeDist.y))) * ice;
            float h = ssFieldHash(floor(cellUV));
            float glint = step(0.97, h) * ice * 0.25;

            col.rgb = mix(col.rgb, vec3(0.80, 0.86, 0.92), ice * 0.55);
            col.rgb += vec3(crack + glint);
        }
    }

    // (f) Frost - a white lift on up-facing/exposed surfaces, fine world-hashed grain, a glint; buried by any real deposit (depBase).
    if (ssSurfaceFrostOn > 0.5)
    {
        // <SS:Nexii> AUDIT (finding 8): gated by exposure (field.w) - without it frost grows on an indoor floor exactly as readily as a cold, humid night outdoors.
        float f = state.y * ssSurfaceFrostStrength * (0.35 + 0.65 * level) * (1.0 - depBase) * clamp(field.w, 0.0, 1.0);
        if (f > 0.001)
        {
            const float grainM = 0.03;
            float grain = ssWorldNoise(p.xy, grainM);
            col.rgb = mix(col.rgb, vec3(0.92, 0.94, 0.97) * (0.85 + 0.15 * grain), f * 0.8);

            float h = ssFieldHash(floor(p.xy / grainM));
            float glint = step(0.95, h) * f * 0.3 * clamp(n.z, 0.0, 1.0);
            col.rgb += vec3(glint);
        }
    }

    // (g) Deposit - the snow/sand/dust/ash layer. Wind carry (doc section 8): blown material settles in the lee of a wall even where the sky-view exposure term alone would leave it bare. AUDIT (finding 11): ssSnowStrength scales the deposit LAYER's own visual coverage only - depBase above (which also buries frost) is left alone, and the whole pass no longer early-returns on this setting, so turning "snow surfaces" off/down still leaves wet darkening, ice and frost working.
    float exposure_d = max(clamp(field.w, 0.0, 1.0), ssFieldWindCarry(p.xy) * SS_DRIFT_CARRY);
    float coverage = depBase * exposure_d * ssSnowStrength;
    float mask = ssDepositMask(coverage, clamp(n.z, 0.0, 1.0), p);
    if (mask > 0.0)
    {
        vec3 tint = mix(ssDepositLook.rgb, ssDepositLook.rgb * 0.75, state.w);   // age greys it
        float grain = ssWorldNoise(p.xy, 0.02);
        vec3 layer = tint * (0.93 + 0.07 * grain);

        // Translucency lets a thin dusting show the albedo underneath; a full layer hides it.
        vec3 under = mix(layer, layer * (0.70 + 0.30 * dot(col.rgb, vec3(0.333))),
                          ssDepositLook2.x * (1.0 - smoothstep(0.3, 1.0, coverage)));

        col.rgb = mix(col.rgb, under, mask);

        // A small albedo glint only - the real, view-dependent sparkle lives in the wet pass's specular treatment (doc section 8 / slice E), not here.
        col.rgb += vec3(0.08 * ssSparkleCell(p, 0.03, 0.04) * mask * ssDepositSparkle(ssDepositLook.a, state.w));
    }

    frag_color = vec4(col.rgb, col.a);
}
