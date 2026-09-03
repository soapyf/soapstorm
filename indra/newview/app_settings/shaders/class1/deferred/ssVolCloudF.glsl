/**
 * @file ssVolCloudF.glsl
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

out vec4 frag_color;

uniform sampler2D diffuseMap;

in vec2 vary_texcoord0;

// <SS:Nexii> Per-puff STRUCTURE, not a finished colour any more: r the CPU builder's form term (facing and shade-through-the-deck, beam-flattened), g the buried depth - the fraction of the puff's own
// column standing above it, 0 at the lid and 1 at the floor, which the storm gloom is graded over below - and a the edge-fade alpha; b spare.
// The light it gets multiplied against arrives in the vary_ss_* varyings below - see the long note in ssVolCloudV.glsl.
in vec4 vary_color;

// <SS:Nexii> The deck's light, computed per corner in the vertex stage by the dome band's maths on the band's own slab ray (see the long notes in ssVolCloudV.glsl): the capped body sun and
// the ambient, both already carrying the authored cloud colour AND the atmosphere's transmittance along the ray - the atmosphere is INSIDE these, exactly as it is inside the band's, so
// there is no separate fog pass to disagree with it - and the forward-scatter glow at this ray's angle from the light, gated below by per-fragment thinness.
in vec3 vary_ss_sunlit;
in vec3 vary_ss_amblit;
in float vary_ss_glow;

// The airlight between eye and puff, kept out of the cloud terms so it never wears the cloud's gloom or wrap shading - see the vary_ss_airlight note in ssVolCloudV.glsl. Added below AFTER
// all cloud shaping, inside the band's clamp, which is exactly where cloudsF folds its own.
in vec3 vary_ss_airlight;

// The graze light's weight - the extra sun a vertex earns for standing high in the deck under a low sun (the alpenglow's lit lid; see the long note in ssVolCloudV.glsl). Spent below on a
// gentler wrap of its own (SS_GRAZE_DARK) past the body's, with the same sunlit colour and the same gloom - see the note where it joins.
in float vary_ss_top;

// The deck's storm gloom - what the weather's moisture says this deck is carrying, one number for the whole of it, so a uniform rather than a vertex channel. Graded over the buried depth where it is
// spent below rather than applied flat; see the note there for why flat was the same as not applying it at all.
uniform float ss_gloom;

// A COPY of the scene depth - see mDepthCopy. Named depthMap because that is one of LLShaderMgr's RESERVED uniform names, and only reserved names can be bound as textures. bindTexture takes an index
// into the shader's reserved-uniform table, while looking a custom name up returns a raw GL location - so binding "sceneDepth", as this was called, indexed that table with a number that meant
// nothing and left the sampler pointed at whatever was on texture unit 0. Which is the cloud noise map, read as depth. The sky dome's cloud map, as a second and third octave - see mDomeTexRef.
// Reserved name, because only reserved names can be bound as textures.
uniform sampler2D cloud_noise_texture;

uniform sampler2D depthMap;
uniform vec2 screen_res;

// <SS:Nexii> The base and detail maps' crossfade partners - bumpMap and specularMap are RESERVED names (see the depthMap note below: only reserved names can be bound as textures). Weights 0 with the partners pinned on the current maps whenever no fade runs.
uniform sampler2D bumpMap;
uniform sampler2D specularMap;
uniform float ss_base_blend;
uniform float ss_detail_blend;

// The beam gate - how much direct celestial light exists, 0..1. Gates every directional shading term; see the CPU note on mBeam.
uniform float ss_beam;

uniform vec2 ss_clip;           // near and far plane, for linearising depth
uniform float ss_soft_m;        // metres of fade; 0 disables it

uniform vec3 ss_light_dir;  // toward whatever is lighting the sky

// Point lights, not per-puff CPU colour: a discharge is LOCAL and puffs are spheres - the face toward it lights, the far face dims. Flat adds also got shaped by the sun's wrap term, wrongly.
#define SS_MAX_STRIKES 4
uniform vec4 ss_strike[SS_MAX_STRIKES];
uniform int ss_strike_count;

// What colour the discharge lights the deck. The sheath colour rather than the core's: what reaches a puff has been through cloud, and the core is the part that does not get out.
uniform vec3 ss_strike_color;

// How strongly cloud BETWEEN a fragment and a strike eats the strike's light (the SSAtmoLightningOcclusion knob, shared with the bolt ribbons). This is what makes the deck read THICK around a
// flash: puffs with dense field between them and the discharge stay dark while the ones in the clear ignite, so the flash carves the deck's structure instead of washing over it.
uniform float ss_strike_occ;

// How far a discharge reaches through the deck, in metres. Not an inverse square: cloud scatters, so the light spreads much further and much more softly than it would through clear air.
#define SS_STRIKE_REACH 700.0
uniform vec3 ss_cam_pos;

uniform float ss_base_z;        // world height of the layer's underside
uniform float ss_layer_thick;   // and how deep it is
uniform float ss_anvil;         // 0 rounded tops, 1 flat against the inversion
uniform float ss_tex_mix;       // authored bias toward the detail map
uniform float ss_puff_density;  // ceiling on one puff's opacity
uniform float ss_detail_scale;  // multiplies the fine octaves' size
uniform float ss_drift_rate;    // multiplies how fast they boil
uniform float ss_noise_tile;    // metres per tile of the convection noise map; 0 = none
uniform float ss_noise_hole;    // the map's hole strength as baked by the builder - the veil's gate shares it
uniform float ss_coverage;      // the builder's coverage threshold - cells whose gate exceeds it hold no puffs
uniform float ss_cell_salt;     // this deck's cell-hash salt (0 primary, 61 under), so both decks' veils gap on their own fields
uniform vec2  ss_tower_ramp;    // the tower ramp's window as baked by the builder - widened as the weather consolidates
uniform float ss_profile;       // 1: the authored vertical profile ramp is bound on bumpMap2
uniform float ss_sheet;         // 0: fragment is a puff. 1: fragment is the deck's base veil

// The DECK'S vertical profile ramp - one thin strip, sampled once at this fragment's height
// through the deck (v 0 base, v 1 lid), whose four channels are four vertical curves: RED the
// tower/ramp weight (how much the noise map counts, ramping toward white near the lid so the
// top consolidates into the anvil), GREEN the carve guard (where the anvil's underside may
// bite - the base band stays black so the deck keeps its body), BLUE the torn cap band, ALPHA
// the thick-base fill. Named bumpMap2 because that is one of LLShaderMgr's RESERVED uniform
// names, and only reserved names can be bound as textures - the same lesson depthMap and
// altDiffuseMap carry.
uniform sampler2D bumpMap2;

// The DECK'S OWN convection noise map - the one the field's towers were grown from, bound here so
// the carving below reads the same geography the builder did. Named altDiffuseMap because that is
// one of LLShaderMgr's RESERVED uniform names, and only reserved names can be bound as textures -
// the same lesson the depthMap declaration above carries: a sampler under a custom name has no
// reserved channel, and bindTexture would index the reserved table with its raw location.
uniform sampler2D altDiffuseMap;

uniform vec2 ss_wind;       // unit, the direction the air is travelling

uniform vec2 ss_drift;      // metres the air has travelled, east and north
uniform float ss_time;      // seconds, for the boil
uniform float ss_churn;     // 0 still air, 1 violently convective

// Where the rim convergence toward the dome runs, in TRUE metres from the eye - x start, y full.
uniform vec2 ss_rim;

// The far-field squash band (x knee, y cap, z virtual radius) - vary_world arrives at the DRAWN position and main() inverts this mapping per fragment to recover the true one, which every
// world-space lookup below uses. Exact per pixel where a true-position varying warped mid-quad.
uniform vec3 ss_squash;

in vec3 vary_world;

// How much of the puff is solid core before the noise starts eating into it, as a fraction of its radius.
const float SS_PUFF_CORE = 0.15;

// Where the rim starts closing, as a fraction of the radius. Inside the window's own falloff, so the noise still ragged-edges the puff well before this takes over.
const float SS_PUFF_RIM = 0.75;

// How hard the noise swings the density either side of the window. Raised, because the window is a circle and the noise is not: the more of the silhouette the noise decides, the less the field looks
// like a pile of spheres. At low contrast the round window wins everywhere and every puff reads as the ball it is.
const float SS_PUFF_CONTRAST = 3.2;

// Over how many metres the underside of the layer is cut flat. A cumulus deck has a flat bottom - the condensation level is a height, the same height everywhere, and cloud simply does not exist
// below it. Rounded puffs cannot produce that on their own; left alone they hang their lower halves below the base and the deck reads as a heap of balls from underneath, which is the giveaway.
// Cutting in WORLD space rather than per puff is what makes it a deck: every puff is sliced by the same plane at the same height, so the cut lines up across all of them into one flat surface. Fading
// over a long stretch rather than a short one. The plane is what makes the base flat; the LENGTH of the fade is what stops it looking stamped. Over a few tens of metres the deck gains a clean edge
// and reads as sheet metal - it is the slow ramp that gives the underside the depth of something you could fly up into. There is a limit: push it past the layer thickness and the cut stops being a
// base at all, just a general dimming of everything low in the field.
const float SS_BASE_SOFT_M = 120.0;

// And the same for the top, once the weather is anvilling. A cumulonimbus stops dead at the tropopause: it has run out of air less dense than itself, so there is nothing to rise into and the tower
// spreads sideways instead. That ceiling is as flat as the base is, and for the mirror-image reason - both are a HEIGHT the air cannot cross, so the cut belongs in world space where every puff meets
// it at the same altitude. Sharper than the base fade: a cloud base is softened by wisps hanging under it, while an anvil top is sheared off by the winds up there.
const float SS_TOP_SOFT_M = 70.0;

// How far height through the layer swings the texture mix, and how far a slow wander across the field does. Height, because a cloud is not the same stuff top to bottom: the base is flat and dense
// where it has just condensed, the top ragged where it is coming apart. Position, because a sky is not uniform either - one part of it can be doing something different from the rest, and a mix that
// varies only with height would band the whole field into horizontal stripes.
const float SS_MIX_HEIGHT = 0.55;
const float SS_MIX_WANDER = 0.45;

// How much faster the top of the layer boils than the base. Convection is a vertical motion, and it is not evenly distributed: the base of a cumulus sits at the condensation level and stays put,
// while the top is where the rising air actually arrives and piles up. So the same churn buys far more movement up there. Driving the whole layer at one rate makes it slide as a slab, which reads as
// a texture animating rather than as air moving.
const float SS_BOIL_TOP = 3.0;

// How far out of step the top of the layer runs from the base, in radians. Fixed, so the layers never drift further apart than this however long the viewer has been open - see the note where it is
// used.
const float SS_BOIL_LEAD = 2.0;

// How much the noise shades the puff internally. Small: the colour is the CPU's per-puff sun/ambient mix, and multiplying that by a mid-grey noise map - as this used to - simply halves the
// brightness of the whole field.
const float SS_PUFF_SHADE = 0.35;

// How dark the side of a puff facing away from the light is left. Not very, because cloud is not opaque - light that enters one side comes out of the other, which is why a cloud has soft shading
// rather than a terminator. But not one flat value either, which is what a per-quad colour gives and why the field read as grey card after grey card with no form to any of it.
const float SS_FORM_DARK = 0.55;

// And the far side's floor for the GRAZE light alone, deliberately well above the body's: the skimmed lid is lit by the burning horizon sky as much as by the beam itself, and sky arrives from
// every side of a crown. See the note where the graze light joins main()'s body.
const float SS_GRAZE_DARK = 0.85;

// How much the thin parts glow. The bright fringe on a cloud is the sun coming THROUGH it where it is thin enough to pass - so the rim lights up while the body stays dull, and that fringe is most of
// what gives a cloud its silhouette. Keyed to low density, so it lands exactly on the ragged edges the noise cuts.
const float SS_RIM = 0.8;

// How far the noise is stretched along the wind when the air is perfectly still, easing back to round as convection rises. Stable air does not make lumps, it makes LAYERS. With nothing lifting it,
// cloud spreads out along the shear instead of piling up, and stratus comes out drawn into long streaks running downwind - which is why a calm overcast reads as a sheet and a convective sky reads as
// heaps. Sampling the noise round at every convection made the calm end look like weak cumulus rather than like stratus. Halved from where it started. At 4x the noise ran so far downwind that the
// sky came out as rails rather than as layers - and it was compounding with two other elongations nothing was accounting for: the quads are already drawn 1.7 wide by 0.62 tall (PUFF_WIDE), and the
// stretch is applied on all three planes, so no orientation broke the direction up. Stacked, a 4x stretch on the map became far more than 4x on screen.
const float SS_STREAK = 2.0;

// The finer octaves, as fractions of the base tile, and how much of the density each contributes. One octave can only ever describe lumps of one size. The base map gives the body of a cloud; these
// give it the curdled surface that a body of vapour has, and - because they SCROLL rather than sitting still - the sense that it is turning over rather than posing. Roughly doubled from where they
// started. At a third and a tenth of the base tile the octaves were resolving detail finer than a puff can carry - so the surface came out as grain rather than as structure, and the Detail Scale
// dial had to be wound up before it looked like cloud at all. A default that needs correcting is the wrong default.
const float SS_OCT2_SCALE = 0.70;
const float SS_OCT3_SCALE = 0.28;
const float SS_OCT2_W = 0.30;
const float SS_OCT3_W = 0.15;

// How far the detail travels along the flow in one cycle, in METRES. Metres, not tiles of its own octave, and that is not a detail. Expressed in tiles the world distance came out as tiles x octave
// size - so Detail Scale, which exists to change how BIG the detail is, was also changing how far it moved, and a dial meant for one thing quietly drove two. Turning it up swept the flow across the
// sky; turning it down left it shimmering in place. Fixed in metres, the motion is the same however finely the map is sampled, and Detail Scale only does what it says.
const float SS_FLOW_M = 90.0;

// Ceiling on that travel once converted to tiles. Past about half a tile the two cross-faded copies are far enough apart to read as two textures rather than one moving - so a very fine octave, where
// 90m is many tiles, is held back to where the cross-fade still holds together.
const float SS_FLOW_MAX_TILES = 0.5;

// How much lift is added to the flow before it is normalised. At zero the equator of a puff flows dead sideways and the underside barely moves; a little of this tilts the whole field upward, which
// is the direction a convective cloud is actually going.
const float SS_FLOW_RISE = 0.45;

// How many laps of the boil cycle the detail makes per second at full convection, and the share of that it keeps in dead calm. Never quite nothing: even still air is not static.
const float SS_OCT_LAPS = 0.06;
const float SS_OCT_DRIFT_FLOOR = 0.25;

// How far the lookup is displaced by a coarse read of the map itself, in metres - domain warping. A tiling map sampled on a straight grid repeats visibly, and at 880m a tile the field is seven
// repeats wide in each direction: the same scrap of cloud over and over, in rows lined up with the world axes because that is what the planes are aligned to. No amount of octaves hides it, because
// every octave repeats on the same grid. Warping bends the grid before it is sampled. The lookup position is pushed around by a much coarser read of the same map, so the repeats stop falling on
// straight lines and stop landing at even spacings - the tile is still there, but there is no longer a pattern to notice. One extra sample per axis buys it.

// How far each plane is skewed along the axis it DROPS, per metre of that axis. A triplanar lookup on xy knows nothing about z, so every height over the same ground samples the same texel - which
// stacks into vertical columns through the layer, most obvious looking straight up. Same for the other two planes and their own missing axes. Sliding each plane's coordinates by the axis it cannot
// see decorrelates them: two points differing only in height now land in different parts of the map. Metres of the dropped axis per tile of skew. Larger is gentler.
const float SS_SKEW_M = 1400.0;

// Metres of world per tile of the noise map. This sets the size of the lumps the field breaks into - not the size of a puff, which is the field's own business. Four times what it started at, i.e.
// the map applied at a quarter scale. At 220m the noise was resolving detail finer than the puffs carrying it, so every puff showed a busy scrap of texture and the field read as fine grain rather
// than as bodies of cloud. Stretching it puts the structure back at the scale of the cloud rather than the scale of the map.
const float SS_NOISE_M = 880.0;

// Metres of world per tile of the BASE VEIL's read of the puff texture. Matched to SS_NOISE_M deliberately: the veil is the deck's own floor, so its mottle sits at the same scale as the field's
// base octave and the two read as one body of cloud rather than as a sheet painted under one.
const float SS_SHEET_TILE_M = 880.0;

// The BASE VEIL's near fade, in metres of TRUE eye distance: gone by the near rail, whole by the far one. The veil is one flat plane and the eye can get arbitrarily close to it - flying up into the
// deck, or standing on ground the under deck's floor nearly touches - and up close a plane is the one thing a billboard field never is: a surface with no parallax, near-constant alpha, sliding
// across the whole screen as a lid. The puffs never need this because they are small and the soft-particle depth fade already dissolves them against whatever they meet; the veil meets nothing, so
// nothing dissolves it. The far rail sits well inside the puffs' own scale so the sheet is always the first thing to go, and the near rail is short enough that the fade is spent before the plane
// can reach the near clip and flash. [interaction: SSVolCloud soft-particle fade, which handles the geometry case and not this one]
const vec2 SS_SHEET_NEAR_M = vec2(8.0, 96.0);

// Eye-space distance from a depth-buffer reading. The projection is the ordinary one, so this is just its inverse.
float ss_eye_z(float d)
{
    float ndc = d * 2.0 - 1.0;
    return (2.0 * ss_clip.x * ss_clip.y)
         / (ss_clip.y + ss_clip.x - ndc * (ss_clip.y - ss_clip.x));
}

float ss_density(vec2 uv)
{
    // <SS:Nexii> The base map's crossfade partner (bumpMap, a reserved channel) and the eased weight, live while the day cycle fades the deck between two keyframed textures. The branch is on a uniform, so every fragment takes the same path and a idle weight costs one sample, as before this existed.
    vec4 s = texture(diffuseMap, uv);
    if (ss_base_blend > 0.0)
    {
        s = mix(s, texture(bumpMap, uv), ss_base_blend);
    }
    return dot(s.rgb, vec3(0.3333));
}

float ss_detail(vec2 uv)
{
    // <SS:Nexii> The detail map's own partner (specularMap) and weight - the fade runs independently of the base's, the two fields keyframe separately.
    vec4 s = texture(cloud_noise_texture, uv);
    if (ss_detail_blend > 0.0)
    {
        s = mix(s, texture(specularMap, uv), ss_detail_blend);
    }
    return dot(s.rgb, vec3(0.3333));
}

// One detail sample, ADVECTED - the flow-map trick. Two copies of the same lookup half a cycle apart, each dragged along the flow by how far through its own cycle it is, cross-faded on a triangle so
// whichever copy is showing is always the one nearest the start of its travel. Neither copy is ever seen resetting, because at the moment one would snap back it has already faded to nothing. This is
// what sliding an offset could never do. Translation moves the whole pattern rigidly - the structure goes past, which reads as wind. What convection actually does is grow structure at one end of the
// motion and destroy it at the other, and the cross-fade is exactly that: detail wells up, travels, and dissolves.
float ss_flow(vec2 uv, vec2 flow, float ph0, float ph1, float w)
{
    return mix(ss_detail(uv - flow * ph0), ss_detail(uv - flow * ph1), w);
}

// One plane's density, with the two maps already blended - see the note at the lookup.
float ss_mixed(vec2 uv, float m)
{
    return mix(ss_density(uv), ss_detail(uv), m);
}

// <SS:Nexii> The BUILDER'S CELL GATE, replicated exactly. The builder places puffs on a 260m air-frame cell grid and SKIPS a cell whenever its gate (cluster noise pushed toward a skip by the
// noise map's holes) exceeds the coverage dial - so at any coverage under full, whole cluster-shaped regions of the field hold no puffs at all. The veil knew nothing of that gate: its only tie to
// the field was the noise map's presence cut, so it drew its sheet under sky the gate had emptied and the deck's floor slid out past the deck. Everything the gate reads is a deterministic hash of
// cell coordinates - the same property that lets every client grow the same field lets this shader grow it a second time - so the veil can ask, per fragment, the exact question the builder asked
// per cell: does this cell hold puffs. The constants are the builder's own (CELL_M, the cluster lattice sizes and octave mix, CLUSTER_WEIGHT, the hole window) and must move with it.
// [interaction: SSVolCloud::buildDeck's gate_raw/gate/coverage check, and hashCell/clusterUnit beside it - one gate, two implementations, byte-matched on the hash and bit-close on the floats]
const float SS_CELL_M = 260.0;

// hashCell: the C++ multiplies signed ints and casts - two's complement wrap, which uint arithmetic reproduces bit-for-bit.
float ss_hash_unit(ivec2 c, uint salt)
{
    uint h = uint(c.x) * 374761393u ^ uint(c.y) * 668265263u ^ salt * 2246822519u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    h = h ^ (h >> 16u);
    return float(h & 0x00ffffffu) / 16777216.0;
}

// clusterOctave: value noise over the cell lattice, cubic-eased - cubic_step(t) is smoothstep's interior, so the easing matches the CPU's.
float ss_cluster_octave(ivec2 c, float cells, uint salt, float shift)
{
    vec2 f = vec2(c) / cells + shift;
    vec2 i = floor(f);
    vec2 t = f - i;
    t = t * t * (3.0 - 2.0 * t);
    ivec2 b = ivec2(i);
    float c00 = ss_hash_unit(b, salt);
    float c10 = ss_hash_unit(b + ivec2(1, 0), salt);
    float c01 = ss_hash_unit(b + ivec2(0, 1), salt);
    float c11 = ss_hash_unit(b + ivec2(1, 1), salt);
    return mix(mix(c00, c10, t.x), mix(c01, c11, t.x), t.y);
}

// clusterUnit: big masses with small-scale raggedness - CLUSTER_CELLS_BIG 9, SMALL 3, OCTAVE_MIX 0.4, and the builder's salts 101/137.
float ss_cluster_unit(ivec2 c, uint salt)
{
    float big = ss_cluster_octave(c, 9.0, 101u + salt, 0.0);
    float rag = ss_cluster_octave(c, 3.0, 137u + salt, 0.37);
    return big * 0.6 + rag * 0.4;
}

// One cell's verdict: 1 the builder put puffs here, 0 it skipped. The presence read is the map at the CELL CENTRE - where the builder sampled - not at this fragment, and through the same hole
// window (0.16/0.52, ss_noise_hole) the builder baked; the gate formula and the coverage comparison are the builder's line for line.
float ss_cell_occupied(ivec2 c, uint salt)
{
    float gate_raw = ss_cluster_unit(c, salt) * 0.85 + ss_hash_unit(c, 1u + salt) * 0.15;
    float presence = 1.0;
    if (ss_noise_tile > 0.0)
    {
        // textureLod, not texture: this uv is constant across a cell, so implicit derivatives are zero inside a cell and enormous for the pixel quads straddling a cell wall - which would fetch the coarsest mip in a one-pixel seam along every boundary. The CPU gated off a 64-across box-filtered cache, so any fixed low lod is at least as faithful as the implicit one.
        vec2 centre = (vec2(c) + 0.5) * SS_CELL_M;
        float n_map = dot(textureLod(altDiffuseMap, centre / ss_noise_tile, 0.0).rgb, vec3(0.3333));
        presence = 1.0 - (1.0 - smoothstep(0.16, 0.52, n_map)) * ss_noise_hole;
    }
    float gate = gate_raw + (1.0 - gate_raw) * (1.0 - presence);
    return (gate <= ss_coverage) ? 1.0 : 0.0;
}

// The gate over the fragment's own air position: the four nearest cells' verdicts, eased bilinearly. The builder's answer is binary per cell and the puffs it places are jittered most of a cell
// wide - so blending verdicts over exactly one cell puts the veil's edge where the outermost puffs of an occupied cell actually reach, soft at the scale a 260m cell is, with no seam at cell walls.
float ss_field_occupancy(vec2 air_xy, uint salt)
{
    vec2 q = air_xy / SS_CELL_M - 0.5;
    vec2 i = floor(q);
    vec2 t = q - i;
    t = t * t * (3.0 - 2.0 * t);
    ivec2 b = ivec2(i);
    float o00 = ss_cell_occupied(b, salt);
    float o10 = ss_cell_occupied(b + ivec2(1, 0), salt);
    float o01 = ss_cell_occupied(b + ivec2(0, 1), salt);
    float o11 = ss_cell_occupied(b + ivec2(1, 1), salt);
    return mix(mix(o00, o10, t.x), mix(o01, o11, t.x), t.y);
}

void main()
{
    // The noise sampled in the AIR's frame, not the quad's. This is the difference between a field of clouds and a field of stickers. The map is world-space noise for a whole body of cloud; a puff
    // is one lump inside that body, so what belongs on a puff is the part of the field it happens to occupy. Sampled per quad - the whole tile on every one, as it was - every puff carries an
    // identical copy of the same picture, and no amount of jittering their positions hides that. Sampled by position, neighbouring puffs continue each other and the lumps that emerge belong to the
    // field rather than to any quad. In the air's frame rather than the world's, so a cloud keeps its shape as the deck drifts instead of dissolving and reforming while it travels. The puffs are
    // placed on cells in that same frame.
    // Recover the TRUE fragment position first - see ss_squash. The view ray is identical for drawn and true (the squash is radial), so the facing frame below is built once and serves both;
    // eye_dist is the TRUE distance and feeds everything ranged (haze, rim convergence), while world_true feeds everything positional (noise, the layer band, the strike lights).
    vec3 to_eye = ss_cam_pos - vary_world;
    float drawn_dist = length(to_eye);
    vec3 nrm = (drawn_dist > 1.0e-4) ? to_eye / drawn_dist : vec3(0.0, 0.0, 1.0);
    float eye_dist = drawn_dist;
    if (drawn_dist > ss_squash.x && ss_squash.z > ss_squash.x)
    {
        eye_dist = ss_squash.x + (drawn_dist - ss_squash.x)
                 * (ss_squash.z - ss_squash.x) / max(ss_squash.y - ss_squash.x, 1.0);
    }
    vec3 world_true = ss_cam_pos - nrm * eye_dist;

    // The dome-handoff band, from TRUE distance - computed here because the base cut below already needs it, ahead of the shading that shares it.
    float dome_rim = smoothstep(ss_rim.x, ss_rim.y, eye_dist);

    vec3 air = world_true - vec3(ss_drift, 0.0);

    // <SS:Nexii> Two fragments share this shader and diverge only here: the puffs, and the deck's BASE VEIL - one soft sheet inset into the deck's floor, drawn under the puffs so the field reads with its gaps filled rather than as balls over empty sky. Both paths hand the shared tail below the same three answers: density (the alpha driver), noise_v (the mottle the shading reads), and sphere_n (what the wrapped light wraps around).
    float density;
    float noise_v;
    vec3 sphere_n;

    // The layer's ceiling, uniform-derived so it sits at main scope: both paths' cuts run under
    // it, and the strike veil in the shared tail below windows itself with it too.
    float top_z = ss_base_z + ss_layer_thick;

    if (ss_sheet > 0.5)
    {
        // The veil reads the PUFF TEXTURE - the same map the puffs wear, so sheet and puffs are
        // unambiguously one material - but an ordinary tiling read would print the map's grid
        // across ten kilometres of open sheet. So it is read five ways at once: five copies of
        // the same lookup, each rotated a fifth of a turn and scaled by a power of the golden
        // ratio - Penrose's own angles and proportion, the P2 tiling's numbers. Five square
        // lattices at incommensurate scales share no repeat period; the blend keeps the cloud
        // character and loses the grid, the cheap honest cousin of an aperiodic tiling.
        vec2 suv = air.xy / SS_SHEET_TILE_M;
        float acc = 0.0;
        float wsum = 0.0;
        for (int k = 0; k < 5; ++k)
        {
            float fk  = float(k);
            float ang = 1.2566371 * fk;
            float c   = cos(ang);
            float s   = sin(ang);
            float sc  = pow(1.6180339887, fk - 2.0);
            vec2 ruv  = mat2(c, -s, s, c) * (suv * sc);
            float wk  = 1.0 / (1.0 + 0.30 * fk);
            acc += ss_density(ruv) * wk;
            wsum += wk;
        }
        float sheet_n = acc / wsum;

        // The field's own geography decides where the veil exists at all: the convection noise
        // map's holes cut it exactly as they cut the puffs, so a gap in the deck stays a gap all
        // the way through and the veil can never paper over what the map opened.
        float presence = 1.0;
        if (ss_noise_tile > 0.0)
        {
            float n_map = dot(texture(altDiffuseMap, air.xy / ss_noise_tile).rgb, vec3(0.3333));
            float cut = smoothstep(0.16, 0.52, n_map);
            presence = 1.0 - (1.0 - cut) * ss_noise_hole;
        }

        // ...and the builder's cell gate decides it too - see ss_field_occupancy. The presence
        // cut above only knows the noise map; the gate also knows the cluster noise and the
        // coverage dial, which between them empty whole regions of cells at any partial
        // coverage. Without this the veil drew its floor under sky the builder gave no puffs,
        // and the sheet's mottle sat unrelated to where the field actually stood.
        float occupancy = ss_field_occupancy(air.xy, uint(ss_cell_salt));

        // And the same edge-of-field fade the puffs run, so sheet and puffs dissolve together
        // toward the dome handoff instead of the sheet outliving them.
        float horiz = length(world_true.xy - ss_cam_pos.xy);
        // The rails are 0.85x the CPU's FIELD_FADE_START_M and just inside its FIELD_DRAW_M - keep them moving with those constants (ssvolcloud.cpp).
        float edge = 1.0 - smoothstep(6800.0, 9800.0, horiz);

        // And the near end of the same idea - see SS_SHEET_NEAR_M. Off TRUE distance, not the drawn one, so the fade measures the metres the eye would actually cross rather than the squashed
        // metres the geometry sits at; near the eye the two agree anyway, and reading eye_dist keeps it agreeing with every other ranged term in this shader.
        float near_fade = smoothstep(SS_SHEET_NEAR_M.x, SS_SHEET_NEAR_M.y, eye_dist);

        // Soft by construction: the mottle shapes the veil but never cuts it, and the ceiling is
        // held at 0.75 - the veil is THIN cloud, and its thin half is where the shared sun-through
        // fringe lives. Run it denser and it reads as a black slab under the deck (the body
        // colour is gloom-crushed in a storm); this soft it glows faintly through its own mottle
        // and reads as the deck's floor lit from within.
        density = clamp(0.30 + 0.40 * sheet_n, 0.0, 0.75) * presence * occupancy * edge * near_fade;
        noise_v = sheet_n;
        sphere_n = vec3(0.0, 0.0, 1.0);
    }
    else
    {

    // A soft radial window. The art has no edge of its own - it is seamless noise, opaque corner to corner, with no alpha channel - so without a window every puff draws as its quad, hard borders and
    // all. That was the wall of rectangles.
    vec2 p = vary_texcoord0.xy * 2.0 - 1.0;
    float r = length(p);
    float shape = 1.0 - smoothstep(SS_PUFF_CORE, 1.0, r);

    // ...and a hard stop at the rim, which the window above cannot provide on its own. The window is ADDED to the noise below, so where it falls to zero the noise alone can still carry a fragment -
    // and it does, right out to the corners of the quad. That is why the puffs were reading as rounded rectangles rather than as cloud: the shape was suggesting an edge while the noise kept drawing
    // past it. This multiplies, so nothing survives the boundary whatever the noise says.
    float rim = 1.0 - smoothstep(SS_PUFF_RIM, 1.0, r);

    // Sampled on all three planes, weighted by the quad's own facing. Two planes was not enough, and failed in a way worth recording: a billboard turned side-on to one of them has almost no
    // variation left in that plane's first coordinate across the whole quad, so the lookup collapses to a single line of the map stretched down the puff. That is where the vertical streaking came
    // from - not an alpha artefact, a texture being read along one axis. The quad's frame, built from the camera rather than from screen-space derivatives. Derivatives were the obvious way to get it
    // - the quad is flat, so its tangents are constant and their cross product is exact - and they are a trap. A puff covering less than a 2x2 pixel quad, or one caught edge-on, has derivatives that
    // collapse to nothing; cross() of those is a zero vector and normalize() of THAT is NaN. A NaN colour draws black, and which puffs are small enough to hit it changes as the camera moves, so they
    // blink in and out. That is the scatter of little black tiles - nothing to do with buffers or blending. These quads face the camera (or lie flat, near the zenith), so the direction to the eye is
    // the normal to within a few degrees in every case that matters, and it can never degenerate. The distance falls out of the same operation for the haze below.
    vec3 tri = abs(nrm);    // the facing frame's inputs were computed with the reconstruction above

    // The sphere the quad stands in for, reconstructed once and used twice - for the light below, and for which way the detail flows. Axes spanning the quad, taken from the world rather than the
    // screen. Any pair perpendicular to the normal will do: rotating the frame within the quad's own plane turns the fake sphere about the view axis, which a wrapped light term cannot tell apart.
    vec3 ref = (abs(nrm.z) < 0.95) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tan_u = normalize(cross(ref, nrm));
    vec3 tan_v = cross(nrm, tan_u);
    sphere_n = normalize(tan_u * p.x + tan_v * p.y + nrm * sqrt(max(1.0 - r * r, 0.0)));
    tri /= max(tri.x + tri.y + tri.z, 1.0e-4);

    // Stretched along the wind, by however little convection there is.
    float streak = mix(SS_STREAK, 1.0, clamp(ss_churn, 0.0, 1.0));

    // The horizontal lookup goes into the wind's own frame first - along and across - so the stretch follows the weather rather than the world axes. Dividing the ALONG coordinate by more metres is
    // what makes the noise change slowly in that direction, and slowly is what a streak is.
    vec2 across = vec2(-ss_wind.y, ss_wind.x);
    vec2 wind_uv = vec2(dot(air.xy, ss_wind) / streak, dot(air.xy, across));

    // No domain warp here any more. It was displacing the lookup by a coarse read of the map to break up the tiling, which it did - and took the cloud with it. Warping bends the sample grid, and the
    // same bend that hides a repeat also drags real structure sideways; at the scale the base map is now sampled at, there was more structure being dragged than repeat being hidden, and the field
    // came out smeared. The tiling it was fighting is better dealt with by sampling nearer the size of a puff, so there is something at puff scale to look at instead. Kept in metres, not yet divided
    // down: every octave needs all three of these at its own scale, so the division happens at the point of use.
    vec2 pl_yz = vec2(air.y / streak, air.z);
    vec2 pl_xz = vec2(air.x / streak, air.z);
    vec2 pl_xy = wind_uv;

    // ...and these are added AFTER that division, in tiles, so they mean the same thing to every octave. Each plane is skewed by the axis it drops (see SS_SKEW_M), which is what stops the three of
    // them agreeing about where the tile boundaries fall.
    vec2 off_yz = vec2(air.x / SS_SKEW_M, 0.0);
    vec2 off_xz = vec2(-air.y / SS_SKEW_M, 0.0);
    vec2 off_xy = vec2(air.z / SS_SKEW_M, air.z / SS_SKEW_M * -0.7);

    // How far this fragment leans toward the other map, and how high it sits in the layer. Both are wanted before the base octave is taken - the mix decides what that octave is sampled FROM, and the
    // height drives the boil rate below. Three things decide the mix: what the author asked for, where in the layer the fragment sits, and a slow wander over the field so the change is regional
    // rather than a stripe. The wander alone is horizontal, and that one is safe: it is sampled three times coarser than the base octave, so it barely changes across a single puff. What it must not
    // do is change FAST on a plane with no variation to give - which is why everything below is triplanar.
    float layer_h = clamp((world_true.z - ss_base_z) / ss_layer_thick, 0.0, 1.0);
    float wander = ss_detail(air.xy / (SS_NOISE_M * 3.0));
    float tex_mix = clamp(ss_tex_mix
                        + (layer_h - 0.5) * SS_MIX_HEIGHT
                        + (wander - 0.5) * SS_MIX_WANDER, 0.0, 1.0);

    // The body of the cloud: triplanar, mixed per plane, and STATIC in the air's frame. Static because the shape of a body of vapour is not what boils - the surface of it is. This used to cross-fade
    // between two offsets of the whole field to fake that, which cost a second full set of samples to animate the one part that should hold still while the deck drifts. The scrolling octaves below
    // do the churning now, and do it better.
    float noise = tri.x * ss_mixed(pl_yz / SS_NOISE_M + off_yz, tex_mix)
                + tri.y * ss_mixed(pl_xz / SS_NOISE_M + off_xz, tex_mix)
                + tri.z * ss_mixed(pl_xy / SS_NOISE_M + off_xy, tex_mix);


    // Two finer octaves from the DOME's own cloud map, sliding across each other - see SS_OCT_LAPS and SS_FLOW_M. Triplanar, like the base. They were taken on the horizontal plane alone, on the
    // reasoning that surface detail is too fine to need placing carefully - which was wrong in a way that showed. A horizontal coordinate barely changes as you move UP a puff, so on any quad facing
    // the camera the octaves held nearly still down the whole height of it and smeared into vertical streaks. Puffs overhead looked right for the same reason: there the horizontal plane is the
    // correct one. Height changes how FAR an octave wanders, not how fast it goes round. It used to scale the rate, and that reintroduced the same failure the orbit was meant to end - one level
    // down. Rate multiplied by height means the PHASE differs across a puff by an amount proportional to elapsed time, so however bounded each orbit is, the gap between the bottom of a puff and the
    // top keeps widening. Given a few minutes it sweeps whole turns over a puff's height, nearby heights land on completely different offsets, and because layer_h runs vertically the tearing comes
    // out as horizontal bands. Winding the rate up only got there sooner. Scaling the amplitude keeps the intent - the top of a convective layer moves more than its base - with nothing that grows.
    // The rate is now uniform across the fragment, so there is no gradient to accumulate, and a fixed phase lead with height keeps the layers out of step without ever drifting further apart.
    float boil = mix(1.0, SS_BOIL_TOP, layer_h);
    float turn = ss_time * SS_OCT_LAPS * ss_drift_rate
               * (SS_OCT_DRIFT_FLOOR + clamp(ss_churn, 0.0, 1.0)) * 6.2831853;
    float lead = layer_h * SS_BOIL_LEAD;

    float oct2_m = SS_NOISE_M * SS_OCT2_SCALE * ss_detail_scale;

    // Where in its cycle the flow is, and its half-cycle partner.
    float cycles = turn / 6.2831853 + lead * 0.15;
    float ph0 = fract(cycles);
    float ph1 = fract(cycles + 0.5);
    float w = abs(1.0 - 2.0 * ph0);

    // Which way the detail travels: OUT of the puff, and up. Along the sphere normal, the same one the light uses. A convective parcel does not slide, it grows - it pushes outward from where it is
    // rising and carries its texture with it, which is why the surface of a cumulus appears to boil out of itself. Following the normal puts that motion where the shape already implies it: detail
    // leaves the middle of a puff, travels out across the curve, and dissolves at the rim. A clock could never produce that. An offset on a circle - which is what this was - moves every fragment of
    // every puff the same way at the same instant, so the field slides as one and reads as wind however the path is dressed up. The direction has to come from the geometry, and the geometry is right
    // here. Never downward, though. Air in a convective cloud goes up and spreads; the underside is where it is fed from, not where it flows to. So the vertical component is clipped at zero and
    // given a lift on top of that - the lower half of a puff flows sideways rather than draining out of the bottom of it.
    vec3 flow_w = normalize(vec3(sphere_n.xy,
                                 max(sphere_n.z, 0.0) + SS_FLOW_RISE));

    // The same direction seen in each plane's own two axes. Whichever plane the triplanar weights favour, the detail is travelling the same way through the world. Scaled by boil, so the top of the
    // layer travels further per cycle than the base does. Safe to vary per fragment here in a way it never was on the rate: this multiplies a DISTANCE that resets every cycle, so it cannot
    // accumulate into the growing shear that scaling the rate caused.
    float reach = min(SS_FLOW_M * boil / oct2_m, SS_FLOW_MAX_TILES);
    vec2 flow_yz = vec2(flow_w.y / streak, flow_w.z) * reach;
    vec2 flow_xz = vec2(flow_w.x / streak, flow_w.z) * reach;
    vec2 flow_xy = vec2(dot(flow_w.xy, ss_wind) / streak,
                        dot(flow_w.xy, across)) * reach;

    float oct2 = tri.x * ss_flow(pl_yz / oct2_m + off_yz, flow_yz, ph0, ph1, w)
               + tri.y * ss_flow(pl_xz / oct2_m + off_xz, flow_xz, ph0, ph1, w)
               + tri.z * ss_flow(pl_xy / oct2_m + off_xy, flow_xy, ph0, ph1, w);

    // Folded in around the midpoint rather than averaged, so the fine octaves push the density either way instead of dragging everything toward mid-grey and flattening the base octave out. One
    // detail octave now, not two. Advecting it costs a second sample per plane, and a single octave that genuinely rises and dissolves says more than two that slide.
    noise += (oct2 - 0.5) * (SS_OCT2_W + SS_OCT3_W);
    noise = clamp(noise, 0.0, 1.0);

    // Window and noise combined by ADDING, the same way the dome layer biases its own noise with coverage (cloudsF.glsl). Multiplying would give a circle with texture painted on it; adding lets the
    // noise decide where the edge falls - solid through the core where the window dominates, ragged and broken toward the rim where the noise does.
    density = clamp((noise - 0.5) * SS_PUFF_CONTRAST + shape, 0.0, 1.0) * rim;

    // ...and cut flat underneath - see SS_BASE_SOFT_M. Softened over a few tens of metres rather than a hard edge, because a real cloud base is ragged at the scale of the wisps hanging off it, just
    // not at the scale of the deck.
    density *= smoothstep(ss_base_z, ss_base_z + SS_BASE_SOFT_M, world_true.z);

    // <SS:Nexii> The convection noise map, read again at the fragment level. The CPU shaped the FIELD with this map - which columns stand up as towers, which fall into pockets - and this stage runs the same two reads the builder does, in the same direction: map HIGHS are towers, map LOWS are pockets. What the map must never do down here is shape the BOTTOM - that inversion (spiky undersides, flattened tops) is what the first cut did before the guard below, and it read as the map being applied upside down. Two cuts, both gated by the anvil weight so an ordinary convective sky keeps every ball it grew: The height ramp - the map blends toward 70% white as the fragment climbs the deck's last stretch toward the cirrus band, so near the lid EVERY column passes the tower window and the whole top consolidates into the anvil rather than only the strong columns' tops. The slope carve - the anvil's UNDERSIDE, deep where a tower feeds it, thinning to a sheet away from one - guarded so it can only bite in the deck's upper half. The base band is where the cloud keeps its body; an anvil carve that reaches the floor is what shredded the undersides. All three vertical windows - and the thick-base fill - come from the authored profile ramp when one is bound, from these built-ins when not. The ramp's four channels mean the author paints the whole vertical story in one strip: solid base, carving middle, white ramp to the anvil.
    float v_h = (world_true.z - ss_base_z) / max(ss_layer_thick, 1.0);
    vec4 prof = (ss_profile > 0.5) ? texture(bumpMap2, vec2(0.5, clamp(v_h, 0.0, 1.0))) : vec4(0.0);

    float anvil_w = ss_anvil;
    if (ss_noise_tile > 0.0)
    {
        float n_map = dot(texture(altDiffuseMap, air.xy / ss_noise_tile).rgb, vec3(0.3333));

        // The tower weight: the map, maxed with the profile's ramp-to-white - built-in
        // 0.7 across the top 30% when no strip is bound.
        float ramp_h = mix(0.7 * smoothstep(0.70, 1.25, v_h), prof.r, ss_profile);
        float tower = smoothstep(ss_tower_ramp.x, ss_tower_ramp.y, max(n_map, ramp_h));

        // The early anvil: the same ramp the builder runs, so the top of the deck takes the lid -
        // and with it both cuts - before the deck-wide anvil figure says so.
        anvil_w = max(anvil_w, smoothstep(0.40, 0.70, ss_churn) * tower);

        // The carve guard: profile green, built-in the upper half only.
        float base_guard = mix(smoothstep(0.20, 0.45, v_h), prof.g, ss_profile);

        float sheet_m = min(ss_layer_thick * 0.35, 420.0);
        float floor_z = mix(top_z - sheet_m, ss_base_z, tower);
        density *= mix(1.0, smoothstep(floor_z - 120.0, floor_z, world_true.z), anvil_w * base_guard);

        // The thick-base fill: profile alpha, none built-in. A density floor, fed by the map's
        // own mottle so the solid base still varies with the geography the deck was carved by.
        float fill = prof.a * clamp(0.6 + 0.8 * (n_map - 0.5), 0.0, 1.0);
        density = max(density, fill * step(v_h, 1.0));
    }

    // ...and flat on top too, once there is an anvil to flatten - see SS_TOP_SOFT_M. Faded in by
    // the anvil weight - the map's early ramp included - so an ordinary convective sky keeps its
    // rounded tops and only a driven one gets the table.
    float lid = 1.0 - smoothstep(top_z - SS_TOP_SOFT_M, top_z, world_true.z);
    density *= mix(1.0, lid, anvil_w);

    // The torn cap band: profile blue where authored, the built-in metres-wide band under the
    // lid otherwise; the noise the puff carries decides what survives inside it.
    float cap_band = mix(smoothstep(top_z - 260.0, top_z - 30.0, world_true.z), prof.b, ss_profile);
    if (cap_band > 0.001 && (ss_profile > 0.5 || ss_noise_tile > 0.0))
    {
        float chunk = smoothstep(0.32, 0.52, noise);
        density *= mix(1.0, chunk, anvil_w * cap_band);
    }

    noise_v = noise;
    }

    // The shared alpha multiply, with the one difference the two paths answer to: the puffs'
    // ceiling is the Puff Density dial, the sheet's ceiling rode in whole on vary_color.a.
    float a = density * vary_color.a * mix(ss_puff_density, 1.0, ss_sheet);

    // Fade out where the puff meets solid geometry. The depth test only ever gives the all-or-nothing answer: a fragment is in front of the surface or it is gone, and the boundary between those two
    // is the quad's own outline drawn across whatever it ran into. That is the hard intersection - the one thing that says "card" no matter how good the shape is. What is wanted is the DISTANCE to
    // that surface, so the puff thins as it closes on it and gathers as haze against it instead of ending on an edge. Same idea as ambient occlusion reading proximity to geometry, spent on alpha
    // rather than on shadow.
    if (ss_soft_m > 0.0)
    {
        float scene_z = ss_eye_z(texture(depthMap, gl_FragCoord.xy / screen_res).r);
        float frag_z = ss_eye_z(gl_FragCoord.z);
        a *= clamp((scene_z - frag_z) / ss_soft_m, 0.0, 1.0);
    }

    if (a <= 2.0 / 255.0)
    {
        discard;
    }

    // Shaded as the sphere the quad stands in for, the same way the celestial discs are: the billboard carries its normal implicitly, because the disc IS the projection of a sphere. Axes spanning
    // the quad, taken from the world rather than the screen - see the note on derivatives above. Any pair perpendicular to the normal will do: rotating the frame within the quad's own plane turns
    // the fake sphere about the view axis, which a wrapped light term cannot tell apart. Wrapped rather than clamped - see SS_FORM_DARK. Light goes through cloud, so there is no dark side, only a
    // dimmer one.
    float wrap = 0.5 + 0.5 * dot(sphere_n, ss_light_dir);

    // Rim convergence toward the dome, the per-fragment half (the vertex stage does the light): the dome band is painted FLAT - no wrap shading, no noise self-shade, no sun-through fringe - so
    // all three ease to their mids across the same range the edge fade runs, and the last rows dissolve into the painting as the same material. The BEAM gate folds into the same flattening: a
    // deck with no direct celestial light has nothing to wrap around or shine through either, and holding these terms through twilight carved dark cores and lit fringes out of plain dim ambient.
    float form_flat = max(dome_rim, 1.0 - ss_beam);

    float thin = 1.0 - density;

    // The puff's OWN light - ambient plus the DIRECT sun, aimed by the CPU's structural form term (facing and shade-through-the-deck, which the builder walks the deck's geometry to know) and
    // the wrap below, with the deck's storm gloom over the whole. The sun arrives UNGATED by the glow term, deliberately: the first cut ran the band's sun*glow_gate composition here, and a
    // dense deck kept 0.35 of an already-extinguished sun - the whole field fell back to ambient grey under the one sky it had to match. The band gates because a flat painting has only the
    // view angle to direct with; this deck directs with its geometry, and the glow spends itself on the transmitted fire below instead. Cloud light only: the airlight joins at the very end,
    // past every one of these multipliers, because none of them are its business - see the vary_ss_airlight notes.
    // <SS:Nexii> The deck's water content, spent as DEPTH rather than as a flat dim. ss_gloom is one number for the whole deck - what the weather's moisture says the cloud is carrying - and multiplying
    // every fragment by it uniformly is what made Storm Darkening read as "nothing happened": it dimmed the lit tops and the dark bellies by the same factor, so the deck kept its exact shape and only
    // its exposure moved, which the eye reads as no change at all. Graded over the buried depth instead (vary_color.g, the fraction of the puff's own column standing above it) it says what water in a
    // cloud actually does - the lid is the surface the light lands on and keeps it, the belly is under a hundred metres of the stuff and loses it - so darkening a deck DEEPENS it. At ss_gloom 1 this is
    // the identity, so a fair-weather sky is untouched. Grading the AMBIENT is the half that matters: the ambient is the larger term under any overcast and it was the flood that washed the CPU's
    // per-puff column shading (vary_color.r) out - a lit top was a tenth brighter than its own base rather than the several times it should be. [interaction: storm darkening]
    float gloom = mix(1.0, ss_gloom, vary_color.g);

    vec3 puff_light = (vary_ss_amblit + vary_ss_sunlit * vary_color.r) * gloom;

    // The noise self-shade's mid, shared by the body and the graze light below - the lid keeps its texture whichever term is carrying it.
    float noise_mid = mix(mix(1.0 - SS_PUFF_SHADE, 1.0, noise_v), 0.83, form_flat);

    vec3 body = puff_light
              * mix(mix(SS_FORM_DARK, 1.0, wrap), 0.78, form_flat)
              * noise_mid;

    // <SS:Nexii> The graze light joins HERE, past the body's full wrap, rather than riding the form term as it used to. Inside the form sum it was multiplied by the sun's own wrap, and the sun's
    // wrap is exactly what a skimmed lid is not shaped by: at a grazing sun the whole horizon band is burning above the deck, and that sky lights the crown of a puff from every side - so the crest
    // came out as a warm stripe up the sun side of each lid puff with the anti-sun half held at the 0.55 floor, and the alpenglow the ramp exists to paint never read as a lit LID. A gentler wrap
    // of its own (SS_GRAZE_DARK) keeps the sun side warmest without ever putting a crown's far side in the dark; the same gloom and the same capped sunlit colour, so the crest stays the sunset's
    // own fire and a storm still eats it.
    body += vary_ss_sunlit * (vary_ss_top * gloom)
          * mix(mix(SS_GRAZE_DARK, 1.0, wrap), 0.78, form_flat)
          * noise_mid;

    // The bright fringe where the puff is thin enough for light to come through it - see SS_RIM. Fed by the capped vertex-stage sun light, so at a low sun the fringes carry the sunset's own
    // hue - the uniform this read before was the CPU replica that had already crushed to grey by then.
    body += vary_ss_sunlit * (SS_RIM * wrap * thin * thin * thin) * (1.0 - form_flat);

    // <SS:Nexii> The forward-scatter fire - the band's glow term (cloudsF's glow_gate, same thinness easing, same 0.35 body sliver), spent here as the ADDITIVE it physically is: light
    // transmitted through the thin parts of a puff standing between the eye and the sun. View-angled where the wrap fringe above is light-angled, so the two light different edges - the wrap
    // fringe rims the lit side everywhere, this one ignites whatever hangs in the glow cone around a low sun.
    float glow_gate = mix(min(vary_ss_glow, 0.35), vary_ss_glow, thin * thin);
    body += vary_ss_sunlit * (glow_gate * thin * thin) * (1.0 - form_flat);

    // Lightning inside the deck. Each strike is a point source, so it gets its own wrapped sphere term against ITS direction - which is the whole difference between a puff that brightens and a puff
    // that is lit from somewhere. Wrapped rather than clamped for the same reason the sun is: light goes through cloud, so the far side dims, it does not go black.
    for (int i = 0; i < ss_strike_count; ++i)
    {
        vec3 to_strike = ss_strike[i].xyz - world_true;
        float dist = length(to_strike);
        if (dist < 0.001) continue;

        float reach = SS_STRIKE_REACH * SS_STRIKE_REACH;
        float atten = reach / (reach + dist * dist * 4.0);

        float lit = 0.5 + 0.5 * dot(sphere_n, to_strike / dist);
        lit = mix(SS_FORM_DARK, 1.0, lit);

        // A thin edge of puff with a discharge behind it glows through, exactly as it does with the sun - and this is what a bolt seen THROUGH cloud actually looks like from below.
        float through = 1.0 + SS_RIM * thin * thin;

        // The veil: two density estimates along the path from this fragment TO the strike, read from the same base map the deck is drawn from, windowed to the layer band. Where the field is
        // dense between here and the discharge the light dies exponentially, where the path runs through a gap it arrives whole - so the flash maps the deck's own thickness left and right of the
        // channel instead of falling off by bare distance [interaction: cloud field -> strike light].
        float veil_sum = 0.0;
        for (int s = 1; s <= 2; ++s)
        {
            vec3 q = mix(world_true, ss_strike[i].xyz, float(s) / 3.0);
            float inz = smoothstep(ss_base_z, ss_base_z + SS_BASE_SOFT_M, q.z)
                      * (1.0 - smoothstep(top_z, top_z + SS_TOP_SOFT_M, q.z));
            float d_est = ss_density((q.xy - ss_drift) / SS_NOISE_M);
            veil_sum += inz * clamp((d_est - 0.35) * 2.0, 0.0, 1.0);
        }
        float veil = exp(-veil_sum * 2.2 * ss_strike_occ);

        body += ss_strike_color * (ss_strike[i].w * atten * lit * through * veil);
    }

    // Bounded exactly the way the dome layer bounds itself (cloudsF.glsl). Everything feeding this is in EEP's HDR units - sunlight and ambient both run well past 1, and the rim term adds a whole
    // sun colour on top - so the shading came out far brighter than anything else in the frame. Nothing writes to a glow buffer here, but the bloom pass takes its bright-pass off the finished
    // screen, and unclamped cloud sails straight over that threshold. Hence the halo around every puff. Clamping to 1 and doubling is not a taste decision: it is the range the dome layer already
    // occupies, so the two kinds of cloud end up on the same scale as well as out of the bloom. NO scene-gamma term here, and its absence is a lesson: pow(c, 1/gamma) with an authored storm-sky
    // gamma of a few tenths is an exponent of several - it crushed every mid-tone to black and left white ridges with pink rims (red dies last), the whole deck reading as a burnt negative. The
    // dome's own gamma response goes through the atmospheric soft-clip curve, which is not a power law, so there is no cheap honest replication - better none than that.
    // No fog pass after this any more: the atmosphere came in through the vertex-stage colours, the same door it enters the band by (the slab-ray transmittance in the cloud terms, the
    // airlight joining here - see ssVolCloudV.glsl). At the rim the transmittance has taken the cloud terms to nothing and BOTH deck and band converge to the same pure airlight, so the
    // handoff is exact whatever the gloom and the shading mids did to the cloud's own light.
    vec3 shaded = clamp(body + vary_ss_airlight, vec3(0.0), vec3(1.0)) * 2.0;

    frag_color = vec4(shaded, a);
}

