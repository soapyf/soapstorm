/**
 * @file class1/deferred/ssSurfaceNormalF.glsl
 * @brief Atmo Magic wet surfaces: normal flattening. A water film smooths out
 *        the micro-relief a rough surface scatters its highlight over, and
 *        the visible result of that is a flatter normal, not only a tighter
 *        specular lobe on the original bumpy one. This is the second half of
 *        the wet look; ssSurfaceWetF.glsl is the first. Also carries the
 *        surface-weather material detail the metre field cannot: static
 *        drops, drips, sheet flow, ice crack relief, the deposit layer's
 *        relief and grain, and puddle impact rings.
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

// <SS:Nexii> Atmo Magic wet surfaces - normal flattening, drops/drips/sheet, ice, deposit relief, rings

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

// <SS:Nexii> AUDIT (finding 4): this is its own GLSL compilation unit like every other pass file - ssSurfaceStateF.glsl declaring ssTime, ssSurfaceIntensity etc. does not make them visible here; they, the ring buffer uniforms bindRingsForShader uploads, and the LOCKSTEP consts this pass's ring/wind-carry code reads all need their own declaration below.
uniform float ssTime;               // gFrameTimeSeconds
uniform float ssSurfaceIntensity;   // liquid precipitation intensity 0..1
uniform float ssSurfaceDrops;       // SSAtmoSurfaceDrops
uniform float ssSurfaceDropScale;   // SSAtmoSurfaceDropScale
uniform float ssSurfaceIceOn;       // SSAtmoSurfaceIce, 0/1
uniform vec3  ssSurfaceWind;        // SSWindFlowMap sample at the camera, agent space m/s
uniform vec4  ssDepositLook2;       // x translucency, y depthFull (metres), z wash, w melts
uniform sampler2D diffuseRect;

// The live impact ring buffer (doc sec 6) - SSSurfaceField::bindRingsForShader uploads these three by name every frame the wet pass draws. SS_RING_ARRAY_MAX must match SSSurfaceState::RING_MAX (24).
#define SS_RING_ARRAY_MAX 24
uniform int   ssRingCount;
uniform vec4  ssRings[SS_RING_ARRAY_MAX];   // xy position, z birth time, w strength
uniform float ssRingZ[SS_RING_ARRAY_MAX];
// How fast the ring's own clock runs against the wall clock (SSSurfaceState::ringRate, uploaded beside the buffer): the analytic ring and the landing ripple quad it stands in for expand at the same metres per second off the same taste controls, and the ring's larger reach is paid for in time rather than in speed.
uniform float ssRingRate;

// LOCKSTEP with sssurfacestatecore.h via ssSurfaceStateF.glsl - re-declared here, same names/values, for the ring loop below.
const float SS_RING_SPEED_MPS = 0.55;
const float SS_RING_LIFE_S    = 2.0;
const float SS_RING_SLOPE_MAX = 0.6;

// Wind-blown snow settling in the lee (doc section 8) - not LOCKSTEP with the core, a shader-side art constant re-declared per unit like the rest.
const float SS_DRIFT_CARRY = 0.8;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);
vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);
vec4 ssFieldAt(vec3 p_agent, vec3 n_agent);
vec4 ssFieldFetch(vec2 xy_agent);
vec3 ssFieldFetchFlow(vec2 xy_agent);
float ssFieldHash(vec2 p);

// <SS:Nexii> AUDIT (finding 4): prototypes of ssSurfaceStateF.glsl's functions - defined there, resolved at link, same idiom as ssFieldFetch etc. above.
vec4 ssFieldFetchState(vec2 xy_agent);
float ssPorosityFromAlbedo(vec2 tc, vec4 albedo);
vec4 ssDropLaw(float i);
float ssSheetLaw(float i);
float ssDepositCoverage(float depth, float depthFull);
float ssDepositSoften(float depth, float depthFull);
float ssRingSlope(float r, float t, float strength);
float ssWorldNoise(vec2 p, float pitchM);
float ssDepositMask(float coverage, float up, vec3 p);
float ssFieldWindCarry(vec2 xy_agent);

// <SS:Nexii> Avatars. The field cannot refuse a body standing in a column: ssFieldAt's height rejection sits at stored + 1.5 cells - 3 m at the window's 2 m cell, however fine the capture texel is - which no body reaches, so a fragment on a person inherits the pavement's wetness and this pass stamped its drop/drip lattices on them (observed: the whole avatar domed over). The capsules are the only "is this a person" answer a screen-space pass has - same stand-in ssSurfaceWetF.glsl's ssAvatarWet uses (ssavatarwet.h), containment only, because nothing this pass draws on a body is a blend: every term is the ground's weather painted on someone, so a body early-returns. The on-surface carve keeps the floor's own drops between a person's feet - the wet pass's dry-island lesson.
#define SS_AVATAR_MAX 8
uniform int ssAvatarCount;
uniform vec4 ssAvatarPos[SS_AVATAR_MAX];    // xyz foot position, w radius
uniform vec4 ssAvatarShape[SS_AVATAR_MAX];  // x height, y soak (unused here; LOCKSTEP with the wet pass's layout)

// How far above the stored surface a fragment stops being the floor and starts being something in the column. Same figure as the wet pass's SS_AVATAR_LIFT.
const float SS_BODY_LIFT = 0.15;

float ssAvatarContain(vec3 p_agent)
{
    float inside = 0.0;
    for (int i = 0; i < SS_AVATAR_MAX; ++i)
    {
        if (i >= ssAvatarCount) break;
        vec3 foot = ssAvatarPos[i].xyz;
        float radius = ssAvatarPos[i].w;
        float height = ssAvatarShape[i].x;
        float rel = p_agent.z - foot.z;
        if (rel < -0.40 || rel > height * 1.15 + 0.50) continue;
        vec2 off = p_agent.xy - foot.xy;
        if (dot(off, off) > radius * radius) continue;
        inside = max(inside, 1.0 - smoothstep(radius * 0.75, radius, length(off)));
    }
    return inside;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    // The env intensity and gbuffer flag live in the same texel as the encoded normal but are not part of what decodeNormal reconstructs - exactly the trap ssSurfaceWetF.glsl's own flag read fell
    // into earlier. Both have to come from the raw fetch and go back out unchanged; only the normal itself is ever supposed to move.
    vec4 raw = getNormRaw(tc);
    float flag = raw.w;
    float env = raw.z;

    // <SS:Nexii> depth/position and the GEOMETRIC normal moved to the very top of main, ahead of every early return below (sky, debug force, skip-exposure, out-of-window) - they used to sit after those branches, which is where the existing flatten code needed them, but this pass now also has to take fwidth() of several new world-XY lattice coordinates (drop/drip/grain footprints) built from p and n_geo_world, and a derivative call is only defined where every fragment in the quad is still live: true uniform flow means before any of this shader's own early returns, not merely before the branch that consumes the result (which is as far as the existing flow_uv/wave_reach pair below bothered, since nothing upstream of them could return early).
    float depth = getDepth(tc);
    vec4 pos_view = getPositionWithDepth(tc, depth);
    vec3 p = (ssFieldInvView * vec4(pos_view.xyz, 1.0)).xyz;

    // <SS:Nexii> the camera's own agent-space position, which is what ssFieldInvView's translation column IS. Used by the drip frame below to measure its projected coordinate as (p - cam) . t + cam . t: the per-fragment term is then a difference of nearby numbers and the whole-lattice offset is one per-frame constant, so a rounding error in it shifts the lattice coherently instead of shearing it per fragment. See sssurfacedropcore.h's ANCHORING RULE for both failure modes and which one was measured to bite. [interaction: ssvolcloud.cpp's air frame, the same rule one system up]
    vec3 cam_agent = ssFieldInvView[3].xyz;

    // <SS:Nexii> the gbuffer's own shading normal, hoisted ahead of the derivative batch below (it is a texture read and some maths, under no derivative constraint) because the drip frame's axis is built from it - see the drip block. Nothing here can early-return before it, so it is as safe here as it was where it used to sit.
    vec3 n_view = decodeNormal(raw).xyz;
    vec3 n_world = normalize(mat3(ssFieldInvView) * n_view);

    // See ssSurfaceWetF.glsl's own copy of this construction for the full rationale: the gradient of view-space position across the screen is the flat GEOMETRIC surface, independent of any normal
    // map, which is what "is this a wall or a roof" actually has to answer.
    vec3 n_geo_view = cross(dFdx(pos_view.xyz), dFdy(pos_view.xyz));
    if (dot(n_geo_view, -pos_view.xyz) < 0.0) n_geo_view = -n_geo_view;
    vec3 n_geo_world = normalize(mat3(ssFieldInvView) * n_geo_view);
    float up_align = dot(n_geo_world, vec3(0.0, 0.0, 1.0));
    float slope_factor = smoothstep(ssWetFlattenCosZero, ssWetFlattenCosFull, up_align);

    // <SS:Nexii> [interaction: ssSurfaceStateF] the porosity estimate, the field state and the drop/sheet laws - all pure functions of tc/p/ssSurfaceIntensity, none gated by anything below, so they belong up here too. Roughness is already wet-modified by the time this pass runs (ssSurfaceWetF runs first in the pipeline order for this pass's own inputs), so the porosity estimate here is deliberately the roughness-FREE one (ssPorosityFromAlbedo), not ssPorosityAt.
    vec4 albedo = texture(diffuseRect, tc);
    float porosity = ssPorosityFromAlbedo(tc, albedo);
    vec4 state = ssFieldFetchState(p.xy);
    vec4 law = ssDropLaw(ssSurfaceIntensity);
    float sheet = ssSheetLaw(ssSurfaceIntensity);

    // Static-drop lattice: an isotropic world-XY grid (both axes divided by the same pitch), so a cell-fraction distance is already a metrically correct one and the cap law below can use it directly,
    // the same way the doc's own d = (p.xy - centre) / radius does.
    // <SS:Nexii> DELIBERATELY un-anchored, and the measurement is why. A camera-anchored spelling - sub-cell coordinate from (p.xy - anchor), world index recovered as an integer sum - is the standard cure for a lattice whose coordinate carries the agent-space magnitude, and it was built and measured here (sssurfacedropcore.h SSSurfaceDrop::anchoredCell, unit_surfaceanchor surfaceanchor_B_control_unanchored_quantum). It does not help this lattice: the anchor's own anchor_i * pitch multiply reintroduces the same magnitude rounding, measured 0.0052 cells of sub-cell error at 4 km against 0.00089 for the plain spelling, and BOTH are two orders below the depth buffer's own contribution (rung A: 3 mm at 12 m, 29 mm at 120 m). The static drop lattice is anchored to within a fiftieth of a cell as spelled - which is what the user sees, the static drops being the ones they report as fine. The core keeps anchoredCell so the next reader does not re-derive it.
    float drop_pitch = max(0.04 * law.y * ssSurfaceDropScale, 1.0e-4);
    vec2 drop_cell_uv = p.xy / drop_pitch;
    float drop_texels = max(fwidth(drop_cell_uv.x), fwidth(drop_cell_uv.y));
    float drop_reach = 1.0 - smoothstep(4.0, 24.0, drop_texels);

    // Drip frame: u along the wall's own horizontal tangent, v along world Z. The cell is anisotropic (0.06 m wide, 0.30 m tall), so this uv pair only ever drives cell INDEXING - the cap/trail maths
    // further down convert back to metres before measuring any distance in it.
    // <SS:Nexii> the axis is a QUANTISED azimuth of the GBUFFER normal (sssurfacedropcore.h, SSSurfaceDrop::dripAxis), not the screen-space derivative normal it used to come from. A projected
    // coordinate multiplies its axis's angular error by |p|, and the derivative normal carries 0.03 to 0.24 rad of it: measured, the lattice coordinate at a FIXED world point moved 4.06 m (68 cells)
    // for a wall 141 m from the agent origin and 161 m (2685 cells) at 1 km, re-randomising the whole pattern every time the camera moved. That is the jitter. n_geo_world stays where it belongs -
    // the up_align/slope gates below, which are smooth and only ask "is this a wall". [interaction: the eight-direction sheet flow frame further down, the same idiom]
    const float SS_DRIP_AZIMUTHS = 16.0;
    float drip_az_step = 6.28318530718 / SS_DRIP_AZIMUTHS;
    // cross(up, n) = (-n.y, n.x, 0). On a perfectly horizontal surface that vanishes and atan(0, 0) is UNDEFINED in GLSL, so it falls back to world X - the drip block is gated off there anyway, but
    // an undefined value computed ahead of the gate is not something to leave lying about.
    vec2 drip_tan_h = vec2(-n_world.y, n_world.x);
    if (dot(drip_tan_h, drip_tan_h) < 1.0e-12) drip_tan_h = vec2(1.0, 0.0);
    float drip_az = floor(atan(drip_tan_h.y, drip_tan_h.x) / drip_az_step + 0.5) * drip_az_step;
    vec3 drip_t = vec3(cos(drip_az), sin(drip_az), 0.0);

    // <SS:Nexii> camera-relative, and the wind lean is now a CONTINUOUS affine shear on absolute z. It used to be 0.02 * (p.z - floor(p.z)) - a sawtooth that jumped the lattice sideways by up to
    // dot(wind, t) * 0.02 m at every whole metre of height, which is a discontinuity straight across the one direction a runnel has to stay connected along.
    float drip_u = dot(p - cam_agent, drip_t) + dot(cam_agent, drip_t)
                 - dot(ssSurfaceWind.xy, drip_t.xy) * 0.02 * p.z;
    vec2 drip_cell_uv = vec2(drip_u / 0.06, p.z / 0.30);

    // <SS:Nexii> the drip's LOD fade measures the FEATURE, not the cell (PLAN.md lesson 33). The old fade ran on cells and started at 4 of them - but a drip's cap is 0.008 * dropScale metres and its
    // trail sigma 0.004 m, seven to fifteen times finer than the 0.06 x 0.30 m cell it was measured against, so the pattern was drawn at full strength while a pixel already covered several caps. That
    // is the speckle: a hard cap point-sampled far below Nyquist reads as isolated dots instead of a runnel. Measured in world metres per pixel, direction-agnostic, so a grazing wall (where a pixel's
    // footprint stretches) fades on the same rule as a face-on one.
    float drip_px_m = max(max(fwidth(p.x), fwidth(p.y)), fwidth(p.z));
    float drip_feature_m = max(0.008 * ssSurfaceDropScale, 1.0e-4);
    float drip_reach = 1.0 - smoothstep(0.5, 3.0, drip_px_m / drip_feature_m);

    // Deposit grain lattice (item 5 below): a 0.03 m pitch, faded the same way. Un-anchored for the same measured reason as the drop lattice above.
    vec2 grain_uv = p.xy / 0.03;
    float grain_texels = max(fwidth(grain_uv.x), fwidth(grain_uv.y));
    float grain_reach = 1.0 - smoothstep(4.0, 24.0, grain_texels);

    // <SS:Nexii> AUDIT (finding 7): the ripple's flow fetch and sample coordinates, hoisted up here beside the other three fwidth batches - ssFieldFetchFlow(p.xy) is a pure function of p, independent of which debug branch below fires, so there is no reason this one alone waited until after them. wave_reach itself (which needs wet/puddle, only known after the branches) is still computed at its original spot further down, from this hoisted texels_per_pixel.
    vec3 flow = ssFieldFetchFlow(p.xy);
    vec2 flow_uv = (p.xy - flow.xy * (ssTime * ssWetFlowSpeed)) / ssWetFlowScale;
    float texels_per_pixel = max(fwidth(flow_uv.x), fwidth(flow_uv.y)) * float(textureSize(ssWaveMap, 0).x);

    // Sky, stars, the sun disc, HDRI - none of them are surfaces with a normal to flatten
    if (GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_HDRI) ||
        GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        frag_color = raw;
        return;
    }

    // <SS:Nexii> A body the pass has nothing to say about - flatten, drops, drips, sheet, ice, grain and rings would all be the ground's weather painted on someone; the body's own wetness model lives in the wet pass (ssavatarwet.h). Inside a capsule but still at the stored surface keeps the floor's answer - the fragment may be the ground between a person's feet. Sits after every derivative batch above, so the quads this return splits are already safe.
    bool on_stored_surface = ssFieldFetch(p.xy).x > p.z - SS_BODY_LIFT;
    if (ssAvatarContain(p) > 0.05 && !on_stored_surface)
    {
        frag_color = raw;
        return;
    }

    float wet;
    float puddle;

    // <SS:Nexii> a single `field` shape every branch below fills in, so the deposit/wind-carry code (item 5, and the drop/drip gates that read its mask) has something sane to read regardless of which debug toggle is active: x wet, y deposit depth (metres), z puddle depth (metres, unused past this point - `puddle` above is already the resolved 0..1 figure), w exposure 0..1.
    vec4 field;
    if (ssWetDebugForce > 0.0)
    {
        wet = ssWetDebugForce;
        puddle = ssWetDebugForce;
        // <SS:Nexii> TODO(core)? no deposit read under the debug-force toggle - it overrides wet/puddle uniformly and was never meant to fake a deposit too; reads as bare ground, fully exposed.
        field = vec4(wet, 0.0, 0.0, 1.0);
    }
    else if (ssWetSkipExposure > 0.0)
    {
        vec4 fieldRaw = ssFieldFetch(p.xy);
        if (fieldRaw.x < -1.0e5)
        {
            frag_color = raw;
            return;
        }
        wet = fieldRaw.y * ssWetStrength;
        puddle = clamp(fieldRaw.w / ssWetPuddleDepthFull, 0.0, 1.0);
        if (wet < 0.004 && puddle < 0.004)
        {
            frag_color = raw;
            return;
        }
        // This toggle isolates the field lookup from the shelter march by treating every fragment as fully exposed - exactly what its own doc comment in ssSurfaceWetF.glsl says it does - so exposure
        // reads 1 here rather than running ssFieldAt just for this one figure.
        field = vec4(wet, fieldRaw.z, fieldRaw.w, 1.0);
    }
    else
    {
        field = ssFieldAt(p, n_world);
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
    // wetness, which is what turned every wet wall into a puddle standing on its side.
    // Blended toward world up rather than toward some notion of the surface's own unweathered flat direction, so a sloped wet roof still tilts its highlight the way a real film of water lying or
    // running on it would - the water's surface answers to gravity, not to whatever the material underneath happens to be shaped like.
    float flatten_wet = wet * ssWetNormalFlatten * slope_factor;
    float flatten_puddle = puddle * ssWetPuddleFlatten;
    float flatten = clamp(max(flatten_wet, flatten_puddle), 0.0, 1.0);
    vec3 flat_world = normalize(mix(n_world, vec3(0.0, 0.0, 1.0), flatten));

    // <SS:Nexii> [interaction: ssSurfaceStateF] the deposit mask, computed here rather than down in its own item-5 section below: items 1/2's drop and drip gates have to multiply by (1 - mask) (a
    // buried surface has no drops), so the mask value itself has to exist before those gates do even though its VISUAL application to flat_world (the softening + grain) still happens in the doc's
    // own item-5 place, after ice.
    float exposure_d = max(clamp(field.w, 0.0, 1.0), ssFieldWindCarry(p.xy) * SS_DRIFT_CARRY);
    float coverage = ssDepositCoverage(field.y, ssDepositLook2.y) * exposure_d;
    float deposit_mask = ssDepositMask(coverage, clamp(n_world.z, 0.0, 1.0), p);
    float deposit_soften = ssDepositSoften(field.y, ssDepositLook2.y) * deposit_mask;

    // Item 1: static drops on level, sealed, wet surfaces - a spherical-cap tilt at each occupied lattice cell.
    {
        float drop_gate = ssSurfaceDrops * law.x * wet * slope_factor * (1.0 - porosity) * (1.0 - state.x) * (1.0 - deposit_mask);
        if (drop_gate > 0.001 && drop_reach > 0.001)
        {
            // <SS:Nexii> DE-LATTICED (sssurfacedropcore.h rule 4, LOCKSTEP SSSurfaceDrop::sampleAt). One drop per cell, a radius band of only
            // 1.8x and an offset clamped to keep the cap inside its own cell is a stamped grid, not rain - and the clamp was itself the second-best
            // answer to a straight edge that fract() was cutting across every dome's base (offset to 0.85 plus radius to 0.45 reached 1.30, past
            // the cell line). Both go away together: the offset spans the whole cell, the fragment tests its own cell AND the eight around it so a
            // cap may cross a boundary and is never clipped, the radius band widens to 4.8x, and the occupancy is divided by SS_DROP_AREA_NORM so
            // the MEAN COVERED AREA is exactly what it was - the surface gets less regular, not wetter. Same three ingredients, same order, as the
            // deck's Tier B macro bodies. [interaction: ssdeckmacrocore.h bodyJitterM/BODY_AREA_NORM, the precedent]
            const float SS_DROP_R_LO = 0.12;
            const float SS_DROP_R_HI = 0.58;
            // meanRadiusSq(0.25, 0.45) / meanRadiusSq(0.12, 0.58), the ratio that holds the covered area fixed. LOCKSTEP SSSurfaceDrop::areaNorm.
            const float SS_DROP_AREA_NORM = 0.897955;

            vec2 dbase = floor(drop_cell_uv);
            vec2 dfrac = drop_cell_uv - dbase;
            float occ = clamp(drop_gate * SS_DROP_AREA_NORM, 0.0, 1.0);

            float best_h = 0.0;
            vec2 best_d = vec2(0.0);
            for (int oy = -1; oy <= 1; ++oy)
            {
                for (int ox = -1; ox <= 1; ++ox)
                {
                    vec2 dcell = dbase + vec2(float(ox), float(oy));
                    if (ssFieldHash(dcell) >= occ) continue;
                    vec2 doff = vec2(ssFieldHash(dcell + vec2(11.7, 3.1)), ssFieldHash(dcell + vec2(3.7, 17.9)));
                    float dradius = SS_DROP_R_LO + (SS_DROP_R_HI - SS_DROP_R_LO) * ssFieldHash(dcell + vec2(29.3, 7.7));
                    vec2 d2v = (dfrac - vec2(float(ox), float(oy)) - doff) / dradius;
                    float d2len2 = dot(d2v, d2v);
                    if (d2len2 >= 1.0) continue;
                    float h = sqrt(1.0 - d2len2 * 0.81);
                    if (h > best_h) { best_h = h; best_d = d2v; }
                }
            }

            if (best_h > 0.0)
            {
                vec3 cap = vec3(-best_d * 0.9, best_h);
                vec3 dropNormal = normalize(vec3(1.0, 0.0, 0.0) * cap.x + vec3(0.0, 1.0, 0.0) * cap.y + flat_world * cap.z);
                flat_world = normalize(mix(flat_world, dropNormal, drop_reach));
            }
        }
    }

    // Item 2: drips on verticals - a sliding drop with a Heartfelt stutter, plus its trailing ridge, in the (t, world-Z) frame.
    {
        float up_align_wall = (1.0 - smoothstep(0.25, 0.70, up_align)) * (1.0 - porosity * 0.7);
        float drip_gate = ssSurfaceDrops * law.z * wet * up_align_wall * (1.0 - deposit_mask);
        if (drip_gate > 0.001 && drip_reach > 0.001)
        {
            vec2 dcell = floor(drip_cell_uv);
            vec2 dlocal = fract(drip_cell_uv);
            float dhash = ssFieldHash(dcell);
            if (dhash < drip_gate)
            {
                float dxoff = 0.2 + 0.6 * ssFieldHash(dcell + vec2(5.3, 9.1));
                float dspeed = law.w * (0.15 + 0.35 * dhash);
                float dphase = ssFieldHash(dcell + vec2(13.1, 27.7)) * 6.28318530718;

                // The Heartfelt idiom: a drop slides down at dspeed, stuttering rather than gliding smoothly.
                float dy = fract(-ssTime * dspeed + dphase);
                dy -= 0.05 * sin(ssTime * 6.0 + dphase * 6.28318530718) * dhash;

                // The cap, in actual metres within the cell - the cell itself is anisotropic (0.06 m x 0.30 m), so the circular cap test has to leave that space before it can compare against a
                // physical radius.
                vec2 local_m = (dlocal - vec2(dxoff, dy)) * vec2(0.06, 0.30);
                float cap_r = 0.008 * ssSurfaceDropScale;
                vec2 dcap = local_m / cap_r;
                float dcaplen2 = dot(dcap, dcap);

                // <SS:Nexii> the cap's edge is a FOOTPRINT-WIDE band, not a hard cut. A cap of radius 0.008 * dropScale metres point-sampled by a pixel that already covers more than that is either
                // fully in or fully out per pixel, which is the speckle rather than a drop; a band the width of one pixel's own world footprint is the analytic filter for it.
                float aa_m = max(drip_px_m, 1.0e-5);
                float cap_edge = max(aa_m / cap_r, 0.02);
                float capMask = 1.0 - smoothstep(1.0 - cap_edge, 1.0 + cap_edge, sqrt(dcaplen2));

                vec3 dripNormal = flat_world;
                float dripWeight = 0.0;
                if (capMask > 0.001)
                {
                    vec3 cap = vec3(-dcap * 0.9, sqrt(max(1.0 - dcaplen2 * 0.81, 0.0)));
                    dripNormal = normalize(drip_t * cap.x + vec3(0.0, 0.0, 1.0) * cap.y + flat_world * cap.z);
                    dripWeight = capMask;
                }

                // The trail: a vertical ridge above the drop (toward larger v, the direction it fell from), a gaussian in u fading out over the trailing 60 percent of the cell.
                // <SS:Nexii> the gaussian's width is FLOORED at the pixel footprint and its amplitude divided by the same widening, so the integral across u is conserved. A 4 mm sigma sampled by a
                // pixel covering more than 4 mm of wall is exactly the "hard threshold on a fine field reads as isolated dots" failure - widening the kernel to the footprint is the filtered answer,
                // and the amplitude term is what keeps a distant runnel from getting brighter as it gets blurrier (the same area-conservation discipline as the deck's BODY_AREA_NORM).
                const float SS_DRIP_TRAIL_SIGMA_M = 0.004;
                float trail_sigma = max(SS_DRIP_TRAIL_SIGMA_M, aa_m);
                float dv_above = fract(dlocal.y - dy);
                float trail_fade = 1.0 - smoothstep(0.0, 0.6, dv_above);
                float du_m = (dlocal.x - dxoff) * 0.06;
                float trail_gauss = exp(-(du_m * du_m) / (2.0 * trail_sigma * trail_sigma))
                                    * (SS_DRIP_TRAIL_SIGMA_M / trail_sigma);
                float trailMask = trail_gauss * trail_fade;
                if (trailMask > dripWeight)
                {
                    float ridgeTiltU = -clamp(du_m / trail_sigma, -1.0, 1.0) * trailMask;
                    dripNormal = normalize(drip_t * ridgeTiltU * 0.6 + flat_world);
                    dripWeight = trailMask;
                }

                flat_world = normalize(mix(flat_world, dripNormal, drip_reach * dripWeight));
            }
        }
    }

    // Water actually running along a channel, laid over the flattened normal above rather than instead of it - a stream is still a flat film first, moving ripples second. A first few drops on a dry
    // gutter do not run, they cling - surface tension holds a thin film in place until enough has gathered to break free and move as a body, and a channel with any wetness on it at all showing full
    // flow the instant rain starts is exactly the "damp reads as a rushing stream" that skipping this would leave in. wet and puddle are the only per-cell figures that build up over time at all
    // here, so this is asking the same question of whichever of them is greater rather than of an actual depth a channel cell does not otherwise keep.
    // flow, flow_uv and texels_per_pixel are all computed at the top of main() now (audit finding 7 - taking the screen-space derivative of flow_uv has to run before any early return, and flow itself
    // is a pure function of p that never needed to wait for the branches below anyway). wet_for_flow is the one piece of this that genuinely cannot move: it needs wet/puddle, only known once the
    // branches resolve.
    float wet_for_flow = smoothstep(ssWetFlowMinWet, 1.0, max(wet, puddle));

    // Faded by what a pixel actually covers, not by how far away the surface sits. The ripple is centimetre-scale detail sampled by world position in a screen-space pass, and its failure mode in
    // the distance is moire - sheets of thin parallel lines - once the pattern's finest waves arrive at a couple of pixels per wavelength. fwidth of the sample coordinates measures exactly that,
    // in every direction at once, so a roof seen square from across the parcel keeps its waves far past where a metres-from-camera fade had been killing them, while a floor seen edge-on loses
    // them the moment its grazing angle stretches the footprint past what the pixels can resolve. Distance conflated those two cases; the footprint tells them apart. The same derivative spikes
    // wherever the input underneath it jumps - the one-pixel seams between drainage cells scrolling the pattern differently, the fringe where a depth edge folds the position - and fades the
    // ripple there too, which quietly takes the worst of the sampling garbage those seams were already producing.
    float wave_reach = 1.0 - smoothstep(8.0, 48.0, texels_per_pixel);

    // ...and gated by slope, but on a far wider band than the flatten term uses. The flow field is indexed by XY alone, so a wall shares the drainage cell of the ground at its foot and was being
    // animated with the floor's ripple - dancing walls - and the wall still has to stay still. But handing this the flatten's pooling taper was answering the wrong question: pooling is about
    // water standing still, and a stream on a pitched roof waves hardest exactly where the roof is too steep to pool at all. This gate only has to tell a wall from a surface water runs along, so
    // everything flatter than about forty five degrees passes at full strength and the gate closes only as the surface approaches vertical.
    float flow_slope = smoothstep(0.25, 0.70, up_align);
    float flow_vis = flow.z * wet_for_flow * ssWetFlowStrength * wave_reach * flow_slope;

    // Item 3: sheet flow at torrential intensity, on ANY sloped wet surface rather than only the drainage cells - a roof in a downpour has to read as a moving skin of water even where it is not
    // draining anywhere in particular. Reuses wave_reach rather than a second fwidth call: the ripple texture's own footprint is the same footprint whichever term drives its visibility.
    float flow_slope_any = smoothstep(0.08, 0.5, 1.0 - up_align);
    float sheet_vis = sheet * wet * ssWetFlowStrength * wave_reach * flow_slope_any;
    float vis = max(flow_vis, sheet_vis);

    // Item 4: ice freezes the flow/sheet motion outright (the puddle flatten above is left alone on purpose - ice stays flat).
    vis *= (1.0 - state.x * ssSurfaceIceOn);

    if (vis > 0.004)
    {
        // A fixed world-XY tangent frame - the same choice the water plane itself makes for this texture, and for the same reason: water is flat, so world X and Y already are its tangent and
        // bitangent, and nothing about which way it happens to be flowing needs to turn that frame. Building it from the flow direction instead, tried first, seemed like the more careful thing to do
        // until it went to a diagonal run: flow is one of eight discrete directions, one per drainage cell, and a tangent frame that spins with it reinterprets the very same sampled ripple texel
        // completely differently in two neighbouring cells that disagree - which reads as broken seams, worst exactly on the diagonals where neighbours disagree most often. The rotate dial still
        // turns this frame, just once, the same way for every fragment, so it stays free to correct the texture's own orientation without ever depending on flow.
        vec3 t = vec3(ssWetFlowRotCos, ssWetFlowRotSin, 0.0);
        vec3 b = vec3(-ssWetFlowRotSin, ssWetFlowRotCos, 0.0);

        // The sheet case scrolls down-slope rather than along a drainage direction that does not exist off the drainage cells; on an actual drainage cell (flow.z > 0) this is exactly flow.xy, so
        // the existing drainage look is unchanged - only the previously-silent sloped, non-drainage surfaces pick up a new scroll direction at all.
        vec2 scroll_dir = (flow.z > 0.0) ? flow.xy : normalize(vec2(n_geo_world.x, n_geo_world.y) + vec2(1.0e-6, 0.0));
        vec2 sheet_uv = (p.xy - scroll_dir * (ssTime * ssWetFlowSpeed)) / ssWetFlowScale;

        // Only the sample position moves with the flow direction - sliding the same fixed pattern along it is what reads as the water actually running that way, diagonals included, without the
        // pattern itself ever having to turn.
        vec3 ripple = texture(ssWaveMap, sheet_uv).xyz * 2.0 - 1.0;
        vec3 flowed = normalize(t * ripple.x + b * ripple.y + flat_world * ripple.z);

        flat_world = normalize(mix(flat_world, flowed, clamp(vis, 0.0, 1.0)));
    }

    // Item 4 (crack relief): a local 0.35 m cell-edge lattice - no shared crack function exists yet to call here (ssSurfaceAlbedoF.glsl, which the doc says draws the matching crack lines, is being
    // written in the same phase as this file) [interaction: ssSurfaceAlbedoF assumed to use the same 0.35 m pitch so the two read as one crack pattern].
    float ice = state.x * max(puddle, wet * 0.5) * ssSurfaceIceOn;
    if (ice > 0.001)
    {
        vec2 crack_uv = p.xy / 0.35;
        vec2 crack_frac = fract(crack_uv) - 0.5;
        vec2 dist_to_edge = 0.5 - abs(crack_frac);
        float edge_dist = min(dist_to_edge.x, dist_to_edge.y);
        float crack = 1.0 - smoothstep(0.0, 0.06, edge_dist);
        vec2 tilt_dir = (dist_to_edge.x < dist_to_edge.y) ? vec2(sign(crack_frac.x), 0.0) : vec2(0.0, sign(crack_frac.y));
        flat_world = normalize(flat_world + vec3(tilt_dir * crack * 0.15 * ice, 0.0));
    }

    // Item 5: the deposit layer rounds the relief under it and carries its own grain micro-normal - applied after the drops/drips above (which already gated on 1 - deposit_mask, since a buried
    // surface shows neither).
    flat_world = normalize(mix(flat_world, vec3(0.0, 0.0, 1.0), deposit_soften));
    {
        float g0 = ssWorldNoise(p.xy, 0.03);
        float gx = ssWorldNoise(p.xy + vec2(0.01, 0.0), 0.03);
        float gy = ssWorldNoise(p.xy + vec2(0.0, 0.01), 0.03);
        flat_world = normalize(flat_world + vec3(-(gx - g0), -(gy - g0), 0.0) * 6.0 * deposit_mask * grain_reach);
    }

    // Item 6: impact rings - the analytic ripple packet from each recorded impact within range, superposed and clamped once as a whole so a crowded puddle cannot shatter its own normal. The radius test is against the ring's REACH (speed x life on its own clock), which ssRingRate leaves alone: the rate decides how long the ring takes to get there, not how far it gets. [interaction: SSSurfaceField::bindRingsForShader uploads the rate, ssPrecipLitF.glsl shades the landing quad this ring stands in for]
    {
        vec3 tilt = vec3(0.0);
        const int SS_RING_LOOP_MAX = 24;
        for (int i = 0; i < SS_RING_LOOP_MAX; ++i)
        {
            if (i >= ssRingCount) break;
            float r = distance(p.xy, ssRings[i].xy);
            if (abs(p.z - ssRingZ[i]) < 0.35 && r < SS_RING_SPEED_MPS * SS_RING_LIFE_S + 0.2)
            {
                float rt = (ssTime - ssRings[i].z) * ssRingRate;
                float slope = ssRingSlope(r, rt, ssRings[i].w);
                vec2 dir = (p.xy - ssRings[i].xy) / max(r, 1.0e-3);
                tilt -= vec3(dir * slope, 0.0) * (puddle * slope_factor + 0.25 * wet * slope_factor) * (1.0 - state.x);
            }
        }
        vec2 tiltxy = tilt.xy;
        float tiltlen = length(tiltxy);
        if (tiltlen > SS_RING_SLOPE_MAX)
        {
            tiltxy *= SS_RING_SLOPE_MAX / tiltlen;
        }
        flat_world = normalize(flat_world + vec3(tiltxy, 0.0));
    }

    // Back to view space the same way the exposure march's normal input got to world space in the first place, undone: ssFieldInvView's rotational part is orthonormal, so its transpose is its
    // inverse and there is no need for a second matrix upload just to run the transform backward.
    vec3 flat_view = normalize(transpose(mat3(ssFieldInvView)) * flat_world);

    frag_color = encodeNormal(flat_view, env, flag);
}
