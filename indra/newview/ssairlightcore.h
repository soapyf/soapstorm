/**
 * @file ssairlightcore.h
 * @brief Atmo Magic: the ONE authority for the atmosphere's light - the beam's extinction to a given elevation, and the airlight over a path. Header-only core. CONTRACT.
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

#ifndef SS_AIRLIGHTCORE_H
#define SS_AIRLIGHTCORE_H

// <SS:Nexii> A CORE header (lldefs.h, other ss*core.h, <cmath>, <cstdint> only - no GL, no LLMath, no settings). Design: doc/atmo_magic_phase8_show.md section 5b. WHAT THIS IS: the windlight atmosphere's light, written once - the sun beam's extinction along its own path to a given elevation (the `off_axis` airmass and the `light_atten` optical depth), the transmittance over a view path of a given length (`combined_haze`), the AIRLIGHT that path has gathered (`additiveColorBelowCloud * (1 - combined_haze)`), and the atmospheric SOFT-CLIP response every cloud surface should end on instead of a hard clamp - all as pure functions of the windlight uniforms. The soft clip's own finding and provenance are at its section below.
// <SS:Nexii> THE FINDING IT EXISTS FOR (section 5b): there is no mismatch between the sky, the cirrus dome band and the volumetric deck - they already agree BY DESIGN. ssVolCloudV.glsl declares the same nine windlight uniforms the sky runs on (blue_horizon, blue_density, haze_density, haze_horizon, density_multiplier, max_y, lightnorm, sunlight_color, ambient_color) and composes vary_ss_sunlit / vary_ss_amblit following, in its own comment, "cloudsV's exact composition", with the airlight handed down separately (vary_ss_airlight) and added AFTER every cloud multiplier so the deck and the dome band converge on the SAME pure airlight at the rim. What they did not share was a single SPELLING of that agreement: the composition was transliterated three times - app_settings/shaders/class1/deferred/skyV.glsl (the dome), cloudsV.glsl (the cirrus band), ssVolCloudV.glsl (the volumetric deck) - and phase 8c's horizon shell would be the FOURTH. This header is the one place those four are meant to read from; two of the three are now transliterations OF IT rather than of each other (see the consumer note below).
// <SS:Nexii> THE CONSUMERS, and what is still outstanding. app_settings/shaders/class1/deferred/skyV.glsl (the dome) and cloudsV.glsl (the cirrus band) are WIRED: every line of their airlight chains - lightAtten, offAxis, beamToElevation, hazeSplit, pathTransmittance, glowAirmass/glowLight, ambientUnderClouds, dimByCloudShadow, skyColor, and then airlightOverPath (band) / domeHazeColor (dome) - is a transliteration of the function of that name below, names this file as its authority at the site, and is held to it by V:\Scratch\atmo\tests\twin_airlight.cpp, which reads both shader files at runtime, pins their text, and asserts this core reproduces every intermediate of both bit-for-bit. ssVolCloudF.glsl is wired for the soft clip only (softClipUnit as ss_soft_clip_unit, pinned by twin_deck_softclip.cpp). THE ONE TRANSLITERATION LEFT TO FOLD IN is ssVolCloudV.glsl, the volumetric deck: it still carries its own copy of the composition, still carries the UNGUARDED spellings the other two have now given up (DECK_SPELLING below), and folding it in is what phase 8c's horizon shell - the fourth consumer - is blocked behind (doc/atmo_magic_phase8_show.md section 5b). So do not assume a shader agrees with this file because the file exists: check whether it is on the wired list. V:\Scratch\atmo\tests\unit_airlight.cpp pins the invariants 8c depends on. AND THE COPIES THAT ARE NOT THIS CORE'S TO FOLD IN, stated so the consolidation is not read as wider than it is: class1/windlight/atmosphericsFuncs.glsl (calcAtmosphericVars - the haze every deferred surface wears), llsettingssky.cpp (calculateLightSettings) and lllegacyatmospherics.cpp (calcSkyColorInDir) each carry their own spelling of the same composition, and all three still lift the ambient and dim the beam WITHOUT the clamps - so for an authored ambient above 1 they darken under cover where the dome and the band now hold flat. That is one split fewer than before the consolidation, not one more (the dome always held flat and the band has now joined it), but it is a real remaining one and twin_airlight.cpp prints the list every run.
// <SS:Nexii> [interaction: doc/atmo_magic_phase8_show.md section 5b, the horizon shell] The rim melt 8c's shell is built on is the property `airlightOverPath` states: as the path transmittance goes to zero the airlight goes to the pure below-cloud sky colour, which is exactly what the dome band and the deck both land on at the horizon. A shell that inherits the sky's air rather than approximating it is a shell that calls these functions with its own path length.
// <SS:Nexii> WHAT THIS CORE DELIBERATELY DOES NOT OWN: the haze GLOW's angular term (1 - dot(ray, glow_dir), the glow.x / glow.z spike and the anti-solar 0.25). That term is genuinely per-site - the dome reads it along the TRUE ray under the horizon mirror, the band along the (possibly mirrored) rel_pos_norm, and the deck caps it at SS_GLOW_CONE_CAP over its own rows - and it is a direction, not air. It enters here as the scalar `hazeGlow`, the way ssVolCloudV.glsl already hands it down as vary_ss_glow. Likewise the sun/moon selection, the horizon-band share (ss_sun_rise), the disc-floored elevation and the ray's own mirroring stay at the call site; this core takes the RESULT of those as `sunlight` and `sunElev`.
// Bodies here are formulas; the shaders transliterate them and twin_airlight.cpp proves the transliteration.
#include "lldefs.h"

#include <cmath>
#include <cstdint>

namespace SSAirlight
{
    // A colour is three floats and nothing else. Cores may not include LLMath (it drags llsimdmath -> llmemory ->
    // llerror -> boost; PLAN.md section 2), so this POD stands in for LLColor3 / vec3 at the core boundary. The
    // component ops below are spelled to match GLSL's per-component semantics exactly, in the same ORDER the shaders
    // write them, because that is what makes the twin's equality bit-for-bit rather than approximate.
    struct Col3
    {
        F32 r, g, b;
    };

    inline Col3 col3(F32 r, F32 g, F32 b)              { return Col3{ r, g, b }; }
    inline Col3 splat(F32 v)                           { return Col3{ v, v, v }; }
    inline Col3 add(const Col3& a, const Col3& b)      { return Col3{ a.r + b.r, a.g + b.g, a.b + b.b }; }
    inline Col3 sub(const Col3& a, const Col3& b)      { return Col3{ a.r - b.r, a.g - b.g, a.b - b.b }; }
    inline Col3 mul(const Col3& a, const Col3& b)      { return Col3{ a.r * b.r, a.g * b.g, a.b * b.b }; }
    inline Col3 scale(const Col3& a, F32 s)            { return Col3{ a.r * s, a.g * s, a.b * s }; }
    inline Col3 addScalar(const Col3& a, F32 s)        { return Col3{ a.r + s, a.g + s, a.b + s }; }
    inline Col3 oneMinus(const Col3& a)                { return Col3{ 1.f - a.r, 1.f - a.g, 1.f - a.b }; }
    inline F32  maxComp(const Col3& a)                 { return llmax(a.r, llmax(a.g, a.b)); }
    inline F32  minComp(const Col3& a)                 { return llmin(a.r, llmin(a.g, a.b)); }

    // The literals the three shaders share, named once. HAZE_ATTEN_SHARE is the 0.25 in
    // `blue_density + vec3(haze_density * 0.25)` - the fraction of the haze's density that attenuates the BEAM (haze
    // scatters forward, so it removes less from the direct path than it adds to the air). DENSITY_FLOOR is the
    // `max(1e-6, ...)` that keeps the airmass reciprocal finite at a horizon ray. AMBIENT_CLOUD_LIFT is the 0.5 in the
    // cloud-cover ambient boost. SUN_GLOW_DEPTH is Atmo's own: the ceiling, in optical depths on the densest
    // attenuation channel, that the GLOW's light may be extinguished by (skyV.glsl / cloudsV.glsl / ssVolCloudV.glsl
    // all spell `const float SS_SUN_GLOW_DEPTH = 2.0;` and all three say "keep in sync"). twin_airlight.cpp pins each
    // of these against the shader text, so a retune on one side is a test failure rather than a silent divergence.
    constexpr F32 HAZE_ATTEN_SHARE   = 0.25f;
    constexpr F32 DENSITY_FLOOR      = 1e-6f;
    constexpr F32 AMBIENT_CLOUD_LIFT = 0.5f;
    constexpr F32 SUN_GLOW_DEPTH     = 2.0f;

    // <SS:Nexii> THE THREE PLACES THE SHIPPED SHADERS USED TO DISAGREE, and the DECISION taken on each (2026-09-06, the consolidation). All three are guards skyV.glsl carried and cloudsV.glsl / ssVolCloudV.glsl did not. skyV's spelling won all three and cloudsV.glsl now carries it; ssVolCloudV.glsl has not been folded in yet, so DECK_SPELLING still describes a shipped file and is not dead code. The flag stays a parameter for exactly that reason - and so the rung that proves the rewire behaviour-preserving has a name for the OLD spelling.
    // THE RANGES THE VIEWER ACTUALLY BINDS, derived from the settings and the appliers rather than assumed, because
    // "inert" has to be a measurement: ambient_color [0,3] per channel (llsettingssky.cpp legacyHazeValidationList,
    // and Atmo's own dial is a colour swatch scaled by SSFloaterAtmoEnv's SCALE_SUN_AMBIENT = 3); blue_density [0,3]
    // per channel with black reachable from the swatch; haze_density [0,5] with 0 reachable from the slider's floor;
    // cloud_shadow [0,1] (validated, and both the slider and the spinner of the dome coverage dial are [0,1] through
    // SSAtmoEnvSkyModulation::cloudCoverage, which is currently the identity); density_multiplier [1e-7,2];
    // max_y [0,10000]; sun_moon_glow_factor exactly 1.0 or moon_brightness * 0.25 <= 0.25.
    //   guardZeroDensity     - skyV: `max(abs(blue_density) + vec3(abs(haze_density)), vec3(1e-6))`; cloudsV and
    //                          ssVolCloudV: the same sum with NO max. The weights divide by this sum, so the
    //                          unguarded spelling is a 0/0 at an all-zero density - NaN weights, and from there a NaN
    //                          cirrus vertex. DECIDED: guarded everywhere. This was NOT unreachable - a black
    //                          blue_density and a haze_density of 0 are both inside the ranges above and both one
    //                          control away - so the old cloudsV line was a live latent NaN, not a courtesy. Bit
    //                          identical to the unguarded form for every summed density >= 1e-6.
    //   clampAmbientHeadroom - skyV: `ambient_color + max(vec3(0), (1. - ambient_color)) * cloud_shadow * 0.5`; the
    //                          other two: the same without the max. DECIDED: guarded everywhere, and this is the one
    //                          of the four that CHANGES a shipped look, because ambient above 1 is inside the bound
    //                          range (the top two thirds of Atmo's ambient dial). Unguarded, the headroom goes
    //                          negative and cloud cover DARKENS the sky - the opposite of the line's own comment -
    //                          so the band and the dome sat on different ambients at the rim they are built to meet
    //                          on. Identical for every ambient <= 1 per channel.
    //   clampShadowDim       - skyV: `sunlight *= max(0.0, (1. - cloud_shadow))`; the other two: `*= (1. -
    //                          cloud_shadow)`. DECIDED: guarded everywhere. Genuinely inert - cloud_shadow is [0,1]
    //                          on every path above - and adopted for one spelling rather than for a fix; above 1 the
    //                          unguarded form negates the light, which is the failing control.
    // A fourth disagreement is angular, not air, and so is deliberately NOT represented here (see the header note on
    // what this core does not own): in the NON-Atmo path skyV computes `haze_glow = factor * (haze_glow + 0.25)`
    // while cloudsV computed `haze_glow * factor + 0.25`. These differ for any factor other than exactly 1 - the
    // anti-solar floor rides undimmed in the second form - and agree at every value the viewer binds, since
    // sun_moon_glow_factor is 1 for the sun and < 1 for the moon, where the ternary zeroes both. DECIDED: skyV's
    // grouping, on the argument that the 0.25 is part of the glow's light and must scale with it; cloudsV.glsl now
    // spells the line identically and twin_airlight.cpp pins the two files' text against each other with an
    // unreachable factor as its failing control. Under SS_ATMO with the rise band live both are
    // `ss_sun_rise * (haze_glow + 0.25)`, identical by construction.
    struct Spelling
    {
        bool guardZeroDensity;      // skyV's max(sum, 1e-6) before the weight division
        bool clampAmbientHeadroom;  // skyV's max(0, 1 - ambient) in the cloud-cover ambient lift
        bool clampShadowDim;        // skyV's max(0, 1 - cloud_shadow) on the sunlight dim
    };

    // The three shipped spellings, named separately on purpose: if a future edit makes two of them differ, it should
    // differ HERE and be visible, not be discovered on a build. SKY_SPELLING and BAND_SPELLING are now the same three
    // choices because skyV.glsl and cloudsV.glsl have been consolidated onto one spelling; DECK_SPELLING is the
    // unguarded one ssVolCloudV.glsl still ships, and it is also the name the rewire's own before/after rung uses for
    // the OLD band spelling.
    constexpr Spelling SKY_SPELLING  { true,  true,  true  };
    constexpr Spelling BAND_SPELLING { true,  true,  true  };
    constexpr Spelling DECK_SPELLING { false, false, false };

    // lightAtten(blueDensity, hazeDensity, densityMultiplier, maxY): the atmosphere's optical depth per unit airmass,
    // per channel - the quantity every extinction below exponentiates.
    //
    //     light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y)
    //
    // Identical in all three shaders, character for character, and in lllegacyatmospherics.cpp's CPU replication.
    // The grouping is the shaders' own: the haze share and the (multiplier * ceiling) product are each formed as
    // scalars first, so the transliteration is bit-exact rather than merely algebraically equal.
    // Invariants: linear and monotone non-decreasing in each of blueDensity, hazeDensity, densityMultiplier and maxY
    // over their non-negative ranges; zero when densityMultiplier or maxY is zero.
    inline Col3 lightAtten(const Col3& blueDensity, F32 hazeDensity, F32 densityMultiplier, F32 maxY)
    {
        const F32 haze_share = hazeDensity * HAZE_ATTEN_SHARE;
        const F32 depth      = densityMultiplier * maxY;
        return Col3{ (blueDensity.r + haze_share) * depth,
                     (blueDensity.g + haze_share) * depth,
                     (blueDensity.b + haze_share) * depth };
    }

    // offAxis(rayUpY, sunElev): THE AIRMASS the beam crosses to reach this point, as a multiple of the vertical path.
    //
    //     off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + sun_elev)
    //
    // A cosecant in disguise: the sum of the view ray's up-ness and the sun's elevation stands in for the geometry of
    // a slab, so a high sun seen at the zenith crosses ~0.5 vertical paths and a horizon ray under a horizon sun runs
    // away to the floor's reciprocal (1e6). Identical in all three shaders. Note what each side contributes: the view
    // ray's up-ness is clamped at 0 (a downward ray gets the sun's own path, not a negative one) while the sun's
    // elevation is NOT - Atmo floors it at the disc's half-angle at the call site instead (the "just-cleared airmass",
    // skyV.glsl's ss_sun_radius note), which is why sunElev is a parameter here rather than something this core
    // derives.
    // Invariants: strictly positive; <= 1/DENSITY_FLOOR; monotone non-increasing in both rayUpY and sunElev;
    // independent of rayUpY for every rayUpY <= 0.
    inline F32 offAxis(F32 rayUpY, F32 sunElev)
    {
        return 1.f / llmax(DENSITY_FLOOR, llmax(0.f, rayUpY) + sunElev);
    }

    // extinction(atten, airmass): exp(-light_atten * airmass), per channel - the surviving FRACTION of a beam that has
    // crossed `airmass` vertical paths of this atmosphere. Every beam term in the three shaders is this function.
    // Invariants: in (0, 1] for non-negative atten and airmass; 1 at airmass 0; strictly decreasing in airmass on any
    // channel with positive atten; never NaN for finite non-negative inputs.
    inline Col3 extinction(const Col3& atten, F32 airmass)
    {
        return Col3{ std::exp(-atten.r * airmass),
                     std::exp(-atten.g * airmass),
                     std::exp(-atten.b * airmass) };
    }

    // beam(light, atten, airmass): a light colour carried through `airmass` of this atmosphere. The REDDENING lives
    // here and nowhere else: an authored blue_density is largest on the blue channel (the default is
    // (0.2447, 0.4487, 0.7599)), so a long path sheds blue fastest and what arrives is warm. Invariants: per channel
    // <= light; equals light at airmass 0; monotone non-increasing in airmass.
    inline Col3 beam(const Col3& light, const Col3& atten, F32 airmass)
    {
        return mul(light, extinction(atten, airmass));
    }

    // beamToElevation(light, atten, rayUpY, sunElev): THE BEAM'S ATTENUATION TO A GIVEN ALTITUDE/ELEVATION, which is
    // the first of the two things this core exists for -
    //
    //     sunlight *= exp(-light_atten * off_axis)
    //
    // with off_axis built from the view ray's up-ness and the sun's elevation. This one line is transliterated
    // identically in skyV.glsl, cloudsV.glsl and ssVolCloudV.glsl.
    // Invariants: equals beam(light, atten, offAxis(rayUpY, sunElev)) exactly; monotone non-increasing in path length
    // through its airmass; finite for every finite input including rayUpY = sunElev = 0.
    inline Col3 beamToElevation(const Col3& light, const Col3& atten, F32 rayUpY, F32 sunElev)
    {
        return beam(light, atten, offAxis(rayUpY, sunElev));
    }

    // glowAirmass(atten, airmass): the CAPPED airmass the haze glow's light is extinguished over -
    //
    //     min(off_axis, SS_SUN_GLOW_DEPTH / max(max3(light_atten), 1e-6))
    //
    // Atmo's own term, identical in all three shaders. The beam maths hands a horizon ray an effectively infinite
    // airmass and exp()s the sun's colour to black, which is the grey horizon band the authored sunset is meant to
    // burn through; the glow is SCATTERED light, not the beam, and does not have to take the beam's full fate. The cap
    // is a ceiling on optical depth, scaled uniformly across the channels so the hue survives - the band deepens TO
    // its colour rather than through it - and it binds only where the crush was total, so there is no edge to see.
    // Invariants: <= airmass; > 0; equals airmass wherever maxComp(atten) * airmass <= SUN_GLOW_DEPTH; the resulting
    // depth maxComp(atten) * glowAirmass never exceeds SUN_GLOW_DEPTH.
    inline F32 glowAirmass(const Col3& atten, F32 airmass)
    {
        return llmin(airmass, SUN_GLOW_DEPTH / llmax(maxComp(atten), DENSITY_FLOOR));
    }

    // glowLight(rawLight, atten, airmass): the glow's own light - the UNATTENUATED sun colour carried through the
    // capped airmass. rawLight is the pre-extinction light the shaders keep aside (`ss_raw_light`), never the already
    // attenuated beam: applying the cap to an attenuated beam would extinguish twice.
    // Invariants: >= beam(rawLight, atten, airmass) per channel; equals it wherever the cap does not bind.
    inline Col3 glowLight(const Col3& rawLight, const Col3& atten, F32 airmass)
    {
        return beam(rawLight, atten, glowAirmass(atten, airmass));
    }

    // The blue/haze split of the atmosphere: the total density the view path is extinguished by, and the share of the
    // sky's colour each of the two scattering populations owns.
    //
    //     combined_haze = abs(blue_density) + vec3(abs(haze_density))    [skyV wraps this in max(., vec3(1e-6))]
    //     blue_weight   = blue_density / combined_haze
    //     haze_weight   = haze_density / combined_haze
    //
    // The weights sum to 1 per channel for non-negative densities, which is what makes blue_horizon and haze_horizon a
    // partition of the sky's colour rather than two independent gains.
    struct HazeSplit
    {
        Col3 density;      // the summed extinction density the path is attenuated by
        Col3 blueWeight;   // Rayleigh share, per channel
        Col3 hazeWeight;   // Mie/haze share, per channel
    };

    // hazeSplit(blueDensity, hazeDensity, spelling): the split above. `spelling.guardZeroDensity` selects skyV's
    // max(., 1e-6) - see the Spelling note: the two agree for every authored sky and differ only at an all-zero
    // density, where the unguarded spelling divides zero by zero.
    // Invariants: blueWeight + hazeWeight == 1 per channel for non-negative densities with a positive sum; density
    // >= DENSITY_FLOOR under the guarded spelling; weights in [0,1] for non-negative densities.
    inline HazeSplit hazeSplit(const Col3& blueDensity, F32 hazeDensity, const Spelling& spelling)
    {
        const F32 haze_abs = std::fabs(hazeDensity);
        Col3 density{ std::fabs(blueDensity.r) + haze_abs,
                      std::fabs(blueDensity.g) + haze_abs,
                      std::fabs(blueDensity.b) + haze_abs };
        if (spelling.guardZeroDensity)
        {
            density = Col3{ llmax(density.r, DENSITY_FLOOR),
                            llmax(density.g, DENSITY_FLOOR),
                            llmax(density.b, DENSITY_FLOOR) };
        }
        HazeSplit out;
        out.density    = density;
        out.blueWeight = Col3{ blueDensity.r / density.r, blueDensity.g / density.g, blueDensity.b / density.b };
        out.hazeWeight = Col3{ hazeDensity / density.r,   hazeDensity / density.g,   hazeDensity / density.b };
        return out;
    }

    // pathTransmittance(density, pathLenM, densityMultiplier): THE TRANSMITTANCE of a view path of this length - the
    // fraction of whatever lies at the far end that still reaches the eye.
    //
    //     density_dist  = rel_pos_len * density_multiplier
    //     combined_haze = exp(-combined_haze * density_dist)
    //
    // Beer's law over the slab, per channel, and the weight the airlight below is the complement of. The shaders name
    // this `combined_haze` and reuse the variable for both the density and the transmittance (the ATI-era comment in
    // all three explains why it cannot be held in a temporary); the two are separate values here.
    // Invariants: in (0, 1] for non-negative density and length; 1 at length 0; strictly decreasing in pathLenM on any
    // channel with positive density; never NaN for finite non-negative inputs.
    inline Col3 pathTransmittance(const Col3& density, F32 pathLenM, F32 densityMultiplier)
    {
        const F32 density_dist = pathLenM * densityMultiplier;
        return Col3{ std::exp(-density.r * density_dist),
                     std::exp(-density.g * density_dist),
                     std::exp(-density.b * density_dist) };
    }

    // ambientUnderClouds(ambient, cloudShadow, spelling): the ambient lifted toward white as cloud cover rises -
    // `ambient + (1 - ambient) * cloud_shadow * 0.5`, skyV clamping the headroom at 0 (see the Spelling note). Cloud
    // cover both dims the beam and multiplies the sky's own bounce, and this is the second half.
    // Invariants: equals ambient at cloudShadow 0; under clampAmbientHeadroom monotone non-decreasing in cloudShadow
    // at EVERY ambient (that is what the clamp buys - unguarded it is monotone only for ambient <= 1);
    // <= (ambient + 1) / 2 at cloudShadow 1 for ambient <= 1.
    inline Col3 ambientUnderClouds(const Col3& ambient, F32 cloudShadow, const Spelling& spelling)
    {
        Col3 headroom = oneMinus(ambient);
        if (spelling.clampAmbientHeadroom)
        {
            headroom = Col3{ llmax(0.f, headroom.r), llmax(0.f, headroom.g), llmax(0.f, headroom.b) };
        }
        return add(ambient, scale(scale(headroom, cloudShadow), AMBIENT_CLOUD_LIFT));
    }

    // dimByCloudShadow(light, cloudShadow, spelling): the beam dimmed by cloud cover - `light * (1 - cloud_shadow)`,
    // skyV clamping at 0 (see the Spelling note). Applied to BOTH the beam and the glow's light in all three shaders,
    // and only to the below-cloud composition: the dome's above-cloud colour is built before it.
    // Invariants: equals light at cloudShadow 0; zero at cloudShadow 1; monotone non-increasing in cloudShadow.
    inline Col3 dimByCloudShadow(const Col3& light, F32 cloudShadow, const Spelling& spelling)
    {
        F32 keep = 1.f - cloudShadow;
        if (spelling.clampShadowDim)
        {
            keep = llmax(0.f, keep);
        }
        return scale(light, keep);
    }

    // skyColor(...): THE COMPOSITION - the colour the air itself has, before any path weighting.
    //
    //     blue_horizon * blue_weight * (sunlight + ambient)
    //   + (haze_horizon * haze_weight) * (glow_light * haze_glow + ambient)
    //
    // Two populations, each taking its share of the beam and the ambient: the Rayleigh half is isotropic in this model
    // (no angular term at all), the haze half carries the forward-scatter glow. This is the line spelled in skyV.glsl
    // twice (above and below cloud), in cloudsV.glsl once (additiveColorBelowCloud) and in ssVolCloudV.glsl once
    // (additiveColorBelowCloud again) - four of the six transliterations this header exists to replace. The grouping
    // is the shaders' own, left to right.
    // Invariants: non-negative for non-negative inputs; linear in sunlight, in ambient and in glowLight; equals
    // blue_horizon * blue_weight * ambient + haze_horizon * haze_weight * ambient when both lights are zero.
    inline Col3 skyColor(const Col3& blueHorizon, const Col3& blueWeight, const Col3& beamLight,
                         const Col3& ambient, F32 hazeHorizon, const Col3& hazeWeight,
                         const Col3& glowLight, F32 hazeGlow)
    {
        const Col3 blue_factor = mul(blueHorizon, blueWeight);
        const Col3 haze_factor = scale(hazeWeight, hazeHorizon);
        return add(mul(blue_factor, add(beamLight, ambient)),
                   mul(haze_factor, add(scale(glowLight, hazeGlow), ambient)));
    }

    // airlightOverPath(skyColor, transmittance): THE AIRLIGHT, which is the second of the two things this core exists
    // for - `additiveColorBelowCloud * (1 - combined_haze)`, the light the path itself has scattered INTO the eye,
    // weighted by exactly the complement of what the path let through.
    //
    // THE PROPERTY 8c's RIM MELT DEPENDS ON: as the transmittance goes to 0 this goes to the pure below-cloud sky
    // colour, whatever was at the far end of the path. That is why the dome band and the deck converge on the same
    // colour at the horizon without either being told to - the band ends at `additiveColorBelowCloud * (1 -
    // combined_haze)` (cloudsV.glsl's oHazeColorBelowCloud) and the deck adds `vary_ss_airlight`, the same expression,
    // AFTER every cloud multiplier. Added at the end rather than folded into the ambient for a recorded reason: folded
    // in, it was multiplied by the storm gloom and the wrap mids, which stripped the warm dawn air off the deck and
    // left it meeting the horizon band about 35% darker than the band it joins.
    // Invariants: in [0, skyColor] per channel for transmittance in [0,1]; zero at transmittance 1; equals skyColor at
    // transmittance 0; monotone non-increasing in transmittance.
    inline Col3 airlightOverPath(const Col3& skyColour, const Col3& transmittance)
    {
        return mul(skyColour, oneMinus(transmittance));
    }

    // domeHazeColor(aboveCloud, belowCloud, transmittance): skyV.glsl's own final blend, which is the dome's spelling
    // of the two colours above and is NOT what the cloud sites use.
    //
    //     color  = above * (1 - combined_haze)
    //     color += (below - color) * (1 - sqrt(sqrt(combined_haze)))
    //
    // The above-cloud colour is built with the raw ambient and the undimmed beam; the below-cloud colour with the
    // lifted ambient and the dimmed beam, and it enters UNWEIGHTED - so at the horizon, where the transmittance goes
    // to 0, the blend weight goes to 1 and the dome lands exactly on the below-cloud colour, which is exactly what
    // airlightOverPath lands on. That coincidence is the handoff, and it is why the two are one function apart rather
    // than two models. The nested sqrt is the shader's own: line 312 halves the optical depth for the below-cloud
    // regime and line 315 takes the root of the already-rooted value, giving the fourth root.
    // Invariants: equals belowCloud at transmittance 0; equals 0 at transmittance 1 (both terms vanish); between the
    // two endpoints per channel for transmittance in [0,1] and non-negative colours.
    inline Col3 domeHazeColor(const Col3& aboveCloud, const Col3& belowCloud, const Col3& transmittance)
    {
        const Col3 weighted{ aboveCloud.r * (1.f - transmittance.r),
                             aboveCloud.g * (1.f - transmittance.g),
                             aboveCloud.b * (1.f - transmittance.b) };
        const Col3 blend{ 1.f - std::sqrt(std::sqrt(transmittance.r)),
                          1.f - std::sqrt(std::sqrt(transmittance.g)),
                          1.f - std::sqrt(std::sqrt(transmittance.b)) };
        return add(weighted, mul(sub(belowCloud, weighted), blend));
    }

    // ---------------------------------------------------------------- the atmospheric soft clip

    // <SS:Nexii> THE SECOND FINDING THIS HEADER OWNS (coordinator, 2026-09-06): the cloud shaders end on a HARD BOUND - cloudsF.glsl's `color.rgb = clamp(color.rgb, vec3(0), vec3(1)); color.rgb *= 2.0;` and the deck's identical `clamp(body + vary_ss_airlight, vec3(0.0), vec3(1.0)) * 2.0`. That clip is why the SSAtmoCloudLightVariant A/B showed no visible difference at any dial value in daylight: EEP's sunlight and ambient run well past 1, the sum saturates, and a 92% cut to the sun body term lands on the same clipped pixel. The band gets away with it because it writes at a low alpha1 over the sky, so its clipped colour is a minority of the pixel; the deck writes at a high alpha, so the clip IS the image. The deck's own comment names the fix and declines it for want of an owner - "the dome's own gamma response goes through the atmospheric soft-clip curve, which is not a power law, so there is no cheap honest replication". This is that owner.
    // <SS:Nexii> WHERE THE CURVE COMES FROM. The windlight-era `scaleSoftClip` is DEAD - app_settings/shaders/class1/windlight/gammaF.glsl returns its argument unchanged and says so ("soft clip effect has been moved to postDeferredGammaCorrect legacyGamma, this file is effectively dead"). Its named successor, `legacyGamma` (postDeferredGammaCorrect.glsl, postDeferredTonemap.glsl), is `1 - pow(1 - clamp(c, 0, 1), gamma)` - still a HARD clamp with a power response behind it, so it is not a soft clip either and is exactly the power law the deck's comment refuses. The live soft clip in this pipeline is the tonemapper: `PBRNeutralToneMapping` in class1/deferred/tonemapUtilF.glsl (Khronos Neutral), which every non-sky pixel already goes through, and whose compression half is the one honest curve available - identity below a knee, a hyperbolic roll-off that approaches the ceiling asymptotically and never reaches it, C1-continuous at the join.
    // <SS:Nexii> WHAT IS TRANSLITERATED AND WHAT IS DROPPED. The compression and the highlight desaturation are taken verbatim from PBRNeutralToneMapping. Its TOE - `offset = x < 0.08 ? x - 6.25 * x * x : 0.04; color -= offset` - is deliberately NOT taken: the toe is a black-level control, it subtracts up to 0.04 (8% of the doubled output) from dark cloud, and the requirement on this curve is that it agree with the clamp at low input so dark cloud does not shift. Below the knee this core's curve is the IDENTITY, bit for bit, which the clamp also is; unit_airlight.cpp pins that with SS_CHECK_BITS rather than asserting it.
    // <SS:Nexii> WHY IT MUST STAY BOUNDED: the clamp was not a taste decision. Unclamped cloud sailed over the bloom pass's bright-pass threshold and haloed every puff (the deck's own comment records it). softClip's output is strictly below its ceiling for every finite input, so a ceiling of 1 followed by the existing doubling occupies exactly the range the clamp did - the bloom sees nothing new - while the values that used to pile up ON the ceiling are now ordered again, which is the whole point: a 92% cut to a body term moves the pixel.
    // <SS:Nexii> AND WHERE THE THREE CURRENT BOUNDS STAND (measured, twin_airlight.cpp prints it): the deck's bound and the cirrus BAND's are the same expression, character for character. The SKY dome's has already drifted - skyF.glsl does `color.rgb *= 2.;` and then a hue-preserving LUMINANCE cap at 5.0 (`if (cmax > 5.0) color.rgb *= 5.0 / cmax;`), with no per-channel clamp anywhere, for a recorded reason: "a per-channel clamp saturates anything past 5 to white ... so the light's own red came out white". So the deck's comment "bounded exactly the way the dome layer bounds itself" is true of the band and NOT of the sky. Note what the sky's cap is: a hard, hue-preserving compression of the PEAK - the degenerate limit of the curve below, which is a soft, hue-preserving compression of the peak. One function with a ceiling parameter covers all three (ceiling 1 then double for band and deck, ceiling 5 for the sky), which is the fourth-consumer argument again in a different register.

    // The stock constants, from tonemapUtilF.glsl's PBRNeutralToneMapping. SOFT_CLIP_KNEE is its `startCompression`
    // (written there as `0.8 - 0.04`, kept in that form so the pin is textual); SOFT_CLIP_DESAT its `desaturation`.
    constexpr F32 SOFT_CLIP_KNEE  = 0.8f - 0.04f;   // 0.76 - below this the curve is the identity
    constexpr F32 SOFT_CLIP_DESAT = 0.15f;          // how far the compressed highlight is pulled toward its own peak

    // GLSL's mix, in GLSL's own order (x * (1 - a) + y * a), so a shader transliteration is bit-exact.
    inline Col3 mix3(const Col3& x, const Col3& y, F32 a)
    {
        return Col3{ x.r * (1.f - a) + y.r * a, x.g * (1.f - a) + y.g * a, x.b * (1.f - a) + y.b * a };
    }

    // softClipPeak(peak): THE RESPONSE CURVE, on the scalar peak, for a ceiling of 1 -
    //
    //     peak < K            ->  peak
    //     otherwise           ->  1 - d*d / (peak + d - K),   d = 1 - K
    //
    // A hyperbola through (K, K) with slope exactly 1 there, asymptotic to 1 from below. Properties, all pinned in
    // unit_airlight.cpp: strictly increasing everywhere (derivative d*d / (peak + d - K)^2, which is d*d/d*d = 1 at
    // the knee and positive for every finite peak, so no input range is ever flattened - the failure the hard clamp
    // has above 1); strictly less than 1 for every finite peak; identity below K to the last bit; C1-continuous at K.
    // Invariants: softClipPeak(0) == 0; softClipPeak(K) == K; monotone; < 1; finite for every finite input.
    inline F32 softClipPeak(F32 peak)
    {
        if (peak < SOFT_CLIP_KNEE)
        {
            return peak;
        }
        const F32 d = 1.f - SOFT_CLIP_KNEE;
        return 1.f - d * d / (peak + d - SOFT_CLIP_KNEE);
    }

    // softClip(colour, ceiling, desaturation): THE SHARED RESPONSE - what a cloud shader should end on instead of
    // `clamp(colour, 0, 1)`. Transliterated from PBRNeutralToneMapping (tonemapUtilF.glsl) minus its toe, with the
    // unit ceiling generalised: the colour is taken into ceiling-relative units, compressed on its own peak so the
    // hue is carried rather than clipped, blended toward that peak by the stock desaturation, and returned to scene
    // units. `ceiling` is the value the output approaches and never reaches - 1 for the band and the deck (whose
    // existing `* 2.0` then stands unchanged), 5 for the sky's own cap.
    //
    // The low end is floored at 0, which is the other half of the `clamp(colour, vec3(0), vec3(1))` this replaces -
    // a negative channel is not a colour and the compression below assumes a non-negative input, as the Khronos
    // source states.
    // Invariants: identity (after the zero floor) for every colour whose peak is below ceiling * SOFT_CLIP_KNEE,
    // bit for bit; every channel strictly below ceiling for every finite input; monotone non-decreasing along any
    // ray from black; finite at 0 and at large input; softClip(colour, ceiling, 0) is the hue-preserving form.
    inline Col3 softClip(const Col3& colour, F32 ceiling, F32 desaturation)
    {
        Col3 c{ llmax(0.f, colour.r), llmax(0.f, colour.g), llmax(0.f, colour.b) };
        const F32 knee = ceiling * SOFT_CLIP_KNEE;
        const F32 raw_peak = maxComp(c);
        if (raw_peak < knee)
        {
            return c;
        }
        const F32 inv  = 1.f / ceiling;
        Col3 x = scale(c, inv);
        const F32 peak = maxComp(x);
        const F32 d = 1.f - SOFT_CLIP_KNEE;
        const F32 newPeak = 1.f - d * d / (peak + d - SOFT_CLIP_KNEE);
        x = scale(x, newPeak / peak);
        const F32 g = 1.f - 1.f / (desaturation * (peak - newPeak) + 1.f);
        return scale(mix3(x, splat(newPeak), g), ceiling);
    }

    // softClipUnit(colour): the band's and the deck's case - ceiling 1, stock desaturation. This is the expression
    // that replaces `clamp(colour, vec3(0.0), vec3(1.0))` at both sites; the `* 2.0` that follows stays as it is.
    inline Col3 softClipUnit(const Col3& colour) { return softClip(colour, 1.f, SOFT_CLIP_DESAT); }

    // hardClip(colour, ceiling): THE FAILING CONTROL, and the expression the two cloud shaders ship today. Kept in
    // the core rather than in the test because it is a shipped spelling and the rung's job is to show, in numbers,
    // what it costs: above the ceiling its derivative is exactly zero, so every difference between two inputs is
    // erased - which is why the SSAtmoCloudLightVariant A/B was invisible in daylight. Never called by shipping code
    // once the consolidation lands.
    inline Col3 hardClip(const Col3& colour, F32 ceiling)
    {
        return Col3{ llclamp(colour.r, 0.f, ceiling), llclamp(colour.g, 0.f, ceiling), llclamp(colour.b, 0.f, ceiling) };
    }

    // ---------------------------------------------------------------- the whole path, in one call

    // The windlight parameters the atmosphere's light is a function of - the nine uniforms skyV.glsl, cloudsV.glsl and
    // ssVolCloudV.glsl all declare, minus the two that are direction rather than air (lightnorm and glow), which enter
    // through Ray below. `sunlight` is the light the call site has already resolved (sun vs moon, the rise-band mix,
    // the 0.7 legacy moon factor); `ambient` is ambient_color as bound.
    struct Params
    {
        Col3 blueHorizon;
        Col3 blueDensity;
        Col3 ambient;
        Col3 sunlight;
        F32  hazeHorizon;
        F32  hazeDensity;
        F32  densityMultiplier;
        F32  maxY;
        F32  cloudShadow;
        bool capGlowExtinction;  // Atmo's SUN_GLOW_DEPTH ceiling on the glow's light; false is the stock line exactly
    };

    // One view path: how far it runs, how far up it points, where the sun stands for it, and the scalar the call site's
    // own angular glow term produced. `sunElev` is the call site's resolved elevation - Atmo floors it at the disc's
    // half-angle while the rise band is live, stock feeds lightnorm.y.
    struct Ray
    {
        F32 lenM;      // the path length the transmittance is taken over (rel_pos_len in the shaders)
        F32 upY;       // the path direction's up component (rel_pos_norm.y)
        F32 sunElev;   // the sun's elevation as the call site resolved it
        F32 hazeGlow;  // the angular forward-scatter term, computed at the call site (see the header note)
    };

    // Everything the four consumers read, so a consolidation is one call plus a pick rather than a re-derivation.
    struct Result
    {
        Col3 atten;          // light_atten
        F32  airmass;        // off_axis
        Col3 transmittance;  // combined_haze after the exp
        Col3 beamLight;      // the attenuated beam, before the cloud-shadow dim
        Col3 glowBeam;       // the glow's light, capped, before the cloud-shadow dim
        Col3 aboveCloud;     // skyV's `color` before its (1 - combined_haze) scale
        Col3 belowCloud;     // additiveColorBelowCloud - the cloud sites' whole composition
        Col3 airlight;       // belowCloud * (1 - transmittance) - what the deck adds and the band ends on
        Col3 domeColor;      // skyV's vary_HazeColor
    };

    // evaluate(params, ray, spelling): the composition, once. Follows the shaders' order exactly - weights, beam,
    // transmittance, glow light, above-cloud colour, ambient lift, shadow dim, below-cloud colour, airlight, dome
    // blend - so every intermediate is the same value the corresponding shader line holds at that point.
    // Invariants (all pinned in unit_airlight.cpp): transmittance in [0,1] and monotone in ray.lenM; airlight ->
    // belowCloud as transmittance -> 0; domeColor -> belowCloud at the same limit; no NaN for zero elevation, zero
    // density, zero length or zero multiplier under SKY_SPELLING.
    inline Result evaluate(const Params& p, const Ray& ray, const Spelling& spelling)
    {
        Result out;
        out.atten   = lightAtten(p.blueDensity, p.hazeDensity, p.densityMultiplier, p.maxY);
        out.airmass = offAxis(ray.upY, ray.sunElev);

        const HazeSplit split = hazeSplit(p.blueDensity, p.hazeDensity, spelling);
        out.transmittance = pathTransmittance(split.density, ray.lenM, p.densityMultiplier);
        out.beamLight     = beam(p.sunlight, out.atten, out.airmass);
        out.glowBeam      = p.capGlowExtinction ? glowLight(p.sunlight, out.atten, out.airmass) : out.beamLight;

        out.aboveCloud = skyColor(p.blueHorizon, split.blueWeight, out.beamLight, p.ambient,
                                  p.hazeHorizon, split.hazeWeight, out.glowBeam, ray.hazeGlow);

        const Col3 lifted     = ambientUnderClouds(p.ambient, p.cloudShadow, spelling);
        const Col3 dimmedBeam = dimByCloudShadow(out.beamLight, p.cloudShadow, spelling);
        const Col3 dimmedGlow = dimByCloudShadow(out.glowBeam, p.cloudShadow, spelling);

        out.belowCloud = skyColor(p.blueHorizon, split.blueWeight, dimmedBeam, lifted,
                                  p.hazeHorizon, split.hazeWeight, dimmedGlow, ray.hazeGlow);
        out.airlight   = airlightOverPath(out.belowCloud, out.transmittance);
        out.domeColor  = domeHazeColor(out.aboveCloud, out.belowCloud, out.transmittance);
        return out;
    }
}

#endif
