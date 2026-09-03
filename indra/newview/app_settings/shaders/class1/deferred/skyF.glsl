/**
 * @file class1/deferred/skyF.glsl
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

// Inputs
in vec3 vary_HazeColor;
in float vary_LightNormPosDot;

#ifdef SS_ATMO
in float vary_ss_below_horizon;
uniform float ss_horizon_clip;
#endif

#ifdef HAS_HDRI
in vec4 vary_position;
in vec3 vary_rel_pos;
uniform float sky_hdr_scale;
uniform float hdri_split_screen;
uniform mat3 env_mat;
uniform sampler2D environmentMap;
#endif

uniform sampler2D rainbow_map;
uniform sampler2D halo_map;

uniform float moisture_level;
uniform float droplet_radius;
uniform float ice_level;

#ifdef SS_ATMO
in vec3 vary_ss_view_dir;

// <SS:Nexii> Weather-driven optics (ss_optics below), bound from the sky pool's active Atmo applier. ss_optic_light is the light direction in the dome's own frame (the same permuted frame lightnorm lives in, with +Y up) and the amplitudes are the weather's corona / crystal drives. ss_optic_gate is 0 unless an ACTIVE Atmo environment is pushing at least one of them, so an idle viewer keeps stock halo_map exactly.
uniform vec3  ss_optic_light;
uniform float ss_optic_gate;
uniform float ss_optic_active;
uniform float ss_optic_corona;
uniform float ss_optic_halo22;
uniform float ss_optic_halo46;
uniform float ss_optic_align;

// <SS:Nexii> The optics' light colour, from the SUN only (for now): vary_ss_optic_sun_col is the sun's own light as the DOME renders it, computed in the vertex shader (skyV.glsl) as the capped-glow light the sunset band actually burns with, evaluated along the light's ray - so the halos, arcs and sundogs keep their sunrise/sunset hue down through the horizon band and below it, where the old CPU-bound replica of the uncapped beam underflowed and snapped to the raw near-white authored colour. Ice-crystal optics scatter the SUN's light - a sundog is a mock sun - so while the sun is physically up the phenomena wear this tint whatever the scene-light handover says. Moonlight optics are not wired up yet; while no sun band is live the shader keeps the bound faint-white tint. The stock sunlight_color uniform is the raw authored colour, near-white even when everything red, so this explicit varying is the honest one.
in vec3 vary_ss_optic_sun_col;

// <SS:Nexii> The sun slot disc's half-angle as a direction-z sine (SSAtmoEnvApplier:: sunSlotRadius) - the same value skyV holds its airmass with. The corona scales its every angle by this relative to the stock quad's 0.05, so it stays a rim around WHATEVER disc is drawn rather than the fixed-angle aureole tuned for the old 10x quad.
uniform float ss_sun_radius;

// <SS:Nexii> The physical rainbow's gate (SSAtmoRainbow, lldrawpoolwlsky.cpp): 1 lets ss_rainbow below take the stock strip's place, 0 keeps the stock single bow bit for bit.
uniform float ss_rainbow_gate;

// <SS:Nexii> The rainbow grades itself by its light, both already uploaded to this program: lightnorm is the active light's direction in the dome's own frame (+Y up, so lightnorm.y is its elevation sine) and sun_up_factor says whether that light is the sun. A sun within a few degrees of the horizon has the short wavelengths scattered out of its long light path - the monochrome red rainbow - and moonlight is too dim for the cones - the white moonbow.
uniform vec3 lightnorm;
uniform int  sun_up_factor;
#endif

out vec4 frag_data[4];

vec3 srgb_to_linear(vec3 c);
vec3 linear_to_srgb(vec3 c);

#define PI 3.14159265

/////////////////////////////////////////////////////////////////////////
// The fragment shader for the sky
/////////////////////////////////////////////////////////////////////////


vec3 rainbow(float d)
{
    // 'Interesting' values of d are -0.75 .. -0.825, i.e. when view vec nearly opposite of sun vec
    // Rainbox tex is mapped with REPEAT, so -.75 as tex coord is same as 0.25.  -0.825 -> 0.175. etc.
    // SL-13629
    // Unfortunately the texture is inverted, so we need to invert the y coord, but keep the 'interesting'
    // part within the same 0.175..0.250 range, i.e. d = (1 - d) - 1.575
    d         = clamp(-0.575 - d, 0.0, 1.0);

    // With the colors in the lower 1/4 of the texture, inverting the coords leaves most of it inaccessible.
    // So, we can stretch the texcoord above the colors (ie > 0.25) to fill the entire remaining coordinate
    // space. This improves gradation, reduces banding within the rainbow interior. (1-0.25) / (0.425/0.25) = 4.2857
    float interior_coord = max(0.0, d - 0.25) * 4.2857;
    d = clamp(d, 0.0, 0.25) + interior_coord;

    float rad = (droplet_radius - 5.0f) / 1024.0f;
    return pow(texture(rainbow_map, vec2(rad+0.5, d)).rgb, vec3(1.8)) * moisture_level;
}

#ifdef SS_ATMO
// <SS:Nexii> The physical rainbow, painted from the same strip texture the stock lookup reads. Stock sweeps one texcoord across the whole strip and adds whatever is there at one strength - the bow band and the bright interior stretch together - which is why the interior arrives as the blinding wash below the arc. The three phenomena the strip (and physics) carry are read separately here, each at its own strength: * the PRIMARY bow, the colour band at 34.4..41.4 deg off the antisolar point (red outer, violet inner), at ~30% - a real bow is a faint thing against the storm light behind it; * the INTERIOR brightening - light re-scattered inside the bow - on the stock stretch above the band, at ~8%: real photos show it, never as a wash; * the SECONDARY bow at ~49..55 deg, the same colour band read REVERSED (red inner, violet outer) at ~a third of the primary. Every rainbow is a double: the second bow rides two internal reflections instead of one and arrives faint. The gap between the bows is Alexander's band and renders as exactly nothing - darker than either bow in every photo. The light's own state then grades the result: a sun within a few degrees of the horizon has blue and green scattered away before the drops ever see it (the monochrome red rainbow), and a moonbow is too dim for the cones - the full spectrum is present but the eye reads white, at a fraction of the strength. The gate (ss_rainbow_gate) leaves the stock strip above untouched when the debug setting is down.
vec3 ss_rainbow(float d)
{
    if (moisture_level <= 0.0)
    {
        return vec3(0.0);
    }

    // Stock's remap, unclamped: t sweeps 0.175..0.25 across the band (red outer, violet
    // inner), 0.25..0.425 across the interior stretch, and 0..0.081 across the secondary's
    // angular window.
    float t   = -0.575 - d;
    float rad = (droplet_radius - 5.0f) / 1024.0f;

    // Primary bow: the band clamp, faded at the red edge where the strip hands over to black.
    // The (1 - w) share keeps it off the interior's half of the stretch.
    float w    = smoothstep(0.25, 0.258, t);
    float m_in = smoothstep(0.172, 0.178, t);
    vec3 bow = pow(texture(rainbow_map, vec2(rad + 0.5, clamp(t, 0.175, 0.25))).rgb, vec3(1.8));

    // Interior: the stock stretched lookup, dead until the violet edge has passed.
    vec3 interior = pow(texture(rainbow_map, vec2(rad + 0.5, 0.25 + max(t - 0.25, 0.0) * 4.2857)).rgb, vec3(1.8));

    // Secondary bow: the colour band reversed - s 1 is the red inner edge (t 0.081, 49 deg off
    // the antisolar point), s 0 the violet outer one (t 0.0, ~55 deg) - windowed so neither
    // edge smears into Alexander's band.
    float s     = clamp(t / 0.081, 0.0, 1.0);
    float m_sec = smoothstep(0.0, 0.005, t) * (1.0 - smoothstep(0.076, 0.081, t));
    vec3 secondary = pow(texture(rainbow_map, vec2(rad + 0.5, 0.25 - s * 0.075)).rgb, vec3(1.8));

    vec3 col = bow * (m_in * (1.0 - w)) * 0.30
             + interior * w * 0.08
             + secondary * m_sec * 0.09;

    col *= moisture_level;

    // The light's grade. lightnorm.y is the active light's elevation sine either way;
    // sun_up_factor says whether that light is the sun.
    float luma = dot(col, vec3(0.299, 0.587, 0.114));
    if (sun_up_factor == 0)
    {
        // Moonbow: needs a near-full moon to see at all, and the cones stand down - the
        // spectrum is present but reads white, at a quarter strength.
        col = mix(vec3(luma), col, 0.2) * 0.25;
    }
    else
    {
        // Red rainbow: the lower the sun, the longer the light path and the more of the short
        // wavelengths is scattered away before the drops ever see it. Full monochrome inside
        // two degrees of the horizon, gone by seven.
        float mono = 1.0 - smoothstep(1.0, 7.0, degrees(asin(clamp(lightnorm.y, -1.0, 1.0))));
        col = mix(col, vec3(luma) * vec3(0.85, 0.22, 0.07), mono);
    }
    return col;
}
#endif

vec3 halo22(float d)
{
    d       = clamp(d, 0.1, 1.0);
    float v = sqrt(clamp(1 - (d * d), 0, 1));
    return texture(halo_map, vec2(0, v)).rgb * ice_level;
}

#ifdef SS_ATMO
// Halo fringe colour: soft white, warmed toward the ring's inner edge and cooled on the outer -
// the classic red-inward / blue-outward halo tint, kept faint so halos read as clean light.
vec3 ss_optic_color(float rho, float ring, float width)
{
    float off = (rho - ring) / max(width, 1e-4);
    vec3 c = vec3(0.42, 0.45, 0.48);
    c += vec3(0.30, 0.10, -0.02) * (1.0 - smoothstep(-1.0, 0.2, off));
    c += vec3(-0.06, 0.02, 0.26) * smoothstep(-0.2, 1.0, off);
    return c;
}

// <SS:Nexii> A spectral gradient across a phenomenon's angular width - the prismatic colouring of the circumzenithal arc and (more vividly) the parhelic circle: red on the INNER edge through orange/yellow to blue-violet on the OUTER, the classic crystal-optics dispersion. off spans the width of the phenomenon (-.. + .. about the ring).
vec3 ss_prismatic(float off)
{
    vec3 red    = vec3(1.0, 0.35, 0.15);
    vec3 orange = vec3(1.0, 0.62, 0.20);
    vec3 yellow = vec3(1.0, 0.92, 0.45);
    vec3 green  = vec3(0.55, 0.92, 0.55);
    vec3 cyan   = vec3(0.4, 0.75, 1.0);
    vec3 blue   = vec3(0.35, 0.45, 1.0);
    float t = clamp(off * 0.5 + 0.5, 0.0, 1.0);   // -1 (inner,red) -> +1 (outer,blue)
    t = t * t * (3.0 - 2.0 * t);
    if (t < 0.25) return mix(red, orange, t * 4.0);
    if (t < 0.42) return mix(orange, yellow, (t - 0.25) * 5.88);
    if (t < 0.6)  return mix(yellow, green, (t - 0.42) * 5.56);
    if (t < 0.78) return mix(green, cyan, (t - 0.6) * 5.56);
    return mix(cyan, blue, (t - 0.78) * 4.55);
}

// Weather-driven split optics. The view ray and light direction share the sky dome's frame (+Y up,
// light permuted like lightnorm), so each phenomenon lands at its true angular position: the corona
// hugs the light, the 22 deg and 46 deg halos are rings at those radii, sundogs sit where the
// 22 deg small circle crosses the light's altitude plane, the parhelic circle is the THIN
// horizontal arc at the light's own altitude (the line the dogs ride on), the circumzenithal arc
// is a small circle around the zenith passing ~47 deg above the light (brightest with the light
// at 15-25 deg elevation, gone past ~32 deg), and the supralateral arc crowns the 46 deg ring
// while the light rides low. The amplitudes and the elevation gates come from the weather
// uniforms - a given sky may simply not be asking for a phenomenon.
vec3 ss_optics(vec3 view)
{
    if (ss_optic_corona <= 0.001 && ss_optic_halo22 <= 0.001
        && ss_optic_halo46 <= 0.001 && ss_optic_align <= 0.001)
    {
        return vec3(0.0);
    }

    vec3 light = normalize(ss_optic_light);
    const vec3 up = vec3(0.0, 1.0, 0.0);

    float rho  = degrees(acos(clamp(dot(view, light), -1.0, 1.0)));
    float elev = degrees(asin(clamp(light.y, -1.0, 1.0)));

    // The light's own vertical: 0 points up the vertical circle of the light, 90 the horizontal.
    vec3 vertical = normalize(up - light * dot(up, light));
    if (dot(up, light) > 0.998)      // light overhead: no usable vertical circle
    {
        vertical = vec3(0.0, 0.0, 1.0);
    }
    float psid = degrees(acos(clamp(dot(view, vertical), -1.0, 1.0)));

    // <SS:Nexii> The horizon-clip state as a mask: 1 everywhere the dome may draw - clip off, or this fragment above the horizon - and 0 under the clip. Every phenomenon except the sundogs multiplies by (1.0 - ss_below), so halos and arcs never paint below the horizon. The dogs alone reach a little UNDER it (their own block re-carves the margin), because the top of the light is still visible when its centre is just below the horizon.
    float ss_below = (ss_horizon_clip > 0.001 && vary_ss_below_horizon < 0.0) ? 1.0 : 0.0;

    // <SS:Nexii> The 22 deg ring falls away as the light sinks toward the horizon: by ~14 deg of light elevation the ring has faded, leaving only the mock-sun sundogs flanking the light - the look of a low winter sun.
    float ring_elev = smoothstep(-1.0, 1.0, elev);

    vec3 col = vec3(0.0);

    // Corona: a thin bluish-white aureole hugging the light plus two faint diffraction rings
    // beyond it (water drops). Kept small and dim - the aureole is half gone by ~0.4 deg and
    // dead by ~1 deg, so a corona reads as a rim around the disc, never a glow that doubles it.
    // Every angle below runs on rho scaled by the drawn disc relative to the stock quad
    // (ss_disc), which is what kept it that rim when the discs shrank 10x off the old quad:
    // the ring radii ride in the disc's own half-angle the way they originally rode in the
    // stock quad's. The crystal halos further down stay at TRUE angles - droplet and ice
    // optics, not disc-relative ones.
    if (ss_optic_corona > 0.001)
    {
        // No drawn disc (active environment, no emitter) keeps the fixed-angle corona rather
        // than letting the 1e-5 floor blow the scale up into a full-sky wash.
        float ss_disc       = (ss_sun_radius > 1e-6) ? ss_sun_radius / 0.05 : 1.0;
        float cor_rho       = rho * ss_disc;
        const float aureole = exp(-pow(cor_rho * 2.4, 2.0));
        const float ringA   = exp(-pow((cor_rho - 2.4) / 1.0, 2.0));
        const float ringB   = exp(-pow((cor_rho - 4.6) / 1.5, 2.0));

        vec3 ccol = vec3(0.42, 0.44, 0.48) * aureole
                  + vec3(0.12, 0.07, 0.04) * ringA
                  + vec3(0.04, 0.07, 0.12) * ringB;
        col += ccol * ss_optic_corona * 0.5 * (1.0 - ss_below);
    }

    // 22 deg halo: the everywhere veil of small platelets. Real halos are faint - a soft ring a
    // few times dimmer than the sun's own glare, so the scales below are kept low; the sundog is
    // the one member of the family that reads as a bright spot.
    if (ss_optic_halo22 > 0.001)
    {
        float w = 1.9;
        col += ss_optic_halo22 * exp(-pow((rho - 22.0) / w, 2.0))
             * (normalize(vary_ss_optic_sun_col) * 0.8 + ss_optic_color(rho, 22.0, w) * 0.2)
             * 0.048 * (1.0 - ss_below) * ring_elev
             * smoothstep(21.3, 22.0, rho);
    }

    // 46 deg halo family (large plates/columns) - wider and fainter still.
    if (ss_optic_halo46 > 0.001)
    {
        float w = 2.3;
        col += ss_optic_halo46 * exp(-pow((rho - 46.0) / w, 2.0))
             * (normalize(vary_ss_optic_sun_col) * 0.8 + ss_optic_color(rho, 46.0, w) * 0.2)
             * 0.048 * (1.0 - ss_below);
    }

    // Aligned-plate phenomena: need the plates to settle, not just plenty of crystals.
    if (ss_optic_align > 0.001)
    {
        // <SS:Nexii> The parhelic circle WITH its sundogs embedded - one construct, not two. The band is the locus of points at the SAME elevation as the light (ep = e), which as the light climbs bulges from a near-horizontal line into a small circle (more "circle"-like at high sun). It fades with distance from the light (rho / 55) - never a full closed ring. The SUNDOGS are bright COMPACT peaks embedded in the band where the 22 deg halo circle crosses it: at az = asin( sin(22) / cos(e) ) from the sun - ~22 deg at the horizon, spreading outward as the sun climbs (26 at e=30, 31 at e=40), and shrinking as they spread. Each dog is kept SMALL and tight - the line stays thin and clean around it, so the dog reads as a distinct bright spot ON a fine line, not a comet. The dogs can never drift off the line, because the line IS where they live.
        {
            // Polar coordinates
            float e  = asin(clamp(light.y, -1.0, 1.0)) * 57.2958;
            float ep = asin(clamp(view.y,  -1.0, 1.0)) * 57.2958;
            float dog_elev = smoothstep(-1.0, 3.0, elev)
                           * (1.0 - smoothstep(45.0, 60.0, elev));

            // Azimuth of the sundogs, which spread out from the 22 halo as sun climbs
            float az = asin(clamp(sin(22.0 * 0.0174533) / max(cos(elev * 0.0174533), 1e-4), -1.0, 1.0));
            az *= 57.2958;   // to degrees
            float spread = clamp(az / 22.0, 1.0, 3.5);
            
            float x_l = rho - az; 
            float x_r = -rho - az;
            float x = max(x_l, x_r); // Horizontal distance from the sundogs
            float y = ep - e;        // Vertical distance from parhelic circle line
            
            // Scaling brightness based on sun elevation
            float brightness = smoothstep(61.0, 5.0, elev);
            
            // Tail length stretches aggressively out as spread increases
            float tail_length = 14.0 * pow(spread, 1.7);
            
            // Vertical scale profiles (Varies dramatically based on sun elevation)
            // Low sun = thick vertical pillars; High sun = narrow daggers
            float core_vertical_width = mix(2.8, 0.9, smoothstep(0.0, 35.0, elev)) * spread;
            float tail_vertical_width = mix(1.2, 0.4, smoothstep(0.0, 35.0, elev)) * spread;

            // Glow profile
            float inner_wall = smoothstep(-0.8, 0.4, x);
            float tail_window = max(1.0 - (x / tail_length), 0.0);
            
            // Exponential horizontal sweep that fuels both the core and the tail smoothly
            float horizontal_glow = pow(tail_window, 2.5) * inner_wall;

            // Parhelic Band
            float base_line_width = 0.5 * clamp(spread * 0.5, 1.0, 2.0);
            float band_thickness = base_line_width + (5.0 / spread) * pow(tail_window, 4.0) * inner_wall;
            
            float band = exp(-pow(y / band_thickness, 2.0))
                       * exp(-pow(rho / 55.0, 2.0));

            // Blanket core streak
            float local_thickness = mix(tail_vertical_width, core_vertical_width, pow(tail_window, 3.0));
            
            // Glowing mass of the dogs
            float dog_intensity = (4.0 / spread) * brightness;
            float dogs = horizontal_glow * exp(-pow(y / local_thickness, 2.0)) * dog_intensity;

            // Prismatic gradient tweaks
            vec3 pc_col = normalize(vary_ss_optic_sun_col) * 0.40
                        + ss_prismatic((x - 0.2) / (7.0 * spread)) * 0.60;

            // Compositing
            band *= 0.6 * (0.5 + inner_wall * 0.5);
            dogs *= 1.8;
            
            float pc = band + dogs;
            col += ss_optic_align * pc_col * pc * dog_elev * 0.12;
        }

    // <SS:Nexii> The circumzenithal arc's REAL geometry and visibility. The arc is a small circle centered on the ZENITH, passing through a point about 47 deg ABOVE the sun in the sun's meridian - its zenith-angle radius is 43 deg minus the sun's elevation, so its summit sits e+47 deg above the horizon. At a low sun (5-15 deg) that circle is wide and low, faint and spread; as the sun climbs toward ~22 deg it tightens and brightens toward its best; past ~32 deg the plate-ray geometry fails and it is gone entirely. The envelope is a triangle peaking at 22 deg, zero at 5 and 32.2. Prismatic: red on the arc's inner (zenith-side) edge, blue-violet outward.
    {
        float cza_env = smoothstep(5.0, 22.0, elev) * (1.0 - smoothstep(22.0, 32.2, elev));
        if (cza_env > 0.002)
        {
            vec3 h_light = normalize(light - up * dot(light, up) + vec3(1e-5));
            vec3 h_view  = normalize(view  - up * dot(view, up)  + vec3(1e-5));
            float az = degrees(acos(clamp(dot(h_light, h_view), -1.0, 1.0)));
            float zd = degrees(acos(clamp(dot(view, up), -1.0, 1.0)));
            float rcz = 43.0 - elev;                 // the arc's zenith-angle radius (43 - e)
            float cza = exp(-pow((zd - rcz) / 2.2, 2.0))
                      * exp(-pow(az / 40.0, 2.0));
            // <SS:Nexii> The CZA is genuinely PRISMATIC - red toward the arc's inner (zenith-side) edge, blue-violet away - the strongest colouring of the crystal halos. The old monotone red-in/blue-out fringe read as a white streak.
            col += ss_optic_align * cza_env * cza
                 * ss_prismatic((zd - rcz) / 2.2)
                 * 0.4 * (1.0 - ss_below);
        }
    }
    }

    // Supralateral arc: the 46 deg ring's tangent arc crowning the light, seen while the light is low.
    if (ss_optic_halo46 > 0.001)
    {
        float low = 1.0 - smoothstep(34.0, 52.0, elev);
        if (low > 0.001)
        {
            float w = 2.3;
            float lateral = exp(-pow((rho - 46.0) / w, 2.0))
                          * exp(-pow(psid / 34.0, 2.0)) * low;
            col += ss_optic_halo46 * lateral
             * (normalize(vary_ss_optic_sun_col) * 0.8 + ss_optic_color(rho, 46.0, w) * 0.2)
             * 0.16 * (1.0 - ss_below);
        }
    }

    return col * ss_optic_gate;
}
#endif

void main()
{
#ifdef SS_ATMO
    // <SS:Nexii> Horizon clip's depth write (SSAtmoEnvAtmosphere::mHorizonClip)
    gl_FragDepth = (ss_horizon_clip > 0.0 && vary_ss_below_horizon < 0.0) ? LL_SHADER_CONST_HORIZON_DEPTH : 1.0;
#endif

    vec3 color;
#ifdef HAS_HDRI
    vec3 frag_coord = vary_position.xyz/vary_position.w;
    if (-frag_coord.x > ((1.0-hdri_split_screen)*2.0-1.0))
    {
        vec3 pos = normalize(vary_rel_pos);
        pos = env_mat * pos;
        vec2 texCoord = vec2(atan(pos.z, pos.x) + PI, acos(pos.y)) / vec2(2.0 * PI, PI);
        color = textureLod(environmentMap, texCoord.xy, 0).rgb * sky_hdr_scale;
        color = min(color, vec3(8192*8192*16)); // stupidly large value arrived at by binary search -- avoids framebuffer corruption from some HDRIs

        frag_data[2] = vec4(0.0,0.0,0.0,GBUFFER_FLAG_HAS_HDRI);
    }
    else
#endif
    {
        // Potential Fill-rate optimization.  Add cloud calculation
        // back in and output alpha of 0 (so that alpha culling kills
        // the fragment) if the sky wouldn't show up because the clouds
        // are fully opaque.

        color = vary_HazeColor;

        float  rel_pos_lightnorm = vary_LightNormPosDot;
        float optic_d = rel_pos_lightnorm;
#ifdef SS_ATMO
        // <SS:Nexii> The horizon clip cuts the lower dome at eye level - and the sun with it
        const bool ss_clipped_below = (ss_horizon_clip > 0.0) && (vary_ss_below_horizon < 0.0);
        if (!ss_clipped_below)
        {
            // <SS:Nexii> The physical rainbow (ss_rainbow above) takes the stock strip's place while the SSAtmoRainbow gate is up; gate down keeps the stock single bow bit for bit.
            color.rgb += (ss_rainbow_gate > 0.0) ? ss_rainbow(optic_d) : rainbow(optic_d);
        }
        if (ss_optic_active > 0.001)
        {
            // <SS:Nexii> Weather-driven optics take over while an active Atmo environment drives the sky (ss_optic_gate): the corona, the 22/46 deg halos, sundogs and the aligned-plate arcs each render at their true angular positions instead of one merged texture strip. When the halo step is switched off the strip is skipped too - an active Atmo sky never draws the old corona-plus-wide-ring halo_map. An IDLE Atmo viewer (boosted master switch, no environment) keeps the stock strip below, so it stays bit-for-bit.
            if (ss_optic_gate > 0.001 && !ss_clipped_below)
            {
                color.rgb += ss_optics(normalize(vary_ss_view_dir));
            }
        }
        else if (!ss_clipped_below)
        {
            color.rgb += halo22(optic_d);
        }
#else
        color.rgb += rainbow(optic_d);
        color.rgb += halo22(optic_d);
#endif
        color.rgb *= 2.;
        // <SS:Nexii> Cap the sky's LUMINANCE, not its channels: a per-channel clamp saturates anything past 5 to white, and the halos/sundogs added just above sit on a bright haze near the sun that easily crosses it - so the light's own red came out white. Scaling the whole colour down to a 5-luminance cap keeps the peak bright for bloom while the sun's tint survives.
        float ss_sky_cmax = max(color.r, max(color.g, color.b));
        if (ss_sky_cmax > 5.0)
        {
            color.rgb *= 5.0 / ss_sky_cmax;
        }

        frag_data[2] = vec4(0.0,0.0,0.0,GBUFFER_FLAG_SKIP_ATMOS);
    }

    frag_data[1] = vec4(0);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(color.rgb, 1.0);
#else
    frag_data[0] = vec4(color.rgb, 1.0);
#endif
}

