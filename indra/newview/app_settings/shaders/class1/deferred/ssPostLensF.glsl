/**
 * @file class1/deferred/ssPostLensF.glsl
 * @brief Atmo Magic: lens drops - a rain-on-glass post pass after depth of field
 *        and before anti-aliasing (the drops are ON the lens, so they are sharp
 *        and they blur what is behind them).
 *
 *        ONE population of water, simulated on its own thread (SSScreenFXPost)
 *        and handed to this pass as a DROP MAP: drops arrive small, grow only by
 *        absorbing each other, break loose when they are too heavy to stay pinned,
 *        and shed what they cannot carry as they run. This file no longer places
 *        any of them - it shades what the map says is there, and thins the
 *        condensation under it by the channel the runners have cut.
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

// <SS:Nexii> Atmo Magic lens drops. EVERY law below is transliterated from indra/newview/sslensdropcore.h - that header is the one
// formula site and V:/Scratch/atmo/tests/twin_lensdrop.cpp holds the two together over a sample grid. Do not retune a constant here;
// retune it there. Design: doc/atmo_magic_surface_weather.md section 12; defect reports R1-R13 of 2026-09-07, then R16 (the rewrite onto
// the reference's one-population merge model) and R17 (the swept channel) the same day.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;
uniform vec2 screen_res;

// How wet the lens is, 0..1 (SSScreenFX::Lens::mWet). The simulation reads it as an ARRIVAL RATE; this pass only uses it for the
// condensation, since what the drops themselves look like is entirely the drop map's business now.
uniform float ssLensWet;

// Condensation, 0..1, already multiplied by the SSAtmoLensCondensation dial - the fog layer's strength before the runners cut it clear
// and before the clear-centre law (R8) thins it toward the middle of the screen.
uniform float ssLensFog;

// The lens clock in seconds (SSScreenFXPost::mLensClock, wall-clock dt accumulated through SSAtmoLensDryRate). Nothing in this pass
// reads it since the simulation moved off it - kept uploaded because the dial it carries still stretches every drop's life.
uniform float ssLensTime;

// screen_res.x / screen_res.y - the window's shape. The drops are placed in stable screen units (x in screen HEIGHTS), so this is what
// turns a normal's x back into UV here, and what the sprite draw divides out on the way in.
uniform float ssLensAspect;

// SSAtmoLensDropsStrength as a refraction-depth dial, 0..2.
uniform float ssLensScale;

// SSAtmoLensStreaks, 0..2 - the population's visibility dial. With one population left it scales the whole drop map's coverage.
uniform float ssLensStreaks;

// SSAtmoLensReflection, 0..2 - the Fresnel reflection's weight on a drop's own normal. 0 is refraction only (the pre-R4 look).
uniform float ssLensReflection;

// SSAtmoLensClearCentre, 0..1 - how strongly the coating thins the CONDENSATION toward the middle of the screen (R8), and how much
// faster it dries the drops there (applied in the simulation). 0 is a uniform field.
uniform float ssLensClear;

// <SS:Nexii> R16: THE DROP MAP. Every drop the simulation carries was drawn once, as a sprite, into this target before the pass ran:
// rgb is the drop's cap normal (SSLensDrop::capNormal, baked into the sprite once at startup) and a is its coverage. ONE fetch per
// fragment, so what is on the glass costs the same here whether it is one drop or five hundred - which is what retired both the 24-slot
// runner array and the per-fragment lattice search the three-population build needed. Where two sprites overlap their normals blend,
// and that neck between them is the surface tension of a merge, for free.
uniform sampler2D ssLensDropMap;

// <SS:Nexii> R17: THE SWEPT CHANNEL. A persistent, quarter-resolution buffer that RUNNING drops mark and that fades on its own much
// slower clock (SSLensDrop::CLEAR_DECAY_S) - so where a drop has torn down the glass stays bare of condensation long after the drop
// itself has gone off the bottom of the frame. The R1-R15 build got this by asking a runner's analytic path "were you here, and how long
// ago"; a texture cannot be asked that, so the state was moved to where it actually belongs - the glass, not the drop that cut it.
uniform sampler2D ssLensClearMap;

// ---------------------------------------------------------------- sslensdropcore.h, transliterated

const float SS_TAU = 6.28318530717958647692;

const float SS_CLEAR_R_IN       = 0.40;
const float SS_CLEAR_R_OUT      = 0.56;
const float SS_CLEAR_COVER_MIN  = 0.12;
const float SS_CONTACT_ANGLE_DEG = 84.0;
const float SS_IOR_WATER         = 1.333;
const float SS_REFRACT_DEPTH     = 0.072;    // 0.12 * 0.60
const float SS_DROP_F0           = 0.02;
const float SS_EDGE_SOFT         = 0.20;

// SSLensDrop::hash21 - same three constants.
float ssLensHash(vec2 p)
{
    return fract(sin(p.x * 12.9898 + p.y * 78.233) * 43758.5453);
}

// SSLensDrop::clearRadial - the ONE FORMULA SITE for the clear-centre geometry (R8), used here on the condensation and, on the
// simulation's side, as a divisor into each drop's evaporation. A hydrophobic coating does not stop water arriving, it stops it
// clinging: a lifetime effect for discrete drops, a straight amount reduction for a continuous haze.
float ssLensClearRadial(float rStable)
{
    float outward = smoothstep(SS_CLEAR_R_IN, SS_CLEAR_R_OUT, rStable);
    return 1.0 - clamp(ssLensClear, 0.0, 1.0) * (1.0 - SS_CLEAR_COVER_MIN) * (1.0 - outward);
}

// SSLensDrop::silhouette. Ascending on purpose: a DESCENDING smoothstep (which is what this file carried before 2026-09-07) is not
// defined by the GLSL spec, and the core's own guarded smoothstep turned it into an inverted hard step - see the note in the core.
float ssLensSilhouette(float r2)
{
    return 1.0 - smoothstep(1.0 - SS_EDGE_SOFT, 1.0, r2);
}

// SSLensDrop::capNormal - the EXACT spherical-cap normal for a drop meeting the glass at CONTACT_ANGLE_DEG, in one line:
// n_xy = delta * sin(theta_c), n_z = sqrt(1 - r2 sin^2(theta_c)). Steepest at the RIM, where a drop actually is steepest. The law this
// replaces (delta * (1 - r2)) was zero at the rim, which is why the drops read as flat dark lumps (defect R4).
vec3 ssLensCapNormal(vec2 delta)
{
    float s = sin(SS_CONTACT_ANGLE_DEG * (SS_TAU / 360.0));
    float r2 = clamp(dot(delta, delta), 0.0, 1.0);
    return vec3(delta * s, sqrt(max(1.0 - r2 * s * s, 1.0e-6)));
}

// SSLensDrop::fresnel - Schlick on the drop's own normal. 0.02 head on, 1.0 at the rim.
float ssLensFresnel(float cosTheta)
{
    float m = 1.0 - clamp(cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return SS_DROP_F0 + (1.0 - SS_DROP_F0) * (m2 * m2 * m);
}

// SSLensDrop::refractGain - Snell at a thin water lens, so the gain is derived rather than tuned.
float ssLensRefractGain()
{
    return SS_REFRACT_DEPTH * (1.0 - 1.0 / SS_IOR_WATER);
}

struct SSLensHit
{
    float cov;
    vec3 n;
};

void ssLensConsider(inout SSLensHit best, float cov, vec3 n)
{
    if (cov > best.cov)
    {
        best.cov = cov;
        best.n = n;
    }
}

void main()
{
    vec2 uv = vary_fragcoord.xy;

    // <SS:Nexii> STABLE SCREEN SPACE (defect R1): aspect-corrected, camera-independent, and the ONLY space anything is placed in. There
    // is no rotation applied to the field at any point: a drop's position is its own simulated state in this frame, and nothing re-derives
    // it from the camera. So when the wind veers or the camera rolls, nothing already on the glass moves - which is the whole of R1.
    vec2 stable = vec2((uv.x - 0.5) * ssLensAspect, uv.y - 0.5);

    // <SS:Nexii> R16: ONE FETCH. The whole population - drops sitting, drops running, and the drops a runner shed behind it - was drawn
    // into ssLensDropMap before this pass, so there is nothing to search here and no per-drop loop to bound. rgb is the cap normal, a is
    // coverage; the sprite's own soft rim is the antialiasing, and an overlap has already blended its two normals into the neck between
    // them. SSAtmoLensStreaks stays as the population's visibility dial - there is only one population left for it to scale.
    vec4 dropMap = texture(ssLensDropMap, uv);

    SSLensHit drop;
    drop.cov = clamp(dropMap.a * clamp(ssLensStreaks, 0.0, 2.0), 0.0, 1.0);
    vec3 nRaw = dropMap.rgb * 2.0 - 1.0;
    float nLen = length(nRaw);
    drop.n = (nLen > 1.0e-4) ? (nRaw / nLen) : vec3(0.0, 0.0, 1.0);

    // <SS:Nexii> Water displaces haze where it sits - immediate, and gone the moment the water moves on. That is only half of it; the
    // other half is the CHANNEL a runner leaves behind, which outlives the water by a long way and comes from ssLensClearMap below.
    float wipe = drop.cov;

    // <SS:Nexii> Refraction: the scene re-sampled through the drop's own normal, sharp - the drops are windows through the fog, not part
    // of it. The x component is divided by the aspect because the normal lives in aspect-corrected units and the sample is in UV.
    vec2 bend = vec2(drop.n.x / max(ssLensAspect, 0.01), drop.n.y) * ssLensRefractGain() * clamp(ssLensScale, 0.0, 2.0);
    vec3 refracted = texture(diffuseRect, clamp(uv + bend, vec2(0.0), vec2(1.0))).rgb;

    // <SS:Nexii> One 5-tap cross blur serves two jobs: the condensation haze, and the environment the drops reflect. On the reflection
    // see the R4 note in ssscreenfx.cpp - this pass runs after tonemap and glow with no reflection probes bound, so the surroundings are
    // approximated by the blurred frame re-sampled along the mirrored normal rather than by a probe tap.
    vec2 texel = 1.0 / screen_res;
    vec3 blurred = texture(diffuseRect, uv).rgb
                 + texture(diffuseRect, uv + vec2(texel.x * 2.0, 0.0)).rgb
                 + texture(diffuseRect, uv - vec2(texel.x * 2.0, 0.0)).rgb
                 + texture(diffuseRect, uv + vec2(0.0, texel.y * 2.0)).rgb
                 + texture(diffuseRect, uv - vec2(0.0, texel.y * 2.0)).rgb;
    blurred *= 0.2;

    // <SS:Nexii> Fresnel split (defect R4): a drop on glass is a specular surface. Head on it is 2% reflective and you see straight
    // through it; at the rim its normal grazes the view and it is almost entirely reflective, which is the bright edge that makes a drop
    // read as a drop rather than a dark lump. The reflected direction's screen-space step is -n.xy, so the rim picks up what lies beyond.
    vec2 mirror = -vec2(drop.n.x / max(ssLensAspect, 0.01), drop.n.y) * 0.06;
    vec3 reflected = texture(diffuseRect, clamp(uv + mirror, vec2(0.0), vec2(1.0))).rgb * 0.5 + blurred * 0.5;
    float fres = clamp(ssLensFresnel(drop.n.z) * clamp(ssLensReflection, 0.0, 2.0), 0.0, 1.0);
    vec3 water = mix(refracted, reflected, fres);

    // <SS:Nexii> Condensation on the glass itself, toward a grey-white tint, present where the fog demand is high and a RUNNER has not
    // just wiped it. R8: the SAME clear-centre law that thins the fine stipple now also thins condensation - the user: "condensation
    // also ignores clear center" was the finding; a hydrophobic patch holds proportionately less haze, so the amount is multiplied
    // directly by ssLensClearRadial rather than routed through the stipple's lifetime machinery (condensation has no per-drop life to
    // shorten). R16: the wipe is now simply where water IS (see the note at `wipe` above) rather than a query against a runner's stored
    // path - the condensation is a haze ON the glass rather than a property of the glass itself, so it re-settles the moment the water
    // that displaced it has moved on, which is the behaviour this term always wanted and now gets without any per-runner bookkeeping.
    vec3 fogged = blurred * 0.85 + vec3(0.15);
    float swept = clamp(texture(ssLensClearMap, uv).r, 0.0, 1.0);
    float fogAmount = clamp(ssLensFog, 0.0, 1.0) * ssLensClearRadial(length(stable))
                    * (1.0 - clamp(wipe, 0.0, 1.0)) * (1.0 - swept);

    vec3 sceneWithFog = mix(refracted, fogged, fogAmount);
    vec3 rgb = mix(sceneWithFog, water, drop.cov);

    frag_color = vec4(rgb, 1.0);
}
