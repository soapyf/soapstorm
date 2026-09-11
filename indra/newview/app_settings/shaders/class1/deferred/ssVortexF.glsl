/**
 * @file ssVortexF.glsl
 * @brief Atmo Magic vortex funnels - Z-billboarded collar quads.
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

// <SS:Nexii> Atmo Magic vortex funnels

out vec4 frag_color;

in vec2 vary_texcoord0;   // x: u across the collar width [0,1]; y: h01, vertical fraction (0 contact, 1 wall cloud)
in vec4 vary_color;       // r: lit-rim facing term for this corner; g: condensation; b: slot / (MAX_ACTIVE - 1); a: base alpha

// A COPY of the scene depth - see the depthMap note in ssVolCloudF.glsl (only RESERVED uniform names may be bound
// as textures; depthMap is one of them, unlike an arbitrary "sceneDepth").
uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform vec2 ss_clip;
uniform float ss_soft_m;   // metres of soft depth fade; 0 disables it

uniform float ss_gloom;    // the coupled weather deck's own storm gloom - SSVolCloud::weatherGloom()
uniform float ss_time;     // the SHARED wall clock (SSAtmoMagic::sharedTime), wrapped - see ssvortexrender.cpp

// <SS:Nexii> The sky's own windlight uniforms, auto-filled the same SG_SKY way ssVolCloudV/F's are (gSSVortexProgram
// carries the identical mFeatures/SG_SKY registration - see llviewershadermgr.cpp) - declared FRAGMENT-side only
// (the vertex stage has nothing to shade), which is fine: a linked program's uniform locations are shared across
// stages regardless of which one declares a given name. Scope-narrowing judgement call: no separate moonlight_color/
// sun_up_factor branch the way ssVolCloudV.glsl carries one - a funnel at night reads by ambient_color alone,
// which is dim but not black; a full day/night split was judged not worth a second light path for a cosmetic rim.
uniform vec3 sunlight_color;
uniform vec3 ambient_color;

// <SS:Nexii> Keep in sync with SSVortex::MAX_ACTIVE (ssvortexcore.h) - the funnel-render budget, three collar
// stacks at once. ss_vortex_multi[i] is (multiN, multiOmega, multiPhase, multiWeight) for the funnel drawn at
// slot i, matching SSVortex::Candidate/State's own fields field-for-field; ss_vortex_n is how many of those
// SSVortex::MAX_ACTIVE slots are actually live this draw (informational - a slot beyond it is zero-filled and
// radiusModulation's own n==0 test already reads that as identity, so nothing depends on this for correctness).
#define SS_VORTEX_MAX_ACTIVE 3
uniform int ss_vortex_n;
uniform vec4 ss_vortex_multi[SS_VORTEX_MAX_ACTIVE];

// <SS:Nexii> The GPU twin of SSVortex::radiusModulation's angular term (ssvortexcore.h) - the ONLY shader-side
// twin this system carries (its own file-top comment: "the only GPU twin is radiusModulation"). Spent here as a
// RADIAL ALPHA MASK rather than a vertex displacement: theta is read straight off the billboard's own horizontal
// texcoord (see the theta line below), never recovered from world position, so this needs no CPU-side twin of its
// own beyond uploading the same four hashed numbers the core already produced (ss_vortex_multi). Invariant carried
// over unchanged: 1.0 when n == 0 or multiWeight <= 0.0.
const float SS_VORTEX_MULTI_AMP_MAX = 0.45; // SSVortex::MULTI_AMP_MAX
// <SS:Nexii> LOCKSTEP with SSVortex::MULTI_RADIUS_EDGE_UNITS (ssvortexcore.h) - a core header cannot be read from
// GLSL, so this is a hand-mirrored copy of the same literal, not a #include. FIX 2: used three times below now -
// once as the smoothstep half-width of radius_mask itself, and TWICE (added into SS_VORTEX_MULTI_AMP_MAX, matching
// cardHalfWidthM's own 1 + AMP_MAX + 2 * EDGE) in the radial_units conversion, so the card edge (radial_units == +-1
// at vary_texcoord0.x's own [0,1] ends) sits strictly past where the smoothstep band actually reaches zero, not
// exactly at it - see cardHalfWidthM's own comment for the hard-edge bug one unit of headroom there left in place.
const float SS_VORTEX_RADIUS_EDGE = 0.05;
float ss_vortex_radiusModulation(float theta, float t, int n, float omega, float phase, float multiWeight)
{
    if (n == 0 || multiWeight <= 0.0)
    {
        return 1.0;
    }
    float amp = SS_VORTEX_MULTI_AMP_MAX * clamp(multiWeight, 0.0, 1.0);
    float ang = float(n) * theta + omega * t + phase;
    return 1.0 + amp * sin(ang);
}

// Eye-space distance from a depth-buffer reading - the ordinary inverse of the projection, same line ssVolCloudF/
// ssLightningF both carry.
float ss_eye_z(float d)
{
    float ndc = d * 2.0 - 1.0;
    return (2.0 * ss_clip.x * ss_clip.y)
         / (ss_clip.y + ss_clip.x - ndc * (ss_clip.y - ss_clip.x));
}

void main()
{
    int slot = int(vary_color.b * float(SS_VORTEX_MAX_ACTIVE - 1) + 0.5);
    slot = clamp(slot, 0, max(ss_vortex_n - 1, 0));
    vec4 multi = ss_vortex_multi[slot];

    // theta from the billboard's own horizontal texcoord: a Z-billboard only ever shows the funnel's FRONT
    // hemisphere, so u in [0,1] is read as spanning that hemisphere, -PI/2 (left silhouette edge) to +PI/2 (right
    // silhouette edge) - a judgement call (the core states theta's UNIT, radians periodic in 2*pi/N, but not how a
    // flat card's own u maps to it; see ssvortexrender.cpp's own comment for the rest of this file's judgement
    // calls).
    float theta = (vary_texcoord0.x - 0.5) * 3.14159265;

    // <SS:Nexii> Review finding 5 / NEW-2: the multi-vortex term is a RADIUS mask, never a brightness/alpha multiply
    // (this system's own file-top comment in ssvortexcore.h and ssvortexrender.cpp's own top-of-file comment: "the
    // multi-vortex term is a RADIUS modulation ... never a brightness multiply"), and the funnel's WHOLE silhouette
    // is owned by this ONE mask - no second, angle-independent ramp attenuates inside the boundary it draws.
    // ssvortexrender.cpp widens every collar card to SSVortex::cardHalfWidthM(radius) == radius * (1 + MULTI_AMP_MAX
    // + 2 * MULTI_RADIUS_EDGE_UNITS) (FIX 2, the core's own invariant), so vary_texcoord0.x's [0,1] span covers that
    // WIDENED half-width; converting it back to TRUE collar-radius units needs that same factor, mirrored here as
    // SS_VORTEX_RADIUS_EDGE added in TWICE (LOCKSTEP, see its own declaration comment). radius_mod is
    // SSVortex::radiusModulation's own GPU twin (ss_vortex_radiusModulation, unchanged above), read here as the
    // boundary a fragment must fall inside of, in collar-radius units, rather than as a scalar to multiply in.
    float radial_units = (vary_texcoord0.x * 2.0 - 1.0) * (1.0 + SS_VORTEX_MULTI_AMP_MAX + 2.0 * SS_VORTEX_RADIUS_EDGE);
    float radius_mod = ss_vortex_radiusModulation(theta, ss_time, int(multi.x + 0.5), multi.y, multi.z, multi.w);
    // Smooth discard edge, a fixed SS_VORTEX_RADIUS_EDGE (collar-radius units) band rather than fwidth() - the
    // collar quad is a near-edge-on billboard at grazing view angles, where screen-space derivatives blow up and
    // fwidth's own band would swing wildly with view angle instead of staying a fixed physical width on the funnel.
    // FIX 2 (NEW-2 continued): this is the funnel's ONLY silhouette edge - the card itself is widened
    // (cardHalfWidthM, see above) STRICTLY past radius_mod's own worst case (1 + SS_VORTEX_MULTI_AMP_MAX at
    // multiWeight 1) plus the band's own half-width, by one further SS_VORTEX_RADIUS_EDGE unit of margin, so this
    // smoothstep always reaches its 0 end (fragment fully discarded) strictly BEFORE radial_units can reach the
    // card's own edge, never exactly at it; there is no second, separate edge fade left to run past this one, and
    // none may be added inside the boundary this draws (the render rule: one mask, one smooth edge).
    float radius_mask = 1.0 - smoothstep(radius_mod - SS_VORTEX_RADIUS_EDGE, radius_mod + SS_VORTEX_RADIUS_EDGE, abs(radial_units));

    // <SS:Nexii> A soft VERTICAL alpha ramp for condensation - deliberately NOT a second twin of
    // SSVortex::collarRadius's own shrink band (that stays the system's only GPU twin, radiusModulation, per the
    // file-top comment): this is a coarser, independent cosmetic fade layered OVER geometry collarRadius has
    // already shrunk toward zero-radius near the un-condensed base, so a nearly-degenerate thin collar there fades
    // out by alpha too rather than surviving as a hairline at full opacity. threshold mirrors collarRadius's own
    // "below (1 - condensation) is un-condensed" convention but with its own, wider edge - no bit-identity is
    // claimed or needed here.
    float threshold = 1.0 - vary_color.g;
    float vert_ramp = smoothstep(threshold - 0.12, threshold + 0.12, vary_texcoord0.y);

    float a = vary_color.a * vert_ramp * radius_mask;

    // Soft depth fade against the scene depth copy - the same idiom the puffs (ssVolCloudF.glsl) and the lightning
    // aura/flare discs (ssLightningF.glsl) both carry.
    if (ss_soft_m > 0.0)
    {
        float scene_z = ss_eye_z(texture(depthMap, gl_FragCoord.xy / screen_res).r);
        float frag_z = ss_eye_z(gl_FragCoord.z);
        a *= clamp((scene_z - frag_z) / ss_soft_m, 0.0, 1.0);
    }

    if (a <= 2.0 / 255.0)
    {
        discard;
    }

    // Dark grey-blue condensation body, lit rim: vary_color.r is the CPU-computed facing term for this corner
    // (ssvortexrender.cpp) - which edge of the billboard faces the sun horizontally - spent here as an ordinary
    // wrap against the deck's own sunlight_color/ambient_color (auto-filled the same SG_SKY way ssVolCloudV/F's
    // own light uniforms are, see gSSVortexProgram's registration in llviewershadermgr.cpp) and graded by the
    // deck's own storm gloom (ss_gloom) - a judgement call on HOW MUCH: the funnel is not a puff with a buried
    // depth to grade over, so gloom is spent as a flat 60% blend toward it rather than SSVolCloud's own buried-depth
    // gradient, which has no equivalent here.
    vec3 base = vec3(0.15, 0.16, 0.20);
    vec3 amb = ambient_color * base;
    vec3 sun = sunlight_color * base * vary_color.r;
    vec3 shaded = (amb * 0.6 + sun * 0.9) * mix(1.0, ss_gloom, 0.6);
    shaded = clamp(shaded, vec3(0.0), vec3(1.0));

    frag_color = vec4(shaded, a);
}
