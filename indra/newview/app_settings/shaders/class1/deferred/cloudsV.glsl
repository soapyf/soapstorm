/**
 * @file WLCloudsV.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, Linden Research, Inc.
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

uniform mat4 modelview_projection_matrix;

in vec3 position;
in vec2 texcoord0;

//////////////////////////////////////////////////////////////////////////
// The vertex shader for creating the atmospheric sky
///////////////////////////////////////////////////////////////////////////////

// Output parameters
out vec3 vary_CloudColorSun;
#ifdef SS_ATMO
out float vary_CloudGlow;   // <SS:Nexii> the sky's forward-scatter glow, handed to the fragment stage separately so thinness can gate it
#endif
out vec3 vary_CloudColorAmbient;
out float vary_CloudDensity;

out vec2 vary_texcoord0;
out vec2 vary_texcoord1;
out vec2 vary_texcoord2;
out vec2 vary_texcoord3;
out float altitude_blend_factor;
#ifdef SS_ATMO
out vec3 vary_ray_dir;   // <SS:Nexii> the TRUE camera-relative view ray, for the fragment stage's plane mapping
#endif

// Inputs
uniform vec3 camPosLocal;

uniform vec3 lightnorm;
uniform vec3 sunlight_color;
uniform vec3 moonlight_color;
uniform int sun_up_factor;
uniform vec3 ambient_color;
uniform vec3 blue_horizon;
uniform vec3 blue_density;
uniform float haze_horizon;
uniform float haze_density;

uniform float cloud_shadow;
uniform float density_multiplier;
uniform float max_y;

uniform vec3 glow;
uniform float sun_moon_glow_factor;

uniform vec3 cloud_color;

uniform float cloud_scale;

#ifdef SS_ATMO
// <SS:Nexii> Region-relative cloud parallax and wind travel (doc/atmo_magic_cloud_parallax.md) have moved to the FRAGMENT stage, which derives the band's UVs from the true view ray - the vertex stage's only remaining job is to hand that ray down. Atmo-only, so a stock environment compiles the pristine texcoord path. The layer's own depth slot, 0.99998 - or 0 for the stock projection squash. Zero unless an ACTIVE Atmo environment is driving the sky (lldrawpoolwlsky.cpp): the slot exists to order the band against the Atmo discs, and with no discs drawn an idle EEP sky keeps stock depth.
uniform float ss_cloud_depth;

// <SS:Nexii> The sun's horizon-band share (SSAtmoEnvApplier::sunRiseFraction): 1 the whole time the disc's centre stands at or above the horizon, easing smoothly to 0 across the twilight band below it; 0 also means no active Atmo environment, which is exactly stock. The layer's sun glow ramps on it below - full strength while the sun hangs at the horizon, the condition the authored skies painted against, easing out through the dusk after it sets.
uniform float ss_sun_rise;

// <SS:Nexii> The sun's TRUE direction while the rise band is live (SSAtmoEnvApplier::sunSlotDirection). lightnorm hands the direction to the moon the moment the disc's centre sets, and TWO things here must keep looking at the sun through the whole band, dusk included: the glow hotspot below, and the disc-neighbourhood body restore further down - the one that keeps horizon clouds solid around the disc so it cannot burn through them. Both swing to the moon's azimuth at centre-set without this. See the ss_sun_dir note in skyV.glsl.
uniform vec3 ss_sun_dir;

// <SS:Nexii> The disc's half-angle as a direction-z sine (SSAtmoEnvApplier::sunSlotRadius) - the airmass floor the layer's sun term holds while the rise band is live - through the whole rise AND the whole dusk below the horizon - the just-cleared light path, for the same reason skyV.glsl does. Zero while no Atmo environment drives the sky.
uniform float ss_sun_radius;

// <SS:Nexii> The stock ray lift, as in skyV.glsl: the layer's atmosphere ray below is computed 50 m above the geometry it belongs to (the + vec3(0, 50, 0) in rel_pos), a legacy fudge that once rode along with the stock sun disc's own legacy 50 m drop. The Atmo discs draw at the TRUE direction (ssCelestialV.glsl carries no offset of any kind), so while they own the sky the lift drops to 0 and TWO things on this layer land on the disc with it: the glow hotspot, and the disc-neighbourhood body restore - the one that keeps horizon clouds solid around the disc, and which is ring-shaped about a direction, so a lifted ray swings the whole ring off the disc it is there to frame. 1 is exactly stock, which is what an enabled-but-idle viewer keeps.
uniform float ss_ray_lift;

// <SS:Nexii> The glow light's extinction ceiling, in optical depths on the densest attenuation channel - see the long note at the glow light below, and the matching one in skyV.glsl. Keep in sync with skyV.glsl.
const float SS_SUN_GLOW_DEPTH = 2.0;
#endif

// NOTE: Keep these in sync!
//       indra\newview\app_settings\shaders\class1\deferred\skyV.glsl
//       indra\newview\app_settings\shaders\class1\deferred\cloudsV.glsl
//       indra\newview\app-settings\shaders\class2\windlight\cloudsV.glsl
//       indra\newview\lllegacyatmospherics.cpp
//       indra\newview\llsettingsvo.cpp
void main()
{
    // World / view / projection
    // <SS:Nexii> The cloud layer's own depth slot. LLGLSPipelineSkyBox's LLGLSquashToFarClip would otherwise put this on 0.99999 along with the haze dome and everything else in the sky, which leaves no room to order the sky's layers against each other - and the celestial discs need to be ordered against this one, since a disc is added to the sky rather than composited over it and so can only be hidden by depth. Its layer parameter cannot express this: it steps in units of 0.0001 (0.99999 - 0.0001 * layer, see setProjectionMatrix), and what is wanted here is one step of 0.00001. Set in the same FORM the disc shader uses - w times a constant, in a vertex shader - and that matters as much as the value. Reaching the same number by two different routes (a multiply here, a rewritten projection row there) leaves the two disagreeing in the last bits, so the depth test flips per pixel and the layers speckle through each other. Same expression, same result, no fight. Runtime-gated by ss_cloud_depth (see the uniform note above): 0 leaves gl_Position exactly as stock computed it, so an idle EEP sky takes the untouched squash the projection row bakes.
    vec4 cloud_pos = modelview_projection_matrix * vec4(position.xyz, 1.0);
#ifdef SS_ATMO
    if (ss_cloud_depth > 0.)
    {
        cloud_pos.z = cloud_pos.w * ss_cloud_depth;
    }
#endif
    gl_Position = cloud_pos;

    // Texture coords
    // SL-13084 EEP added support for custom cloud textures -- flip them horizontally to match the preview of Clouds > Cloud Scroll
    vary_texcoord0 = vec2(-texcoord0.x, texcoord0.y);  // See: LLSettingsVOSky::applySpecial

    vary_texcoord0.xy -= 0.5;
    vary_texcoord0.xy /= cloud_scale;
    vary_texcoord0.xy += 0.5;

    vary_texcoord1 = vary_texcoord0;
    vary_texcoord1.x += lightnorm.x * 0.0125;
    vary_texcoord1.y += lightnorm.z * 0.0125;

    vary_texcoord2 = vary_texcoord0 * 16.;
    vary_texcoord3 = vary_texcoord1 * 16.;

    // Get relative position
#ifdef SS_ATMO
    // <SS:Nexii> The lift rides ss_ray_lift (see the uniform note above): 1 keeps the stock ray bit for bit, 0 aims the glow hotspot and the disc-neighbourhood restore at the true direction the Atmo discs draw at.
    vec3 rel_pos = position.xyz - camPosLocal.xyz + vec3(0, 50.0 * ss_ray_lift, 0);
#else
    vec3 rel_pos = position.xyz - camPosLocal.xyz + vec3(0, 50, 0);
#endif

#ifdef SS_ATMO
    // <SS:Nexii> The TRUE camera-relative view ray, handed to the fragment stage for the plane mapping (cloudsF.glsl). The lift is 0 whenever the plane path runs - an active Atmo environment - so this is the exact direction the ray actually travels, not the stock-fudged one.
    vary_ray_dir = normalize(position.xyz - camPosLocal.xyz);
#endif

#ifdef SS_ATMO
    // <SS:Nexii> The horizon fade decoupled from max altitude: dividing by max_y let the ATMOSPHERE ceiling thin every low-sky cloud - at an authored 1000m ceiling a cloud at the horizon sat at half alpha before anything else touched it, which is exactly the "sun disc through solid clouds" leak, and no amount of disc-side machinery could out-engineer an alpha the author never chose. A short fixed ramp keeps the horizon soft; the below-horizon droop cut below is untouched. (Eased back a touch from the first cut of (y+100)/200, which held clouds fully solid to ~1 degree and read as too hard a wall at the waterline.)
    altitude_blend_factor = clamp((rel_pos.y + 90.0) / 300.0, 0.0, 1.0);

    // ...except INTO the sun: the eased fade reads beautifully against sky but lets the disc burn through the same half-faded clouds, and the two aesthetics only collide inside the disc's
    // angular neighbourhood - so exactly there, and nowhere else, horizon clouds keep their body. The ramp spans roughly the width of a large authored sun disc.
    // <SS:Nexii> And the neighbourhood follows the DISC (ss_sun_dir), not the lightnorm: lightnorm hands the direction to the moon the moment the disc's centre sets, which silently revoked the restore for the still-half-risen disc - the clouds around it collapsed onto their eased fade, and the disc burned through them exactly at centre-set. See skyV.glsl's ss_sun_dir note. .yzx puts the world-axes ss_sun_dir into the ogl frame rel_pos and lightnorm share - see the frame note in skyV.glsl.
    vec3 disc_dir = (ss_sun_rise > 0.0) ? ss_sun_dir.yzx : lightnorm.xyz;
    float sun_prox = smoothstep(0.965, 0.992, dot(normalize(rel_pos), disc_dir));
    altitude_blend_factor = max(altitude_blend_factor, sun_prox);
#else
    altitude_blend_factor = clamp((rel_pos.y + 512.0) / max_y, 0.0, 1.0);
#endif

    // Set altitude
    if (rel_pos.y > 0)
    {
        rel_pos *= (max_y / rel_pos.y);
    }
    if (rel_pos.y < 0)
    {
        altitude_blend_factor = 0; // SL-11589 Fix clouds drooping below horizon
        rel_pos *= (-32000. / rel_pos.y);
    }

    // Can normalize then
    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    // Initialize temp variables
    vec3 sunlight = sunlight_color;
    vec3 light_atten;

    // Sunlight attenuation effect (hue and brightness) due to atmosphere
    // this is used later for sunlight modulation at various altitudes
    light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

#ifdef SS_ATMO
    // <SS:Nexii> While the rise band is live, the sun's term in the light path floors at the DISC'S OWN half-angle (ss_sun_radius) - see the long note in skyV.glsl. Stock collapses every ray near the horizon to unlit the moment the disc's CENTRE dips under, cutting the sunset band out from under a disc that is still half up; elevation 0 is the infinite airmass, not the horizon-sitting one, so the floor holds the just-cleared path (1/radius at the horizon line) through the whole rise, and the dusk below the horizon keeps it while the band's share fades the glow out. The floor releases once the centre clears the radius and the gate is stock with it off.
    float sun_elev = (ss_sun_rise > 0.0) ? max(ss_sun_dir.z, ss_sun_radius) : lightnorm.y;
#else
    float sun_elev = lightnorm.y;
#endif

    // Calculate relative weights
    vec3 combined_haze = abs(blue_density) + vec3(abs(haze_density));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = haze_density / combined_haze;

    // Compute sunlight from rel_pos & lightnorm (for long rays like sky)
    float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + sun_elev);
#ifdef SS_ATMO
    // The pre-attenuation light, kept for the glow's own extinction below - see skyV.glsl.
    vec3 ss_raw_light = sunlight;
#endif
    sunlight *= exp(-light_atten * off_axis);

    // Distance
    float density_dist = rel_pos_len * density_multiplier;

    // Transparency (-> combined_haze)
    // ATI Bugfix -- can't store combined_haze*density_dist in a variable because the ati
    // compiler gets confused.
    combined_haze = exp(-combined_haze * density_dist);

    // Compute haze glow
    // <SS:Nexii> The glow tracks the disc (ss_sun_dir), not the lightnorm - lightnorm belongs to the moon below centre-set, and the layer's glow must stay on the sun while any part of it is in sight. See the ss_sun_dir note above and in skyV.glsl. .yzx puts the world-axes ss_sun_dir into the ogl frame rel_pos and lightnorm share - see the frame note in skyV.
#ifdef SS_ATMO
    vec3 glow_dir = (ss_sun_rise > 0.0) ? ss_sun_dir.yzx : lightnorm.xyz;
#else
    vec3 glow_dir = lightnorm.xyz;
#endif
    float haze_glow = 1.0 - dot(rel_pos_norm, glow_dir);
    // haze_glow is 0 at the sun and increases away from sun
    haze_glow = max(haze_glow, .001);
        // Set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
    haze_glow *= glow.x;
        // Higher glow.x gives dimmer glow (because next step is 1 / "angle")
    haze_glow = pow(haze_glow, glow.z);
        // glow.z should be negative, so we're doing a sort of (1 / "angle") function

#ifdef SS_ATMO
    // <SS:Nexii> The glow is the light the disc sheds, so while the rise band is live it is built from the RAW angular term and scaled by the horizon-band share - full strength the whole time the disc is up (the stock sun line, and the condition the authored skies painted against), easing out across the twilight below the horizon. Stock's factor lines cannot be allowed to touch it in the band: below centre-rise the factor belongs to the moon (< 1.0), whose branch zeroes the term entirely (SL-13768 - right for the moon, which must not glow), and ramping on the zeroed term grew a FLAT 0.25 wash with no hotspot at all until the factor snapped to 1.0 at centre-rise - the sunrise horizon simply was not there while the disc poked over. See the matching note in skyV.glsl. With the gate off, stock.
    if (ss_sun_rise > 0.0)
    {
        haze_glow = ss_sun_rise * (haze_glow + 0.25);
    }
    else
#endif
    {
        haze_glow *= sun_moon_glow_factor;

        // Add "minimum anti-solar illumination"
        // For sun, add to glow.  For moon, remove glow entirely. SL-13768
        haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (haze_glow + 0.25);
    }

#ifdef SS_ATMO
    // <SS:Nexii> The layer's glow rides the same capped light as the dome (see the long note in skyV.glsl): the extinction crush is bounded - airmass floored at the depth where the densest channel has shed SS_SUN_GLOW_DEPTH optical depths, scaled uniformly so the hue survives - and binds only where the beam maths would total the colour. It is a ceiling, not a window: no edge to see, the sunset band simply keeps its fire. Live only while the rise band is; idle environments keep stock bit for bit.
    vec3 ss_glow_light = sunlight;
    if (ss_sun_rise > 0.0)
    {
        float ss_max_atten = max(light_atten.r, max(light_atten.g, light_atten.b));
        float ss_glow_airmass = min(off_axis, SS_SUN_GLOW_DEPTH / max(ss_max_atten, 1e-6));
        ss_glow_light = ss_raw_light * exp(-light_atten * ss_glow_airmass);
    }
#endif

    // Increase ambient when there are more clouds
    vec3 tmpAmbient = ambient_color;
    tmpAmbient += (1. - tmpAmbient) * cloud_shadow * 0.5;

    // Dim sunlight by cloud shadow percentage
    sunlight *= (1. - cloud_shadow);
#ifdef SS_ATMO
    ss_glow_light *= (1. - cloud_shadow);
#endif

    // Haze color below cloud
    vec3 additiveColorBelowCloud =
#ifdef SS_ATMO
        (blue_horizon * blue_weight * (sunlight + tmpAmbient) + (haze_horizon * haze_weight) * (ss_glow_light * haze_glow + tmpAmbient));
#else
        (blue_horizon * blue_weight * (sunlight + tmpAmbient) + (haze_horizon * haze_weight) * (sunlight * haze_glow + tmpAmbient));
#endif

    // CLOUDS
    sunlight = sunlight_color;
    off_axis = 1.0 / max(1e-6, lightnorm.y * 2.);
    sunlight *= exp(-light_atten * off_axis);

    // Cloud color out
#ifdef SS_ATMO
    // <SS:Nexii> The glow SPLIT OUT of the cloud body colour instead of multiplied into it: haze_glow is the sky's forward-scatter airlight, which lives BEHIND a cloud - baked into vary_CloudColorSun it recoloured every cloud in a wide cone around the sun toward the glow, when a backlit cloud is a dark silhouette whose thin fringes alone transmit the fire. The fragment shader gates it by per-fragment thinness; the body keeps glow-free sunlight.
    vary_CloudColorSun = sunlight * cloud_color;
    vary_CloudGlow     = haze_glow;
#else
    vary_CloudColorSun     = (sunlight * haze_glow) * cloud_color;
#endif
    vary_CloudColorAmbient = tmpAmbient * cloud_color;

    // Attenuate cloud color by atmosphere
#ifdef SS_ATMO
    // <SS:Nexii> Full-strength optical depth, no sqrt: the stock halving left horizon clouds crisp, which never showed while the max_y alpha fade was thinning them into the sky anyway - with that fade decoupled, the honest colour convergence has to carry the melt alone. This is the density/distance dials doing on the dome band exactly what they do on the volumetric deck: cloud extinguishes and takes on the airlight over the same slab path the sky itself is hazed by, so at the horizon the band dissolves into the atmosphere instead of silhouetting against it.
#else
    combined_haze = sqrt(combined_haze);  // less atmos opacity (more transparency) below clouds
#endif
    vary_CloudColorSun *= combined_haze;
    vary_CloudColorAmbient *= combined_haze;
    vec3 oHazeColorBelowCloud = additiveColorBelowCloud * (1. - combined_haze);

    // Make a nice cloud density based on the cloud_shadow value that was passed in.
    vary_CloudDensity = 2. * (cloud_shadow - 0.25);

    // Combine these to minimize register use
    vary_CloudColorAmbient += oHazeColorBelowCloud;

    // needs this to compile on mac
    //vary_AtmosAttenuation = vec3(0.0,0.0,0.0);

    // END CLOUDS
}
