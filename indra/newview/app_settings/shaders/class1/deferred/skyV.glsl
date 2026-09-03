/**
 * @file WLSkyV.glsl
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

// SKY ////////////////////////////////////////////////////////////////////////
// The vertex shader for creating the atmospheric sky
///////////////////////////////////////////////////////////////////////////////

// Output parameters
out vec3 vary_HazeColor;
out float vary_LightNormPosDot;

#ifdef SS_ATMO
out float vary_ss_below_horizon;
out vec3 vary_ss_view_dir;

// <SS:Nexii> The dome's FINAL sun light, for the weather optics' tint (skyF.glsl's ss_optics). Computed below as the same capped-glow light the sunset band renders with, evaluated along the sun's own path - a per-frame constant the halo family wears uniformly. The optics' old tint was bound CPU-side from an uncapped beam replication that underflowed to zero within a degree of the horizon and snapped to the raw near-white authored sun colour; this is the light the eye actually sees, so the halos keep their sunrise/sunset colour down through the horizon band and below it. Degenerate only when the base light itself is - an active env with no sun up and no moon colour, where the optics are gated off anyway.
out vec3 vary_ss_optic_sun_col;
#endif

#ifdef HAS_HDRI
out vec4 vary_position;
out vec3 vary_rel_pos;
#endif

// Inputs
uniform vec3 camPosLocal;

uniform vec3  lightnorm;
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform int   sun_up_factor;
uniform vec3  ambient_color;
uniform vec3  blue_horizon;
uniform vec3  blue_density;
uniform float haze_horizon;
uniform float haze_density;

#ifdef SS_ATMO
// <SS:Nexii> Below-horizon ray treatment: 1 mirrors the ray (Atmo look), 0 leaves the stock -32000 collapse. Zero unless an ACTIVE Atmo environment is driving the sky (lldrawpoolwlsky.cpp), so an enabled-but-idle viewer's EEP sky stays stock.
uniform float ss_horizon_mirror;

// <SS:Nexii> The horizon clip (SSAtmoEnvAtmosphere::mHorizonClip), sampled at the applied phase like the mirror - the same uniform skyF.glsl reads for the depth write and the optics cut. 1 while the authored sky asks for the hard waterline: the lower dome draws (to take the disc with it), and the sun's glow and haze effects stop at the horizon line - see the fade below.
uniform float ss_horizon_clip;

// <SS:Nexii> The sun's horizon-band share (SSAtmoEnvApplier::sunRiseFraction): 1 the whole time the disc's centre stands at or above the horizon, easing smoothly to 0 across the twilight band below it; 0 also means no active Atmo environment, which is exactly stock. The dome's light and its sun glow are ramped on this below. Stock switches BOTH the moment the disc's centre crosses zero - and the first cut of the ramp ran it across the disc's own span, which scaled the glow down by the risen share of the disc and halved the sunset exactly where the authored skies put it at full strength. So the band runs DOWN from the horizon instead: the disc's light hits the atmosphere long before the disc reaches the horizon and keeps lighting it long after, and the ramp now honours both - full glow from centre-rise to centre-set, the dusk easing out below.
uniform float ss_sun_rise;

// <SS:Nexii> The sun's TRUE direction while the rise band is live (SSAtmoEnvApplier::sunSlotDirection). The shared lightnorm direction switches from the sun to the moon the moment the disc's CENTRE sets (LLSettingsSky::getLightDirection) - stock never saw it because stock zeroes the glow and culls the disc below the horizon, but the ramps keep both alive through the whole band, dusk included, and the takeover rule is sun > moon until the band is spent: the glow and the extinction below keep aiming at the SUN while ss_sun_rise is positive, and the moon takes the lightnorm back only once it is not.
uniform vec3 ss_sun_dir;

// <SS:Nexii> The disc's half-angle as a direction-z sine (SSAtmoEnvApplier::sunSlotRadius) - the unit the horizon band is sized in. The extinction below holds the sun's airmass at the JUST-CLEARED path (off_axis 1/radius at the horizon line) while the rise band is live - through the whole rise AND the whole dusk below the horizon - because elevation 0 is not the horizon-sitting airmass - it is the infinite one, and 1/max(1e-6, rel_pos.y + 0) collapses every horizon ray to unlit. The floor releases continuously once the centre climbs past it, and the afterglow keeps the just-cleared light path while the band's share fades it out. Zero while no Atmo environment drives the sky.
uniform float ss_sun_radius;

// <SS:Nexii> The optics' light colour's MOONLIGHT fallback, bound by the sky pool: while no sun band is live the optics are the moonlight family, which deliberately keeps its faint white tint (moonlight optics are not wired to their own light yet). The sun-band case is computed here in the vertex shader (vary_ss_optic_sun_col above); this uniform only stands in below the band.
uniform vec3 ss_optic_sun_col;

// <SS:Nexii> The stock ray lift. The atmosphere ray below is computed 50 m above the geometry it belongs to (the + vec3(0, 50, 0) in rel_pos), a legacy fudge that once rode along with the stock sun disc's own legacy 50 m drop (sunDiscV.glsl) - glow hotspot and drawn disc wrong together, which passed for agreement. The Atmo discs draw at the TRUE direction (ssCelestialV.glsl carries no offset of any kind), so while they own the sky the lift drops to 0 and the glow hotspot lands on the disc: 50 m of lift at the ~5000 m dome is 0.57 degrees, about one sun-diameter of visible droop. 1 is exactly stock - 50 * 1.0 is the same ray bit for bit - which is what an enabled-but-idle viewer keeps.
uniform float ss_ray_lift;

// <SS:Nexii> The glow light's extinction ceiling, in optical depths on the densest attenuation channel. 2.0 is the warm-orange horizon band of a real sunset - deep enough to burn, shallow enough to stay bright; raise it toward the darker reds, lower it toward the golds. See the long note at the glow light below.
const float SS_SUN_GLOW_DEPTH = 2.0;

// <SS:Nexii> How far below the horizon line the sun's glow and haze effects fade out over when the clip is on, as a SINE of the angle below eye level - about a degree - so the cut reads the same from any camera height. See the fade at vary_ss_below_horizon below.
const float SS_SUN_HORIZON_FADE = 0.02;
#endif

uniform float cloud_shadow;
uniform float density_multiplier;
uniform float distance_multiplier;
uniform float max_y;

uniform vec3  glow;
uniform float sun_moon_glow_factor;

uniform int cube_snapshot;

// NOTE: Keep these in sync!
//       indra\newview\app_settings\shaders\class1\deferred\skyV.glsl
//       indra\newview\app_settings\shaders\class1\deferred\cloudsV.glsl
//       indra\newview\lllegacyatmospherics.cpp
void main()
{
    // World / view / projection
    vec4 pos = modelview_projection_matrix * vec4(position.xyz, 1.0);

    gl_Position = pos;

#ifdef SS_ATMO
    // <SS:Nexii> Which half of the dome this vertex belongs to, for the horizon clip (SSAtmoEnvAtmosphere::mHorizonClip - see skyF.glsl and lldrawpoolwlsky.cpp).
    vary_ss_below_horizon = position.y - camPosLocal.y;

    // <SS:Nexii> Under the clip the sun's glow and haze effects stop at the horizon line. The clip already takes the disc and the weather optics with it (skyF.glsl's depth write and ss_clipped_below), but the glow's true-ray hotspot and its capped light would otherwise carry on down past the waterline - the natural continuation the mirror was built to provide. An authored sky that asks for the clip asked for the hard waterline instead, so the directional glow term fades out over SS_SUN_HORIZON_FADE below the line (sine of the angle below eye level - scale-free, the same cut from any height) and only the mirrored AIR - the body terms - continues below. With the clip off the fade stays 1 and the glow keeps its continuation.
    float ss_glow_horizon_fade = 1.0;
    if (ss_horizon_clip > 0.0)
    {
        vec3 ss_dome_ray = position.xyz - camPosLocal.xyz;
        float ss_below_sin = clamp(-ss_dome_ray.y / max(length(ss_dome_ray), 1e-6), 0.0, 1.0);
        ss_glow_horizon_fade = 1.0 - smoothstep(0.0, SS_SUN_HORIZON_FADE, ss_below_sin);
    }
#endif

    // Get relative position
#ifdef SS_ATMO
    // <SS:Nexii> The lift rides ss_ray_lift (see the uniform note above): 1 keeps the stock ray bit for bit, 0 aims the glow - and the rainbow dot below - at the true direction the Atmo discs draw at.
    vec3 rel_pos = position.xyz - camPosLocal.xyz + vec3(0, 50.0 * ss_ray_lift, 0);
#else
    vec3 rel_pos = position.xyz - camPosLocal.xyz + vec3(0, 50, 0);
#endif

#ifdef HAS_HDRI
    vary_rel_pos = rel_pos;
    vary_position = pos;
#endif

    // <SS:Nexii> Below the horizon, mirror the ray instead of collapsing it. Stock stretches a below-horizon ray to 32000 units, and separately the sunlight term below clamps its elevation with max(0., rel_pos_norm.y). Together those make the lower dome black: full extinction over an enormous path, lit by ambient alone. That is invisible as long as land or water covers the lower dome, and it does not. A flat sea of radius R seen from height h ends atan(h/R) below eye level, so there is always a wedge between the water's edge and the true horizon - about two degrees at twenty metres up - with nothing in it but lower dome. Hence the black band under a sunset, widening as you climb, papered over only when cloud coverage is high enough to fill it. Mirroring shades a ray a degree below the horizon exactly like one a degree above, so the haze simply carries on down. Continuous at the horizon, and truer than black: what is actually down there is the same air, seen along the same sort of path. Runtime-gated by ss_horizon_mirror (see the uniform note above): with the mirror off, the stock behaviour below is restored exactly - the -32000 branch stays compiled because the SS_ATMO variant serves both the idle (stock collapse) and active (mirror) cases per frame, and under the mirror it is simply unreachable: the abs keeps y >= 0. The mirror is for the AIR terms only - extinction and haze body, which have no direction of their own. The directional terms (the glow's hotspot, the rainbow and halo strip, the weather optics) scatter from the light at a TRUE angle, so they read the ray captured here before the flip: feeding them the mirrored ray painted a second copy of the sun's glow and corona below the horizon, the mirror image of the one above it.
#ifdef SS_ATMO
    vec3 ss_true_dir = vec3(0.0);
    if (ss_horizon_mirror > 0.)
    {
        ss_true_dir = normalize(rel_pos);
        rel_pos.y = abs(rel_pos.y);
    }
#endif
    if (rel_pos.y > 0.)
    {
        rel_pos *= (max_y / rel_pos.y);
    }
    if (rel_pos.y < 0.)
    {
        rel_pos *= (-32000. / rel_pos.y);
    }

    // Normalized
    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    // Grab this value and pass to frag shader for rainbows
    // <SS:Nexii> Under the mirror the rainbow and halo strip read the true ray (see above), so their rings land at true angular distances from the light below the horizon too - never mirrored ones.
    float rel_pos_lightnorm_dot = dot(rel_pos_norm, lightnorm.xyz);
#ifdef SS_ATMO
    if (ss_horizon_mirror > 0.)
    {
        rel_pos_lightnorm_dot = dot(ss_true_dir, lightnorm.xyz);
    }
#endif
    vary_LightNormPosDot = rel_pos_lightnorm_dot;

#ifdef SS_ATMO
    // <SS:Nexii> The view ray, for the weather-driven optics (ssOptics in skyF.glsl) to split into corona, 22/46 halos, sundogs and the aligned-plate arcs by true angular position around the light. Same frame as lightnorm, so the fragment stage compares it against the same light direction uniform and the same +Y up the dome already shades with. Under the mirror this is the TRUE ray (see above): a below-horizon fragment gets only what its real angle from the light scatters - the natural continuation of the phenomena across the horizon line - never the mirrored copy the flipped ray used to paint.
    vary_ss_view_dir = (ss_horizon_mirror > 0.) ? ss_true_dir : rel_pos_norm;
#endif

    // Initialize temp variables
    vec3 sunlight = (sun_up_factor == 1) ? sunlight_color : moonlight_color * 0.7; //magic 0.7 to match legacy color

#ifdef SS_ATMO
    // <SS:Nexii> The disc sheds light across the horizon band, not the instant its centre clears the horizon: the dome's sunlight is the full day colour from centre-rise up - the condition the authored skies painted against - and walks back down to the night value as the sun sinks through the twilight below the horizon (ss_sun_rise is the horizon-band share). Zero leaves the stock switch untouched - night, idle environments, and the fully-set case.
    if (ss_sun_rise > 0.0)
    {
        sunlight = mix(moonlight_color * 0.7, sunlight_color, ss_sun_rise);
    }
#endif

    // Sunlight attenuation effect (hue and brightness) due to atmosphere
    // this is used later for sunlight modulation at various altitudes
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

#ifdef SS_ATMO
    // <SS:Nexii> While the rise band is live, the sun's term in the light path floors at the DISC'S OWN half-angle (ss_sun_radius). Stock feeds the raw (clamped) lightnorm elevation in here, so the moment the disc's CENTRE dips under, every ray near the horizon loses its light path - the max(1e-6, ...) below collapses them to unlit - and the whole sunset band is cut out from under a disc that is still half up. Holding the elevation at the radius is the JUST-CLEARED airmass (1/radius at the horizon line, the same path a sun that has only just cleared gets), so the band stays lit for the whole rise, and the dusk below the horizon keeps that same just-cleared path while the band's share fades it out - the afterglow dies of the fade, never of the airmass. The floor releases continuously once the centre clears the radius, and the gate is stock with it off.
    float sun_elev = (ss_sun_rise > 0.0) ? max(ss_sun_dir.z, ss_sun_radius) : lightnorm.y;
#else
    float sun_elev = lightnorm.y;
#endif

    // Calculate relative weights
    vec3 combined_haze = max(abs(blue_density) + vec3(abs(haze_density)), vec3(1e-6));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = haze_density / combined_haze;

    // Compute sunlight from rel_pos & lightnorm (for long rays like sky)
    float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + sun_elev);
#ifdef SS_ATMO
    // The pre-attenuation light, kept for the glow's own extinction below - the glow is
    // scattered light, not the beam, and does not have to take the beam's full fate.
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
    // <SS:Nexii> The glow tracks the disc (ss_sun_dir), not the lightnorm: lightnorm hands the direction to the moon at centre-set, which would swing the whole sunset band across the sky to the moon's azimuth while the disc is still half up. See the ss_sun_dir note above. Frame note: rel_pos and lightnorm live in the ogl frame lightnorm is uploaded in (LLEnvironment::toLightNorm permutes world x,y,z to y,z,x), while ss_sun_dir arrives in world axes - the same raw vector the celestial discs phase against - so the swizzle below puts both directions in one frame. ss_sun_dir.z keeps meaning the true elevation.
#ifdef SS_ATMO
    vec3 glow_dir = (ss_sun_rise > 0.0) ? ss_sun_dir.yzx : lightnorm.xyz;
    // <SS:Nexii> Calibration to EEP: the hotspot's shape was EEP-tuned against the stock ~5.7 degree quad. The drawn disc's angular size must NOT rescale it - the glow is the LIGHT the authored sky sheds, and EEP sized the lobe by the stock quad's angular width. Perceived disc size is a look, not a light: the lobe keeps the stock width whatever the Atmo discs draw. The horizon-band share (ss_sun_rise) already ramps the strength; the removed ss_disc * ss_disc term was the only disc-to-light coupling.
    vec3 glow_ray = (ss_horizon_mirror > 0.) ? ss_true_dir : rel_pos_norm;
    float haze_glow = 1.0 - dot(glow_ray, glow_dir);
#else
    vec3 glow_dir = lightnorm.xyz;
    vec3 glow_ray = rel_pos_norm;
    float haze_glow = 1.0 - dot(glow_ray, glow_dir);
#endif
    // haze_glow is 0 at the sun and increases away from sun
    haze_glow = max(haze_glow, .001);
    // Set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
    haze_glow *= glow.x;
    // Higher glow.x gives dimmer glow (because next step is 1 / "angle")
    haze_glow = pow(haze_glow, glow.z);
    // glow.z should be negative, so we're doing a sort of (1 / "angle") function

    // Add "minimum anti-solar illumination"
    // For sun, add to glow.  For moon, remove glow entirely. SL-13768
#ifdef SS_ATMO
    // <SS:Nexii> The glow is the light the disc sheds, so while the rise band is live it is built from the RAW angular term and scaled by the horizon-band share - full strength the whole time the disc is up, which is exactly the stock sun line and the condition the authored skies painted against, then easing out across the twilight below the horizon. Stock's factor line cannot be allowed to touch it in the band: below centre-rise the factor belongs to the moon (< 1.0), whose branch zeroes the term entirely (SL-13768 - right for the moon, which must not glow), and ramping on the zeroed term grew a FLAT 0.25 wash with no hotspot at all until the factor snapped to 1.0 at centre-rise - the sunrise horizon simply was not there while the disc poked over. With the gate off, stock.
    if (ss_sun_rise > 0.0)
    {
        haze_glow = ss_sun_rise * (haze_glow + 0.25);
    }
    else
#endif
    {
        haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (sun_moon_glow_factor * (haze_glow + 0.25));
    }

#ifdef SS_ATMO
    // <SS:Nexii> The horizon clip's cut of the sun's effects at the waterline - see the note at ss_glow_horizon_fade above. 1 unless the clip is on and this vertex sits below the line.
    haze_glow *= ss_glow_horizon_fade;
#endif

#ifdef SS_ATMO
    // <SS:Nexii> The sun's glow and haze get their OWN light, with the beam extinction's crush BOUNDED rather than the beam's. The beam maths above (off_axis 1/elevation) hands horizon rays an effectively infinite airmass, and the density product exp()s the sun's colour to black there - the glow's SHAPE survives but arrives colourless, which is the grey horizon band, the "edge" the authored sunset is supposed to burn through. (A radial fade of the extinction around the disc was tried first and read as a giant ball: the window's inner zone rode the hotspot at full strength, the mid-ring swept through the deepest saturation, and the window's edge popped back to the crushed sky - a bullseye around the sun quad, which is itself correct.) So instead the glow's extinction is CAPPED: its airmass may not exceed the depth at which the densest attenuation channel has shed SS_SUN_GLOW_DEPTH optical depths - scaled uniformly across the channels, so the hue survives and the band deepens TO its colour, never through it and out the other side. Where the beam maths is mild the cap never binds and the stock line stands; it only binds where the crush was total, and a min() has no edge to see - the grey band simply becomes the bright warm gradient a real sunset carries, core to horizon and on down past it. Density keeps every other duty untouched - the dome body, the blue_horizon wash, the distance haze, the (1 - combined_haze) scale. Live only while the rise band is (ss_sun_rise > 0): an idle environment keeps the stock line bit for bit.
    vec3 ss_glow_light = sunlight;
    if (ss_sun_rise > 0.0)
    {
        float ss_max_atten = max(light_atten.r, max(light_atten.g, light_atten.b));
        float ss_glow_airmass = min(off_axis, SS_SUN_GLOW_DEPTH / max(ss_max_atten, 1e-6));
        ss_glow_light = ss_raw_light * exp(-light_atten * ss_glow_airmass);
    }
#endif

#ifdef SS_ATMO
    // <SS:Nexii> The weather optics' final light colour (vary_ss_optic_sun_col above) - the light the dome ACTUALLY renders with, evaluated along the light's OWN ray, so it is a per-frame constant the whole halo family wears uniformly. The old tint was bound CPU-side from an uncapped beam replication whose cosecant (1/max(1e-6, sin(elev))) underflowed to zero within a degree of the horizon and fell back to the raw near-white authored sun colour - the white snap. This is the hue the eye actually sees: full warm sun through the rise band, eased toward the night value as the band fades, never white. The airmass cap is the glow's own (SS_SUN_GLOW_DEPTH), so the tint deepens TO the sunset colour instead of through it. During the band the active light for the optics is always the SUN (skyF gates on that band), whatever lightnorm handed to the moon at centre-set, so the sun path uses the sun's TRUE elevation, floored at the disc - the glow's own just-cleared path for a sun below the horizon, which is the whole point: the red rides down past the line while the band fades. While no band is live the optics are the moonlight family and keep their bound faint-white tint (ss_optic_sun_col below) bit for bit. Constant across the dome - it proceeds from uniforms alone - so per-vertex interpolation is a no-op.
    vary_ss_optic_sun_col = (ss_sun_rise > 0.0)
        ? (ss_raw_light * exp(-light_atten
            * min(1.0 / max(1e-6, max(ss_sun_dir.z, 0.0) + max(ss_sun_dir.z, ss_sun_radius)),
                  SS_SUN_GLOW_DEPTH
                  / max(max(light_atten.r, max(light_atten.g, light_atten.b)), 1e-6))))
        : ss_optic_sun_col;
#endif

    // Haze color above cloud
    vec3 color = (blue_horizon * blue_weight * (sunlight + ambient_color)
#ifdef SS_ATMO
               + (haze_horizon * haze_weight) * (ss_glow_light * haze_glow + ambient_color));
#else
               + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient_color));
#endif

    // Final atmosphere additive
    color *= (1. - combined_haze);

    // Increase ambient when there are more clouds
    vec3 ambient = ambient_color + max(vec3(0), (1. - ambient_color)) * cloud_shadow * 0.5;

    // Dim sunlight by cloud shadow percentage
    sunlight *= max(0.0, (1. - cloud_shadow));
#ifdef SS_ATMO
    ss_glow_light *= max(0.0, (1. - cloud_shadow));
#endif

    // Haze color below cloud
    vec3 add_below_cloud = (blue_horizon * blue_weight * (sunlight + ambient)
#ifdef SS_ATMO
                         + (haze_horizon * haze_weight) * (ss_glow_light * haze_glow + ambient));
#else
                         + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient));
#endif

    // Attenuate cloud color by atmosphere
    combined_haze = sqrt(combined_haze);  // less atmos opacity (more transparency) below clouds

    // At horizon, blend high altitude sky color towards the darker color below the clouds
    color += (add_below_cloud - color) * (1. - sqrt(combined_haze));

    // Haze color above cloud
    vary_HazeColor = color;
}