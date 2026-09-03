/**
 * @file class1/deferred/ssSurfaceNormalF.glsl
 * @brief Atmo Magic wet surfaces: normal flattening. A water film smooths out
 *        the micro-relief a rough surface scatters its highlight over, and
 *        the visible result of that is a flatter normal, not only a tighter
 *        specular lobe on the original bumpy one. This is the second half of
 *        the wet look; ssSurfaceWetF.glsl is the first.
 *
 * A companion pass to ssSurfaceWetF.glsl rather than folded into it: that
 * shader is already proven end to end, and every early return in it would
 * have needed a matching normal output added by hand to extend it in place.
 * A second, independent pass over the same field costs one more full-screen
 * triangle and touches none of that proven code.
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

// <SS:Nexii> Atmo Magic wet surfaces - normal flattening

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform mat4 ssFieldInvView;

uniform float ssWetStrength;
uniform float ssWetDebugForce;
uniform float ssWetSkipExposure;

// How far a fully wet surface's normal leans toward world up, 0 leaves the normal alone and 1 goes all the way flat. Deliberately its own dial rather than reusing wet directly - a puddle wants this
// at 1, a merely damp wall wants barely any of it, and that is a judgement about the look, not something the wetness value itself should decide.
uniform float ssWetNormalFlatten;

// Cosines of the two angles from vertical-up that bound the taper: at or below ssWetFlattenCosFull the surface is close enough to flat that water on it pools the way it does on a roof or the ground,
// and gets the full flatten amount; at or above ssWetFlattenCosZero it is close enough to a wall that water on it runs as a thin sheet following the wall's own plane rather than pooling flat, and
// gets none. Uploaded as cosines rather than angles so the shader never has to take an inverse cosine to use them - the surface's own normal dotted with up is already a cosine.
uniform float ssWetFlattenCosFull;
uniform float ssWetFlattenCosZero;

// Standing water flattens on its own terms, the same way it earns its own specular treatment in ssSurfaceWetF.glsl: a puddle's surface is level because it is a pool, not because the wall/roof slope
// test here happened to allow it, so it bypasses that gate entirely rather than being scaled by it. ssWetPuddleDepthFull is the same figure the wetness pass uses, so a spot the two shaders agree is
// a full puddle reads as one consistently.
uniform float ssWetPuddleDepthFull;
uniform float ssWetPuddleFlatten;

// Flow motion: water visibly running along the drainage rather than merely sitting on it. Reuses the same tileable wave-normal texture the water plane itself scrolls for its own ripples, rather than
// authoring a second one - the look this is chasing is exactly that texture's, just carried by the flow direction of a channel instead of the wind blowing the lake.
uniform sampler2D ssWaveMap;
uniform float ssTime;
uniform float ssWetFlowScale;     // metres per tile of the wave texture
uniform float ssWetFlowSpeed;     // metres per second the pattern scrolls
uniform float ssWetFlowStrength;  // how far a fully flowing cell blends toward it

// Surface tension's stand-in: how wet (or how full a pool) a channel cell has to be before it counts as spilling at all, below which it stays a still, undisturbed film. 0 disables the threshold
// outright.
uniform float ssWetFlowMinWet;

// The wave texture was authored for the water plane's own UV convention, not for a tangent frame built straight off a drainage flow vector - whichever way its streaks actually run, there is no
// reason to expect it lines up with "downstream" in this frame. Turning the sampled normal's tangent- plane components by this before they are laid onto the flow-aligned frame is exactly the same
// fix as rotating the texture itself would be, without a second copy of it rotated on disk.
uniform float ssWetFlowRotSin;
uniform float ssWetFlowRotCos;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);
vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);
vec4 ssFieldAt(vec3 p_agent, vec3 n_agent);
vec4 ssFieldFetch(vec2 xy_agent);
vec3 ssFieldFetchFlow(vec2 xy_agent);

void main()
{
    vec2 tc = vary_fragcoord.xy;

    // The env intensity and gbuffer flag live in the same texel as the encoded normal but are not part of what decodeNormal reconstructs - exactly the trap ssSurfaceWetF.glsl's own flag read fell
    // into earlier. Both have to come from the raw fetch and go back out unchanged; only the normal itself is ever supposed to move.
    vec4 raw = getNormRaw(tc);
    float flag = raw.w;
    float env = raw.z;

    // Sky, stars, the sun disc, HDRI - none of them are surfaces with a normal to flatten
    if (GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_HDRI) ||
        GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        frag_color = raw;
        return;
    }

    float depth = getDepth(tc);
    vec4 pos_view = getPositionWithDepth(tc, depth);
    vec3 p = (ssFieldInvView * vec4(pos_view.xyz, 1.0)).xyz;

    vec3 n_view = decodeNormal(raw).xyz;
    vec3 n_world = normalize(mat3(ssFieldInvView) * n_view);

    float wet;
    float puddle;
    if (ssWetDebugForce > 0.0)
    {
        wet = ssWetDebugForce;
        puddle = ssWetDebugForce;
    }
    else if (ssWetSkipExposure > 0.0)
    {
        vec4 field = ssFieldFetch(p.xy);
        if (field.x < -1.0e5)
        {
            frag_color = raw;
            return;
        }
        wet = field.y * ssWetStrength;
        puddle = clamp(field.w / ssWetPuddleDepthFull, 0.0, 1.0);
        if (wet < 0.004 && puddle < 0.004)
        {
            frag_color = raw;
            return;
        }
    }
    else
    {
        vec4 field = ssFieldAt(p, n_world);
        wet = field.x * field.w * ssWetStrength;
        puddle = clamp(field.z / ssWetPuddleDepthFull, 0.0, 1.0);
        if (field.w < 0.0 || (wet < 0.004 && puddle < 0.004))
        {
            frag_color = raw;
            return;
        }
    }

    // How much this surface's own tilt lets water pool flat on it at all. Roofs and the ground read close to 1; a wall reads close to 0, because water clinging to a wall runs down as a sheet that
    // still follows the wall's own plane rather than levelling out the way standing water does. Without this a vertical surface would flatten exactly as much as a horizontal one for the same
    // wetness, which is what turned every wet wall into a puddle standing on its side. The gbuffer's normal is the final SHADING normal - whatever a bump or normal map perturbed it to - not the flat
    // geometric surface underneath. "Is this a wall or a roof" is a question about the geometry, not the brickwork on it, and answering it from the pixel normal would have a heavily bump-mapped
    // vertical wall flattening in some pixels and not others depending on which way each individual bump happened to tilt. The gradient of view-space position across the screen is the actual surface
    // the geometry describes, independent of any normal map, and costs nothing beyond two derivatives already sitting in hardware.
    vec3 n_geo_view = cross(dFdx(pos_view.xyz), dFdy(pos_view.xyz));
    if (dot(n_geo_view, -pos_view.xyz) < 0.0) n_geo_view = -n_geo_view;
    vec3 n_geo_world = normalize(mat3(ssFieldInvView) * n_geo_view);

    float up_align = dot(n_geo_world, vec3(0.0, 0.0, 1.0));
    float slope_factor = smoothstep(ssWetFlattenCosZero, ssWetFlattenCosFull, up_align);

    // Blended toward world up rather than toward some notion of the surface's own unweathered flat direction, so a sloped wet roof still tilts its highlight the way a real film of water lying or
    // running on it would - the water's surface answers to gravity, not to whatever the material underneath happens to be shaped like.
    float flatten_wet = wet * ssWetNormalFlatten * slope_factor;
    float flatten_puddle = puddle * ssWetPuddleFlatten;
    float flatten = clamp(max(flatten_wet, flatten_puddle), 0.0, 1.0);
    vec3 flat_world = normalize(mix(n_world, vec3(0.0, 0.0, 1.0), flatten));

    // Water actually running along a channel, laid over the flattened normal above rather than instead of it - a stream is still a flat film first, moving ripples second. A first few drops on a dry
    // gutter do not run, they cling - surface tension holds a thin film in place until enough has gathered to break free and move as a body, and a channel with any wetness on it at all showing full
    // flow the instant rain starts is exactly the "damp reads as a rushing stream" that skipping this would leave in. wet and puddle are the only per-cell figures that build up over time at all
    // here, so this is asking the same question of whichever of them is greater rather than of an actual depth a channel cell does not otherwise keep.
    vec3 flow = ssFieldFetchFlow(p.xy);
    float wet_for_flow = smoothstep(ssWetFlowMinWet, 1.0, max(wet, puddle));

    // The ripple's sample coordinates, built out here rather than inside the branch below because the reach that decides the branch reads their screen-space derivatives, and taking a derivative
    // inside non-uniform control flow is not defined.
    vec2 flow_uv = (p.xy - flow.xy * (ssTime * ssWetFlowSpeed)) / ssWetFlowScale;

    // Faded by what a pixel actually covers, not by how far away the surface sits. The ripple is centimetre-scale detail sampled by world position in a screen-space pass, and its failure mode in
    // the distance is moire - sheets of thin parallel lines - once the pattern's finest waves arrive at a couple of pixels per wavelength. fwidth of the sample coordinates measures exactly that,
    // in every direction at once, so a roof seen square from across the parcel keeps its waves far past where a metres-from-camera fade had been killing them, while a floor seen edge-on loses
    // them the moment its grazing angle stretches the footprint past what the pixels can resolve. Distance conflated those two cases; the footprint tells them apart. The same derivative spikes
    // wherever the input underneath it jumps - the one-pixel seams between drainage cells scrolling the pattern differently, the fringe where a depth edge folds the position - and fades the
    // ripple there too, which quietly takes the worst of the sampling garbage those seams were already producing.
    float texels_per_pixel = max(fwidth(flow_uv.x), fwidth(flow_uv.y)) * float(textureSize(ssWaveMap, 0).x);
    float wave_reach = 1.0 - smoothstep(8.0, 48.0, texels_per_pixel);

    // ...and gated by slope, but on a far wider band than the flatten term uses. The flow field is indexed by XY alone, so a wall shares the drainage cell of the ground at its foot and was being
    // animated with the floor's ripple - dancing walls - and the wall still has to stay still. But handing this the flatten's pooling taper was answering the wrong question: pooling is about
    // water standing still, and a stream on a pitched roof waves hardest exactly where the roof is too steep to pool at all. This gate only has to tell a wall from a surface water runs along, so
    // everything flatter than about forty five degrees passes at full strength and the gate closes only as the surface approaches vertical.
    float flow_slope = smoothstep(0.25, 0.70, up_align);
    float flow_vis = flow.z * wet_for_flow * ssWetFlowStrength * wave_reach * flow_slope;
    if (flow_vis > 0.004)
    {
        // A fixed world-XY tangent frame - the same choice the water plane itself makes for this texture, and for the same reason: water is flat, so world X and Y already are its tangent and
        // bitangent, and nothing about which way it happens to be flowing needs to turn that frame. Building it from the flow direction instead, tried first, seemed like the more careful thing to do
        // until it went to a diagonal run: flow is one of eight discrete directions, one per drainage cell, and a tangent frame that spins with it reinterprets the very same sampled ripple texel
        // completely differently in two neighbouring cells that disagree - which reads as broken seams, worst exactly on the diagonals where neighbours disagree most often. The rotate dial still
        // turns this frame, just once, the same way for every fragment, so it stays free to correct the texture's own orientation without ever depending on flow.
        vec3 t = vec3(ssWetFlowRotCos, ssWetFlowRotSin, 0.0);
        vec3 b = vec3(-ssWetFlowRotSin, ssWetFlowRotCos, 0.0);

        // Only the sample position moves with the flow direction - sliding the same fixed pattern along it is what reads as the water actually running that way, diagonals included, without the
        // pattern itself ever having to turn. flow_uv was computed above the reach so its derivatives could be read there; this is the same coordinates, consumed.
        vec3 ripple = texture(ssWaveMap, flow_uv).xyz * 2.0 - 1.0;
        vec3 flowed = normalize(t * ripple.x + b * ripple.y + flat_world * ripple.z);

        flat_world = normalize(mix(flat_world, flowed, clamp(flow_vis, 0.0, 1.0)));
    }

    // Back to view space the same way the exposure march's normal input got to world space in the first place, undone: ssFieldInvView's rotational part is orthonormal, so its transpose is its
    // inverse and there is no need for a second matrix upload just to run the transform backward.
    vec3 flat_view = normalize(transpose(mat3(ssFieldInvView)) * flat_world);

    frag_color = encodeNormal(flat_view, env, flag);
}

