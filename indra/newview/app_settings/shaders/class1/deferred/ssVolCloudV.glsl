/**
 * @file ssVolCloudV.glsl
 * @brief Atmo Magic volumetric cloud field - camera-facing puffs.
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

// <SS:Nexii> Atmo Magic volumetric cloud field

uniform mat4 modelview_projection_matrix;

// The far-field squash: x knee, y cap (just inside the far plane), z virtual field radius. Beyond the knee, vertices pull radially toward the camera - each keeps its exact ray, so the
// projected image is identical to the true positions and only the depth compresses - what lets the field read out to 10km through a 2km far plane. vary_world stays the TRUE position: the
// fragment shader samples noise and measures distances in the real world, never the compressed one.
uniform vec3 ss_squash;
uniform vec3 ss_cam_pos;

in vec3 position;
in vec2 texcoord0;
in vec4 diffuse_color;

out vec2 vary_texcoord0;

// <SS:Nexii> Per-puff STRUCTURE only, no longer a finished colour: r is the CPU builder's form term (facing toward the light and the exponential shade down through the deck, beam-flattened),
// g the puff's buried depth (SSVolCloud::Puff::mBuried - sunless, so it survives a moonless overcast), a the puff's edge-fade alpha; b is spare. The colour it used to carry was (ambient + sun * form) with the sun run through a CPU replica of the beam extinction - the replica whose
// cosecant underflows to grey at every low sun, so the deck sat flat white-grey under every authored sunset while the dome beside it burned. The light half is computed below instead.
out vec4 vary_color;

// Where this fragment sits in the world, for the noise lookup. The map is a field the whole sky is carved out of, not a picture of one puff, so it samples by position - see ssVolCloudF.glsl.
out vec3 vary_world;

// <SS:Nexii> The deck's light: the DOME CLOUD BAND'S OWN COLOURS (cloudsV.glsl's vary_CloudColorSun / vary_CloudColorAmbient / vary_CloudGlow), computed here per corner by the band's own
// maths on the band's own ray convention, so the puffs are shaded as a piece of the same layer the cirrus band is - which is the whole assignment. Two conventions matter and both bit us:
//
// FRAME - the dome shaders run in the ogl frame lightnorm is uploaded in (LLEnvironment::toLightNorm permutes world x,y,z to y,z,x; +Y is up), while this field's geometry lives in world
// axes (+Z up). The view ray is permuted into the ogl frame below and everything past that line is cloudsV's code shape, swizzles and all.
//
// RAY LENGTH - the band's cloud colours are multiplied by the transmittance along the ray before anything is drawn, and the length of that ray is a convention, not a measurement: the dome
// extends every ray to the atmosphere slab (tens of km at the horizon, and a 32km stretch for the downward rays a dome only shows below its horizon). The deck is real geometry seen from
// any side, so it runs the rim-eased hybrid instead - the deck's own true range near, the dome's slab convention at the rim where it must melt into the band - and the atmosphere is
// inside these colours with no separate fog pass to disagree with it, exactly as it is inside the band's. See the long note at haze_len in main() for how both pure conventions failed.
out vec3 vary_ss_sunlit;
out vec3 vary_ss_amblit;

// The sky's forward-scatter glow at this ray's angle from the light, handed down separately (the band's own split - cloudsV/cloudsF) so the fragment stage can gate it by per-fragment
// thinness: a backlit cloud is a dark silhouette whose thin fringes alone transmit the fire.
out float vary_ss_glow;

// <SS:Nexii> The GRAZE LIGHT's weight - how much extra of the warm capped sun this vertex earns for standing high in the deck while the sun stands low against it. At a grazing sun the
// deck's real occlusion IS height: the beam runs nearly horizontal, so the top of the layer has a clear line to the sun while everything under it sits in the deck's own shadow - which is
// the alpenglow, the lit lid over a dark volume, seen from above, from inside, and through any gap. The per-puff form term cannot say this: its facing half scales with the sun's VERTICAL
// component (zero at the horizon, exactly when the effect peaks) and its shade half only ever darkens. A ramp of the sun coefficient over height-in-layer says it directly - the volume
// brightening bottom to top - with a sharp crest over the lid band, where the puffs that ARE the deck's surface catch the skim. Gated by how grazing the sun is: as it climbs past ~30
// degrees the beam arrives from above, the CPU shade term already owns the vertical story, and this fades out rather than double-lighting the tops. Spent in the fragment stage on a gentler
// wrap of its own past the body's full one (SS_GRAZE_DARK there), riding the same warm sunlit colour and the same gloom - the sun side of each crown still burns brightest, but the far side
// is never left out of a sky that lights the lid from every quarter.
out float vary_ss_top;

// <SS:Nexii> The AIRLIGHT along this ray - the below-cloud haze colour scattered into the eye by the air BETWEEN eye and puff, weighted by what the slab transmittance removed - handed down
// on its own rather than folded into the ambient, because it is not the cloud's light and must not wear the cloud's shading: folded in, the fragment stage multiplied it by the storm gloom
// and by the wrap/noise mids, which stripped the warm dawn air off the deck (the cold navy puffs under a pink cirrus band) and left the deck meeting the horizon band ~35% darker than the
// band it joins. Added after all cloud shaping instead, both converge to the SAME pure airlight at the rim - the handoff is exact by construction. [interaction: dome handoff]
out vec3 vary_ss_airlight;

// The windlight uniforms, auto-filled from the live settings - each compilation unit needs its own declarations, same as the fragment stage does it.
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
uniform float max_y;
uniform vec3  glow;
uniform float sun_moon_glow_factor;

// The sky's AUTHORED cloud colour - the material the dome band paints its clouds with, worn here the same way for the same reason.
uniform vec4 cloud_color;

// <SS:Nexii> The sun's horizon-band share, true direction and disc radius (SSAtmoEnvApplier - see the long notes in skyV.glsl): the same three that keep the dome's sunset alive through the
// whole band, dusk included, instead of snapping at centre-set. ss_sun_dir arrives in WORLD axes and is swizzled at the point of use, exactly as cloudsV does it.
uniform float ss_sun_rise;
uniform vec3 ss_sun_dir;
uniform float ss_sun_radius;

// Where the rim convergence toward the dome runs, in TRUE metres from the eye - x start, y full. Same uniform the fragment stage reads for its shading flattening; here it eases the haze
// path from the deck's own metric range up to the dome band's slab convention - see the path note in main().
uniform vec2 ss_rim;

// The deck's vertical band, for the graze light's height ramp - the same two uniforms the fragment stage carves with.
uniform float ss_base_z;
uniform float ss_layer_thick;

// <SS:Nexii> The glow light's extinction ceiling, in optical depths on the densest attenuation channel. Keep in sync with skyV.glsl / cloudsV.glsl.
const float SS_SUN_GLOW_DEPTH = 2.0;

// <SS:Nexii> The glow CONE's ceiling over the deck's own rows. The band's haze_glow is a painting of the sun itself - the pow spike runs to the hundreds at the disc's direction - and handing it
// to the puffs whole repainted the sun THROUGH the deck: the fragment stage's additive glow and the airlight's haze term both wear it, so a dense overcast kept a burning white blob at the sun's
// position however much cloud stood in the way, thin streaks around it ignited and the one thick puff on the disc left as a dark silhouette inside the fire. The deck's sun story is its GEOMETRY
// - the wrap, the CPU form term, the graze lid - and the disc itself belongs to the dome behind, obscured by the puffs' ordinary alpha like anything else the deck stands in front of. So the cone
// is capped here and the cap eased back out across the rim band, where the last rows must wear exactly the glow the band they melt into wears - a horizon sunset keeps its fire (those puffs ARE
// at the rim), an overhead sun stays behind the deck. Set well above the +0.25 baseline so the broad warmth toward the sun survives; orders of magnitude under the spike. [interaction: dome handoff]
const float SS_GLOW_CONE_CAP = 1.0;

// <SS:Nexii> The floor under the near haze path, as a share of the ZENITH slab (max_y). The true range fixed the hole the horizontal range put in the sky, but it left that cone's ghost: a steep
// ray meets the deck at little more than the layer's own height above or below the eye, so the puffs straight overhead - and straight underfoot, flying above the deck - carried a fraction of the
// air the band behind them wears at the same angle (the band's SHORTEST ray is the whole max_y column), and the deck kept a dim unhazed disc riding the camera's vertical: dark navy under a dusk
// sky while the slant field around it took the airlight, grey under a sunrise while the band burned. Absolute rather than a share of the ray's own slab, deliberately - near the horizon the slab
// convention runs to twenty slabs' length, and a share of THAT would dissolve a puff at arm's length at eye level into pure airlight. max_y is the one length the atmosphere's maths is calibrated
// against, so half of it veils a puff exactly as much as half the zenith column veils the dome; and slab_len >= max_y at every angle, so the floor can never exceed the slab convention and the
// rim easing below stays a one-way trip to the band's own figure. The same floor is what lets a sunrise reach the whole deck: the airlight between eye and puff is where the horizon's fire
// lives, and an unhazed puff had none of it. [interaction: dome handoff]
const float SS_NEAR_AIR_FLOOR = 0.5;

// NOTE: Keep the lighting below in sync with cloudsV.glsl's cloud colour path - it is that path, run per puff corner.
void main()
{
    // The quads arrive pre-built in world space - the puff field turns its positions camera-facing on the CPU, where it also sorts them back to front, so nothing is left to orient
    // here.
    vec3 rel = position.xyz - ss_cam_pos;
    float d = length(rel);
    vec3 drawn_pos = position.xyz;
    if (d > ss_squash.x && ss_squash.z > ss_squash.x)
    {
        float drawn = ss_squash.x + (d - ss_squash.x) * (ss_squash.y - ss_squash.x) / (ss_squash.z - ss_squash.x);
        drawn = min(drawn, ss_squash.y * 0.999);
        drawn_pos = ss_cam_pos + rel * (drawn / d);
    }
    gl_Position = modelview_projection_matrix * vec4(drawn_pos, 1.0);

    vary_texcoord0 = texcoord0;
    vary_color = diffuse_color;

    // The DRAWN position, deliberately: the fragment shader inverts the squash per fragment to recover the true one. Interpolating the true position as a varying warped it mid-quad -
    // perspective correction follows the drawn geometry, not the true one - which showed as noise swimming on the far rim.
    vary_world = drawn_pos;

    // Into the ogl frame (see the FRAME note above): from here down this is cloudsV.glsl's maths on the TRUE ray, straight off the attribute - position never wore the squash, and the squash
    // is radial so drawn and true share the ray anyway. No 50m stock ray lift: the Atmo discs draw at the true direction, and this field only exists under an active Atmo environment.
    vec3 rel_pos_norm = rel.yzx / max(d, 1.0e-4);

    // <SS:Nexii> The haze PATH, and the lesson its first two cuts each taught. The dome's own convention extends every ray to the atmosphere slab (max_y/y upward, a 32km stretch DOWNWARD) -
    // an honest convention for a dome, which is all sky and only ever looked at from inside. Adopting it whole meant a camera ABOVE the deck saw every puff through the downward branch's 32km
    // of air: transmittance zero, cloud terms extinguished, and the entire field rendered as bare airlight - pale grey everywhere, white fire in the glow cone, no trace of the sun on any
    // cloud body. But shading by bare metric distance is no better - that was the first grey deck - because the band the far puffs must join IS painted with the slab maths. So the path is the
    // deck's own TRUE range near, capped at the slab convention the ray would carry anyway, eased up to that convention across the same rim band the fragment stage flattens over, so the last
    // rows are hazed by exactly the maths that hazes the band they melt into. abs() on the slab term so a downward grazing ray converges like an upward one, never to the dome's 32km collapse.
    //
    // The near term was the HORIZONTAL range for a while, on the argument that the haze layer is horizontally uniform - and that argument, applied to a distance, puts a hole in the sky. Straight
    // up and straight down the horizontal range is ZERO however far the deck actually is, so the airlight vanished there and came back as the ray tipped over: a dark cone of unhazed puffs
    // centred on the eye, sliding across the deck with every camera move, which is the one thing a sky must never do. The true range is a real distance at every angle and agrees with the old
    // rule to within a few percent everywhere the old rule was defensible - by 45 degrees of elevation the two are a factor of 1.4 apart and both are far inside the rim easing. [interaction: dome handoff]
    float dome_rim = smoothstep(ss_rim.x, ss_rim.y, d);
    float slab_len = max_y / max(abs(rel_pos_norm.y), 0.05);
    float near_len = max(min(d, slab_len), max_y * SS_NEAR_AIR_FLOOR);
    float haze_len = mix(near_len, max(slab_len, near_len), dome_rim);

    // Initialize temp variables
    vec3 sunlight = (sun_up_factor == 1) ? sunlight_color : moonlight_color * 0.7;

    // <SS:Nexii> The rise band's three gates, as in skyV/cloudsV: the band-mixed light, the elevation floored at the disc's half-angle (elevation 0 is the INFINITE airmass, and feeding it in
    // is what cut the sunset out from under a half-risen disc), and the glow aimed at the disc rather than the lightnorm the moon takes at centre-set.
    float sun_elev;
    vec3 glow_dir;
    if (ss_sun_rise > 0.0)
    {
        sunlight = mix(moonlight_color * 0.7, sunlight_color, ss_sun_rise);
        sun_elev = max(ss_sun_dir.z, ss_sun_radius);
        glow_dir = ss_sun_dir.yzx;
    }
    else
    {
        sun_elev = lightnorm.y;
        glow_dir = lightnorm.xyz;
    }

    // Sunlight attenuation effect (hue and brightness) due to atmosphere
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

    // Calculate relative weights
    vec3 combined_haze = abs(blue_density) + vec3(abs(haze_density));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = haze_density / combined_haze;

    // Compute sunlight from rel_pos & lightnorm (for long rays like sky)
    float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + sun_elev);
    vec3 raw_light = sunlight;
    sunlight *= exp(-light_atten * off_axis);

    // Distance - the rim-eased haze path (see the long note at haze_len above).
    float density_dist = haze_len * density_multiplier;
    combined_haze = exp(-combined_haze * density_dist);

    // Compute haze glow - skyV/cloudsV's lines, band ramp included: built from the RAW angular term and scaled by the horizon-band share while the band is live (the stock factor line
    // belongs to the moon below centre-rise and would zero the whole sunset), the stock factor path otherwise, which deliberately zeroes the moon's glow (SL-13768).
    float haze_glow = 1.0 - dot(rel_pos_norm, glow_dir);
    haze_glow = max(haze_glow, .001);
    haze_glow *= glow.x;
    haze_glow = pow(haze_glow, glow.z);
    if (ss_sun_rise > 0.0)
    {
        haze_glow = ss_sun_rise * (haze_glow + 0.25);
    }
    else
    {
        haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (sun_moon_glow_factor * (haze_glow + 0.25));
    }

    // The disc spike stops here, at the deck's own rows - see SS_GLOW_CONE_CAP. Capped AFTER the full composition so both branches (band-mixed and stock) obey the same ceiling, and before the
    // airlight below is built, so both of the spike's rides down - vary_ss_glow and the airlight's haze term - are covered by the one line.
    haze_glow = mix(min(haze_glow, SS_GLOW_CONE_CAP), haze_glow, dome_rim);

    // <SS:Nexii> The glow's own capped light, as in skyV/cloudsV: the beam extinction's crush BOUNDED at SS_SUN_GLOW_DEPTH optical depths on the densest channel, scaled uniformly so the hue
    // survives - the sunset band deepens TO its colour, never through it to black. Feeds the haze term of the below-cloud airlight, exactly where cloudsV spends it.
    float ss_max_atten = max(light_atten.r, max(light_atten.g, light_atten.b));
    vec3 ss_glow_light = sunlight;
    if (ss_sun_rise > 0.0)
    {
        float ss_glow_airmass = min(off_axis, SS_SUN_GLOW_DEPTH / max(ss_max_atten, 1e-6));
        ss_glow_light = raw_light * exp(-light_atten * ss_glow_airmass);
    }

    // Increase ambient when there are more clouds
    vec3 tmpAmbient = ambient_color + (vec3(1.) - ambient_color) * cloud_shadow * 0.5;

    // Dim sunlight by cloud shadow percentage
    vec3 cs = sunlight * (1. - cloud_shadow);
    vec3 gs = ss_glow_light * (1. - cloud_shadow);

    // Haze color below cloud - the airlight between the eye and the deck, sunset fire included via the capped glow light.
    vec3 additiveColorBelowCloud = (blue_horizon * blue_weight * (cs + tmpAmbient)
                                 + (haze_horizon * haze_weight) * (gs * haze_glow + tmpAmbient));

    // CLOUDS - the deck's body sun: the authored sun extinguished along the sun's OWN path (cloudsV's 1/(2 elevation) line, with the band floor above), and CAPPED at the glow depth like
    // skyV's glow light - bright and warm at a horizon sun, never crushed to nothing. This is a deliberate departure from the band, learned the grey way: the band's body sun goes near-black
    // at a low sun and the band still reads because its glow gate repaints the thin cirrus near the disc - a flat painting's only directional cue. A volumetric deck's cue is its GEOMETRY:
    // the fragment stage aims this light with the puffs' own form and wrap terms, so a sunrise grazes the deck and burns its sun-facing sides - the alpenglow - while the anti-sun sides keep
    // the grey-blue ambient. Gating this behind the glow term instead (the first cut) threw the deck's one advantage away: dense puffs kept 0.35 of a light that was already extinguished, and
    // the whole field fell back to ambient grey. Where the beam maths is mild the cap never binds, so the day sky keeps the band's own line.
    vec3 cloud_sun = raw_light * exp(-light_atten
        * min(1.0 / max(1e-6, sun_elev * 2.), SS_SUN_GLOW_DEPTH / max(ss_max_atten, 1e-6)));

    // Cloud colour out: sun and ambient BOTH through the authored cloud colour and BOTH attenuated by the slab transmittance, with the airlight taking over what the attenuation removes -
    // cloudsV's exact composition (full-strength optical depth, no sqrt - the Atmo band dropped the stock halving, see cloudsV). This is what keeps the HDR inputs under the fragment stage's
    // clamp in the day sky, and what melts a horizon puff into the very air the band melts into. The airlight ships separately - see the vary_ss_airlight note above.
    vary_ss_sunlit = cloud_sun * cloud_color.rgb * combined_haze;
    vary_ss_amblit = tmpAmbient * cloud_color.rgb * combined_haze;
    vary_ss_airlight = additiveColorBelowCloud * (1. - combined_haze);
    vary_ss_glow = haze_glow;

    // The graze light - see the long note at vary_ss_top. The gentle square ramp fills the volume bottom to top; the crest takes the lid band, the puffs that are the deck's sunward surface
    // at a skimming sun. sun_elev is already the branch's own (band-floored sun, else the lightnorm body), so a horizon MOON earns the same skim in its own dim colour - vary_ss_sunlit is
    // the moonlight family then, and a moonrise deck keeping a faintly lit lid is true, not a leak.
    float layer_h = clamp((position.z - ss_base_z) / max(ss_layer_thick, 1.0), 0.0, 1.0);
    float graze = 1.0 - smoothstep(0.25, 0.60, sun_elev);
    vary_ss_top = graze * (0.35 * layer_h * layer_h + 0.50 * smoothstep(0.78, 1.0, layer_h));
}
