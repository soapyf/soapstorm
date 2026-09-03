/**
 * @file class1/deferred/ssWhiteoutF.glsl
 * @brief Atmo Magic: the whiteout veil - volumetric fog marched through the
 *        surface field along the view ray.
 *
 *        Not the environment's fog (global, fogs interiors) and not surface
 *        tinting (each fragment fogged by its own column - walls stand out,
 *        sheltered doorways next to fogged streets read wrong). The air is the
 *        fog: the view ray is marched from the camera to the fragment, and at
 *        each step the surface field answers whether that point is outdoors
 *        (the column test), how high above its surface it is (the layer's
 *        vertical falloff), and whether it is underwater (no fog). The
 *        transmittance is the product of the steps; a camera standing inside
 *        the whiteout fogs everything it sees by the air in front of the lens,
 *        and a sheltered surface seen through ten metres of storm fogs by
 *        those ten metres.
 *
 *        The sky has no endpoint: its veil integrates the camera-height density
 *        along the ray's climb through the layer, so the horizon collapses into
 *        the fog while steep-up rays leave it. Composited by the call site
 *        through an inscatter-plus-transmit blend (blendFunc ONE /
 *        SOURCE_ALPHA, ZERO / SOURCE_ALPHA): rgb = in-scattered fog light,
 *        alpha = the scene's transmittance.
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

// <SS:Nexii> Atmo Magic whiteout

// <SS:Nexii> The ray march's fixed step count. The product of the steps is exact in the limit (exp(-s*dx) raised to N rhs is exp(-s*N*dx), the same extinction whatever N), so the count only prices how faithfully the steps catch the density's variation along the ray - the vertical falloff and the covered/outdoors flips. 32 samples a depth range up to the far clip well and stays cheap as a full-screen pass. Kept a shader-side constant: there is no dial's worth of tuning in it.
const int SS_WHITEOUT_STEPS = 32;

out vec4 frag_color;

in vec2 vary_fragcoord;

// Agent space from view space. The field is anchored to the world; everything the gbuffer hands back is relative to the eye.
uniform mat4 ssFieldInvView;

// The layer's colour: the environment's horizon colour, smoothed CPU-side, so the veil reads as the sky the env author built rather than as a hardcoded white.
uniform vec3 ssWhiteoutColor;

// The layer's two demand curves, already ramped (in fast, out slow), already dialed: the squall's falling-snow veil and the ground-blizzard's drift-band veil.
uniform float ssWhiteoutSquall;
uniform float ssWhiteoutLift;

// The drift band's height above the stored surface, metres, and the extinction range, metres - the visibility inside a full-strength layer.
uniform float ssWhiteoutBand;
uniform float ssWhiteoutRange;

// The layer's depth scale, metres - 10 in light snow growing to 100 in a
// blizzard (set CPU-side from the ramped intensity). The fog is strongest at
// the ground and thins across this scale: objects above it sit above the
// weather.
uniform float ssWhiteoutFalloff;

// The ground reference for columns the field knows nothing about - the void
// ocean and everything past the stitched window. Fog still reaches there; it
// just measures height against the region's floor.
uniform float ssWhiteoutGroundZ;

// The water plane: samples below it are underwater, and fog does not.
uniform float ssWhiteoutWaterZ;

// The layer's density AT THE CAMERA - the ramped squall, dialed, faded by the
// camera's own height through the layer's depth scale. Sky rays integrate this
// along their climb through the layer.
uniform float ssWhiteoutSkyDensity;

// Diagnosis: 1 shows the fog amount as grayscale, 2 the density at the ray's
// midpoint (times five, so partial densities read), drawn as a replace.
uniform float ssWhiteoutDebug;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);
vec4 ssFieldFetch(vec2 xy_agent);

// The fog density at one point of the ray: zero under cover (the stored column
// top more than 0.75 m above the point - the footstep picker's indoor test),
// the squall's vertical falloff above the surface, plus the drift band hugging
// it. Past the stitched window the ground reference stands in for the surface.
float ssWhiteoutDensityAt(vec3 q)
{
    vec4 here = ssFieldFetch(q.xy);

    float surface_z = ssWhiteoutGroundZ;
    if (here.x > -1.0e5)
    {
        if (here.x - q.z > 0.75) return 0.0;   // covered - indoors, under a roof
        surface_z = here.x;
    }

    float h = max(q.z - surface_z, 0.0);
    float s = ssWhiteoutSquall * exp(-h / max(ssWhiteoutFalloff, 4.0));

    if (h < ssWhiteoutBand && q.z > surface_z - 1.0)
    {
        s += ssWhiteoutLift;
    }

    return s;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    float depth = getDepth(tc);
    vec4 raw = getNormRaw(tc);
    float flag = raw.w;

    // The sky has no surface and no endpoint: its veil is the camera-height
    // density integrated along the view ray's climb through the layer. Looking
    // down a street at the horizon, the ray stays inside the layer and the sky
    // collapses into the fog; looking steeply up, the ray leaves it fast. Sky
    // detection is by depth first - the WL sky dome does not carry the HDRI
    // flag - with the flag kept for anything else that marks itself off.
    if (depth >= 0.99995 ||
        GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_HDRI) ||
        GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        vec4 far_pos = getPositionWithDepth(tc, 1.0);
        vec3 ray = normalize(far_pos.xyz);
        float through = min(ssWhiteoutFalloff / max(ray.z, 0.15),
                            ssWhiteoutFalloff * 4.0);
        float veil = 1.0 - exp(-ssWhiteoutSkyDensity * through
                               * (3.0 / max(ssWhiteoutRange, 4.0)));
        frag_color = vec4(ssWhiteoutColor * veil, 1.0 - veil);
        return;
    }

    vec4 pos_view = getPositionWithDepth(tc, depth);
    vec3 end_agent = (ssFieldInvView * vec4(pos_view.xyz, 1.0)).xyz;
    vec3 cam_agent = (ssFieldInvView * vec4(0.0, 0.0, 0.0, 1.0)).xyz;

    // March the ray. Midpoint sampling of a fixed step count - the density
    // field is smooth (a solved flow's vertical falloff and a stitched
    // heightfield), so midpoint is plenty at this step size.
    float dist = length(end_agent - cam_agent);
    float transmittance = 1.0;
    float mid_density = 0.0;

    for (int i = 0; i < SS_WHITEOUT_STEPS; ++i)
    {
        float t = (float(i) + 0.5) / float(SS_WHITEOUT_STEPS);
        vec3 q = mix(cam_agent, end_agent, t);

        // Fog stops at the water surface. The boundary is a fade straddling
        // the plane, not a cut - the reconstructed depth wobbles centimetres
        // frame to frame, and a hard test there z-fights (observed).
        float water_gate = smoothstep(ssWhiteoutWaterZ - 0.10,
                                      ssWhiteoutWaterZ + 0.15, q.z);

        float s = ssWhiteoutDensityAt(q) * water_gate;
        if (i == SS_WHITEOUT_STEPS / 2) mid_density = s;

        // Extinction coefficient: a full-strength layer collapses visibility
        // to roughly ssWhiteoutRange metres.
        transmittance *= exp(-s * (dist / float(SS_WHITEOUT_STEPS))
                             * (3.0 / max(ssWhiteoutRange, 4.0)));
    }

    float fog = 1.0 - transmittance;

    // Diagnosis: alpha 1 = the blend replaces the pixel with rgb (no scene
    // contribution). Sky-flagged pixels still read as black - they carry no
    // fog by design.
    if (ssWhiteoutDebug > 0.5)
    {
        if (ssWhiteoutDebug < 1.5)
        {
            frag_color = vec4(vec3(fog), 1.0);                              // fog amount
        }
        else
        {
            frag_color = vec4(vec3(mid_density * 5.0), 1.0);                // midpoint density x5
        }
        return;
    }

    // The call site's blend is inscatter + transmit (rgb added, the scene
    // multiplied by alpha) - rgb carries the in-scattered fog light, alpha the
    // scene's transmittance.
    frag_color = vec4(ssWhiteoutColor * fog, transmittance);
}

