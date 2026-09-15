/**
 * @file class1/deferred/ssSurfaceStateF.glsl
 * @brief Atmo Magic surface weather material state. Shared by all three
 *        surface passes (wet, normal, albedo): the field-window lookup for
 *        the new state channels (ice, frost, stain, age), the resolved
 *        liquid/deposit look uniforms, and the per-fragment laws twinned
 *        with sssurfacestatecore.h (porosity, wet darkening, drop and
 *        deposit coverage, impact rings, the deposit accumulation mask).
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

// <SS:Nexii> Atmo Magic surface weather state. Design: doc/atmo_magic_surface_weather.md section 3. Linked into all three surface programs, AFTER ssSurfaceFieldF.glsl and BEFORE the pass file. AUDIT (findings 1-5): each pass file is its own compilation unit (LLShaderMgr::loadShaderFile -> one glShaderSource/glCompileShader per mShaderFiles entry); GLSL LINK resolves undefined function prototypes across units and MERGES matching uniform declarations, but it does NOT propagate a `const` or a fully-defined function body between units - so ssFieldOrigin and ssFieldFlowMap (defined with bodies in ssSurfaceFieldF.glsl) still need their OWN `uniform` line here, ssFieldHash needs a prototype (below), and every SS_ constant/function this include defines needs re-declaring (or prototyping) in whichever pass file actually uses it. [interaction: ssSurfaceWetF.glsl, ssSurfaceNormalF.glsl, ssSurfaceAlbedoF.glsl read for uniform names, uv math and hash idioms to keep twinned].

// Re-declared here (own compilation unit) even though ssSurfaceFieldF.glsl already declares them with the same name/type - uniform declarations of the same name+type across a program's units are merged at link, this is not a redefinition.
uniform vec4 ssFieldOrigin;
uniform sampler2D ssFieldFlowMap;

// diffuseRect/specularRect: LLShaderMgr reserved texture names. Declared once here; a pass file that also needs them (this file's own compile unit does not extend to the pass files) redeclares them itself - see ssSurfaceWetF.glsl/ssSurfaceAlbedoF.glsl's own copies.
uniform sampler2D diffuseRect;
uniform sampler2D specularRect;

// The state window: ice, frost, stain, age, same lattice and origin as ssFieldMap.
uniform sampler2D ssFieldStateMap;

// The resolved looks (SSSurfaceState::resolve), uploaded once per frame by SSSurfaceField::bindLooksForShader.
uniform vec4 ssLiquidLook;     // rgb tint, a opacity
uniform vec2 ssLiquidLook2;    // x stain, y metal
uniform vec4 ssDepositLook;    // rgb tint, a sparkle (base, before age)
uniform vec4 ssDepositLook2;   // x translucency, y depthFull (metres), z wash, w melts

uniform float ssSurfaceIntensity;   // liquid precipitation intensity 0..1 (0 while a granular type falls)
uniform float ssSurfaceTempC;
uniform float ssSurfaceDrops;       // SSAtmoSurfaceDrops
uniform float ssSurfaceDropScale;   // SSAtmoSurfaceDropScale
uniform float ssSurfaceIceOn;       // SSAtmoSurfaceIce, 0/1
uniform float ssSurfaceFrostOn;     // SSAtmoSurfaceFrost, 0/1
uniform float ssSurfaceFrostStrength; // SSAtmoSurfaceFrostStrength
uniform float ssSurfaceDebug;       // SSAtmoSurfaceDebug as a float: 0 off, 1 ice, 2 frost, 3 stain, 4 age, 5 porosity
uniform vec3  ssSurfaceWind;        // SSWindFlowMap sample at the camera, agent space m/s
uniform float ssTime;               // gFrameTimeSeconds

// LOCKSTEP with sssurfacestatecore.h - the twin test reads both sources. Same names, SS_ prefixed.
const float SS_WET_DARKEN_POW     = 0.8;
const float SS_POROSITY_LUM_CUT   = 0.3;
const float SS_POROSITY_VAR_FLOOR = 0.55;
const float SS_DROP_INTENSITY_LO  = 0.02;
const float SS_DROP_INTENSITY_HI  = 0.50;
const float SS_SHEET_INTENSITY_LO = 0.55;
const float SS_SHEET_INTENSITY_HI = 0.95;
const float SS_DROP_SIZE_LO       = 0.6;
const float SS_DROP_SIZE_HI       = 1.6;
const float SS_DRIP_INTENSITY_LO  = 0.05;
const float SS_DRIP_INTENSITY_HI  = 0.60;
const float SS_DRIP_SPEED_LO      = 0.3;
const float SS_DRIP_SPEED_HI      = 1.0;
const float SS_DEPOSIT_SOFTEN_MAX = 0.7;
const float SS_DEPOSIT_SOFTEN_DEPTHS = 3.0;
const float SS_SPARKLE_AGE_LOSS   = 0.7;
const float SS_ICE_ROUGHNESS      = 0.22;
const float SS_FROST_ROUGHNESS    = 0.85;
const float SS_DEPOSIT_ROUGHNESS  = 0.85;

const float SS_RING_SPEED_MPS     = 0.55;
const float SS_RING_WAVELENGTH_M  = 0.045;
const float SS_RING_WIDTH_M       = 0.06;
const float SS_RING_DAMP_S        = 1.4;
const float SS_RING_AMPLITUDE     = 0.0072;
const float SS_RING_LIFE_S        = 2.0;
const float SS_RING_SLOPE_MAX     = 0.6;
const float SS_RING_SPREAD_R0_M   = 0.35;
const float SS_RING_FADE_FROM     = 0.55;

// Wind-blown snow settling in the lee of a precipitation shadow (doc section 8, "wind carry"). Not LOCKSTEP with the core - a shader-side art constant, documented here rather than in sssurfacestatecore.h.
const float SS_DRIFT_CARRY = 0.8;

float ssFieldHash(vec2 p);

// The fine shoreline lattice (audit finding 10): moved here from ssSurfaceWetF.glsl so the albedo pass can carve the same shore edge the wet pass does, rather than the two passes each deciding "where is the puddle" on their own. Bit-for-bit the CPU's ssPuddleMaskNoise/lattice hash; identical maths, one copy.
uniform float ssPuddleMaskScaleM;   // lattice pitch, metres

float ssPuddleLatHash(ivec2 c)
{
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffffffu) / 16777216.0;
}

float ssPuddleMaskNoise(vec2 m)
{
    vec2 f = m / max(ssPuddleMaskScaleM, 1.0);
    ivec2 i0 = ivec2(floor(f));
    vec2 t = f - vec2(i0);
    vec2 s = t * t * (3.0 - 2.0 * t);
    return mix(mix(ssPuddleLatHash(i0),               ssPuddleLatHash(i0 + ivec2(1, 0)), s.x),
               mix(ssPuddleLatHash(i0 + ivec2(0, 1)), ssPuddleLatHash(i0 + ivec2(1, 1)), s.x), s.y);
}

// The state window at an agent-space column: (ice, frost, stain, age), zero outside the window - same uv math as ssFieldFetch.
vec4 ssFieldFetchState(vec2 xy_agent)
{
    float span = ssFieldOrigin.z * ssFieldOrigin.w;
    vec2 uv = (xy_agent - ssFieldOrigin.xy) / span;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0))))
    {
        return vec4(0.0);
    }
    return texture(ssFieldStateMap, uv);
}

// The ground wind speed carried into the flow window's spare channel (see doc section 8 / sssurfacefield.h mGroundSpeed01), 0 outside the window.
float ssFieldWindCarry(vec2 xy_agent)
{
    float span = ssFieldOrigin.z * ssFieldOrigin.w;
    vec2 uv = (xy_agent - ssFieldOrigin.xy) / span;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0))))
    {
        return 0.0;
    }
    return texture(ssFieldFlowMap, uv).w;
}

// How porous a surface reads from PBR roughness/metal plus the albedo read (luminance, variance). Twin of SSSurfaceState::porosity.
float ssPorosity(float rough, float metal, float lum, float var)
{
    float val = pow(rough, 1.5) * (1.0 - metal)
        * (SS_POROSITY_VAR_FLOOR + (1.0 - SS_POROSITY_VAR_FLOOR) * var) * (1.0 - SS_POROSITY_LUM_CUT * lum);
    return clamp(val, 0.0, 1.0);
}

// The legacy (Blinn-Phong) porosity estimate, from the specular colour's brightness. Twin of SSSurfaceState::porosityLegacy.
float ssPorosityLegacy(float specBright, float var)
{
    float val = (1.0 - specBright) * (SS_POROSITY_VAR_FLOOR + (1.0 - SS_POROSITY_VAR_FLOOR) * var);
    return clamp(val, 0.0, 1.0);
}

// Local albedo variance, 0 flat .. 1 very noisy: four taps one texel apart, mean absolute luminance difference against the centre, times four, clamped.
float ssAlbedoVar(vec2 tc)
{
    vec2 texel = 1.0 / vec2(textureSize(diffuseRect, 0));
    float centre = dot(texture(diffuseRect, tc).rgb, vec3(0.333));
    float d = 0.0;
    d += abs(dot(texture(diffuseRect, tc + vec2(texel.x, 0.0)).rgb, vec3(0.333)) - centre);
    d += abs(dot(texture(diffuseRect, tc - vec2(texel.x, 0.0)).rgb, vec3(0.333)) - centre);
    d += abs(dot(texture(diffuseRect, tc + vec2(0.0, texel.y)).rgb, vec3(0.333)) - centre);
    d += abs(dot(texture(diffuseRect, tc - vec2(0.0, texel.y)).rgb, vec3(0.333)) - centre);
    return clamp((d * 0.25) * 4.0, 0.0, 1.0);
}

// The roughness-free porosity estimate the normal pass uses (it runs after the wet commit, so the ORIGINAL roughness is gone by then - see doc section 3 / the pipeline ORDER note in sssurfacefield.h).
float ssPorosityFromAlbedo(vec2 tc, vec4 albedo)
{
    float var = ssAlbedoVar(tc);
    float lum = clamp(dot(albedo.rgb, vec3(0.333)), 0.0, 1.0);
    return clamp((SS_POROSITY_VAR_FLOOR + (1.0 - SS_POROSITY_VAR_FLOOR) * var) * (1.0 - SS_POROSITY_LUM_CUT * lum), 0.0, 1.0);
}

// The albedo/wet passes' porosity estimate, which DO still have the pristine spec to read: PBR reads roughness/metal, legacy reads specular brightness.
float ssPorosityAt(vec2 tc, vec4 albedo, vec4 spec, bool is_pbr)
{
    float var = ssAlbedoVar(tc);
    float lum = clamp(dot(albedo.rgb, vec3(0.333)), 0.0, 1.0);
    if (is_pbr)
    {
        return ssPorosity(spec.g, spec.b, lum, var);
    }
    return ssPorosityLegacy(max(max(spec.r, spec.g), spec.b), var);
}

// The wet albedo exponent: pow(albedo, this) darkens and saturates, more so the more porous. Twin of SSSurfaceState::wetAlbedoExponent.
float ssWetAlbedoExponent(float wet, float porosity)
{
    return 1.0 + SS_WET_DARKEN_POW * wet * porosity;
}

// Rain drop coverage laws from intensity 0..1: (static density, size scale, drip density, drip speed scale). Twin of SSSurfaceState::dropLaw.
vec4 ssDropLaw(float i)
{
    float dropRamp = smoothstep(SS_DROP_INTENSITY_LO, SS_DROP_INTENSITY_HI, i);
    float sheetRamp = smoothstep(SS_SHEET_INTENSITY_LO, SS_SHEET_INTENSITY_HI, i);
    float sz = mix(SS_DROP_SIZE_LO, SS_DROP_SIZE_HI, dropRamp);
    float drip = smoothstep(SS_DRIP_INTENSITY_LO, SS_DRIP_INTENSITY_HI, i);
    float dripSpeed = mix(SS_DRIP_SPEED_LO, SS_DRIP_SPEED_HI, drip);
    return vec4(dropRamp * (1.0 - sheetRamp), sz, drip, dripSpeed);
}

// Sheet flow density on any sloped wet surface. Twin of SSSurfaceState::DropLaw::mSheet.
float ssSheetLaw(float i)
{
    return smoothstep(SS_SHEET_INTENSITY_LO, SS_SHEET_INTENSITY_HI, i);
}

// Deposit thickness coverage, twin of SSSurfaceState::depositCoverage. depthFull guarded on BOTH sides of the LOCKSTEP pair - smoothstep(0,0,x) is undefined in GLSL (audit R4).
float ssDepositCoverage(float depth, float depthFull)
{
    return smoothstep(0.0, max(depthFull, 1.0e-6), depth);
}

// How far a thick deposit rounds the normal toward up. Twin of SSSurfaceState::depositSoften.
float ssDepositSoften(float depth, float depthFull)
{
    float full = max(depthFull, 1.0e-6);
    return SS_DEPOSIT_SOFTEN_MAX * clamp(depth / (full * SS_DEPOSIT_SOFTEN_DEPTHS), 0.0, 1.0);
}

// Sparkle strength after age. Twin of SSSurfaceState::depositSparkle.
float ssDepositSparkle(float base, float age)
{
    return base * (1.0 - SS_SPARKLE_AGE_LOSS * age);
}

// Amplitude left in a crest that has spread out to radius r. Twin of SSSurfaceState::ringSpread.
float ssRingSpread(float r)
{
    return sqrt(SS_RING_SPREAD_R0_M / max(r, SS_RING_SPREAD_R0_M));
}

// The terminal taper that eases the ring to zero by the end of its life instead of cutting it off. Twin of SSSurfaceState::ringTaper.
float ssRingTaper(float t)
{
    return 1.0 - smoothstep(SS_RING_FADE_FROM * SS_RING_LIFE_S, SS_RING_LIFE_S, t);
}

// Impact ring height at radius r (metres) and age t (the ring's own clock - the pass scales the wall clock by ssRingRate before calling). Twin of SSSurfaceState::ringHeight.
float ssRingHeight(float r, float t, float strength)
{
    if (t < 0.0 || t > SS_RING_LIFE_S) return 0.0;
    const float kTwoPi = 6.283185307179586;
    float front = SS_RING_SPEED_MPS * t;
    float env = (r - front) / SS_RING_WIDTH_M;
    return SS_RING_AMPLITUDE * strength * exp(-t / SS_RING_DAMP_S) * ssRingSpread(r) * ssRingTaper(t) * exp(-env * env)
        * sin(kTwoPi * (r - front) / SS_RING_WAVELENGTH_M);
}

// The ring's slope (dh/dr) by central difference, clamped. Twin of SSSurfaceState::ringSlope.
float ssRingSlope(float r, float t, float strength)
{
    float h = SS_RING_WAVELENGTH_M / 16.0;
    float slope = (ssRingHeight(r + h, t, strength) - ssRingHeight(r - h, t, strength)) / (2.0 * h);
    return clamp(slope, -SS_RING_SLOPE_MAX, SS_RING_SLOPE_MAX);
}

// How level the GEOMETRIC surface is (the flatten taper), from the world-space geometric normal the pass computed at the top of main via dFdx/dFdy of view position, in uniform flow - exactly as ssSurfaceWetF does today.
float ssSurfaceLevel(vec3 n_geo_world, float cosZero, float cosFull)
{
    return smoothstep(cosZero, cosFull, n_geo_world.z);
}

// A local hash, distinct from ssPuddleLatHash (which lives in ssSurfaceWetF.glsl, a pass file linked AFTER this include - not visible here) but the same bit-mixing idiom, so the shore-carve and this pass's noise agree in spirit if not in one shared function.
float ssStateHash(ivec2 c)
{
    uint h = uint(c.x) * 374761393u + uint(c.y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffffffu) / 16777216.0;
}

// Bilinear value noise on a world-XY lattice of pitch pitchM, 0..1.
float ssWorldNoise(vec2 p, float pitchM)
{
    vec2 f = p / max(pitchM, 1.0e-4);
    ivec2 i0 = ivec2(floor(f));
    vec2 t = f - vec2(i0);
    vec2 s = t * t * (3.0 - 2.0 * t);
    return mix(mix(ssStateHash(i0),               ssStateHash(i0 + ivec2(1, 0)), s.x),
               mix(ssStateHash(i0 + ivec2(0, 1)), ssStateHash(i0 + ivec2(1, 1)), s.x), s.y);
}

// THE SNOW ACCUMULATION MASK (Zucconi / MinionsArt): coverage lowers a threshold the up-facing component must clear, dithered by two octaves of world noise so the edges feather and patch rather than draw a hard contour.
float ssDepositMask(float coverage, float up, vec3 p)
{
    float n1 = ssWorldNoise(p.xy, 0.5);
    float n2 = ssWorldNoise(p.xy * 3.7 + 11.0, 0.5);
    float t = 1.0 - coverage;
    float m = smoothstep(t - 0.12, t + 0.12, up * 0.9 + 0.2 * n1 + 0.06 * n2);
    return m * step(0.001, coverage);
}

// 1.0 where the world-hashed pitchM cell (3D: xy plus 17.3 * the z layer, the same "hash of xy plus a z term" idiom ssSurfaceSnowF.glsl used for its glint) falls below fraction, else 0.0 - the sparse cells that glint.
float ssSparkleCell(vec3 p, float pitchM, float fraction)
{
    float pitch = max(pitchM, 1.0e-4);
    float h = ssFieldHash(floor(p.xy / pitch) + 17.3 * floor(p.z / pitch));
    return step(h, fraction);
}
