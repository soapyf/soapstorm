/**
 * @file class2\windlight\atmosphericsFuncs.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

uniform vec3  lightnorm;
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform int   sun_up_factor;
uniform vec3  ambient_color;
uniform vec3  blue_horizon;
uniform vec3  blue_density;
uniform float haze_horizon;
uniform float haze_density;
uniform float cloud_shadow;
uniform float density_multiplier;
uniform float distance_multiplier;
uniform float max_y;
uniform vec3  glow;
uniform float scene_light_strength;
uniform float sun_moon_glow_factor;
uniform float sky_sunlight_scale;
uniform float sky_ambient_scale;
uniform int classic_mode;

#ifdef SS_ATMO
// <SS:Nexii> The sun's horizon-band share (SSAtmoEnvApplier::sunRiseFraction): 1 the whole time the disc's centre stands at or above the horizon, easing smoothly to 0 across the twilight band below it; 0 also means no active Atmo environment, which is exactly stock. Stock switches the sunlight and the sun glow the moment the disc's CENTRE crosses zero, which reads as the whole sunrise lighting snapping on at once; the ramps below grow it through the band instead - and because the band runs down from the horizon rather than across the disc's span, the sunset glow holds its full authored strength while the sun hangs at the horizon and eases out through the dusk after it sets.
uniform float ss_sun_rise;

// <SS:Nexii> The sun's TRUE direction while the rise band is live (SSAtmoEnvApplier::sunSlotDirection). lightnorm switches to the moon the moment the disc's centre sets, which would swing the surface glow's hotspot across the sky to the moon's azimuth mid-sunset - see the ss_sun_dir note in skyV.glsl.
uniform vec3 ss_sun_dir;

// <SS:Nexii> The two light slots' scene-light contributions, each already carried through the atmosphere on its OWN elevation (SSAtmoEnvApplier::sunSlotLight / moonSlotLight), and the gate for the dominant-light handover below. Zero keeps the stock single-lightnorm switch.
uniform vec3  ss_sun_light;
uniform vec3  ss_moon_light;
uniform float ss_light_max;

// <SS:Nexii> Atmo Magic: haze altitude falloff (SSHaze::invHeight/pathFactor, sshazecore.h). ss_haze_inv_height is
// 1/H, H the scale height derived from the authored cirrus/dome height - 0 when off or Atmo is inactive, which
// reproduces the stock density_dist line exactly. ss_haze_cam_height is the camera's altitude above the owning
// track's floor (mTrackFloorZ) - presentation-only and per-client by nature (it is the camera's own altitude), so
// it never feeds world state.
uniform float ss_haze_inv_height;
uniform float ss_haze_cam_height;

// <SS:Nexii> ss_haze_up_view: the world-up axis (world Z) expressed in this shader's view/eye space - rel_pos here
// is a VIEW-space vector (view space is x right, y up, -z forward; see softenLightF.glsl's ss_cshadow note), so
// rel_pos.y is the CAMERA's up axis, not world altitude, whenever the camera is pitched off level. dot(rel_pos,
// ss_haze_up_view) recovers the true world Delta-altitude instead. (0,1,0) when Atmo is inactive, which reproduces
// the old rel_pos.y reading exactly (moot anyway: ss_haze_inv_height is 0 too, so ss_dh is 0 regardless of axis).
uniform vec3 ss_haze_up_view;
#endif

float getAmbientClamp() { return 1.0f; }

vec3 srgb_to_linear(vec3 col);

// return colors in sRGB space
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten)
{
    vec3 rel_pos = inPositionEye;

    // <SS:Nexii> Atmo Magic: haze altitude falloff - capture the fragment's TRUE world altitude delta from the
    // camera (dot with ss_haze_up_view, the world-up axis expressed in this view-space frame - rel_pos.y alone is
    // the CAMERA's up axis, which swims as the camera pitches) BEFORE the stock clamp below mutates rel_pos. The
    // clamp bounds ray LENGTH (and, for a steep look, flips rel_pos's direction - haze_glow below relies on that
    // flip happening exactly like stock); the falloff scales density by real altitude. They are different
    // quantities, so the clamp stays live under SS_ATMO too rather than being skipped - skipping it re-baselines
    // the haze against a ray length the design's shares table never assumed, inverting the effect on steep views.
#ifdef SS_ATMO
    float ss_world_dh = dot(rel_pos, ss_haze_up_view);
#endif

    //(TERRAIN) limit altitude
    if (abs(rel_pos.y) > max_y) rel_pos *= (max_y / rel_pos.y);

    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    vec3  sunlight     = (sun_up_factor == 1) ? sunlight_color: moonlight_color;

    // sunlight attenuation effect (hue and brightness) due to atmosphere
    // this is used later for sunlight modulation at various altitudes
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);
    // I had thought blue_density and haze_density should have equal weighting,
    // but attenuation due to haze_density tends to seem too strong

    vec3 combined_haze = max(blue_density + vec3(haze_density), vec3(1e-6));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = vec3(haze_density) / combined_haze;

    //(TERRAIN) compute sunlight from lightnorm y component. Factor is roughly cosecant(sun elevation) (for short rays like terrain)
    float above_horizon_factor = 1.0 / max(1e-6, lightnorm.y);

#ifdef SS_ATMO
    // <SS:Nexii> Dominant-light handover. Stock picks ONE light with lightnorm (sun while its centre is up, else the moon) and attenuates it by THAT body's elevation - so at sunrise, the flip from a possibly-high moon to a horizon-grazing sun swaps a mild cosecant for a huge one and the whole scene light collapses to near-black in a frame. Here instead each slot's light arrives already carried through the atmosphere on its own elevation, and the scene takes the per-channel MAX: the light is always the DOMINANT emitter's, so a handover happens exactly where the two lights are equally bright and nowhere else. The moon keeps the world lit while the risen sun is still the dimmer source; bounded by the brighter single-light value, so the handover can never overexpose; and with a lone sun the sun contribution IS the stock line, so a plain EEP-style day reproduces stock exactly. The slots hold the top-2 light emitters (SSAtmoEnvPlanetaryResolver::resolveLightRoles), so two suns hand over by the same rule - the bigger star holds the light until the other's contribution crosses it.
    if (ss_light_max > 0.0)
    {
        sunlight = max(ss_sun_light, ss_moon_light);
    }
    else
#endif
    {
        sunlight *= exp(-light_atten * above_horizon_factor);  // for sun [horizon..overhead] this maps to an exp curve [0..1]
    }

    // main atmospheric scattering line integral
    // <SS:Nexii> Atmo Magic: haze altitude falloff, LOCKSTEP with SSHaze::pathFactor (sshazecore.h). The stock line
    // is homogeneous (no altitude term); this is the closed-form exponential-atmosphere path integral, which
    // reduces EXACTLY to the stock line when ss_haze_inv_height is 0 (falloff off / Atmo inactive). ss_world_dh
    // (captured above, before the clamp) is dHeightM - the fragment's TRUE world altitude minus the camera's.
#ifdef SS_ATMO
    float ss_dh = ss_world_dh * ss_haze_inv_height;
    float ss_t  = (abs(ss_dh) < 1e-4) ? 1.0 : (1.0 - exp(-ss_dh)) / ss_dh;
    float density_dist = rel_pos_len * density_multiplier * exp(-ss_haze_cam_height * ss_haze_inv_height) * ss_t;
#else
    float density_dist = rel_pos_len * density_multiplier;
#endif

    // Transparency (-> combined_haze)
    // ATI Bugfix -- can't store combined_haze*density_dist*distance_multiplier in a variable because the ati
    // compiler gets confused.
    combined_haze = exp(-combined_haze * density_dist * distance_multiplier);

    // final atmosphere attenuation factor
    atten = combined_haze.rgb;

    // compute haze glow
    // <SS:Nexii> The glow's direction tracks the disc (ss_sun_dir), not the lightnorm - lightnorm belongs to the moon below centre-set. See the ss_sun_dir note in skyV.glsl. .yzx puts the world-axes ss_sun_dir into the frame rel_pos and lightnorm share here - NOT a genuinely shared "ogl frame": lightnorm is world-permuted only (LLEnvironment::toLightNorm swaps world x,y,z to y,z,x, llenvironment.cpp, never further transformed - see llsettingsvo.cpp), while rel_pos is a VIEW-space vector (this stock dot product has always mixed the two; ss_haze_up_view above is the fix applied only where altitude, not glow direction, is being read out).
#ifdef SS_ATMO
    vec3 glow_dir = (ss_sun_rise > 0.0) ? ss_sun_dir.yzx : lightnorm.xyz;
#else
    vec3 glow_dir = lightnorm.xyz;
#endif
    float haze_glow = dot(rel_pos_norm, glow_dir);

    // dampen sun additive contrib when not facing it...
    // SL-13539: This "if" clause causes an "additive" white artifact at roughly 77 degreees.
    //    if (length(light_dir) > 0.01)
    haze_glow *= max(0.0f, dot(light_dir, rel_pos_norm));

    haze_glow = 1. - haze_glow;
    // haze_glow is 0 at the sun and increases away from sun
    haze_glow = max(haze_glow, .001);  // set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
    haze_glow *= glow.x;
    // higher glow.x gives dimmer glow (because next step is 1 / "angle")
    haze_glow = clamp(pow(haze_glow, glow.z), -100000, 100000);
    // glow.z should be negative, so we're doing a sort of (1 / "angle") function

    // add "minimum anti-solar illumination"
    haze_glow += .25;

    haze_glow *= sun_moon_glow_factor;

#ifdef SS_ATMO
    // <SS:Nexii> And the glow walks with the horizon-band share too - full while the disc's centre is up (the stock sun value), easing out through the dusk below the horizon, never less than whatever the sun-down state already shed - instead of snapping at centre-rise. Continuous at both ends of the band.
    if (ss_sun_rise > 0.0)
    {
        haze_glow *= max(sun_moon_glow_factor, ss_sun_rise);
    }
#endif

    vec3 amb_color = ambient_color;

    // increase ambient when there are more clouds
    vec3 tmpAmbient = amb_color + (vec3(1.) - amb_color) * cloud_shadow * 0.5;

    // Similar/Shared Algorithms:
    //     indra\llinventory\llsettingssky.cpp                                        -- LLSettingsSky::calculateLightSettings()
    //     indra\newview\app_settings\shaders\class1\windlight\atmosphericsFuncs.glsl -- calcAtmosphericVars()
    // haze color
    vec3 cs = sunlight.rgb * (1. - cloud_shadow);
    additive = (blue_horizon.rgb * blue_weight.rgb) * (cs + tmpAmbient.rgb) + (haze_horizon * haze_weight.rgb) * (cs * haze_glow + tmpAmbient.rgb);

    // brightness of surface both sunlight and ambient

    sunlit = sunlight.rgb;
    amblit = pow(tmpAmbient.rgb, vec3(0.9)) * 0.57;

    additive *= vec3(1.0 - combined_haze);

    // sanity clamp haze contribution
    additive = min(additive, vec3(10));
}

vec3 srgb_to_linear(vec3 col);

// provide a touch of lighting in the opposite direction of the sun light
    // so areas in shadow don't lose all detail
float ambientLighting(vec3 norm, vec3 light_dir)
{
    float ambient = min(abs(dot(norm.xyz, light_dir.xyz)), 1.0);
    ambient *= 0.5;
    ambient *= ambient;
    ambient = (1.0 - ambient);
    return ambient;
}

// return lit amblit in linear space, leave sunlit and additive in sRGB space
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten)
{
    calcAtmosphericVars(inPositionEye, light_dir, 1.0, sunlit, amblit, additive, atten);

    amblit *= ambientLighting(norm, light_dir);

    if (classic_mode < 1)
    {
        amblit = srgb_to_linear(amblit);
        amblit = vec3(dot(amblit, vec3(0.2126, 0.7152, 0.0722)));
        sunlit = srgb_to_linear(sunlit);
    }

    // multiply to get similar colors as when the "scaleSoftClip" implementation was doubling color values
    // (allows for mixing of light sources other than sunlight e.g. reflection probes)
    sunlit *= sky_sunlight_scale;
    amblit *= sky_ambient_scale;
}
