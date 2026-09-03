/**
 * @file class1/deferred/ssSurfaceWetF.glsl
 * @brief Atmo Magic wet surfaces. A screen space pass over the gbuffer that
 *        tightens the specular lobe of anything the weather has been falling
 *        on, and touches nothing else.
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

// <SS:Nexii> Atmo Magic wet surfaces

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D specularRect;

// Agent space from view space. The field is anchored to the world; everything the gbuffer hands back is relative to the eye.
uniform mat4 ssFieldInvView;

// The field lattice, for the cell size. Declared here as well as in ssSurfaceFieldF.glsl - same program, same uniform, one value.
uniform vec4 ssFieldOrigin;

// Cosines of the angles from level at which standing water stops being possible. Shared verbatim with the normal-flatten pass, which asks the same question about the same fragment and must not
// answer differently.
uniform float ssWetFlattenCosFull;
uniform float ssWetFlattenCosZero;

// Master scale on the whole effect, so a user who wants none of it pays for none of it and one who wants it subtle can have that instead
uniform float ssWetStrength;

// What a soaked surface's roughness is multiplied by, and the floor it may never go below. The floor is not cosmetic: a perfect mirror in the gbuffer is what the screen space reflections fall apart
// on.
uniform float ssWetRoughness;
uniform float ssWetRoughMin;

// Legacy materials carry glossiness rather than roughness, and a great deal of older content sits at zero - multiplying that stays at zero forever, so the wet value converges on a target instead of
// scaling what is already there.
uniform float ssWetGlossTarget;

// The specular colour a water film puts on a legacy surface that had none. Written in the same sRGB-ish encoding the legacy gbuffer uses, so a quarter here lands near the 0.04 a dielectric reflects
// once soften linearises it.
uniform float ssWetSpecular;

// A lower target for legacy surfaces that carried no baked specular data at all - terrain always writes exactly zero here, by construction, and a great deal of plain, matte content does too. A
// surface with nothing shiny about it to begin with is unlikely to turn glossy under rain the way an already-finished one does, and on terrain specifically the normal is coherent over a large area
// with none of the micro-detail that breaks a tight highlight into many small glints instead of one sliding blob - so it wants noticeably less specular energy than ordinary content, not just a
// slightly lower amount of the same thing.
uniform float ssWetSpecularMatte;

// 0 by day, 1 at night. A full puddle is a near-mirror, and a mirror under a
// moonless zenith reflects almost nothing - full puddle patches read as pitch
// black holes in the ground after dark (observed: soft mask-shaped blobs that
// only the whiteout's veil covered). The night factor pulls the puddle
// treatment back toward damp so the patches stay readable.
uniform float ssWetNight;

// Standing water is not the same thing as a damp surface, and reusing the wetness dials for it would have made the two impossible to tune apart: a puddle is a pool of actual water sitting on top of
// the material rather than a film soaked into it, so it wants to read as close to a mirror as this pass can make it, independent of how shiny the material underneath would ever get from being merely
// rained on. Driven off the drainage's own standing-depth channel rather than a second wetness figure, because a puddle is a place water collects and stays, which is exactly what that channel
// already tracks.
uniform float ssWetPuddleDepthFull;   // standing depth, metres, that reads as a full puddle
uniform float ssWetPuddleRoughness;   // PBR roughness multiplier at full puddle
uniform float ssWetPuddleRoughMin;    // PBR roughness floor at full puddle
uniform float ssWetPuddleSpecular;    // legacy specular colour target at full puddle
uniform float ssWetPuddleGloss;       // legacy gloss target at full puddle

// The fine shoreline. The field's cell lattice can only say "a pool lives around here" at metre resolution; the actual pool EDGE is carved per fragment from this value-noise lattice - identical
// maths to the CPU's ssPuddleMaskNoise, anchored at the camera region's origin so both sides (and the footstep query) walk one pattern. The soft threshold in main() is the shore band.
uniform float ssPuddleMaskAmt;      // 0 disables the carve, 1 full
uniform float ssPuddleMaskScaleM;   // lattice pitch, metres
uniform vec2 ssPuddleMaskAnchor;    // agent-space origin of the lattice

// The lattice hash, bit-for-bit the CPU's: uint(int) wraps two's complement exactly like the C cast does.
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

// Diagnostic. Above zero this is used as the wetness for every fragment the pass reaches, ignoring the field, the exposure and the shelter march entirely. It answers one question and only one: does
// anything this pass writes reach the screen. Everything else here is downstream of that.
uniform float ssWetDebugForce;

// Diagnostic. Above zero, samples the REAL field texture for wetness at this fragment's own position, but skips the exposure march entirely (treats every fragment as fully exposed). Isolates the
// field lookup and coordinate math from the shelter/overhang logic - the one piece of this shader that has never been independently verified, everything else having been proven tonight one layer at
// a time.
uniform float ssWetSkipExposure;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNormRaw(vec2 screenpos);
vec4 decodeNormal(vec4 norm);
vec4 ssFieldAt(vec3 p_agent, vec3 n_agent);
vec4 ssFieldFetch(vec2 xy_agent);

//-----------------------------------------------------------------------------
// Avatars The field cannot answer for them - it is a 2D thing describing the top of each column, and a person is something standing IN a column - so they carry their own wetness, tested here as a
// capsule per avatar. See ssavatarwet.h.
//-----------------------------------------------------------------------------

#define SS_AVATAR_MAX 8

// How far above the stored surface a fragment has to stand before it counts as something other than the surface itself. The field's own heights are cell averages, so a real floor can sit a little
// either side of one.
const float SS_AVATAR_LIFT = 0.15;

// Knee height, as a fraction of the body. Below this the capsule and the ground want the SAME answer - wet feet and a wet floor - so there is nothing to disambiguate and the two are simply blended.
// Above it, a fragment inside the capsule is a body and the floor's answer would be wrong, so the exclusion has to be exact. Splitting the problem at the knee means the accurate test only has to
// work where it can: well clear of the ground it keeps being confused by.
const float SS_AVATAR_KNEE = 0.28;

// How much sooner the windward side of a body wets than the lee. Rain arrives along a direction, not from everywhere. A body standing in a slanting downpour is soaked on the side facing it and stays
// drier behind, which is most of what makes wind visible on a person.
const float SS_AVATAR_LEE = 0.55;

uniform vec3 ssRainDir;     // unit, the way the rain is travelling
uniform int ssAvatarCount;
uniform vec4 ssAvatarPos[SS_AVATAR_MAX];    // xyz foot position, w radius
uniform vec4 ssAvatarShape[SS_AVATAR_MAX];  // x height, y soak

// How wet a point at height fraction t up a body is, given how soaked that body is overall. Rain arrives from above, so the head and shoulders go first. The feet are close behind them but for the
// opposite reason - they are being splashed from the ground rather than rained on - and the middle of the body is last, which is why a light shower reads as damp hair and wet boots on an otherwise
// dry person while a downpour eventually soaks all of them. Each height has a soak threshold it starts wetting at; the two curves below are "distance from the head" and "distance from the feet", and
// a band takes whichever of the two reaches it first.
float ssAvatarWetAt(float t, float soak)
{
    // Every threshold has to be reachable, which these did not used to be. Soak runs 0 to 1, and the old curves peaked at 1.15 - so the middle of a body needed more soak than exists and could never
    // wet at all, however long someone stood in a downpour. Only the two ends came within range, and the band between them switched on all at once at whatever soak finally crossed it, which is the
    // hard edge with no gradient. Now the far end of both curves is 0.72: the last part of a body to wet does so around three quarters soaked, and is fully wet before soak reaches 1.
    float from_head = mix(0.72, 0.02, smoothstep(0.25, 1.0, t));
    float from_feet = mix(0.12, 0.72, smoothstep(0.0, 0.40, t));
    float threshold = min(from_head, from_feet);

    // The band wets over a range rather than switching on at its threshold, so the boundary between wet and dry is a gradient up the body instead of a waterline.
    return smoothstep(threshold, threshold + 0.30, soak);
}

// Wetness from whichever avatar capsule contains this fragment, 0 if none. Returns the wetness in x, how far up the body it was found in y, and CONTAINMENT in z - how firmly any capsule holds
// this fragment regardless of how soaked that body is. Containment is deliberately independent of wetness: a dry body standing in a puddle still needs the puddle kept off it, and gating the body
// test on soak was exactly what let the standing-water treatment frost a freshly-arrived avatar head to toe.
vec3 ssAvatarWet(vec3 p_agent, vec3 n)
{
    float best = 0.0;
    float best_t = 1.0;
    float inside = 0.0;

    for (int i = 0; i < SS_AVATAR_MAX; ++i)
    {
        if (i >= ssAvatarCount) break;

        vec3 foot = ssAvatarPos[i].xyz;
        float radius = ssAvatarPos[i].w;
        float height = ssAvatarShape[i].x;
        float soak = ssAvatarShape[i].y;

        // Generous slack under the soles and over the head. mBodySize is the shape's own measurement of a body, and what gets drawn is not only that: hair, hats, heels, a hovering avatar and
        // anything rigged past the skeleton all sit outside it. With only 15cm underneath and 25cm above, the capsule ended somewhere around the shoulders and started above the shoes - so the feet
        // and the head fell outside it entirely and a band across the legs was the only part that could wet. Hence knees, and nothing else.
        float rel = p_agent.z - foot.z;
        if (rel < -0.40 || rel > height * 1.15 + 0.50) continue;

        vec2 off = p_agent.xy - foot.xy;
        if (dot(off, off) > radius * radius) continue;

        float t = clamp(rel / max(height, 0.1), 0.0, 1.0);

        // Softened at the capsule's rim so the edge of the test is not a visible cylinder cut across a shoulder.
        float edge = 1.0 - smoothstep(radius * 0.75, radius, length(off));
        if (edge > inside)
        {
            inside = edge;
            best_t = t;
        }

        // Facing into the rain wets sooner - see SS_AVATAR_LEE. Applied to the soak rather than to the result, so the windward side crosses each band's threshold earlier instead of merely being
        // drawn stronger once it has.
        float into = clamp(dot(n, -ssRainDir), 0.0, 1.0);
        float aim = mix(SS_AVATAR_LEE, 1.0, into);

        float w = ssAvatarWetAt(t, soak * aim) * edge;
        best = max(best, w);
    }

    return vec3(best, best_t, inside);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 spec = texture(specularRect, tc);

    float depth = getDepth(tc);

    // decodeNormal() reconstructs xyz from the octahedral encoding but never assigns w - the flag channel comes along for the ride in the same texture but is not part of what that function decodes,
    // and reading it off its result is uninitialised GLSL output. The flag has to come from the raw fetch; only the normal itself goes through decodeNormal.
    vec4 raw = getNormRaw(tc);
    float flag = raw.w;
    vec4 norm = decodeNormal(raw);

    // Sky, stars, the sun disc, HDRI - none of them are surfaces and none of them have a specular response to spoil
    if (GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_HDRI) ||
        GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        frag_color = spec;
        return;
    }

    vec4 pos_view = getPositionWithDepth(tc, depth);
    vec3 p = (ssFieldInvView * vec4(pos_view.xyz, 1.0)).xyz;
    vec3 n = normalize(mat3(ssFieldInvView) * norm.xyz);

    float wet;
    float puddle;

    // Avatars first - but only where the field genuinely has nothing to say. The capsule is a screen-space pass's only way of asking "is this fragment a person": there is no per-object identity in a
    // G-buffer, so a world-space cylinder stands in for one. It cannot tell a shin from the floor between two feet, and on its own it claimed both - handing the ground the avatar's soak and zeroing
    // its puddle. That is the dry island that followed people around. The field already draws the line the capsule cannot: ssFieldAt rejects anything standing ABOVE the stored surface height, which
    // is exactly what an avatar's body is and exactly what the floor is not. Pairing the two makes them complementary rather than overlapping - the capsule answers only for fragments the field has
    // declined, and the ground under someone stays as wet as the ground beside them.
    vec4 field_here = ssFieldFetch(p.xy);
    bool field_knows = field_here.x > -1.0e5;
    bool on_surface = field_knows && (p.z <= field_here.x + SS_AVATAR_LIFT);

    vec3 av = ssAvatarWet(p, n);
    float avatar_wet = av.x;
    bool in_body = av.z > 0.05;    // inside SOMEONE's capsule, however dry they are - see ssAvatarWet

    // How much of this fragment is still "the ground someone is standing on" rather than "someone": 1 at the soles, easing to 0 by the knee. A ramp, not a threshold. The two things it feeds -
    // whether the floor is allowed to answer, and whether standing water is drawn - both look wrong switched. Cut at the knee, a person on a wet floor wears the floor's puddle up their shins and it
    // stops dead in a line across them. Faded, the standing water thins out of the shoe and is gone by the leg, which is what happens to water on a boot.
    float ground_share = 1.0 - smoothstep(0.04, SS_AVATAR_KNEE, av.y);

    // Above the knee, a fragment the field still claims as its own surface is a floor the capsule happens to pass through - a mezzanine, a table - and the floor's answer is the right one. Below the
    // knee the test is not asked, because down there the two answers agree anyway. Containment, NOT avatar_wet: a body counts as a body while it is still dry, or the field's answer for the ground
    // cell lands on the whole silhouette until the first moment soak registers - and the wet fold below already returns the right answer for a dry body (nothing) through body_wet being zero.
    bool avatar_here = in_body && (ground_share > 0.5 || !on_surface);

    if (ssWetDebugForce > 0.0)
    {
        wet = ssWetDebugForce;
        puddle = ssWetDebugForce;
    }
    else if (ssWetSkipExposure > 0.0)
    {
        vec4 raw = ssFieldFetch(p.xy);
        if (raw.x < -1.0e5)
        {
            // Outside the stitched window entirely - unless somebody is standing here, who is not the window's business anyway.
            if (!avatar_here) { frag_color = spec; return; }
            raw = vec4(0.0);
        }
        wet = raw.y * ssWetStrength;
        puddle = clamp(raw.w / ssWetPuddleDepthFull, 0.0, 1.0);
        if (!avatar_here && wet < 0.004 && puddle < 0.004)
        {
            frag_color = spec;
            return;
        }
    }
    else
    {
        vec4 field = ssFieldAt(p, n);

        // Outside the window, or nothing has fallen here yet. Either way the surface is left exactly as the material author wrote it.
        wet = field.x * field.w * ssWetStrength;

        // Standing depth is not scaled by exposure the way wet is: a puddle under an eave was filled by water that ran there, not by rain falling on that exact spot, so whether the sky above it is
        // open right now says nothing about whether it is still full.
        puddle = clamp(field.z / ssWetPuddleDepthFull, 0.0, 1.0);
        if (!avatar_here && (field.w < 0.0 || (wet < 0.004 && puddle < 0.004)))
        {
            frag_color = spec;
            return;
        }
        if (field.w < 0.0) { wet = 0.0; puddle = 0.0; }
    }

    // The avatar folded in rather than substituted. Taking the larger of the two keeps the floor's own wetness and its puddles intact under someone's feet - the dry island was this branch
    // overwriting them with a body's soak - while a body still wets on a dry floor, because there the field has nothing to beat.
    if (avatar_here)
    {
        // Weighted by the same ramp, and NOT a plain max. Taking the larger of the two everywhere was right at the soles and wrong above them. On a body the field is still answering about the ground
        // underneath - and in the skip-exposure path it answers from a raw fetch with no surface test at all - so a soaked floor handed its own wetness to everyone standing on it and painted them
        // uniformly drenched, erasing the top-down gradient and the windward side along with it. At the soles the larger still wins, because down there the fragment may genuinely be the floor. By
        // the knee only the body's own answer remains, and the floor cannot reach up it.
        float body_wet = avatar_wet * ssWetStrength;
        wet = mix(body_wet, max(wet, body_wet), ground_share);

    }

    // Water stands on a floor, not on a person - so the standing-water term fades out up the body: at the soles it is whatever the floor has, because down there the fragment may well BE the
    // floor; by the knee it is gone. Keyed to CONTAINMENT, not to avatar_here: avatar_here needs the body to be measurably wet, and a dry body standing in a puddle was failing it - which handed
    // the raw field's standing depth to their whole silhouette and frosted them head to toe.
    if (in_body)
    {
        puddle *= ground_share;
    }

    // Standing water lies flat, so it cannot climb. The field is a heightfield indexed by XY alone: a wall shares the cell of the ground at its foot, and the puddle in that cell was being handed to
    // every fragment of the wall within the lookup's height tolerance. What made it read as a glitch rather than as too much water is that the tolerance is a hard cut against a bilinear height - so
    // the band ended in a straight line running up the wall wherever the neighbouring cell stored a different ground height. A seam, at a cell boundary, on a surface that should never have had
    // standing water on it at all. Asked of the GEOMETRIC normal, from the screen-space derivatives of view position, not the gbuffer normal - same reasoning as the normal flatten pass, which
    // already does this: whether a surface can hold a pool is a question about the geometry, and a bump map would otherwise have a wall pooling in whichever pixels its brickwork happened to tilt
    // upward. The two passes now agree about which fragments are level, because they compute it the same way from the same uniforms.
    vec3 n_geo_view = cross(dFdx(pos_view.xyz), dFdy(pos_view.xyz));
    if (dot(n_geo_view, -pos_view.xyz) < 0.0) n_geo_view = -n_geo_view;
    vec3 n_geo_world = normalize(mat3(ssFieldInvView) * n_geo_view);
    float level = smoothstep(ssWetFlattenCosZero, ssWetFlattenCosFull,
                             dot(n_geo_world, vec3(0.0, 0.0, 1.0)));

    // ...and it lies ON the surface, not somewhere in the column above it. The height tolerance in ssFieldAt has to be generous - a kerb or the crown of a road genuinely sits above the cell average
    // that stored it - but generous and hard-edged is what draws the line. Fading the last part of the range costs nothing and leaves no edge to see: a level surface at the stored height is
    // unaffected, and one drifting up out of its column loses its puddle gradually instead of at a line.
    // In METRES, clamped, not in cells: a kerb or a road crown sits a few tens of centimetres above its cell's average and deserves its puddle, but standing water half a metre up is not standing
    // any more. Scaled purely by the cell size this fade reached metres above the ground, which is how the puddle climbed people's bodies and any raised levelish facet near one.
    float cell = ssFieldOrigin.z;
    level *= 1.0 - smoothstep(min(cell * 0.5, 0.20), min(cell * 1.5, 0.45), p.z - field_here.x);

    // Not applied under the debug override, whose whole job is to soak every fragment the pass reaches regardless of what anything thinks.
    if (ssWetDebugForce <= 0.0)
    {
        puddle *= field_knows ? level : 0.0;

        // The shoreline itself, carved at fragment resolution: the smoothstep is the shore band - full water inside, a shallow thinning rim across it, dry ground beyond - which is the actual
        // pool edge the metre-grid cells can only gesture at. Same lattice the CPU envelope and the footstep query evaluate, so all three agree where the water stops.
        if (ssPuddleMaskAmt > 0.0 && puddle > 0.004)
        {
            float mask_v = ssPuddleMaskNoise(p.xy - ssPuddleMaskAnchor);
            float shore = smoothstep(0.47, 0.56, mask_v);
            puddle *= mix(1.0, shore, ssPuddleMaskAmt);
        }
    }

    // What actually drives the blend toward the wet/puddle look below - a spot can be a full puddle while the general wetness pass call for this frame is low (just after the rain stopped, say), and
    // it should still shine like standing water rather than fade with the film around it. The puddle's own weight yields at night - see ssWetNight.
    float puddle_night = puddle * mix(1.0, 0.3, clamp(ssWetNight, 0.0, 1.0));
    float wetBlend = max(wet, puddle_night);

    if (GET_GBUFFER_FLAG(flag, GBUFFER_FLAG_HAS_PBR))
    {
        // Occlusion, roughness, metal. Only the middle one moves. Water fills the micro-relief a rough surface scatters its highlight over, so the same specular energy comes back in a tighter lobe:
        // a brighter, sharper highlight and a sharper reflection, with nothing added to the light budget. That is the whole reason this is the one channel worth touching - it reads as wet without
        // overwriting a single thing the creator authored.
        float rough_mul = mix(ssWetRoughness, ssWetPuddleRoughness, puddle);
        float rough_min = mix(ssWetRoughMin, ssWetPuddleRoughMin, puddle);
        spec.g = max(mix(spec.g, spec.g * rough_mul, wetBlend), rough_min);
    }
    else
    {
        // The legacy gbuffer packs specular colour in rgb and a Blinn-Phong exponent in alpha, and gloss runs the opposite way to roughness - the same tightening is an increase here, not a decrease.
        // The colour has to move as well, and that is not optional. Every legacy path that spends the exponent multiplies by the colour first - the sun highlight does, and so does the gloss
        // environment - and a plain prim carries black there. Raising the exponent alone opens the gate and then multiplies by nothing, which is a great deal of work for no pixels. A water film is a
        // weak neutral dielectric laid over whatever was underneath, so the wet end of this is a floor rather than a cap: it puts a specular on a surface that had none and never takes one away from
        // a surface whose author gave it one. But wet has to be the blend factor toward that floor, not baked into the floor itself - max(spec.rgb, ssWetSpecular*wet) makes the floor rise WITH wet,
        // which means whether the transition looks smooth or like a switch depends on how bright the surface already was relative to that rising floor, different for every piece of content. Blending
        // toward a fixed target the way the gloss line already does makes the ramp wet-driven and predictable regardless of what the surface started at. Terrain, and plain content like it, carries
        // no baked specular at all to begin with - checked once, before wet moves anything, so the choice of target does not itself depend on what wet already did to spec this frame.
        bool matte = max(max(spec.r, spec.g), max(spec.b, spec.a)) < 0.02;
        float target = matte ? ssWetSpecularMatte : ssWetSpecular;
        target = mix(target, ssWetPuddleSpecular, puddle);
        float gloss_target = mix(ssWetGlossTarget, ssWetPuddleGloss, puddle);

        spec.rgb = mix(spec.rgb, max(spec.rgb, vec3(target)), wetBlend);
        spec.a = mix(spec.a, max(spec.a, gloss_target), wetBlend);
    }

    frag_color = spec;
}

