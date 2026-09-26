/**
 * @file ssInfoLookF.glsl
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

// <SS:Nexii> Atmo Magic info-view LOOK - the post-screen pass that replaced the world-dim quad (doc/atmo_magic_phase8_show.md section 6 item 4; the user's verdict on the quad: "the world dimming still sucks, it is just making the world black at max"). It runs ONCE, over the already-presented image, at the info view's own draw site (SSAtmoInfoView::renderInfoLook, called from render_ui() after gPipeline.renderFinalize() and before render_hud_attachments): post-tonemap, default framebuffer, world camera, blend OFF - it OVERWRITES the world, it does not tint it. Every formula below is a transliteration of indra/newview/ssinfolookcore.h (namespace SSInfoLook); V:\Scratch\atmo\tests\twin_infolook.cpp parses this file and asserts the two agree. ALPHA IS HANDLED BY CONSTRUCTION: a blended surface (water, glass, particles, rain curtains) is already composited into the colour this pass reads, and it takes the occlusion, hemisphere shade and fog of the OPAQUE surface behind it, because depth and normal at that pixel belong to the opaque one - which is what a real fog does to a pane of glass, and is why the decision was "a pass that works like extreme fog" instead of a warm-gray variant of every material shader in the fork. [interaction: render_ui] [interaction: LLPipeline::mSSLastPresented]

out vec4 frag_color;

in vec2 vary_fragcoord;

// The PRESENTED colour: the exact render target LLPipeline::renderFinalize handed to the "Present the screen target"
// pass (LLPipeline::mSSLastPresented). Not the HDR scene buffer - tonemap, CAS, glow, DoF, FSAA, the RLV sphere and
// the vignette have all already run, so the luminance read here is display-space and the ramp is authored against
// what the eye is actually shown.
uniform sampler2D diffuseRect;

// World up and the key direction, both in VIEW space (the space the G-buffer normals live in), uploaded by the call
// site: up = mat3(modelview) * (0,0,1), key = the environment's own light direction flattened toward the horizon.
uniform vec3 ss_look_up;
uniform vec3 ss_look_key;

// deferredUtil.glsl (attached by mFeatures.isDeferred) owns normalMap, depthMap, inv_proj and these four:
float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);

// syncMatrices uploads this from the gGL projection stack whenever the shader declares it; the occlusion needs it to
// put a view-space sample point back on screen.
uniform mat4 projection_matrix;

// ---------------------------------------------------------------------------
// The core's constants, transliterated from ssinfolookcore.h. LOCKSTEP: the twin parses these lines.
// ---------------------------------------------------------------------------
const vec3  LOOK_DARK        = vec3(0.36, 0.33, 0.30);
const vec3  LOOK_LIGHT       = vec3(0.92, 0.89, 0.84);
const float LOOK_GAMMA       = 0.70;
const float LOOK_AMBIENT     = 0.45;
const float LOOK_FOG_START   = 400.0;
const float LOOK_FOG_END     = 6000.0;
const vec3  LOOK_FOG         = vec3(0.88, 0.85, 0.80);
const float LOOK_AO_STRENGTH = 0.85;
const vec3  LOOK_SKY         = vec3(0.90, 0.88, 0.84);

// The occlusion kernel. Twelve taps is the design's budget ("one full-screen pass with a dozen depth taps"); the
// radius is a metre and a half of view space, which is the scale that reads on doorways, kerbs and the ground under
// a prim without turning tree canopies into soot. No noise texture: the spiral is fixed and only its phase is hashed
// per pixel, so the pattern is stable frame to frame (no shimmer) while neighbouring pixels decorrelate.
const int   SS_LOOK_AO_TAPS     = 12;
const float SS_LOOK_AO_RADIUS_M = 1.5;
const float SS_LOOK_AO_BIAS_M   = 0.03;

// ---------------------------------------------------------------------------
// The core, transliterated
// ---------------------------------------------------------------------------

// SSInfoLook::lum
float ssLookLum(vec3 c)
{
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

// SSInfoLook::warmGray
vec3 ssLookWarmGray(float l)
{
    float t = pow(clamp(l, 0.0, 1.0), LOOK_GAMMA);
    return mix(LOOK_DARK, LOOK_LIGHT, t);
}

// SSInfoLook::hemiShade
float ssLookHemiShade(float n_up, float n_key)
{
    float hemi = 0.5 + 0.5 * n_up;
    float key  = 0.75 + 0.25 * max(n_key, 0.0);
    return LOOK_AMBIENT + (1.0 - LOOK_AMBIENT) * hemi * key;
}

// SSInfoLook::fogMix
float ssLookFogMix(float dist)
{
    return smoothstep(LOOK_FOG_START, LOOK_FOG_END, dist);
}

// SSInfoLook::occlusionTerm
float ssLookOcclusionTerm(float ao)
{
    return mix(1.0, clamp(ao, 0.0, 1.0), LOOK_AO_STRENGTH);
}

// SSInfoLook::surfaceLook
vec3 ssLookSurface(float l, float n_up, float n_key, float ao, float dist)
{
    vec3  base = ssLookWarmGray(l);
    float s    = ssLookHemiShade(n_up, n_key) * ssLookOcclusionTerm(ao);
    return mix(base * s, LOOK_FOG, ssLookFogMix(dist));
}

// ---------------------------------------------------------------------------
// The occlusion, computed in this pass
// ---------------------------------------------------------------------------

// The pipeline's own SSAO lives in mRT->deferredLight, which the post chain (tonemap/CAS) has already overwritten by
// the time this pass runs, so there is nothing to sample and the occlusion is recomputed here from depth + normal.
float ssLookHash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// A hemisphere direction about n: a golden-angle spiral in the tangent frame, phase-rotated by the per-pixel hash.
vec3 ssLookTapDir(float fi, float rnd, vec3 n)
{
    float ang = 6.2831853 * (fi * 7.0 * 0.61803399 + rnd);
    float z   = sqrt(fi);
    float r   = sqrt(max(1.0 - z * z, 0.0));
    vec3  d   = vec3(cos(ang) * r, sin(ang) * r, z);

    vec3 up = (abs(n.z) < 0.9) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t  = normalize(cross(up, n));
    vec3 b  = cross(n, t);
    return t * d.x + b * d.y + n * d.z;
}

// Visibility in [0,1]: 1 open sky, 0 fully enclosed. View space is right-handed with -z forward, so a stored surface
// OCCLUDES a sample point when its z is GREATER (nearer the eye) than the sample's by more than the bias.
float ssLookOcclusion(vec3 pos, vec3 nrm)
{
    float rnd = ssLookHash(gl_FragCoord.xy);
    float occ = 0.0;

    for (int i = 0; i < SS_LOOK_AO_TAPS; ++i)
    {
        float fi  = (float(i) + 0.5) / float(SS_LOOK_AO_TAPS);
        vec3  dir = ssLookTapDir(fi, rnd, nrm);
        vec3  sp  = pos + dir * (SS_LOOK_AO_RADIUS_M * (0.35 + 0.65 * fi));

        vec4 clip = projection_matrix * vec4(sp, 1.0);
        if (clip.w <= 0.0) continue;

        vec2 stc = (clip.xy / clip.w) * 0.5 + 0.5;
        if (stc.x < 0.0 || stc.x > 1.0 || stc.y < 0.0 || stc.y > 1.0) continue;

        float sd = getDepth(stc);
        if (sd >= 0.99995) continue;   // the sky occludes nothing

        vec3 sample_pos = getPositionWithDepth(stc, sd).xyz;

        // Range check: a wall ten metres in front of a doorway must not shade the doorway. Falls off over the
        // kernel radius, exactly the distance the taps can reach.
        float range = clamp(SS_LOOK_AO_RADIUS_M / max(abs(sample_pos.z - pos.z), 0.0001), 0.0, 1.0);

        if (sample_pos.z > sp.z + SS_LOOK_AO_BIAS_M)
        {
            occ += range;
        }
    }

    return clamp(1.0 - occ / float(SS_LOOK_AO_TAPS), 0.0, 1.0);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    vec3  scene = texture(diffuseRect, tc).rgb;
    float depth = getDepth(tc);
    vec4  raw   = getNormRaw(tc);

    // Sky detection is the whiteout pass's, verbatim and for the same reason: depth first, because the windlight sky
    // dome carries no HDRI flag, with the two flags kept for anything that marks itself off.
    if (depth >= 0.99995 ||
        GET_GBUFFER_FLAG(raw.w, GBUFFER_FLAG_HAS_HDRI) ||
        GET_GBUFFER_FLAG(raw.w, GBUFFER_FLAG_SKIP_ATMOS))
    {
        frag_color = vec4(LOOK_SKY, 1.0);
        return;
    }

    vec3 pos = getPositionWithDepth(tc, depth).xyz;
    vec3 nrm = normalize(decodeNormal(raw).xyz);

    float n_up  = dot(nrm, ss_look_up);
    float n_key = dot(nrm, ss_look_key);
    float ao    = ssLookOcclusion(pos, nrm);

    frag_color = vec4(ssLookSurface(ssLookLum(scene), n_up, n_key, ao, length(pos)), 1.0);
}
