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

// <SS:Nexii> S1 vertex-decode channel fix (doc/atmo_magic_flow_field.md section 2, opus review Q2b): widened
// vec2 -> vec4 in the vertex stage, zero new interpolator slots. .xy is the decoded CORNER (round()'d pre-
// interpolation, so it interpolates exactly as the old raw 0/1 texcoord did - every .xy read below is
// bit-identical to the old vary_texcoord0 whenever payload is 0, which it is throughout S0-S2); .zw is the
// decoded PAYLOAD, unread anywhere in this file yet.
in vec4 vary_texcoord0;

// <SS:Nexii> Per-puff STRUCTURE, not a finished colour any more: r the CPU builder's form term (facing and shade-through-the-deck, beam-flattened), g the buried depth - the fraction of the puff's own
// column standing above it, 0 at the lid and 1 at the floor, which the storm gloom is graded over below - and a the edge-fade alpha; b is EITHER the shaft flag/drive (see the branch below) OR, for
// an ordinary puff (S2, doc/atmo_magic_flow_field.md section 2), the per-puff advected-detail phase on its low half [0, 0.49] (SSVolCloud::Puff::mPhase * 0.49, see the puff branch's own decode).
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

// <SS:Nexii> S4 (doc/atmo_magic_flow_field.md section 2 S4, "Lighting (scene-proven)" verdict): SSAtmoCloudLightVariant's live value, for an in-build A/B of the scene-proven lighting tail terms -
// 0 today's tail, BIT-IDENTICAL by construction (nothing in the lighting tail below moves unless this is nonzero). 1 POWDER: the direct-sun body term gains the Beer-powder factor (research_lighting.md
// #1). 2 HG2: the thin^3 rim fringe is replaced by a two-lobe Henyey-Greenstein phase on the view/sun angle (research_lighting.md #3). 3 FULL: POWDER + HG2 + a subtle green storm tint on buried
// bellies under gloom. Ported from V:\Scratch\atmo\flow\scene.h's LightVariant enum (V_BASE/V_POWDER/V_HG2/V_FULL) - the CPU prototype the ladder actually judged (doc's "scene_r3 powder vs base A/B").
uniform int ss_light_variant;

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

// <SS:Nexii> F2 (2026-09-06 review), extended phase 8e item 2 [interaction: ssvirgacore.h embedTopZ/embedAlpha]:
// the virga embed span this deck's shaft cards start inside of, PLUS the ground reference - x the deck base Z, y
// the embedded stack's top Z (embedTopZ(baseZ, thicknessM), the SAME value buildDeck's shaft loop bakes
// shaft_top_z from), z the ground reference Z (SSAtmoEnvApplier::windProfileGroundZ, the SAME shaft_ground_z the
// card stack spans down to), w spare. Uploaded for the WEATHER deck's pass only, same rule as ss_storm_n above;
// zero-filled for the other deck's pass. Harmless for one reason only: that deck never emits a shaft fragment (shafts
// only ever hang from weatherDeck(), the same couple_storm gate), so nothing reads the zeros - the values they would
// produce (embedAlpha 0 above 0 m, h == clamp(z, 0, 1) == 1 for any fragment above 1 m) are never evaluated.
uniform vec4 ss_shaft_embed;    // (deck base Z, embedded stack top Z, ground reference Z, 0)

// <SS:Nexii> Phase 3 (doc/atmo_magic_storm_dynamics.md section 3) [interaction: ssstormcouplecore.h]: the coupled storm cells, uploaded for the WEATHER deck only - ss_storm_n 0 for the under deck reads as "no cells" (see ssvolcloud.cpp's buildDeck upload). Layout LOCKSTEP with SSStormCouple::CellUniform, field for field: ss_storm_a.xy centre / .z radius / .w boost, ss_storm_b.x anvil / .y meso / .z overshoot / .w mammatus, ss_storm_c.xy motion direction / .z rotation sign.
#define SS_STORM_MAX_CELLS 4
uniform int  ss_storm_n;
uniform vec4 ss_storm_a[SS_STORM_MAX_CELLS];
uniform vec4 ss_storm_b[SS_STORM_MAX_CELLS];
uniform vec4 ss_storm_c[SS_STORM_MAX_CELLS];

// <SS:Nexii> 8a item 2 (doc/atmo_magic_phase8_show.md section 3) [interaction: ssstormcouplecore.h LineBand]: the
// active squall line's deck coupling, uploaded for the WEATHER deck's pass only, same rule as ss_storm_n above -
// zero-filled (ss_line_c.y strength 0) for the other deck's pass, which ss_line_field below reads as "disabled".
// LOCKSTEP with SSStormCouple::LineBand field for field: ss_line_a.xy segment centre / .zw unit direction,
// ss_line_b.xy unit motion / .z halfLen / .w bandM, ss_line_c.x shelfM / .y strength.
uniform vec4 ss_line_a; // (ox, oy, dirX, dirY)
uniform vec4 ss_line_b; // (motX, motY, halfLen, bandM)
uniform vec4 ss_line_c; // (shelfM, strength, 0, 0)

// <SS:Nexii> Phase 4 (doc/atmo_magic_wind_profile.md section 4 "Drift: one accumulator + bounded shear offsets", doc/atmo_magic_storm_dynamics.md section 3 "Storm motion vs cloud drift") [interaction: ssdeckframecore.h]: the deck's frame transforms, LOCKSTEP with SSDeckFrame's own uniform layout field for field (ssvolcloud.cpp's upload uses these exact names). ss_shear_z is the baked O(z) table's (z0, z1); ss_shear_table[i] is SSDeckFrame::ShearTable::o[i] in the same order - together the ONLY inputs ss_frame_shearTableAt below reads, so the GPU never evaluates the wind profile itself (see the core header's REPLICATION STRATEGY note). ss_hero is the hero's (motion.xy, ageS, radius); ss_hero_c its centre; ss_drift_vel the applier's curve-resolved drift rate in m/s - the SAME vector SSDeckFrame::HeroFrame::driftVel carries, never the eased mWind and never ss_wind (a unit direction, not a rate). Every deck uploads its own shear table; the hero fields are zero-filled (ageS 0) for a deck with no hero, which is what makes ss_frame_heroShift's result the zero vector and every read below an identity.
#define SS_SHEAR_TABLE_N 8
uniform vec2 ss_shear_z;
uniform vec2 ss_shear_table[SS_SHEAR_TABLE_N];
uniform vec4 ss_hero;      // motion.xy, ageS, radius
uniform vec2 ss_hero_c;    // centre
uniform vec2 ss_drift_vel; // m/s, the applier's curve-resolved rate

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
// <SS:Nexii> D3 [interaction: ssdeckboilcore.h]: ss_time is GONE, and its absence is the fix - it was an ABSOLUTE clock (LLFrameTimer::getElapsedSeconds) multiplied by a rate that then stepped once a second (the D3 follow-up, 2026-09-07, made that root clock continuous too - SSAtmoEnvTrack::currentDayCyclePhase now reads SSAtmoMagic::sharedTime(); this absence is still the fix, because integrating the rate is what makes ANY rate change invisible in the phase),
// which is what made every puff's appearance jump; see the note at `cycles` in main(). These two are the INTEGRALS the CPU now keeps in its place (SSDeckBoil::advanceLaps / advanceFallM, F64
// accumulators on the deck, folded to their own periods before they cross into these F32 uniforms so they stay exact however long the viewer has been open).
uniform float ss_boil_laps; // the advected detail octave's phase, in laps, already folded into [0,1)
uniform float ss_fall_m;    // how far the rain curtain's streak has fallen, in metres, folded on the noise map's tile period
uniform float ss_churn;     // 0 still air, 1 violently convective

// Where the rim convergence toward the dome runs, in TRUE metres from the eye - x start, y full.
uniform vec2 ss_rim;

// <SS:Nexii> The deck's ONE edge-fade rail pair (SSDeckLod::edgeFade, ssdecklodcore.h): x FIELD_FADE_START_M, y DECK_EDGE_M - uploaded from the core so the veil's horizontal fade agrees with the puff loop's own edgeFade() call instead of a second, hand-tuned pair of literals.
uniform vec2 ss_edge_rails;

// <SS:Nexii> 8g veil law (SSVeil, ssveilcore.h): the BASE VEIL's OWN four rails, uploaded from the core - xy the far-thinning pair (SSDeckLod::THIN_START_M, FIELD_DRAW_M) the veil's rim FADE-IN is the complement of, zw the veil's outward reach (SSVeil::REACH_START_M, REACH_END_M) past the puffs' DECK_EDGE_M. Kept separate from ss_edge_rails on purpose. Precisely what ss_edge_rails is, since this is easy to get wrong reading the file: the puffs' own edgeFade is applied on the CPU (ssvolcloud.cpp bakes it into puff.mAlpha), so this uniform is the GLSL MIRROR of that rail pair and its only reader in this file is the sheet - which now reads it INVERTED, as the edge_keep factor inside the veil's fade-in. The pair still means what it always meant (SSDeckLod::edgeFade, the deck's dissolve line); what changed is that the veil complements it instead of multiplying by it.
uniform vec4 ss_veil_rails;

// The far-field squash band (x knee, y cap, z virtual radius) - vary_world arrives at the DRAWN position and main() inverts this mapping per fragment to recover the true one, which every
// world-space lookup below uses. Exact per pixel where a true-position varying warped mid-quad.
uniform vec3 ss_squash;

in vec3 vary_world;

// <SS:Nexii> S3 flow-field feature flags (doc/atmo_magic_flow_field.md section 2 stage 3, review_opus.md's
// G2/G3/G5 correction list, V:\Scratch\atmo\flow\proto.h's f1_ring/f4_anvil/f5_swirl - f1_ring ONLY, the ring
// cores plus the central updraft; f3_ring_curl's extra 0.5x curl noise is deliberately NOT ported here, see
// the SS_G2_BILLOW block's own comment below for why): each defaults ON and independently reverts its OWN
// piece of code to today's exact behaviour at 0 - SS_G2_BILLOW the vortex-ring flow_w replacing the plain
// radial-and-up construction, SS_G3_OUTFLOW the anvil outflow blend (new; today has no anvil flow bias at
// all, so 0 is simply "absent, byte-identical"), SS_G5_SWIRL the meso tangential field replacing the
// constant-angle SS_STRIATION_ROT nudge (0 restores that nudge verbatim), SS_G2_VIGOR the vigor-scaled
// detail contrast (new; today's SS_PUFF_CONTRAST is applied unscaled at 0). Each flag is independent, not a
// staged ladder - any combination compiles and runs. Fetch cost: ZERO new texture or textureLod calls behind
// any of these four flags - every new term below is ALU only (rot90/safeDir/mix/length/dot/normalize/clamp),
// reusing storm_sample fields and uniforms already resident from the shared frame block above. That ALU is
// NOT cheap, though, and it is NOT gated off-cell - review_opus.md's hand count against the shipped blocks,
// all four flags on, comes to ~170 ALU worst case (roughly 6x an earlier +~30 estimate), paid on every
// fragment including clear-sky ones where storm_sample.owner == 0 and the whole pipeline computes an
// identity; see doc/atmo_magic_flow_field.md section 3 for the per-block breakdown and the suggested
// storm_sample.owner > 0.0 gate as the cheap win if this shows up in a frame-time pass.
#define SS_G2_BILLOW 1
#define SS_G3_OUTFLOW 1
#define SS_G5_SWIRL 1
#define SS_G2_VIGOR 1

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

// <SS:Nexii> S4 POWDER variant (doc/atmo_magic_flow_field.md's settled verdict, research_lighting.md #1): the buried-depth optical-depth stand-in, d = SS_POWDER_DEPTH * vary_color.g, ported
// verbatim from scene.h's V_POWDER (`const float dOpt = 2.5f * buriedC;`) - the SAME 2.5 the doc's own verdict text quotes ("d ~ 2.5 * buried"). This is the RAW Beer-powder curve
// exp(-d)*(1-exp(-2d)), UNNORMALIZED: it is 0 at d=0 (a lid), rises to a peak around d~0.55 (buried~0.22), then falls to ~0.08 by d=2.5 (a fully buried belly) - so it does NOT preserve "today's
// brightness" at a lid the way a naive first read of "deepens bellies" might suggest, and no constant-divisor renormalization can fix that (a zero numerator stays zero however it is scaled). This
// is the literal curve scene.h shipped and the ladder judged (scene_r3 powder vs base A/B) and the literal curve the doc's verdict spells out - ported as-is rather than re-derived, and applied
// ONLY to the direct-sun body term (not the graze, not the rim/HG2 fringe, not the glow, not the airlight), so the ambient and the graze/fringe/glow terms still carry a lid's usual brightness even
// though the direct-sun body contribution itself dips there. See where SS_POWDER_DEPTH is spent, below.
const float SS_POWDER_DEPTH = 2.5;

// <SS:Nexii> S4 HG2 variant (research_lighting.md #3): gain on the two-lobe phase fringe (ss_hg2_phase below), chosen so its peak (c=1, thin=1) lands close to the OLD thin^3 rim term's own peak
// (SS_RIM * thin^3 = 0.8 at thin=1) instead of the raw HG's own ~2.0 peak there - phase(c=1) = mix(hg(1,-0.2), hg(1,0.8), 0.55) = mix(0.0442, 3.581, 0.55) = 1.989, so 0.4 * 1.989 = 0.796, within a
// couple percent of 0.8. The dial changing the fringe's SHAPE (a view/sun-angle-dependent silver lining instead of a thin-only glow) rather than the field's overall exposure is the point - an
// exposure jump would read as a brightness bug, not a lighting-model swap.
const float SS_HG2_GAIN = 0.4;

// <SS:Nexii> Distant rain shafts (ssvirgacore.h, doc/atmo_magic_far_clouds.md section 3): the fragment-only box and streak constants a virga card carries. Nothing here is a CPU twin - the qualifying
// cell set, the taper and the vertical alpha profile are all baked by ssvirgacore.h and already ride in on vary_color.a and the card's own geometry; these four only round the card's own edges off and
// give it a texture to carry. SS_SHAFT_V_SOFT: how much of the card's own v range eases in from its top and bottom before the next stacked card takes over, as a fraction of the card. SS_SHAFT_H_CORE:
// how much of the card's own width, either side of centre, stays solid before the taper's side edges fall away. SS_SHAFT_FALL_MPS: how fast the fall streak's sample point travels down the vertical
// axis (SS_SHAFT_FALL_MPS itself has MOVED into ssdeckboilcore.h as SSDeckBoil::FALL_MPS, unchanged in value - see ss_fall_m). Phase 8e item 4, DRIVE REACHES THE FRAGMENT: the streak's own AMPLITUDE is no longer this fixed constant - a light-rain curtain is a few eroded filaments, a heavy one a near-solid wall, so it
// now ramps between SS_SHAFT_STREAK_WISPY and SS_SHAFT_STREAK_HEAVY by drive (see where it joins, below).
// F12: vary_color.a (SSVirga::alphaAt * handoff * edgeFade) is NOT the shaft's whole on-screen ceiling - the shared
// alpha multiply below (`a = density * vary_color.a * mix(ss_puff_density, 1.0, ss_sheet)`) still folds in the
// Puff Density dial (ss_sheet is 0 on this draw call, so the ceiling is ss_puff_density, same as an ordinary
// puff) AND this branch's own `density = mask * mix(body_floor, 1.0, body) * streak_term * ss_virga_embedAlpha(...)
// * evap` (F2, 2026-09-06 review: the per-pixel embed fade joined this product in place of the CPU's old per-card
// alphaMul; phase 8e item 5 then adds the evaporation mask on top) - a shaft at vary_color.a's own ceiling can
// still read far short of it near a card's soft top/bottom seam, its taper edge, while still inside the deck's own
// embedded span, or below its own evaporation height.
const float SS_SHAFT_V_SOFT = 0.22;
const float SS_SHAFT_H_CORE = 0.10; // 7d: was 0.45 - a wide solid core gave every card flat sides; the column now falls away from its centre line

// <SS:Nexii> Phase 8e item 4 (doc/atmo_magic_phase8_show.md section 3b), DRIVE REACHES THE FRAGMENT: the streak's
// amplitude by drive (the SAME SSVirga::drive the cell qualified with, recovered from vary_color.b - see the shaft
// branch's own `drive` line) - a light-rain curtain (low drive) is a few eroded filaments (little amplitude, close
// to solid, SS_SHAFT_STREAK_WISPY), a heavy one (high drive) a near-solid dark wall with streak texture (more
// amplitude, SS_SHAFT_STREAK_HEAVY is actually LOWER than WISPY here because streak_term's own mix runs
// mix(1.0 - amp, 1.0, streak) - a smaller amplitude means streak_term stays closer to 1 (more solid), a larger one
// swings further toward 0 (more eroded); 7d's old single SS_SHAFT_STREAK (0.65, "was 0.30 - the fall streak now
// erodes most of the card, so no card reads as a uniform slab") is DELETED, replaced by this pair.
const float SS_SHAFT_STREAK_WISPY = 0.85;
const float SS_SHAFT_STREAK_HEAVY = 0.45;

// <SS:Nexii> Phase 8e (doc/atmo_magic_phase8_show.md section 3b): the shaft's BODY. Before this, `density = vbox *
// hedge` (the box/streak above) was the whole silhouette - a soft rectangle with a falling streak eroding it, no
// relation to the puff field it hangs from. Now that box is a MASK only, and the actual silhouette comes from
// sampling the puff's own cloud noise (ss_density, the same base-map read ss_mixed/the sheet's Penrose blend both
// call) on the card's two vertical planes - (world x, world z * SS_SHAFT_Z_STRETCH) and (world y, world z *
// SS_SHAFT_Z_STRETCH) - weighted by the card's own facing (|nrm.x|, |nrm.y|), so a curtain reads as the deck's own
// texture pulled downward rather than a second, unrelated material. SS_SHAFT_Z_STRETCH squashes the vertical axis
// before the lookup so a card many hundred metres tall does not read as one flat streak of the map's z-period;
// the body floor (mix(floor, 1, body), never body alone, so the box's taper/seam softening below still shapes the
// silhouette instead of the noise being able to erase it outright) keeps the mask's own soft edges visible where
// the body noise happens to read near zero. LOCKSTEP nothing: ssvirgacore.h and the twin harness own
// the qualifying-cell set, the card geometry (including this card's own gate_air-derived world position) and the
// alpha profile; this body noise has a CPU twin ONLY for the coordinate construction (gate_air, the frame the
// producer placed the card in), never for ss_density's own return value - no test asserts a particular texel.
const float SS_SHAFT_Z_STRETCH = 0.25;

// <SS:Nexii> Phase 8e item 4, DRIVE REACHES THE FRAGMENT: the body floor by drive - a light-rain curtain floors at
// 0 (its soft mask edges are all that is left where the body noise itself goes dark, so the wispiest filaments can
// erode all the way to nothing), a heavy one floors at 0.45 (a near-solid wall does not lose its silhouette to a
// single dark noise sample). The single SS_SHAFT_BODY_FLOOR (0.25) that 8e's first body pass introduced is DELETED,
// replaced by this pair (7d never had a body floor - it only retuned the streak).
const float SS_SHAFT_BODY_FLOOR_WISPY = 0.0;
const float SS_SHAFT_BODY_FLOOR_HEAVY = 0.45;

// <SS:Nexii> Phase 8e item 2 (doc/atmo_magic_phase8_show.md section 3b): a curtain is lit like a puff's lower rim
// (see the shaft branch's ss_sphere_normal call and its own comment), tilting further down toward the ground the
// closer to the ground it hangs - continuous across stacked cards because it is keyed on h, the CURTAIN's own
// height fraction (SSVirga::baseProfileH01's twin), not the card's own v. SS_SHAFT_DROOP_GROUND is the tilt at the
// ground (h 0); SS_SHAFT_DROOP_BASE is the (shallower) tilt at the deck base (h 1) - a curtain leans more toward
// the ground than toward the cloud that is dropping it.
const float SS_SHAFT_DROOP_GROUND = 0.85;
const float SS_SHAFT_DROOP_BASE = 0.35;

// <SS:Nexii> Phase 8e item 5 (doc/atmo_magic_phase8_show.md section 3b), EVAPORATION MASK (the user's rule: alpha
// is intensity x density across the volume; virga is masked out from the bottom up with wispy streaks). EVAP_H_
// WISPY/EVAP_H_HEAVY/EVAP_SOFT LOCKSTEP ssvirgacore.h's own SSVirga::EVAP_H_WISPY/EVAP_H_HEAVY/EVAP_SOFT (the twin
// pins them from both sources - the CORE's evapHeight01/cardEmittedFor decide which CARDS are emitted at all, this
// shader's own evap decides the per-pixel fade within an emitted card, so the two must agree on the same height and
// softness or the geometry cutoff and the pixel fade would disagree about where a curtain ends). EVAP_JITTER is
// shader-only (no CPU twin): it swings the mask's own threshold by the streak detail read so the bottom edge comes
// out as ragged wisps of different lengths, never a single flat line.
const float SS_EVAP_H_WISPY = 0.65;
const float SS_EVAP_H_HEAVY = 0.0;
const float SS_EVAP_SOFT = 0.25;
const float SS_EVAP_JITTER = 0.35;

// <SS:Nexii> [interaction: ssstormcouplecore.h] How much the mesocyclone darkens the wall cloud (doc/atmo_magic_storm_dynamics.md section 3 "Local gloom"): multiplied over the SAME gloom both the puff and sheet paths already read, so the darkening under a wall cloud shares one number with no seam at the veil directly beneath it - see where it joins below.
const float SS_MESO_GLOOM = 0.35;

// <SS:Nexii> Optional cosmetic (doc/atmo_magic_storm_dynamics.md section 3 "Supercell"): how far the detail flow's direction is nudged by the storm's own rotation, meso-weighted and signed by rotSign - see where it joins the flow direction below. Shader-only flavour, not a mirrored formula: no CPU counterpart to keep in lockstep with.
const float SS_STRIATION_ROT = 0.6;

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

// <SS:Nexii> S3 flow-field constants (doc/atmo_magic_flow_field.md section 1 "Flow fields"; ring/anvil/swirl
// forms are LITERAL ports of V:\Scratch\atmo\flow\proto.h's f1_ring/f4_anvil/f5_swirl, the ladder-proven forms
// - "never normalize a summed field vector" is the law those prototypes exist to prove, not a style choice).
// SS_FLOW_EPS: the shared denominator floor for every eps-floored direction/magnitude below (proto.h's own
// SS_FLOW_EPS, unitless in the prototype's [-1,1] disc space; reused here at metre scale too - it still only
// ever guards an exact-zero denominator, and at 0.05 m it is negligible against any real storm-cell or
// puff-radius distance, so it never softens a genuine direction, only the literal singularity). SS_BILLOW_RING_U/
// SS_BILLOW_RING_V: where the two counter-rotating ring cores sit along the puff's own meridional (radial, bulge)
// axes - (+-SS_BILLOW_RING_U, SS_BILLOW_RING_V), below centre, matching cumulus turret morphology, as a fraction
// of the puff's own radius (unitless, same domain as p). SS_BILLOW_UPDRAFT_MAG/SS_BILLOW_UPDRAFT_SPREAD: the
// central updraft term f1_ring adds on top of the ring pair - a plain gaussian in the radial coordinate, +y
// (bulge) up, no division and so no singularity of its own; f1_ring's OWN curl companion (f3_ring_curl, 0.5x
// fbm-derivative noise) is deliberately NOT ported - it costs 8 extra fbm evaluations (2 central differences x 2
// axes x 4 octaves) against this block's current zero-new-fetch budget, so ring+updraft-only (f1_ring, not f3)
// is what ships; the escalation if a build wants more turbulence in the roll is porting f2_curl/f3_ring_curl in
// full, at that cost. SS_ANVIL_UPWIND_CAP: the anvil outflow's magnitude floor
// on the upwind (back-shear) side, as a fraction of the downwind magnitude - short, not absent; a real
// back-shear flank still shows some outward push. SS_ANVIL_STREAK_GAIN: extra streak-stretch multiplier at
// full downwind alignment under a full anvil, unitless (multiplies the SAME `streak` divisor SS_STREAK
// already scales). SS_MESO_SWIRL_MIN_M: metres - floors the meso swirl's own radius so the tangential field
// never divides by a (near-)zero distance at the mesocyclone's own axis. SS_MESO_SWIRL_M: metres - the swirl
// magnitude's falloff scale, chosen so the field is strong within a couple of SS_MESO_SWIRL_MIN_M and fades
// over a few hundred more. SS_MESO_SWIRL_MAX: unitless ceiling on swirlMag (review_opus.md's own finding:
// unfloored, meso*(SS_MESO_SWIRL_M/rMeso) reaches meso*15 at the floor radius, which would let rotation alone
// dictate flow_w's post-normalize direction with nothing else surviving the renormalize).
const float SS_FLOW_EPS = 0.05;
const float SS_BILLOW_RING_U = 0.55;
const float SS_BILLOW_RING_V = -0.15;
const float SS_BILLOW_UPDRAFT_MAG = 0.6;
const float SS_BILLOW_UPDRAFT_SPREAD = 0.18;
const float SS_ANVIL_UPWIND_CAP = 0.35;
const float SS_ANVIL_STREAK_GAIN = 2.5;
const float SS_MESO_SWIRL_MIN_M = 60.0;
const float SS_MESO_SWIRL_M = 900.0;
const float SS_MESO_SWIRL_MAX = 4.0;

// <SS:Nexii> [interaction: ssdeckflowcore.h] THE CARD'S OWN FRAME, and the per-puff swirl. LOCKSTEP with
// SSDeckFlow::CARD_FLATTEN_LO / CARD_FLATTEN_SPAN / FRAME_EPS / FALLBACK_RIGHT / SWIRL_MAX_RAD - one construction,
// spelled on the CPU in ssvolcloud.cpp's render() (which orients the actual quad) and transliterated here (which
// has to resolve the quad's own p back into world directions). THE FINDING (fifth build report: "the flow map is
// going in reverse direction", "picking from a few different presets which dont mash well together"): this block
// used to build its own frame from `ref = (abs(nrm.z) < 0.95) ? +Z : +X` on the UNFLATTENED view ray, while the
// card it is drawn on was built from a ref BLENDED +Z -> +X across |n.z| in [0.6, 0.95] on a FLATTENED one. The two
// agree only at the ends of that band. Measured over 6552 sky directions (unit_deckflow.cpp frame_regimes): the old
// frame's `right` was the exact NEGATIVE of the card's on 7.7% of them and within 60 degrees of perpendicular on
// 9.6%, with a hard sign flip across the |n.z| = 0.95 cone in 61 of 72 azimuth columns. So the advected octave ran
// outward near the horizon, sideways in one band, and INWARD - against the drift the whole deck travels on - in
// another. SS_SWIRL_MAX_RAD is deliberately under a quarter turn so the swirl rotates the outflow's azimuth and
// can never invert it (worst measured turn cos 0.625).
const float SS_CARD_FLATTEN_LO   = 0.6;
const float SS_CARD_FLATTEN_SPAN = 0.35;
const float SS_FRAME_EPS         = 0.001;
const vec3  SS_FALLBACK_RIGHT    = vec3(0.0, 1.0, 0.0);
const float SS_SWIRL_MAX_RAD     = 0.9;

// <SS:Nexii> D3: SS_OCT_LAPS (laps of the boil cycle per second at full convection) and SS_OCT_DRIFT_FLOOR (the share of that a dead-calm sky keeps - never quite nothing, even still air is not
// static) have MOVED into ssdeckboilcore.h as SSDeckBoil::LAPS_PER_S and SSDeckBoil::DRIFT_FLOOR, because the rate is integrated on the CPU now rather than multiplied by a clock here, and because a
// rate that the build reports ask to retune is not something a shader const can be reasoned about. LAPS_PER_S was retuned with the move: 0.06 -> 0.012, a full advection cycle every 67 s at maximum
// convection instead of every 13 s. This shader reads only the result, ss_boil_laps.

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

// <SS:Nexii> 8g (SSVeil::RIM_FULL, ssveilcore.h): the BASE VEIL's amplitude ceiling AT THE RIM, which sits deliberately above the near-field cap of 0.75 in the density line below. That cap's reason is a near-field reason - a dense flat plane a few tens of metres overhead shows its whole underside at once and reads as a black slab under a gloom-crushed deck - and none of it applies at 8-14 km, where the veil is seen at a grazing angle through the haze and into the dome handoff and is the only cloud left out there. 0.90 and not 1.0 so the shared sun-through fringe survives at the horizon.
const float SS_VEIL_RIM_FULL = 0.90;

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

// <SS:Nexii> [interaction: ssdecknoisecore.h] Atmo Magic far-clouds step 4 (doc/atmo_magic_far_clouds.md section 2 step 4): the deck's noise map tiles every ss_noise_tile metres, so a straight read of it repeats visibly across the 10km horizon - LITERAL twin of SSDeckNoise::detileCoord, the second octave's incommensurate sample coordinate (rotate by DETILE_ROT_RAD, scale by DETILE_SCALE), in the same AIR metres the caller divides by the tile for the first read. Constants LOCKSTEP with ssdecknoisecore.h.
const float SS_NOISE_DETILE_SCALE = 0.6180339887; // LOCKSTEP SSDeckNoise::DETILE_SCALE (1/phi)
const float SS_NOISE_DETILE_ROT   = 0.6457718;    // LOCKSTEP SSDeckNoise::DETILE_ROT_RAD (37 degrees)
const float SS_NOISE_DETILE_WEIGHT = 0.65;        // LOCKSTEP SSDeckNoise::DETILE_WEIGHT - share of the FIRST read in the mix

vec2 ss_noise_detileCoord(vec2 air)
{
    float c = cos(SS_NOISE_DETILE_ROT);
    float s = sin(SS_NOISE_DETILE_ROT);
    return vec2(air.x * c - air.y * s, air.x * s + air.y * c) * SS_NOISE_DETILE_SCALE;
}

// <SS:Nexii> [interaction: ssdecknoisecore.h] LITERAL twin of SSDeckNoise::mixDetile.
float ss_noise_mixDetile(float n1, float n2)
{
    return SS_NOISE_DETILE_WEIGHT * n1 + (1.0 - SS_NOISE_DETILE_WEIGHT) * n2;
}

// <SS:Nexii> [interaction: ssdecknoisecore.h] The de-tiled presence/n_map read shared by all three replicated sites (ss_cell_occupied's own fetch, the sheet presence read, the puff n_map read): the SAME map read twice - first at air/ss_noise_tile as before this existed, second at detileCoord(air)/ss_noise_tile - and mixed by ss_noise_mixDetile. Two overloads so each site keeps the exact texture call it used before: the lod-taking one for ss_cell_occupied's textureLod fetch (a constant-per-cell uv, so implicit derivatives would pick the coarsest mip at cell-wall seams - see that site's own comment), the plain one for the two per-fragment texture() reads.
float ss_noise_mapDetiled(vec2 air, float lod)
{
    float n1 = dot(textureLod(altDiffuseMap, air / ss_noise_tile, lod).rgb, vec3(0.3333));
    float n2 = dot(textureLod(altDiffuseMap, ss_noise_detileCoord(air) / ss_noise_tile, lod).rgb, vec3(0.3333));
    return ss_noise_mixDetile(n1, n2);
}

float ss_noise_mapDetiled(vec2 air)
{
    float n1 = dot(texture(altDiffuseMap, air / ss_noise_tile).rgb, vec3(0.3333));
    float n2 = dot(texture(altDiffuseMap, ss_noise_detileCoord(air) / ss_noise_tile).rgb, vec3(0.3333));
    return ss_noise_mixDetile(n1, n2);
}

// One cell's verdict: 1 the builder put puffs here, 0 it skipped. The presence read is the map at the CELL CENTRE - where the builder sampled - not at this fragment, and through the same hole
// window (0.264/0.530, ss_noise_hole) the builder baked; the gate formula and the coverage comparison are the builder's line for line. LOCKSTEP ssvolcloud.cpp SS_HOLE_LO/HI, SSDeckNoise::HOLE_LO/HI
// - phase 6b retune v2, see that constant's own comment for the occupancy-preserving derivation and the measured before/after table (the de-tile mix narrows the map's value spread, so the window
// had to scale toward the mean by sqrt(varianceRatio(DETILE_WEIGHT)) to keep the same statistical occupancy through the continuous gate, not just the same population beyond a hard edge).
// <SS:Nexii> Phase 4 fixup (F1/#3, doc/atmo_magic_wind_profile.md section 4): this function adds NO frame shift of its own - `c` is an integer cell index handed in by the caller, and `centre`
// below is simply that cell's plain centre in whatever xy frame `c` was hashed from. The chain that makes it correct: main() derives `gate_air = air.xy - hero_shift` (the observer's gate/presence
// coordinate, O(z)-free per the frame contract) and hands gate_air to ss_field_occupancy, which floors it to get `c`; c is therefore already a gate-space cell index, so `centre = (vec2(c)+0.5)*SS_CELL_M`
// is the plain centre of THAT hashed cell in gate space - exactly the point the CPU builder samples for the same cell - and lands on the right texel with no second shift applied here. If a caller
// ever floored shape_air (gate_air with O(z) subtracted) instead, c would be a different cell than the CPU hashed and this comment's claim would be false; there is exactly one caller
// (ss_field_occupancy, called from main() with gate_air) so the invariant holds by inspection, not by anything this function itself enforces.
float ss_cell_occupied(ivec2 c, uint salt)
{
    float gate_raw = ss_cluster_unit(c, salt) * 0.85 + ss_hash_unit(c, 1u + salt) * 0.15;
    float presence = 1.0;
    if (ss_noise_tile > 0.0)
    {
        // textureLod, not texture: this uv is constant across a cell, so implicit derivatives are zero inside a cell and enormous for the pixel quads straddling a cell wall - which would fetch the coarsest mip in a one-pixel seam along every boundary. The CPU gated off a 64-across box-filtered cache, so any fixed low lod is at least as faithful as the implicit one.
        vec2 centre = (vec2(c) + 0.5) * SS_CELL_M;
        // <SS:Nexii> [interaction: ssdecknoisecore.h] De-tiled (doc/atmo_magic_far_clouds.md section 2 step 4) - see ss_noise_mapDetiled.
        float n_map = ss_noise_mapDetiled(centre, 0.0);
        presence = 1.0 - (1.0 - smoothstep(0.264, 0.530, n_map)) * ss_noise_hole; // LOCKSTEP ssvolcloud.cpp SS_HOLE_LO/HI, SSDeckNoise::HOLE_LO/HI (phase 6b retune v2)
    }
    float gate = gate_raw + (1.0 - gate_raw) * (1.0 - presence);
    return (gate <= ss_coverage) ? 1.0 : 0.0;
}

// The gate over the fragment's own air position: the four nearest cells' verdicts, eased bilinearly. The builder's answer is binary per cell and the puffs it places are jittered most of a cell
// wide - so blending verdicts over exactly one cell puts the veil's edge where the outermost puffs of an occupied cell actually reach, soft at the scale a 260m cell is, with no seam at cell walls.
// <SS:Nexii> Phase 4 fixup (F1/#3): `air_xy` MUST be gate_air (hero shift only, O(z)-free), never shape_air - this is what makes the `centre` read inside ss_cell_occupied land on the same cell the
// CPU builder hashed; see ss_cell_occupied's own comment for the chain. The one call site (main(), the sheet path) already passes gate_air.
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

// <SS:Nexii> S3 flow-field helpers (doc/atmo_magic_flow_field.md section 1 "Flow fields"; LITERAL port of
// V:\Scratch\atmo\flow\proto.h's ssflow::rot90/safeDir - the same two idioms the CPU ladder proved out over
// five rounds of visual tests, transliterated here rather than re-derived, so the shipped math carries the
// same singularity guarantees the prototypes were judged on).
// ss_rot90: 90-degree rotation of a 2-D vector - which way it turns is picked per caller via a signed weight
// (see each call site below), exactly as proto.h's own comment states.
vec2 ss_rot90(vec2 v) { return vec2(-v.y, v.x); }

// ss_safe_dir: "direction that fades out near zero" - v/(eps+|v|). Not a unit vector (magnitude ramps from 0
// toward 1 as |v| grows past eps) but always finite, including at v=(0,0), so it stands in for normalize() at
// any point that can pass through or sit near a singularity (a storm's own centre, a fragment exactly on the
// meso axis) without a hard normalize()-of-zero branch anywhere below.
vec2 ss_safe_dir(vec2 v) { return v * (1.0 / (SS_FLOW_EPS + length(v))); }

// <SS:Nexii> [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::influence: the radial influence of a storm cell at world point p - 1 inside INFLUENCE_CORE (0.4) of the radius, 0 at and beyond it, 0 whenever the cell is disabled (radius <= 0). LOCKSTEP with SSStormCell::influence and the CPU twin; the phase-3 twin test (V:\Scratch\atmo\tests\twin_stormcouple.cpp) transliterates this text and asserts equality over a grid.
float ss_storm_influence(vec2 c, float radius, vec2 p)
{
    if (radius <= 0.0)
    {
        return 0.0;
    }
    float dist = length(p - c);
    return 1.0 - smoothstep(radius * 0.4, radius, dist);
}

// <SS:Nexii> [interaction: ssstormcouplecore.h] What the field reads at one world point - LITERAL twin of SSStormCouple::Sample.
struct SSStormSample
{
    float boost;
    float anvil;
    float overshoot;
    float mammatus;
    float meso;
    vec2  dir;
    float rotSign;
    float owner;
    // <SS:Nexii> S3 G3/G5 (doc/atmo_magic_flow_field.md section 2 stage 3, review_opus.md's A-S0 verdict "A-W-FIX
    // ... ownerC is genuinely free inside the existing owner_idx block"): the owner cell's own centre, read
    // alongside dir/rotSign/owner in the SAME owner_idx branch below - free because ss_storm_a[owner_idx].xy is
    // already-resident uniform data, no new upload, no new CPU work. This is what lets the anvil outflow and
    // meso swirl below read a fragment's true offset from the storm cell that owns it, rather than the whole
    // coupled field's influence-weighted MAX (boost/anvil/overshoot/mammatus above), which has no single
    // "centre" a MAX combine could point away from. See ss_storm_samplePoint's own comment for why ownerRadius
    // is deliberately NOT added here even though it would be just as free to read.
    vec2  ownerC;
    float lineWall;  // 8a: [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::Sample::lineWall
    float lineShelf; // 8a: LITERAL twin of SSStormCouple::Sample::lineShelf
};

// <SS:Nexii> [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::sampleAt over the ss_storm_a/b/c uniform arrays: boost/anvil/overshoot/mammatus combine by MAX of influence*value; meso/dir/rotSign come from the single OWNER cell (largest influence here; ties broken by lower index so the result is order-stable, matching the CPU loop's strict > comparison).
SSStormSample ss_storm_sampleAt(vec2 p)
{
    SSStormSample s;
    s.boost = 0.0;
    s.anvil = 0.0;
    s.overshoot = 0.0;
    s.mammatus = 0.0;
    s.meso = 0.0;
    s.dir = vec2(0.0);
    s.rotSign = 0.0;
    s.owner = 0.0;
    s.ownerC = vec2(0.0); // S3: off-cell default, same shape as the other owner_idx-only fields above
    s.lineWall = 0.0;  // 8a: set by ss_line_apply at the call site, never here - ss_storm_sampleAt knows no line
    s.lineShelf = 0.0;

    float owner_inf = 0.0;
    int owner_idx = -1;
    int count = clamp(ss_storm_n, 0, SS_STORM_MAX_CELLS);
    for (int i = 0; i < SS_STORM_MAX_CELLS; ++i)
    {
        if (i >= count)
        {
            break;
        }
        float radius = ss_storm_a[i].z;
        if (radius <= 0.0)
        {
            continue;
        }
        float inf = ss_storm_influence(ss_storm_a[i].xy, radius, p);
        s.boost     = max(s.boost,     inf * ss_storm_a[i].w);
        s.anvil     = max(s.anvil,     inf * ss_storm_b[i].x);
        s.overshoot = max(s.overshoot, inf * ss_storm_b[i].z);
        s.mammatus  = max(s.mammatus,  inf * ss_storm_b[i].w);
        if (inf > owner_inf)
        {
            owner_inf = inf;
            owner_idx = i;
        }
    }
    if (owner_idx >= 0)
    {
        s.meso = owner_inf * ss_storm_b[owner_idx].y;
        s.dir = ss_storm_c[owner_idx].xy;
        s.rotSign = ss_storm_c[owner_idx].z;
        s.owner = owner_inf;
        s.ownerC = ss_storm_a[owner_idx].xy; // S3: same already-resident uniform the influence test above just read .z (radius) from
    }
    return s;
}

// <SS:Nexii> 8a item 2 [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::LineField.
struct SSLineField
{
    float wall;
    float shelf;
};

// <SS:Nexii> 8a item 2 [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::lineField over the
// ss_line_a/b/c uniforms: ss_line_a.xy/zw is b.ox/oy/dirX/dirY, ss_line_b.xyzw is b.motX/motY/halfLen/bandM,
// ss_line_c.xy is b.shelfM/strength - field for field, matching SSStormCouple::LineBand's own layout comment.
// Twin test transliterates this text.
SSLineField ss_line_field(vec2 p)
{
    SSLineField f;
    f.wall = 0.0;
    f.shelf = 0.0;
    float strength = ss_line_c.y;
    float bandM = ss_line_b.w;
    if (strength <= 0.0 || bandM <= 0.0)
    {
        return f;
    }
    float dx = p.x - ss_line_a.x;
    float dy = p.y - ss_line_a.y;
    float along = dx * ss_line_a.z + dy * ss_line_a.w;
    float perp = sqrt(max(dx * dx + dy * dy - along * along, 0.0));
    float ahead = dx * ss_line_b.x + dy * ss_line_b.y;
    float halfLen = ss_line_b.z;
    float shelfM = ss_line_c.x;
    float endcap = 1.0 - smoothstep(halfLen, halfLen + bandM, abs(along));
    f.wall = strength * endcap * (1.0 - smoothstep(0.5 * bandM, bandM, perp));
    f.shelf = strength * endcap * smoothstep(0.0, 0.3 * shelfM, ahead) * (1.0 - smoothstep(0.6 * shelfM, shelfM, ahead));
    return f;
}

// <SS:Nexii> 8a item 2 [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::applyLineBand: boost/anvil
// take MAX with the wall term, lineWall/lineShelf record it directly, every other field of s untouched. anvilFrac
// is passed at the call site (SSSquall::LINE_ANVIL_FRAC, 0.8), mirroring the CPU signature exactly.
void ss_line_apply(inout SSStormSample s, SSLineField f, float anvilFrac)
{
    s.boost = max(s.boost, f.wall);
    s.anvil = max(s.anvil, f.wall * anvilFrac);
    s.lineWall = f.wall;
    s.lineShelf = f.shelf;
}

// <SS:Nexii> [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::towerWindow: the deck's baked tower window, blended toward the storm's own (0.12..0.60) by boost.
void ss_storm_towerWindow(float lo, float hi, float boost, out float loOut, out float hiOut)
{
    float b = clamp(boost, 0.0, 1.0);
    loOut = lo + (0.12 - lo) * b;
    hiOut = hi + (0.60 - hi) * b;
}

// <SS:Nexii> [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::towerFromMap: the tower weight of a raw map sample n under the (possibly storm-widened) window.
float ss_storm_towerFromMap(float n, float lo, float hi, float boost)
{
    float loP, hiP;
    ss_storm_towerWindow(lo, hi, boost, loP, hiP);
    return smoothstep(loP, hiP, n);
}

// <SS:Nexii> [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::anvilWeight: the fragment's anvil weight folding in the storm's own anvil term alongside the existing deck-anvil / convection-tower max.
float ss_storm_anvilWeight(float deckAnvil, float convection, float tower, float stormAnvil)
{
    return max(max(deckAnvil, smoothstep(0.40, 0.70, convection) * tower), stormAnvil);
}

// <SS:Nexii> [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::overshootBonusM (review 3b NEW-4): the overshooting-top height bonus the CPU adds to a coupled cell's tallest sub-puff only - 0.35 is OVERSHOOT_HEIGHT_FRAC, the same share of the deck's own thickness at overshoot 1.
float ss_storm_overshootBonusM(float thicknessM, float overshoot)
{
    return 0.35 * max(thicknessM, 0.0) * clamp(overshoot, 0.0, 1.0);
}

// <SS:Nexii> [interaction: ssstormcouplecore.h] LITERAL twin of SSStormCouple::lidTopM (review 3b NEW-4, doc/atmo_magic_storm_dynamics.md section 3 "Overshooting top vs the lid cut"): the effective lid altitude for the lid cut and cap band below, in place of a bare top_z, so the lid rises to meet the sub-puff the CPU actually lifted rather than clipping it. Sampled with the same quantized-cell storm_sample the density carve above already reads (ss_storm_samplePoint), never world_true.xy, so the lid only rises where the CPU lifted the puff for THIS cell.
float ss_storm_lidTopM(float topZ, float thicknessM, float overshoot)
{
    return topZ + ss_storm_overshootBonusM(thicknessM, overshoot);
}

// <SS:Nexii> [interaction: ssstormcouplecore.h] S5 sample point lockstep (doc/atmo_magic_storm_dynamics.md section 3, Sample point lockstep bullet): quantizes a fragment's air-frame position to its 260m cell CENTRE, the same lattice ss_cell_occupied already snaps to, before the storm field is sampled, so a fragment reads the storm sample the builder used for ITS CELL - matching the CPU's own cell-centre sample point (buildDeck: world_x/world_y = (cx + 0.5) * CELL_M + drift; floor(air / SS_CELL_M) recovers cx/cy from the air-frame position exactly as this helper does). Honest bound, stated rather than hidden: agreement is per CELL, not per fragment - a puff's quad can straddle a cell boundary (placement jitter, radius), so a fragment can sit in one cell while the puff drawing over it was placed and shaped from a neighbouring cell's sample; carrying the owning cell on a vertex channel would close that gap and is deferred (design doc, Sample point lockstep residual). Not a struct field: ownerRadius is CPU-only (precipShift's cap) and stays out of the GLSL SSStormSample.
// <SS:Nexii> S3 update (doc/atmo_magic_flow_field.md section 2 stage 3, review_opus.md's A-S0 verdict): ownerC
// (the owner cell's centre) WAS added to SSStormSample above - it is genuinely free, the same already-resident
// ss_storm_a[owner_idx] read this function's own influence test uses. ownerRadius above stays OUT on purpose,
// not by oversight: nothing below reads a per-fragment distance-to-owner-radius ratio (the anvil outflow and
// meso swirl only need a DIRECTION and a MAX-combined magnitude - ss_storm_influence's own smoothstep already
// supplies the radius-relative falloff through storm_sample.owner), so adding it would be a field with no
// reader, the exact opposite of ownerC's justification.
vec2 ss_storm_samplePoint(vec2 air)
{
    return (floor(air / SS_CELL_M) + 0.5) * SS_CELL_M + ss_drift;
}

// <SS:Nexii> [interaction: ssdeckcellsoftcore.h] LITERAL twin of SSDeckCellSoft::heroInfluenceSoft, and THE FIX for the sixth build report's two artefacts ("hard stair-stepped rectangular tiling across the whole cloud field" top-down; "large hard-edged grey wedges" from deck altitude). What stood here was `ss_storm_influence(ss_hero_c, ss_hero.w, storm_p)` on the QUANTIZED point, so the hero's influence - and therefore ss_frame_heroShift, and therefore gate_air, the coordinate the veil's mottle, its presence cut, its ss_field_occupancy gate, the puffs' shape_air and the virga body/streak reads are ALL taken in - was piecewise constant per 260 m cell and STEPPED at every cell wall. Measured (V:\Scratch\atmo\tests\unit_deck_cellsoft.cpp, hero r 2076 as photographed): the frame slid 95.94 m across one wall, unchanged at a tenth of the sampling separation (ratio 1.001 - a step, not a slope), which is 0.37 of a whole occupancy cell, and it moved the veil's own alpha by 0.188 across one pixel. The step lives only in the influence FALLOFF RING (0.4r..r; inside that the influence saturates at 1 and the shift is constant, outside it is 0), which is exactly the annulus the screenshots show - stair-stepped, world-axis-aligned, centred on the HERO rather than on the camera, and blown up into screen-filling wedges when the eye sits in the veil's own plane and one cell subtends a fan to the horizon.
// <SS:Nexii> Why the blend rather than simply dropping the quantization: the quantization is ssdeckframecore.h's producer/observer rule, not an accident - the CPU builder evaluates this influence ONCE PER CELL at that cell's own centre and the observer must recover the same number or it reads a pattern the puff was never built from. This stencil keeps that: t is exactly 0 at a cell centre, so the blend collapses to that one cell's value BIT-IDENTICALLY (unit_deck_cellsoft.cpp checks all 3721 centres of the ring), and interpolates only between the points the producer never evaluated. It is also the idiom this file already uses one step downstream - ss_field_occupancy softens the builder's binary per-cell gate over this same lattice, for this same reason. Note the two cell indices are deliberately different: ss_storm_samplePoint floors air/CELL (the CONTAINING cell), this floors air/CELL - 0.5 (the lower-left of the four cells whose CENTRES bracket the point), which is the only one that can interpolate.
// <SS:Nexii> Bounded, stated: the blend's own slope is at most 1.5/CELL_M per cell of influence change times the displacement cap, measured at |d gate_air / d air| = 1.527, i.e. |d shift / d air| = 0.527 < 1 - so the observer's frame stays a fold-free map of the world and the pattern can never double back on itself. The storm SAMPLE (boost/anvil/meso/dir) is still read at the quantized point on purpose and is NOT softened here: those are per-cell amplitudes the producer genuinely built each cell's puffs from, and blending them would need four full ss_storm_sampleAt loops. Their residual steps are measured in unit_deck_cellsoft.cpp and reported, not hidden.
float ss_hero_influenceSoft(vec2 air)
{
    if (ss_hero.w <= 0.0)
    {
        return 0.0;
    }
    vec2 q = air / SS_CELL_M - 0.5;
    vec2 i = floor(q);
    vec2 t = q - i;
    t = t * t * (3.0 - 2.0 * t);
    vec2 c0 = (i + 0.5) * SS_CELL_M + ss_drift;
    float i00 = ss_storm_influence(ss_hero_c, ss_hero.w, c0);
    float i10 = ss_storm_influence(ss_hero_c, ss_hero.w, c0 + vec2(SS_CELL_M, 0.0));
    float i01 = ss_storm_influence(ss_hero_c, ss_hero.w, c0 + vec2(0.0, SS_CELL_M));
    float i11 = ss_storm_influence(ss_hero_c, ss_hero.w, c0 + vec2(SS_CELL_M, SS_CELL_M));
    return mix(mix(i00, i10, t.x), mix(i01, i11, t.x), t.y);
}

// <SS:Nexii> F2 (2026-09-06 review) [interaction: ssvirgacore.h]: LITERAL twin of SSVirga::embedAlpha - 1 at or
// below baseZ, falling linearly to 0 at topZ (the embedded stack's top, ssvirgacore.h's embedTopZ), degenerate
// topZ <= baseZ stepping from 1 to 0 exactly at baseZ. This is the ONLY place the embed fade is evaluated now: the
// CPU no longer bakes one alphaMul per whole card (that never let the fade ramp WITHIN a card - a card can span
// most of the embedded span, so one CardGeom-mid-height sample either over- or under-faded most of its own
// pixels); this reads world_true.z per fragment instead, so the curtain visibly emerges from inside the cloud
// pixel by pixel rather than in per-card steps.
float ss_virga_embedAlpha(float z, float baseZ, float topZ)
{
    if (z <= baseZ) return 1.0;
    float span = topZ - baseZ;
    if (span <= 1.0e-6) return 0.0;
    return clamp(1.0 - (z - baseZ) / span, 0.0, 1.0);
}

// <SS:Nexii> [interaction: ssairlightcore.h SSAirlight::softClipUnit] THE DECK'S OUTPUT RESPONSE, transliterated - the core is the AUTHORITY here and this is the transliteration, pinned bit-identical by V:\Scratch\atmo\tests\twin_deck_softclip.cpp. Replaces the hard `clamp(colour, vec3(0.0), vec3(1.0))` this shader used to end on (see the long note at `shaded` in main() for the finding: EEP's sunlight and ambient run well past 1, the sum saturated, and a 92% cut to the sun body term - SSAtmoCloudLightVariant's POWDER - landed on the same clipped pixel, so the A/B dial showed nothing in daylight). The curve is the compression half of PBRNeutralToneMapping (Khronos Neutral, app_settings/shaders/class1/deferred/tonemapUtilF.glsl - the tonemapper every non-sky pixel in this frame already passes through): identity below the knee, a hyperbola through (K, K) with slope 1 there and asymptotic to 1 from below, so it is C1 at the join, strictly increasing everywhere (no input range is ever flattened) and strictly UNDER 1 for every finite input - which is what keeps the following `* 2.0` inside exactly the range the clamp occupied, so the bloom bright-pass the clamp was introduced for sees nothing new.
// <SS:Nexii> The stock TOE (`offset = x < 0.08 ? x - 6.25 * x * x : 0.04; color -= offset`) is deliberately NOT transliterated: it is a black-level control that subtracts up to 0.04, which is 8% of this shader's doubled output, and the requirement on this curve was that DARK cloud not move. Below the knee this function is the identity, bit for bit, exactly as the clamp was. Compression runs on the colour's own PEAK, so the hue is carried through rather than clipped - the per-channel clamp desaturated anything past 1 toward white, which is why bright deck stopped matching the sky (skyF.glsl caps the peak, hue-preserving, and says so).
// <SS:Nexii> Ceiling 1 is folded out rather than passed: the core's `inv = 1/ceiling` and its closing `scale(..., ceiling)` are both multiplications by exactly 1.0 at ceiling 1, which IEEE754 makes the identity, so omitting them is bit-identical and not an approximation. The two constants are SSAirlight::SOFT_CLIP_KNEE (written 0.8 - 0.04 so the pin against tonemapUtilF.glsl's `startCompression` stays textual) and SSAirlight::SOFT_CLIP_DESAT.
const float SS_SOFT_CLIP_KNEE  = 0.8 - 0.04;
const float SS_SOFT_CLIP_DESAT = 0.15;

vec3 ss_soft_clip_unit(vec3 colour)
{
    vec3 c = max(colour, vec3(0.0));
    float raw_peak = max(c.r, max(c.g, c.b));
    if (raw_peak < SS_SOFT_CLIP_KNEE)
    {
        return c;
    }
    vec3 x = c;
    float peak = max(x.r, max(x.g, x.b));
    float d = 1.0 - SS_SOFT_CLIP_KNEE;
    float newPeak = 1.0 - d * d / (peak + d - SS_SOFT_CLIP_KNEE);
    x *= newPeak / peak;
    float g = 1.0 - 1.0 / (SS_SOFT_CLIP_DESAT * (peak - newPeak) + 1.0);
    return mix(x, vec3(newPeak), g);
}

// <SS:Nexii> S4 HG2 variant (research_lighting.md #3, "Henyey-Greenstein two-lobe silver-lining term"), ONE FORMULA SITE: single-lobe HG, literal port of scene.h's hgPhase - the RAW form
// (1-g^2)/(4*pi*(1+g^2-2*g*c)^1.5), normalized only by its own physical 4*pi solid-angle constant, NOT re-normalized to peak at 1 (SS_HG2_GAIN above is where the amplitude gets bounded instead,
// against the OLD rim term's own peak, not against this function's own range). Finite and bounded for every g in (-1,1) and c in [-1,1]: the denominator's base 1+g^2-2*g*c >= (1-|g|)^2 > 0 there,
// so it never divides by zero or blows up. Spent as phase = mix(ss_hg2_phase(c,-0.2), ss_hg2_phase(c,0.8), 0.55), the same backward/forward blend and weight scene.h's V_HG2 uses.
float ss_hg2_phase(float c, float g)
{
    float g2 = g * g;
    float denom = pow(max(1.0 + g2 - 2.0 * g * c, 1.0e-4), 1.5);
    return (1.0 - g2) / (4.0 * 3.14159265 * denom);
}

// <SS:Nexii> Phase 8e item 1 (doc/atmo_magic_phase8_show.md section 3b, ONE FORMULA SITE): the fake-sphere normal
// reconstruction, factored out of the puff branch below so the shaft branch (item 2) can call the SAME function
// rather than a second, hand-copied inline block. nrm is the fragment's reconstructed view-ray direction (the
// puff/shaft's own facing frame); p is the point on the quad, in [-1,1] per axis, whose implied third (depth)
// coordinate this reconstructs - for the puff that is its own texcoord-derived p, for the shaft it is
// vec2(u, -droop) (see the shaft branch). ref/tan_u/tan_v/r/the final normalize are the SAME statements in the
// SAME order the puff branch always evaluated inline, so the puff's own result is bit-identical to before this
// factoring - only the shaft branch is new behaviour.
vec3 ss_sphere_normal(vec3 nrm, vec2 p)
{
    vec3 ref = (abs(nrm.z) < 0.95) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tan_u = normalize(cross(ref, nrm));
    vec3 tan_v = cross(nrm, tan_u);
    float r = length(p);
    return normalize(tan_u * p.x + tan_v * p.y + nrm * sqrt(max(1.0 - r * r, 0.0)));
}

// <SS:Nexii> [interaction: ssdeckflowcore.h] LITERAL twin of SSDeckFlow::cardFrame - the SAME statements in the
// SAME order as the core's body, which is itself the CPU block ssvolcloud.cpp's render() orients the quad with.
// This is the frame a fragment must resolve its own p into: `right`/`up` are the directions the quad's own corners
// were laid out along, so `right * p.x + up * p.y` is the fragment's true world offset from the puff's centre (up
// to the card's 1.7:0.62 aspect, which p-space ignores here exactly as shape/rim/ss_sphere_normal already do). See
// the SS_CARD_FLATTEN_LO block above for what the frame this replaces did instead. NOT called by
// ss_sphere_normal: the fake sphere's own frame is unchanged on purpose, so the lighting on this build is
// untouched and twin_virga.cpp's verbatim text pin on ss_sphere_normal still holds - only the flow moves.
void ss_card_frame(vec3 to_cam, out vec3 card_n, out vec3 card_right, out vec3 card_up)
{
    float ss_cf_l = length(to_cam);
    vec3 ss_cf_n = (ss_cf_l > SS_FRAME_EPS) ? to_cam / ss_cf_l : vec3(0.0, 0.0, 1.0);
    float ss_cf_flat = clamp((abs(ss_cf_n.z) - SS_CARD_FLATTEN_LO) / SS_CARD_FLATTEN_SPAN, 0.0, 1.0);
    if (ss_cf_flat > 0.0)
    {
        float ss_cf_sgn = (ss_cf_n.z >= 0.0) ? 1.0 : -1.0;
        vec3 ss_cf_blend = ss_cf_n * (1.0 - ss_cf_flat) + vec3(0.0, 0.0, ss_cf_sgn) * ss_cf_flat;
        float ss_cf_bl = length(ss_cf_blend);
        ss_cf_n = (ss_cf_bl > SS_FRAME_EPS) ? ss_cf_blend / ss_cf_bl : vec3(0.0, 0.0, ss_cf_sgn);
    }
    card_n = ss_cf_n;

    vec3 ss_cf_ref = vec3(0.0, 0.0, 1.0) * (1.0 - ss_cf_flat) + vec3(1.0, 0.0, 0.0) * ss_cf_flat;
    float ss_cf_rl = length(ss_cf_ref);
    ss_cf_ref = (ss_cf_rl > SS_FRAME_EPS) ? ss_cf_ref / ss_cf_rl : vec3(0.0, 0.0, 1.0);

    vec3 ss_cf_r = cross(ss_cf_ref, ss_cf_n);
    float ss_cf_crl = length(ss_cf_r);
    card_right = (ss_cf_crl > SS_FRAME_EPS) ? ss_cf_r / ss_cf_crl : SS_FALLBACK_RIGHT;
    card_up = cross(ss_cf_n, card_right);
}

// <SS:Nexii> [interaction: ssdeckflowcore.h] LITERAL twin of SSDeckFlow::rotate2 - the per-puff swirl's azimuth
// turn, applied to the QUAD POINT before the billow's radial mapping so it stays in the card's own plane at every
// view angle. The meridional field itself depends only on |p| (the ring cores and the updraft are functions of r
// alone), so this rotates the outflow's direction and changes nothing else about it.
vec2 ss_rot2(vec2 p, float ang)
{
    float c = cos(ang);
    float s = sin(ang);
    return vec2(c * p.x - s * p.y, s * p.x + c * p.y);
}

// <SS:Nexii> [interaction: ssdeckframecore.h] THE one lerp formula both sides use, spelled out rather than called via mix() - LOCKSTEP with SSDeckFrame::lerpExact. GLSL's mix(x, y, a) = x*(1-a) + y*a
// rounds differently in floating point than a + (b - a)*t, which is exactly why this shader spells the formula out instead of calling mix(): every table read built on this (ss_frame_shearTableAt,
// ss_frame_frameAir below) is bit-exact via lerpExact, pinned by twin_deckframe - never a near twin, never "about 2mm apart".
vec2 ss_frame_lerpExact(vec2 a, vec2 b, float t)
{
    return a + (b - a) * t;
}

// <SS:Nexii> [interaction: ssdeckframecore.h] Bit-exact via lerpExact, pinned by twin_deckframe (see ss_frame_lerpExact above), of SSDeckFrame::shearTableAt - the ONLY formula this shader mirrors for O(z)
// (see the core header's REPLICATION STRATEGY note: the GPU never evaluates the wind profile itself, only this lerp over the CPU-baked table). Same clamp/floor/lerp order as the core: t is 0 with a
// degenerate span, i clamped to N - 2 so z >= z1 returns table[N - 1] within one ulp (not bit-exactly, per the core's own stated invariant), frac lerps the remainder.
vec2 ss_frame_shearTableAt(float z)
{
    float span = ss_shear_z.y - ss_shear_z.x;
    float t = (span > 0.0) ? (clamp((z - ss_shear_z.x) / span, 0.0, 1.0) * float(SS_SHEAR_TABLE_N - 1)) : 0.0;
    int i = int(floor(t));
    i = clamp(i, 0, SS_SHEAR_TABLE_N - 2);
    float frac = t - float(i);
    return ss_frame_lerpExact(ss_shear_table[i], ss_shear_table[i + 1], frac);
}

// <SS:Nexii> [interaction: ssdeckframecore.h] Revised 2026-09-05, superseding the old F4/#10 fixup below - that fixup had the rule backwards and the corrected core header (ssdeckframecore.h,
// "Revised 2026-09-05 after the phase-4 opus review found the first contract unsound") says so explicitly: bit-exact twin of SSDeckFrame::frameAir (bit-exact, because it is built on
// ss_frame_shearTableAt's bit-exact lerpExact above, pinned by twin_deckframe) - gate_air with O(z) subtracted on top. This is the OBSERVER coordinate for EVERY pattern read on the puff path: the n_map column read (tower window, anvil, floor_z, fill),
// the detail octaves, the triplanar skew, the cap band's noise - doc/atmo_magic_wind_profile.md section 4 "Frame contract" lists all of them together for a reason: the CPU places a puff of column
// c at c + O(z_puff) (placeWorld) but classifies that column - gate hash, n_map - from the UNSHIFTED c, so an observer reading the column back from the placed puff's world position must undo O(z)
// too, or it lands on whichever column the lean happened to drop it over (the old F4/#10 draft's failure: it read n_map at gate_air, up to 2.5 km from the column that actually grew the puff - see
// ssdeckframecore.h's header note, "The 4b draft that read n_map at gateAir was inverted"). The cell gate and any PRESENCE read are the one exception, and only because they are base-anchored BY
// DEFINITION (the sheet, the shadow bake, precipNoiseAt): those reads never call this function at all, not because O(z0) happens to be 0 at the deck floor - the sheet itself is drawn 46-106 m
// above z0, where a storm profile's O is already 90-210 m, so gate_air and this function's result genuinely diverge there; "O(z0) == 0 so it reduces to a no-op" was the old, wrong framing - see
// the sheet path's own comments, which read gate_air directly because the sheet is base-anchored, not because the two coordinates happen to agree.
vec2 ss_frame_frameAir(vec2 gateAirXY, float z)
{
    return gateAirXY - ss_frame_shearTableAt(z);
}

// LOCKSTEP SSDeckFrame::HERO_SHIFT_RADIUS_FRAC / HERO_SHIFT_CAP_M (doc/atmo_magic_wind_profile.md section 4, ssdeckframecore.h heroShiftCapM): the displacement cap on the hero's local frame
// shift, a share of the hero's own influence radius or this absolute ceiling, whichever is smaller. Bounded in DISPLACEMENT, not merely rate - a rate clamp alone let a 70-minute-old hero's shift
// reach 12km against a 2km radius (phase-4 review finding), at which point the cells the pattern was read from and the cells the shifted content lands on are disjoint. This cap is what keeps the
// falloff ring's shear under about half a cell - see the core header's derivation.
const float SS_HERO_SHIFT_RADIUS_FRAC = 0.15;
const float SS_HERO_SHIFT_CAP_M       = 390.0;

// <SS:Nexii> [interaction: ssdeckframecore.h] LITERAL twin of SSDeckFrame::heroShift: d is the hero's motion less the applier's drift rate, clamped to HERO_SLIDE_MAX_MS (3 m/s, the artistic clamp)
// in magnitude while keeping direction; raw = d * ageS is then capped in DISPLACEMENT at heroShiftCapM(radius) (SS_HERO_SHIFT_RADIUS_FRAC/SS_HERO_SHIFT_CAP_M above, direction kept) BEFORE the
// influence weight is applied - same order as the core (cap first, then multiply by influence), which is what makes the shift saturate with age instead of growing without bound. influence is the
// SAME ss_storm_influence this file already carries, evaluated by the caller at the UNSHIFTED quantized cell point (see main()'s frame block below), never a second copy here.
vec2 ss_frame_heroShift(float influence)
{
    vec2 d = ss_hero.xy - ss_drift_vel;
    float len = length(d);
    const float SS_HERO_SLIDE_MAX_MS = 3.0;
    if (len > SS_HERO_SLIDE_MAX_MS)
    {
        d *= SS_HERO_SLIDE_MAX_MS / len;
    }
    vec2 raw = d * ss_hero.z; // ss_hero.z is the hero's age in seconds
    float mag = length(raw);
    float cap = max(0.0, min(SS_HERO_SHIFT_RADIUS_FRAC * max(ss_hero.w, 0.0), SS_HERO_SHIFT_CAP_M)); // ss_hero.w is the hero's influence radius
    if (mag > cap)
    {
        raw *= (mag > 0.0) ? (cap / mag) : 0.0;
    }
    return raw * influence;
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

    // <SS:Nexii> Phase 4, revised 2026-09-05 (doc/atmo_magic_wind_profile.md section 4 "Frame contract", doc/atmo_magic_storm_dynamics.md section 3 "Storm motion vs cloud drift" and "Sample point
    // lockstep") [interaction: ssdeckframecore.h]: computed ONCE, shared by both the puff and sheet paths below. OBSERVER QUANTIZATION (ssdeckframecore.h's own header note of that name): the storm
    // field and the hero's influence weight must be read at a quantized point with O(z) already removed, O FIRST because O does not depend on influence and the CPU producer evaluated influence at
    // the UN-LEANED column (samplePointM before the gate; heroShift derived from that unshifted influence; placeWorld's O(z) lean only after) - quantizing from the leaned air instead (the old
    // draft's failure) puts a high puff's sample up to 2.5 km from its producer cell and outside the hero entirely. So: o = O(world_true.z) is computed FIRST, and storm_p quantizes from
    // air.xy - o for the puff path. The SHEET path is the one deliberate exception, not an oversight: the veil is base-anchored BY DEFINITION - it never evaluates O(z) at all, however high above
    // the floor it is actually drawn (the sheet sits 46-106 m above z0, where O is already 90-210 m into a storm profile, so "O(z0) == 0 reduces it to a no-op" is false; see ss_frame_frameAir's own
    // comment and the sheet path's below) - so a sheet fragment's storm_p quantizes from air.xy DIRECTLY, o never subtracted. The ternary below is that sheet/puff distinction made explicit, one
    // storm_p formula per path rather than a shared approximation of both. The storm sample itself stays at storm_p on BOTH paths, never routed through gate_air or shape_air, because it is the
    // coupling field, not a pattern read. gate_air (hero shift only, O(z)-free) feeds ONLY the base-anchored reads. Two different reasons put them there, not one: the shadow bake and precipNoiseAt
    // genuinely sit at the deck floor, where O(z0) == 0 by construction, so gate_air already IS the frame-air answer there and no special case is needed; the sheet's presence cut, its own mottle
    // and its cell gate are base-anchored BY DEFINITION instead - the veil is drawn 46-106 m above z0, where O(z) is not zero, so gate_air and shape_air genuinely diverge there and the sheet simply
    // never routes through shape_air/frameAir at all, whatever the divergence is at its actual altitude (see the sheet path's own comments). shape_air is removed from the sheet path entirely.
    // shape_air = gate_air - o (hero shift AND the wind profile's O(z) on top) feeds EVERY puff-path pattern read - detail octaves,
    // triplanar skew, the cap band's noise, AND (as of the 2026-09-05 revision) the n_map column read that drives the tower window, anvil, floor_z and thick-base fill: the old F4/#10 fixup read
    // that column at gate_air and had the rule backwards (see ss_frame_frameAir's own comment and the core header's note on the inverted 4b draft) - see the puff-path call site below for the
    // corrected read. Off a hero-less, table-less deck ss_frame_heroShift returns the zero vector and ss_frame_shearTableAt's lerpExact returns (0,0) bit-exactly (pinned by twin_deckframe), so
    // gate_air == shape_air == air.xy bit-identical to before this existed.
    vec2 o = ss_frame_shearTableAt(world_true.z); // O(z) at this fragment's own altitude - removed FIRST, before either path's storm_p quantizes, per OBSERVER QUANTIZATION above.
    // <SS:Nexii> [interaction: ssdeckcellsoftcore.h] The observer's own un-quantized frame point, named once and spent twice below - the storm SAMPLE still floors it to its cell (those are per-cell amplitudes; see ss_hero_influenceSoft's own comment), the hero's INFLUENCE no longer does. sheet: base-anchored, O never applied. puffs: the un-leaned air.xy - o.
    vec2 frame_air = (ss_sheet > 0.5) ? air.xy : (air.xy - o);
    vec2 storm_p = ss_storm_samplePoint(frame_air);
    SSStormSample storm_sample = ss_storm_sampleAt(storm_p);
    // <SS:Nexii> 8a item 2 (doc/atmo_magic_phase8_show.md section 3): the squall line's wall/shelf folded in right
    // after the discrete-cell sample, at the SAME storm_p - the shader's one call site for both, matching the CPU
    // builder's own call-site order. 0.8 is SSSquall::LINE_ANVIL_FRAC, LOCKSTEP the same way ss_storm_influence's
    // 0.4 above is SSStormCouple::INFLUENCE_CORE.
    ss_line_apply(storm_sample, ss_line_field(storm_p), 0.8);
    // <SS:Nexii> [interaction: ssdeckcellsoftcore.h] ss_hero_influenceSoft(frame_air), NOT ss_storm_influence(..., storm_p): the frame every base-anchored read below is taken in has to be CONTINUOUS, and the quantized read made it a 260 m staircase - see ss_hero_influenceSoft's own comment for the finding, the numbers and why this still recovers the builder's per-cell value exactly at every cell centre.
    float hero_inf = ss_hero_influenceSoft(frame_air);
    vec2 hero_shift = ss_frame_heroShift(hero_inf);
    vec2 gate_air = air.xy - hero_shift;
    vec2 shape_air = gate_air - o; // EVERY puff-path pattern read - see this block's own comment for what takes this coordinate and why; the sheet path never reads it.

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
        // <SS:Nexii> Phase 4, revised 2026-09-05 [interaction: ssdeckframecore.h]: gate_air, not shape_air - the sheet is BASE-ANCHORED BY DEFINITION (ssdeckframecore.h, doc/atmo_magic_wind_profile.md
        // section 4 "Frame contract"): every sheet read, the mottle included, takes the hero's rigid shift only and never the wind profile's O(z), regardless of how high above the deck floor the
        // veil is actually drawn. This is a definition, not a coincidence of altitude - "the sheet sits at z0 where O(z0) == 0 so it reduces to a no-op" was the OLD, WRONG framing: the veil is
        // drawn 46-106 m above z0, where a storm profile's O is already 90-210 m, so shape_air and gate_air genuinely differ there and using shape_air was a real bug, not a redundant read of the
        // same value. See main()'s frame block and ss_frame_frameAir's own comment for the full base-anchored/pattern-read split; shape_air is not read anywhere in this sheet path.
        vec2 suv = gate_air / SS_SHEET_TILE_M;
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
            // <SS:Nexii> Phase 4, revised 2026-09-05 [interaction: ssdeckframecore.h]: gate_air, not shape_air - unlike the puff path's own n_map read below, the sheet is base-anchored BY DEFINITION
            // (ssdeckframecore.h, doc/atmo_magic_wind_profile.md section 4 "Frame contract"): this read never evaluates O(z) at all, full stop - NOT because "the sheet sits at the deck floor where
            // O(z0) == 0 so it reduces to a no-op" (that framing was wrong: the veil is drawn 46-106 m above z0, where a storm profile's O is already 90-210 m, so gate_air and shape_air genuinely
            // differ here and this is a real choice, not a redundant table lerp). Still takes the hero's rigid shift (doc/atmo_magic_storm_dynamics.md section 3: "gate, presence, ... shape,
            // placement" all move with the hero).
            // <SS:Nexii> [interaction: ssdecknoisecore.h] De-tiled (doc/atmo_magic_far_clouds.md section 2 step 4) - see ss_noise_mapDetiled. Still gate_air, per this block's own comment above.
            float n_map = ss_noise_mapDetiled(gate_air);
            float cut = smoothstep(0.264, 0.530, n_map); // LOCKSTEP ssvolcloud.cpp SS_HOLE_LO/HI, SSDeckNoise::HOLE_LO/HI (phase 6b retune v2)
            presence = 1.0 - (1.0 - cut) * ss_noise_hole;
        }

        // ...and the builder's cell gate decides it too - see ss_field_occupancy. The presence
        // cut above only knows the noise map; the gate also knows the cluster noise and the
        // coverage dial, which between them empty whole regions of cells at any partial
        // coverage. Without this the veil drew its floor under sky the builder gave no puffs,
        // and the sheet's mottle sat unrelated to where the field actually stood.
        // <SS:Nexii> Phase 4 [interaction: ssdeckframecore.h]: gate_air, not air.xy - the cell gate stays on base drift plus the hero's rigid shift only, never the wind profile's O(z) (frame contract: "the gate stays on base drift ... nothing repops").
        // <SS:Nexii> 8g: `holes`, not `occupancy`, and it is the ONLY term of the old law's distance chain that survives - see ssveilcore.h's header for the finding. This is the sky's genuine geography (the coverage gate softened bilinearly) and the veil keeps it whole; what the veil no longer multiplies itself by is the deck's RENDERING economy (the far thinning and the puffs' outward edge fade), which are camera-distance decisions about how many bodies to draw, not statements about where cloud exists.
        float holes = ss_field_occupancy(gate_air, uint(ss_cell_salt));

        float horiz = length(world_true.xy - ss_cam_pos.xy);

        // <SS:Nexii> 8g THE VEIL'S FADE-IN [interaction: ssveilcore.h SSVeil::rimIn, LITERAL transliteration; twin_veil_law.cpp proves it]: rim_w == 1 - puffRimShare, the complement of the share of the deck's own rim presence still being drawn, so the veil rises by exactly what the puffs give up. thin_keep is keepNorm(SSDeckLod::keepFrac(horiz)) - identically 1 - smoothstep(THIN_START_M, FIELD_DRAW_M, horiz), because keepFrac is lerp(1, THIN_KEEP_MIN, that same smoothstep) and keepNorm divides the (1 - THIN_KEEP_MIN) span back out - and edge_keep is SSDeckLod::edgeFade spelled exactly as the puff path spells it. So rim_w is 0 inside 5000 m, rises on the thinning's own curve from there, and is 1 at DECK_EDGE_M where the last puff has gone. What it weights is NOT the base amplitude - see the amp line below and ssveilcore.h's note on why the obvious base * rim_w product is wrong in both directions. The old law had all of this backwards: it multiplied by edge_keep, so the veil was thinnest exactly where the puffs were sparsest and gone entirely past 9800 m.
        float thin_keep = 1.0 - smoothstep(ss_veil_rails.x, ss_veil_rails.y, horiz);
        float edge_keep = 1.0 - smoothstep(ss_edge_rails.x, ss_edge_rails.y, horiz);
        float rim_w     = 1.0 - thin_keep * edge_keep;

        // <SS:Nexii> 8g THE VEIL'S REACH [interaction: ssveilcore.h SSVeil::reachOut]: the veil's own outward dissolve, on its own rails (10000 -> 14000) beyond the puffs' 9800, drawn on the coarse 2*CELL_M tile ring ssvolcloud.cpp emits past FIELD_DRAW_M. This is the term that puts thin cloud where there is no puff at all. PLACEHOLDER, and ssveilcore.h says so at length: when the horizon deck (doc/atmo_magic_phase8_show.md section 5, phase 8c - design text only today, no HORIZON_* symbol exists) is built, its far annulus REPLACES this term and the veil goes back to ending at its rim.
        float reach_out = 1.0 - smoothstep(ss_veil_rails.z, ss_veil_rails.w, horiz);

        // And the near end of the same idea - see SS_SHEET_NEAR_M. Off TRUE distance, not the drawn one, so the fade measures the metres the eye would actually cross rather than the squashed
        // metres the geometry sits at; near the eye the two agree anyway, and reading eye_dist keeps it agreeing with every other ranged term in this shader.
        float near_fade = smoothstep(SS_SHEET_NEAR_M.x, SS_SHEET_NEAR_M.y, eye_dist);

        // Soft by construction: the mottle shapes the veil but never cuts it, and the ceiling is
        // held at 0.75 - the veil is THIN cloud, and its thin half is where the shared sun-through
        // fringe lives. Run it denser and it reads as a black slab under the deck (the body
        // colour is gloom-crushed in a storm); this soft it glows faintly through its own mottle
        // and reads as the deck's floor lit from within.
        // <SS:Nexii> 8g THE VEIL LAW, transliterated from SSVeil::veilAlpha (ssveilcore.h) times the near fade this file keeps: amp(sheet_n, rim_w) * holes * reach_out, where amp is base + (SS_VEIL_RIM_FULL - base) * rim_w. rim_w weights the DEFICIT, not the base: base * rim_w would delete the veil inside 5 km - the deck-floor support that is the veil's original job - and a rim_w starting at 1 could not rise, so there is no pure-product spelling of this and ssveilcore.h says so at length. Two consequences worth knowing while reading this line: inside SSDeckLod::THIN_START_M rim_w is 0 and reach_out is 1, so the whole expression collapses to the OLD one bit-for-bit (base * presence * holes * near_fade), and outward it is >= the old law at every distance. base's three literals are SSVeil::BASE_MIN/BASE_SPAN/BASE_CAP and the ceiling is SSVeil::RIM_FULL (twin_veil_law.cpp parses all four out of these lines and pins them against the core's own constants, so a hand-edit to one side is a test failure). `presence` - the sheet's own de-tiled noise-map hole cut - and `holes` together are the core's single holesSoft argument. near_fade stays OUT of the core deliberately: it is a function of TRUE eye distance, not horizontal distance, and it solves a parallax problem (a flat plane the eye can fly into) rather than a reach one.
        float veil_base = clamp(0.30 + 0.40 * sheet_n, 0.0, 0.75);
        float veil_amp  = veil_base + (SS_VEIL_RIM_FULL - veil_base) * rim_w;
        density = veil_amp * presence * holes * reach_out * near_fade;
        noise_v = sheet_n;
        // <SS:Nexii> D2, MEASURED AND STATED, NOT CHANGED [V:\Scratch\atmo\tests\unit_deck_radiance.cpp]: this normal is +Z, i.e. the veil is shaded as a surface facing straight UP, and the shared
        // tail's wrap term reads it - `wrap = 0.5 + 0.5 * dot(sphere_n, ss_light_dir)`. But the veil is the deck's UNDERSIDE and is wound to face DOWN (ssvolcloud.cpp's sheet draw says so in as many
        // words), so this is the wrong side of the sheet by inspection: under a high sun it hands the veil wrap = 1, the maximum the term can give, and under a sun BELOW the horizon it hands it wrap
        // < 0.5 while the puffs beside it get > 0.5. Measured, on identical light at the same point: the veil comes out 39% BRIGHTER than a low puff at sunZ 0.8 and 11% DARKER at sunZ -0.15.
        // NOT flipped here, deliberately: the sign flip is a one-token change (-1.0) but it cuts the daylight veil by up to 45% (wrap 1 -> SS_FORM_DARK), which is a large unrequested change to the
        // look on the same build as the gloom-depth fix beside it (Deck::SHEET_BURIED, ssvolcloud.h - the term that actually made the veil dark), and the two would not be judgeable apart. The honest
        // reading is that a cloud BASE is lit by the whole lower sky and the ground rather than by a cosine against the beam, so neither +Z nor -Z is right and the veil's real answer is a flat
        // ambient - which is what mSheetForm already is. Recorded here as the next thing to try if the veil still reads wrong in daylight.
        sphere_n = vec3(0.0, 0.0, 1.0);
    }
    else
    {

    // <SS:Nexii> Distant rain shaft (ssvirgacore.h, doc/atmo_magic_far_clouds.md section 3) [interaction: ssvolcloud.cpp's shaft emission, the vary_color note above]: b > 0.5 flags a virga card - the
    // CPU has already baked this cell's drive, taper and vertical alpha profile into the card's own geometry and into vary_color.a (SSVirga::halfWidthM/alphaAt/handoff), so this branch supplies only
    // what a hanging curtain needs and a puff's own carve does not: a soft box across the card in place of the puff's radial window, and a falling streak in place of the puff's rising boil. No
    // shape/rim window, no n_map tower/anvil/cap-band logic - a shaft has no lid to consolidate into and no floor to cut flat, it is the curtain hanging under one.
    // <SS:Nexii> S2: the puff branch's own phase (mPhase * 0.49, always < 0.5) can never trip this test - it lives on the SAME channel but under the flag's floor by construction (0.49 * 255 =
    // 124.95 -> 125/255 = 0.4902, still < 0.5 after U8 quantization), so an ordinary puff never misreads as a shaft and vice versa.
    if (vary_color.b > 0.5)
    {
        // The card's own uv: x across its width (0 one side edge, 1 the other), y up its height (0 this
        // card's own bottom, 1 its own top) - see the shaft's texCoord2f calls in ssvolcloud.cpp. A soft
        // box rather than a hard quad on both axes, eased in from every edge by the same cubic the rest
        // of this file uses, so a shaft never shows its card as a rectangle: SS_SHAFT_V_SOFT rounds the
        // seam between one stacked card and the next, SS_SHAFT_H_CORE leaves the column's own centre
        // solid and falls away toward the two side edges the taper already narrows.
        // <SS:Nexii> Phase 8e: this box is a MASK ONLY now - it no longer IS the silhouette (that is the body
        // noise below), it only shapes where the body is allowed to show through, exactly as SS_PUFF_RIM masks
        // the puff branch's own window+noise sum rather than replacing it.
        // <SS:Nexii> S1: reads go through .xy, the decoded CORNER - shafts carry NO payload (ssvolcloud.cpp's
        // render() never applies the payload encode to a shaft's corners), so corner == the raw card u/v here,
        // exactly as vary_texcoord0 always was before the vec4 widen; this read is unchanged in value.
        vec2 shaft_uv = vary_texcoord0.xy;
        float vbox = smoothstep(0.0, SS_SHAFT_V_SOFT, shaft_uv.y)
                   * smoothstep(1.0, 1.0 - SS_SHAFT_V_SOFT, shaft_uv.y);
        float u = abs(shaft_uv.x * 2.0 - 1.0);
        float hedge = 1.0 - smoothstep(SS_SHAFT_H_CORE, 1.0, u);
        float mask = vbox * hedge;

        // <SS:Nexii> Phase 8e item 4, DRIVE REACHES THE FRAGMENT: the SAME SSVirga::drive this cell qualified with
        // (SSVirga::drive/qualifies, ssvirgacore.h), recovered from the vertex colour's spare b channel where
        // render() encoded it alongside the shaft flag (b = 0.5 + 0.5 * drive, see ssvolcloud.cpp's color4f call
        // and the vary_color note in ssVolCloudV.glsl) - never a second, independent per-fragment computation.
        float drive = clamp((vary_color.b - 0.5) * 2.0, 0.0, 1.0);

        // <SS:Nexii> Phase 8e item 2 (doc/atmo_magic_phase8_show.md section 3b): the curtain's OWN height fraction
        // (0 at the ground reference, 1 at the deck base) - the SAME fraction as SSVirga::baseProfileH01 with a 1 m span
        // floor in place of the core's 1e-6 (identical for any real deck; not a literal twin), evaluated on
        // this fragment's true world height rather than a card's own v, so it is continuous across stacked cards'
        // seams exactly as the embed fade (ss_virga_embedAlpha) already is. Feeds both the droop (below, item 2)
        // and the evaporation mask (item 5).
        float h = clamp((world_true.z - ss_shaft_embed.z) / max(ss_shaft_embed.x - ss_shaft_embed.z, 1.0), 0.0, 1.0);

        // <SS:Nexii> F12 (2026-09-06 review), stated rather than fixed: a shaft fragment now pays 2 ss_density
        // calls (the x-facing and y-facing body planes below) plus 1 ss_detail call (the fall streak further
        // down) - 3 texture fetches at minimum, up to 6 while a base/detail crossfade is live (each of those two
        // functions samples a second texture and mixes when its own blend weight is > 0), on top of whatever the
        // shared frame block above already paid. No action taken: measure the actual cost on a build before
        // trading a fetch away.
        // <SS:Nexii> Phase 8e body (doc/atmo_magic_phase8_show.md section 3b, SS_SHAFT_Z_STRETCH/SS_SHAFT_BODY_
        // FLOOR_WISPY/HEAVY above): the puff's own cloud noise (ss_density, the puff body's base-map read), on the card's two vertical
        // planes - x-facing and y-facing, the two planes that actually carry variation on a vertical Z-billboard
        // (the horizontal xy plane a puff samples would barely change up a shaft's height, the same reasoning the
        // puff body's own triplanar comment gives for not trusting a single plane on a billboard that can face any
        // way). gate_air.xy, NOT shape_air - same base-anchored reasoning as the streak below: a shaft carries no
        // O(z) lean at the producer side (SS_VIRGA_NO_SHEAR), so nothing here undoes one either. world_true.z is
        // stretched by SS_SHAFT_Z_STRETCH before the divide so the lookup does not read one flat band of the map's
        // z-period down a card many hundred metres tall. Weighted by the card's own facing (|nrm.x|, |nrm.y| - the
        // same nrm the puff branch's `tri` weights triplanar with), so a shaft end-on to one plane leans on the
        // other instead of collapsing to a single texel column the way an unweighted average would.
        vec2 pl_xz_shaft = vec2(gate_air.x, world_true.z * SS_SHAFT_Z_STRETCH) / SS_NOISE_M;
        vec2 pl_yz_shaft = vec2(gate_air.y, world_true.z * SS_SHAFT_Z_STRETCH) / SS_NOISE_M;
        float wx = abs(nrm.x);
        float wy = abs(nrm.y);
        float wsum_shaft = max(wx + wy, 1.0e-4);
        // <SS:Nexii> 8e-c review F1: the weight for |nrm.x| goes with pl_YZ - a card whose normal is +-X spans world Y and
        // Z, so the plane that VARIES across its width is (y, z); pairing |nrm.x| with the plane that contains x read
        // one column of the map smeared across the card whenever the camera looked along an axis (the puff branch's
        // tri.x weights pl_yz for the same reason). Pinned as text by twin_virga.cpp - a text pin, so the pairing is
        // ALSO stated here in words.
        float body = (wx * ss_density(pl_yz_shaft) + wy * ss_density(pl_xz_shaft)) / wsum_shaft;

        // <SS:Nexii> F9 fix: the one piece of texture a shaft carries: the SAME detail map every puff already
        // reads (ss_detail), sampled at this column's own frame coordinate. This is gate_air, NOT shape_air -
        // shafts are BASE-ANCHORED BY DEFINITION (ssvirgacore.h's BURIED comment, ssdeckframecore.h's
        // producer/observer rule: the sheet and the shadow bake/precipNoiseAt read gate_air for the same reason),
        // placed by SSVolCloud::buildDeck through a ZERO shear table (SSVolCloud.cpp's SS_VIRGA_NO_SHEAR) so
        // there is no O(z) lean at the producer side to undo here either - reading shape_air would subtract this
        // fragment's own-altitude lean from a placement that never carried one, sliding the streak off the card
        // it is meant to hold to as the deck drifts. The old comment's "exactly as the puff body does" was wrong
        // for that reason: the puff body genuinely IS leaned (shape_air undoes ITS placement's own O(z)), a
        // shaft never is. Scrolled down the vertical axis only; ss_fall_m (SSDeckBoil::advanceFallM) only
        // ever GROWS and is never added back with a positive sign the way the puff's flow_w.z is lifted by
        // SS_FLOW_RISE below - the shaft's mirror of that clamp is this term's fixed sign, not a second clamp,
        // so the streak has nowhere to reverse into. Cosmetic only, like the storm rotation's SS_STRIATION_ROT
        // above: no CPU counterpart, no twin to keep in lockstep with, and no claim here that the sign reads as
        // "falling" on screen - that needs eyes on a build, not a comment.
        // <SS:Nexii> D3 [interaction: ssdeckboilcore.h]: the fall term was `ss_time * ss_drift_rate * SS_SHAFT_FALL_MPS` - the same rate-times-absolute-clock shape the boil phase had, with the same
        // 1 Hz tread under it (ss_drift_rate then stepped with the whole-second day-cycle phase; that phase is continuous as of the D3 follow-up, 2026-09-07), so a curtain's streak jumped once a second exactly as the puffs did. ss_fall_m is the integral instead
        // (SSDeckBoil::advanceFallM), folded on SS_NOISE_M so the subtraction is exact against a wrapping map.
        vec2 streak_uv = vec2(dot(gate_air, ss_wind) / SS_NOISE_M,
                               (world_true.z - ss_fall_m) / SS_NOISE_M);
        float streak = ss_detail(streak_uv);
        noise_v = streak;

        // <SS:Nexii> Phase 8e item 4, DRIVE REACHES THE FRAGMENT: the streak amplitude by drive - see
        // SS_SHAFT_STREAK_WISPY/HEAVY's own comment above for why heavy rain's amplitude is the SMALLER number.
        float streak_amp = mix(SS_SHAFT_STREAK_WISPY, SS_SHAFT_STREAK_HEAVY, drive);
        float streak_term = mix(1.0 - streak_amp, 1.0, streak);

        // <SS:Nexii> Phase 8e: mask * body * streak, not mask * streak alone - the body noise decides the
        // silhouette (mix(body_floor, 1, body) so the mask's own soft seam/taper edges stay visible rather than
        // being erasable to nothing by a low body sample), the mask still bounds it to the card, and the streak
        // still adds the falling erosion on top. Phase 8e item 4: body_floor is now drive-shaped (see
        // SS_SHAFT_BODY_FLOOR_WISPY/HEAVY's own comment above), not the old fixed SS_SHAFT_BODY_FLOOR.
        // <SS:Nexii> F2 (2026-09-06 review): the per-pixel embed fade, in place of the CPU's old one-per-card
        // alphaMul (ssvirgacore.h's CardGeom no longer carries one - deleted, not left as an always-1 field). A
        // baked-per-card value never ramped WITHIN a card; this reads world_true.z, the fragment's own true world
        // height, so the curtain visibly emerges from inside the cloud pixel by pixel rather than in per-card
        // steps. ss_shaft_embed.xy is (deck base Z, embedded stack top Z) - zero for the under deck's pass, which
        // never draws a shaft fragment anyway (shafts only ever hang from weatherDeck()).
        float body_floor = mix(SS_SHAFT_BODY_FLOOR_WISPY, SS_SHAFT_BODY_FLOOR_HEAVY, drive);
        density = mask * mix(body_floor, 1.0, body) * streak_term
                * ss_virga_embedAlpha(world_true.z, ss_shaft_embed.x, ss_shaft_embed.y);

        // <SS:Nexii> Phase 8e item 5, EVAPORATION MASK (the user's rule: alpha is intensity x density across the
        // volume; virga is masked out from the bottom up with wispy streaks). evap_h LOCKSTEP
        // SSVirga::evapHeight01 (ssvirgacore.h) - light rain (low drive) ends two thirds of the way up from the
        // ground, heavy rain (high drive) reaches it. The jitter term ((streak - 0.5) * SS_EVAP_JITTER) swings the
        // mask's own threshold by the SAME streak detail already read above, so the bottom edge comes out as
        // ragged wisps of different lengths rather than one flat evaporation line. This is GEOMETRY's own
        // per-pixel evaporation, layered on top of cardEmittedFor's per-card skip (ssvirgacore.h) - that core
        // function decides whether a card is drawn AT ALL, this decides how a drawn card's own density fades out
        // near that same height.
        float evap_h = mix(SS_EVAP_H_WISPY, SS_EVAP_H_HEAVY, smoothstep(0.15, 0.85, drive));
        float evap = smoothstep(evap_h - SS_EVAP_SOFT, evap_h + SS_EVAP_SOFT * 0.3, h + (streak - 0.5) * SS_EVAP_JITTER);
        density *= evap;

        // <SS:Nexii> Phase 8e item 2 (doc/atmo_magic_phase8_show.md section 3b): the shaft is lit like a puff's
        // lower rim, tilting further down toward the ground - continuous across stacked cards because h (above) is
        // the CURTAIN's own height fraction, not the card's own v. droop is the tilt amount at this fragment's own
        // height (SS_SHAFT_DROOP_GROUND at the ground, SS_SHAFT_DROOP_BASE at the deck base, see their own
        // comment); p_shaft is the same [-1,1]-per-axis point ss_sphere_normal (item 1, ONE FORMULA SITE) takes
        // for a puff, x across the card's own width and y fixed at -droop (the curtain leans DOWN, away from the
        // light, along its own height axis rather than across its width), length-clamped to 0.999 so the
        // function's own sqrt(1 - r*r) never hits exactly 0 at the rim. Same frame, same tail, same form/buried
        // inputs (mSheetForm, SSVirga::BURIED, see ssvolcloud.cpp's shaft emitter comment) - the curtain is the
        // cloud base pulled down; only the droop differs. F7 (2026-09-06 review)'s own point stands: this is still
        // the CAMERA-FACING approximation the shaft has always used (nrm, not the tilted parallelogram's true
        // geometric normal), now folded through the same reconstruction the puff branch uses rather than the old
        // flat vec3(nrm.xy, 0.0).
        float droop = mix(SS_SHAFT_DROOP_GROUND, SS_SHAFT_DROOP_BASE, h);
        // <SS:Nexii> S1: shaft_uv.x (== vary_texcoord0.xy.x, the decoded corner - see the mask block above), not
        // the bare varying - same value as before the vec4 widen since a shaft carries no payload.
        vec2 p_shaft = vec2(shaft_uv.x * 2.0 - 1.0, -droop);
        float p_shaft_len = length(p_shaft);
        if (p_shaft_len > 0.999)
        {
            p_shaft *= 0.999 / p_shaft_len;
        }
        sphere_n = ss_sphere_normal(nrm, p_shaft);
    }
    else
    {

    // A soft radial window. The art has no edge of its own - it is seamless noise, opaque corner to corner, with no alpha channel - so without a window every puff draws as its quad, hard borders and
    // all. That was the wall of rectangles.
    // <SS:Nexii> S1: .xy already, unchanged by the vec4 widen (this was the ONLY read that already swizzled rather than using the bare varying) - it is now the decoded CORNER rather than the raw
    // interpolated texcoord, bit-identical whenever payload is 0 (every puff today, S0-S2).
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
    // <SS:Nexii> Phase 8e item 1: this reconstruction is now ss_sphere_normal (ONE FORMULA SITE, see its own
    // comment above the shared frame block) - same statements, same order, so this result is bit-identical to
    // the old inline block (pinned by twin_virga.cpp's phase8ec_item1 source-text test, which requires exactly two
    // call sites with these arguments); only the shaft branch (item 2, below) gained a second caller of it.
    sphere_n = ss_sphere_normal(nrm, p);
    tri /= max(tri.x + tri.y + tri.z, 1.0e-4);

    // <SS:Nexii> S3 G3/G5 shared input (doc/atmo_magic_flow_field.md section 1 "Flow fields", review_opus.md's Q
    // findings on both A-G3/A-G5), HOISTED ahead of `streak`'s own declaration below (2026-09-06 opus S0-S3 fix
    // pass, finding 1: the downwind streak gain has to land IN `streak` itself so wind_uv/pl_yz/pl_xz just below
    // read the SAME stretched value the later flow offsets do - see that comment for the fuller story). The
    // fragment's UN-LEANED offset from the storm cell that owns it - both the anvil outflow, the meso swirl, and
    // now the streak gain need this SAME vector. world_true.xy - o, not the raw world_true.xy the rejected
    // drafts read: `o` is O(z) at this fragment's own altitude, computed ONCE in the shared frame block above
    // ("o = O(z) at this fragment's own altitude - removed FIRST, before either path's storm_p quantizes") and
    // measured by review_opus.md to reach ~2.5km at anvil altitude - comparable to a whole storm radius, so
    // reading world_true.xy alone would displace the outflow direction and the swirl centre by up to a storm
    // radius at exactly the altitude an anvil lives at. storm_sample.ownerC is the owner cell's own centre (S3
    // widen), not the influence-weighted MAX the other storm_sample fields carry. Every input this needs - o,
    // storm_sample.ownerC, ss_wind - is already resident from the shared frame block well before this point (see
    // main()'s frame block above), so hoisting it here costs nothing and reorders nothing else.
#if SS_G3_OUTFLOW || SS_G5_SWIRL
    vec2 ss_owner_delta = (world_true.xy - o) - storm_sample.ownerC;
#endif
#if SS_G3_OUTFLOW
    // Radial alignment with the wind, needed by the streak gain immediately below AND by the anvil outflow
    // block further down (which reuses ss_radial_dir/ss_wind_align rather than recomputing them).
    vec2 ss_radial_dir = ss_safe_dir(ss_owner_delta);
    float ss_wind_align = clamp(dot(ss_radial_dir, ss_wind) * 0.5 + 0.5, 0.0, 1.0); // remap [-1,1] alignment to [0,1]: 0 upwind, 1 downwind
#endif

    // Stretched along the wind, by however little convection there is. Under a full anvil, the downwind flank
    // stretches FURTHER still (fibrous streak; upwind stays short) - folded in HERE, before wind_uv/pl_yz/pl_xz
    // below read `streak`, so every along-wind lookup on this fragment (the static base body's coordinates below
    // AND the advected detail's flow offsets further down) agrees on the one gained value. (2026-09-06 opus S0-S3
    // fix pass, finding 1: the old placement mutated `streak` AFTER these coordinates had already been built from
    // the un-mutated value, so only the advected detail's travel saw the gain, and it saw it as a SHORTENING -
    // dividing by a larger streak on an already-divided coordinate - not the lengthening the comment there
    // claimed. Revert is still SS_ANVIL_STREAK_GAIN = 0, or SS_G3_OUTFLOW = 0 to drop the gain entirely.)
    float streak = mix(SS_STREAK, 1.0, clamp(ss_churn, 0.0, 1.0));
#if SS_G3_OUTFLOW
    streak *= mix(1.0, 1.0 + SS_ANVIL_STREAK_GAIN * ss_wind_align, storm_sample.anvil);
#endif

    // The horizontal lookup goes into the wind's own frame first - along and across - so the stretch follows the weather rather than the world axes. Dividing the ALONG coordinate by more metres is
    // what makes the noise change slowly in that direction, and slowly is what a streak is.
    vec2 across = vec2(-ss_wind.y, ss_wind.x);
    // <SS:Nexii> Phase 4 [interaction: ssdeckframecore.h]: shape_air, not air.xy - the triplanar body below is a pattern/carve read, so it takes both the wind profile's O(z) lean and the hero's rigid shift (see main()'s frame block).
    vec2 wind_uv = vec2(dot(shape_air, ss_wind) / streak, dot(shape_air, across));

    // No domain warp here any more. It was displacing the lookup by a coarse read of the map to break up the tiling, which it did - and took the cloud with it. Warping bends the sample grid, and the
    // same bend that hides a repeat also drags real structure sideways; at the scale the base map is now sampled at, there was more structure being dragged than repeat being hidden, and the field
    // came out smeared. The tiling it was fighting is better dealt with by sampling nearer the size of a puff, so there is something at puff scale to look at instead. Kept in metres, not yet divided
    // down: every octave needs all three of these at its own scale, so the division happens at the point of use.
    // <SS:Nexii> Phase 4 [interaction: ssdeckframecore.h]: shape_air.x/.y, not air.x/.y - same reasoning as wind_uv above. air.z (== world_true.z, ss_drift only ever offsets xy) is untouched: O(z)/the hero shift are xy-only, so the vertical axis has nothing to shift.
    vec2 pl_yz = vec2(shape_air.y / streak, air.z);
    vec2 pl_xz = vec2(shape_air.x / streak, air.z);
    vec2 pl_xy = wind_uv;

    // ...and these are added AFTER that division, in tiles, so they mean the same thing to every octave. Each plane is skewed by the axis it drops (see SS_SKEW_M), which is what stops the three of
    // them agreeing about where the tile boundaries fall.
    vec2 off_yz = vec2(shape_air.x / SS_SKEW_M, 0.0);
    vec2 off_xz = vec2(-shape_air.y / SS_SKEW_M, 0.0);
    vec2 off_xy = vec2(air.z / SS_SKEW_M, air.z / SS_SKEW_M * -0.7);

    // How far this fragment leans toward the other map, and how high it sits in the layer. Both are wanted before the base octave is taken - the mix decides what that octave is sampled FROM, and the
    // height drives the boil rate below. Three things decide the mix: what the author asked for, where in the layer the fragment sits, and a slow wander over the field so the change is regional
    // rather than a stripe. The wander alone is horizontal, and that one is safe: it is sampled three times coarser than the base octave, so it barely changes across a single puff. What it must not
    // do is change FAST on a plane with no variation to give - which is why everything below is triplanar.
    float layer_h = clamp((world_true.z - ss_base_z) / ss_layer_thick, 0.0, 1.0);
    // <SS:Nexii> Phase 4 [interaction: ssdeckframecore.h]: shape_air, not air.xy - the wander feeds tex_mix, a pattern decision, same reasoning as wind_uv above.
    float wander = ss_detail(shape_air / (SS_NOISE_M * 3.0));
    float tex_mix = clamp(ss_tex_mix
                        + (layer_h - 0.5) * SS_MIX_HEIGHT
                        + (wander - 0.5) * SS_MIX_WANDER, 0.0, 1.0);

    // The body of the cloud: triplanar, mixed per plane, and STATIC in the air's frame. Static because the shape of a body of vapour is not what boils - the surface of it is. This used to cross-fade
    // between two offsets of the whole field to fake that, which cost a second full set of samples to animate the one part that should hold still while the deck drifts. The scrolling octaves below
    // do the churning now, and do it better.
    float noise = tri.x * ss_mixed(pl_yz / SS_NOISE_M + off_yz, tex_mix)
                + tri.y * ss_mixed(pl_xz / SS_NOISE_M + off_xz, tex_mix)
                + tri.z * ss_mixed(pl_xy / SS_NOISE_M + off_xy, tex_mix);


    // Two finer octaves from the DOME's own cloud map, sliding across each other - see SSDeckBoil::LAPS_PER_S and SS_FLOW_M. Triplanar, like the base. They were taken on the horizontal plane alone, on the
    // reasoning that surface detail is too fine to need placing carefully - which was wrong in a way that showed. A horizontal coordinate barely changes as you move UP a puff, so on any quad facing
    // the camera the octaves held nearly still down the whole height of it and smeared into vertical streaks. Puffs overhead looked right for the same reason: there the horizontal plane is the
    // correct one. Height changes how FAR an octave wanders, not how fast it goes round. It used to scale the rate, and that reintroduced the same failure the orbit was meant to end - one level
    // down. Rate multiplied by height means the PHASE differs across a puff by an amount proportional to elapsed time, so however bounded each orbit is, the gap between the bottom of a puff and the
    // top keeps widening. Given a few minutes it sweeps whole turns over a puff's height, nearby heights land on completely different offsets, and because layer_h runs vertically the tearing comes
    // out as horizontal bands. Winding the rate up only got there sooner. Scaling the amplitude keeps the intent - the top of a convective layer moves more than its base - with nothing that grows.
    // The rate is now uniform across the fragment, so there is no gradient to accumulate, and a fixed phase lead with height keeps the layers out of step without ever drifting further apart.
    float boil = mix(1.0, SS_BOIL_TOP, layer_h);
    float lead = layer_h * SS_BOIL_LEAD;

    float oct2_m = SS_NOISE_M * SS_OCT2_SCALE * ss_detail_scale;

    // <SS:Nexii> S2 per-puff phase (doc/atmo_magic_flow_field.md section 2, review_opus.md Q1/Stage 2): recovered
    // from vary_color.b's low half - SSVolCloud::Puff::mPhase * 0.49 on the CPU (ssvolcloud.cpp's render()),
    // undone here by the inverse scale and clamped to guard against any future b use pushing past 0.49 (the
    // shaft flag's own >0.5 test already keeps this branch's b under 0.49 by construction; the clamp is a
    // second, cheap guard rather than trust alone). A per-cell hashed phase seams hard across every puff face (a
    // quad spans 2-10 hash cells) and shears with the wind profile if keyed on shape_air; a per-PUFF constant has
    // ZERO phase gradient inside one quad, so no seam, no warp, no fold - it costs one CPU hash, zero fetches,
    // zero varyings, and nothing else reads this deck's b channel for a puff (the sheet takes a different branch
    // entirely; the shaft's own b>0.5 test is the only other consumer, and phase never reaches it).
    float phase = clamp(vary_color.b, 0.0, 0.49) / 0.49;

    // Where in its cycle the flow is, and its half-cycle partner.
    // <SS:Nexii> D3 (fourth build report: "every puff's appearance jumps in a sudden step about once per second, and the flow motion is far too exaggerated") [interaction: ssdeckboilcore.h]: this used
    // to open `float turn = ss_time * SS_OCT_LAPS * ss_drift_rate * (SS_OCT_DRIFT_FLOOR + ss_churn) * 6.2831853;` - a RATE multiplied by an ABSOLUTE clock (seconds since the viewer started). Both
    // factors of that rate are resolved from the day-cycle phase every frame, and that phase was then SSAtmoEnvTrack::currentDayCyclePhase() == dayCyclePhaseAt((F64)time(nullptr)) - WHOLE SECONDS (continuous since the D3 follow-up, 2026-09-07). So the
    // rate is a staircase with a 1 Hz tread and phase = rate x t turned each tread into a jump of t x delta-rate: at an hour of uptime a one-part-in-ten-thousand rate change moved the detail phase a
    // fifth of a cycle in a single frame. The same multiplication made the apparent rate rate + t x d(rate)/dt, whose second term grows without bound with uptime - the exaggeration half of the same
    // report. ss_boil_laps is now the INTEGRAL of that rate, accumulated in F64 on the CPU (SSDeckBoil::advanceLaps, driven in ssvolcloud.cpp's update()) and folded into [0,1) before it crosses into
    // this uniform - exact, because only fract() of it is ever read. A step in the rate now changes only the derivative, which is invisible. SS_OCT_LAPS and SS_OCT_DRIFT_FLOOR moved into the core
    // with it (SSDeckBoil::LAPS_PER_S / DRIFT_FLOOR) and the rate was retuned there, 0.06 -> 0.012 laps/s.
    float cycles = ss_boil_laps + lead * 0.15 + phase;
    float ph0 = fract(cycles);
    float ph1 = fract(cycles + 0.5);
    float w = abs(1.0 - 2.0 * ph0);

    // Which way the detail travels: OUT of the puff, and up. Along the sphere normal, the same one the light uses. A convective parcel does not slide, it grows - it pushes outward from where it is
    // rising and carries its texture with it, which is why the surface of a cumulus appears to boil out of itself. Following the normal puts that motion where the shape already implies it: detail
    // leaves the middle of a puff, travels out across the curve, and dissolves at the rim. A clock could never produce that. An offset on a circle - which is what this was - moves every fragment of
    // every puff the same way at the same instant, so the field slides as one and reads as wind however the path is dressed up. The direction has to come from the geometry, and the geometry is right
    // here. Never downward, though. Air in a convective cloud goes up and spreads; the underside is where it is fed from, not where it flows to. So the vertical component is clipped at zero and
    // given a lift on top of that - the lower half of a puff flows sideways rather than draining out of the bottom of it.
#if SS_G2_BILLOW
    // <SS:Nexii> S3 G2_BILLOW (doc/atmo_magic_flow_field.md section 1 "Flow fields", review_opus.md's G2
    // correction, proto.h's f1_ring): the vortex-ring roll, built in the puff's OWN meridional frame instead of
    // the plain "outward and up" direction the #else branch below still carries. Meridional coordinate m =
    // (r, bulge) reuses the SAME r this branch already computed for shape/rim above (length(p)) and
    // bulge = sqrt(max(1-r*r,0)), the identical quantity ss_sphere_normal folds onto its own nrm axis - together
    // they are the puff's own (radial-distance, height-above-the-tangent-plane) cross-section, which by the
    // ring's rotational symmetry reads the SAME at every azimuth around the puff, so one 2-D field per fragment
    // is enough; no separate azimuthal parameter is needed. Two counter-rotating cores at
    // (+-SS_BILLOW_RING_U, SS_BILLOW_RING_V) in that plane, contribution rot90(d)*s*|d|/(SS_FLOW_EPS+|d|^3) per
    // core (proto.h f1_ring, the ladder-proven form): the numerator ~|d|^2 vanishes at the core faster than the
    // eps-floored denominator settles, so the term is C0 (in fact C1) at the core with NO normalize anywhere -
    // the singularity class both the ladder (billow_v1_ring's seam) and review_opus.md's NaN finding hit
    // independently. NEVER normalize a summed flow vector. This is now a COMPLETE port of f1_ring (2026-09-06
    // opus S0-S3 fix pass, finding 3 - the first landing was ring-cores-only, at (+-SS_BILLOW_RING_U, 0), missing
    // both the below-centre offset and the central updraft below; comments and doc/atmo_magic_flow_field.md
    // section 1 both overclaimed "LITERAL port" against that shorter field). f1_ring's OWN curl companion
    // (f3_ring_curl, +0.5x fbm-derivative noise) is still deliberately NOT ported - 8 extra fbm evaluations
    // against a currently zero-new-fetch budget - see SS_BILLOW_UPDRAFT_MAG's own constant comment for the
    // escalation path if a build wants more turbulence in the roll.
    vec2 ss_bulge_m = vec2(r, sqrt(max(1.0 - r * r, 0.0)));
    vec2 ss_billow_uv = vec2(0.0);
    for (int ss_ring_i = 0; ss_ring_i < 2; ++ss_ring_i)
    {
        vec2 ss_ring_c = (ss_ring_i == 0) ? vec2(SS_BILLOW_RING_U, SS_BILLOW_RING_V) : vec2(-SS_BILLOW_RING_U, SS_BILLOW_RING_V);
        float ss_ring_s = (ss_ring_i == 0) ? -1.0 : 1.0;
        vec2 ss_ring_d = ss_bulge_m - ss_ring_c;
        float ss_ring_len = length(ss_ring_d);
        ss_billow_uv += ss_rot90(ss_ring_d) * (ss_ring_s * ss_ring_len / (SS_FLOW_EPS + ss_ring_len * ss_ring_len * ss_ring_len));
    }

    // Central updraft (proto.h f1_ring: "V2(0,0.6) * exp(-p.x^2/0.18); central updraft, +y = up") - the term the
    // first landing dropped. On the constrained per-fragment arc it evaluates to (0, SS_BILLOW_UPDRAFT_MAG) at
    // the apex (ss_bulge_m.x == 0 there), nearly doubling the ring-only apex value and pushing the up/down
    // crossover outward - the difference between a bare vortex pair and cumulus turret morphology. A plain
    // gaussian in ss_bulge_m.x, no division, so it adds no singularity of its own and needs no eps guard.
    ss_billow_uv += vec2(0.0, SS_BILLOW_UPDRAFT_MAG) * exp(-(ss_bulge_m.x * ss_bulge_m.x) / SS_BILLOW_UPDRAFT_SPREAD);

    // Map the meridional (radial, bulge) result back to WORLD axes through the CARD'S OWN frame. The radial part is
    // written as p * (ss_billow_uv.x / (SS_FLOW_EPS+r)), NOT p_hat * ss_billow_uv.x: p already IS
    // the tangent-plane vector of length r, so scaling it directly keeps the SAME vanishing-numerator idiom as
    // the ring cores above - at r=0 (dead centre of the puff) p is exactly (0,0), so this term is exactly zero
    // however large ss_billow_uv.x is, no separate r==0 branch, and physically right too: an axisymmetric ring
    // field has no preferred horizontal direction on its own axis (and the swirl rotation below cannot disturb
    // that: rotating (0,0) is still (0,0), and |p| - the only thing the meridional field reads - is preserved).
    // The bulge part is ss_billow_uv.y along the card's normal, exactly as ss_sphere_normal's own nrm*sqrt(1-r*r)
    // term is along the view ray - a WORLD-space vector either way (card_n/tan_u/tan_v are each individual
    // world-space unit vectors combined into one world-space result).
    // <SS:Nexii> The claim this comment used to carry - that the frame here is "the SAME tan_u/tan_v/nrm frame
    // ss_sphere_normal builds sphere_n from", duplicated deliberately so twin_virga.cpp's verbatim text pin on
    // ss_sphere_normal would keep holding - was TRUE about the text and FALSE about the geometry, and that is the
    // whole fifth-build-report finding: ss_sphere_normal's frame is not the card's frame either, it is just the
    // one the OTHER duplicate also got wrong. The flow now builds the card's frame (below); ss_sphere_normal is
    // left exactly as it was, so the lighting on this build is untouched and its text pin still holds.
    // <SS:Nexii> THE CARD'S OWN FRAME (fifth build report; [interaction: ssdeckflowcore.h], see SS_CARD_FLATTEN_LO
    // above for the measurement). This block used to spell `ref = (abs(nrm.z) < 0.95) ? +Z : +X; tan_u =
    // normalize(cross(ref, nrm)); tan_v = cross(nrm, tan_u)` - a SECOND, independently written frame that is not
    // the frame the CPU laid the quad out in. It is ss_card_frame now, the literal twin of SSDeckFlow::cardFrame
    // that render() also calls, so `tan_u * p.x + tan_v * p.y` is the fragment's real outward direction on the
    // card at every view angle instead of that direction's negative over a third of the sky. The bulge term rides
    // ss_billow_card_n, the CARD'S normal (the view ray eased toward +-world-Z near the poles) rather than the raw
    // view ray, for the same reason: it is the axis the card actually stands perpendicular to.
    // STILL OPEN, and unchanged by this: the ring's AXIS is the card's normal, which still tracks the camera, so a
    // turret seen from below and one seen edge-on show the same roll where a real one's ring axis is vertical -
    // review_opus.md's G2 finding part (i), scheduled as Stage 6. This fix makes the frame ONE frame; it does not
    // make it a world frame.
    vec3 ss_billow_card_n, ss_billow_tan_u, ss_billow_tan_v;
    ss_card_frame(nrm, ss_billow_card_n, ss_billow_tan_u, ss_billow_tan_v);

    // <SS:Nexii> S3 per-puff SWIRL (fifth build report: "not handling the different angles and instead seems to be
    // picking from a few different presets which dont mash well together or look in unison") [interaction:
    // ssdeckflowcore.h]: the quad point turned by this puff's own swirl angle before the radial mapping reads it.
    // The angle arrives on the texcoord PAYLOAD channel (vary_texcoord0.z, S1's own encode, constant across the
    // quad because all four corners carry it), and the CPU sampled it from SSDeckFlow::swirlUnit - a value-noise
    // FIELD at the puff's air-frame cell centre, NOT a per-puff hash. That is what buys both halves of the
    // complaint at once: continuous (the turn's distribution over 10920 puffs occupies 22 of 72 five-degree bins
    // with no bin over 8.1%, against the old frame's 46.8% spike - unit_deckflow.cpp direction_distribution) and
    // COHERENT (0.82 correlation between puffs one 260 m cell apart, -0.003 at 3120 m; an independent per-puff
    // hash measures -0.006 at both - swirl_neighbour_correlation). Clamped rather than trusted: nothing else
    // writes this channel today, and a future payload user must not be able to spin the flow.
    float ss_swirl_ang = clamp(vary_texcoord0.z, -1.0, 1.0) * SS_SWIRL_MAX_RAD;
    vec2 ss_billow_p = ss_rot2(p, ss_swirl_ang);

    vec3 ss_billow_w = (ss_billow_tan_u * ss_billow_p.x + ss_billow_tan_v * ss_billow_p.y) * (ss_billow_uv.x / (SS_FLOW_EPS + r))
                      + ss_billow_card_n * ss_billow_uv.y;

    // The world-up rise and no-downdraft clamp, on the WORLD z component of ss_billow_w - NOT along nrm, which
    // is the view ray and re-orients with the camera (review_opus.md's exact G2 finding: "A's replacement...
    // puts the rise along the view direction, deleting both the world-up lift and the no-downdraft clamp").
    // ss_billow_w's axes are already world axes (built from the card frame's right/up/normal, all world-space
    // vectors), so its OWN .z IS the true world-vertical component, exactly as sphere_n.z was in the #else branch.
    // Left UN-normalized here on purpose - see the shared normalize below, which this and the #else branch both
    // feed. THIS FIXES ONLY HALF of review_opus.md's G2 finding, and the other half is still open (2026-09-06
    // opus S0-S3 fix pass, finding 4): the finding had two independent parts - (i) the meridional plane's AXIS
    // is spanned by the quad's radial direction and the CARD'S normal, which is the view ray eased toward +-Z near
    // the poles, so the ring still re-orients as the camera orbits a puff (always face-on: a puff seen from below
    // and one seen edge-on show the SAME roll pattern, where a real turret's ring axis is vertical) - unfixed, and
    // the ss_card_frame call just above builds exactly that camera-framed axis (correctly, now - it is the axis the
    // quad really stands on - but still a camera-framed one); (ii) the rise/clamp being on the view axis rather
    // than world-up - fixed, here, correctly, as the rest
    // of this comment argues. review_opus.md's own adoption plan schedules the axis fix as Stage 6 ("G2 billow
    // roll: prototype only... Build the field in the world frame (world-up as the ring axis, radial distance from
    // the puff's own axis as the meridional coordinate)... Land only if the ring is visible in the PPM") -
    // deliberately not landed in this stage; visual_flowfields_shader.cpp's PPM proves the roll sense (out and
    // over) but cannot show the orbit dependence, which is what Stage 6 exists to fix.
    vec3 flow_w = vec3(ss_billow_w.xy, max(ss_billow_w.z, 0.0) + SS_FLOW_RISE);
#else
    // RAW, un-normalized - see the shared normalize a few lines below, in the SS_G5_SWIRL #else branch, which
    // this feeds exactly as it fed the old single-expression `normalize(vec3(...))` (split across two
    // statements now purely so SS_G2_BILLOW and this branch can share the same later normalize; the VALUE is
    // identical to before, since normalize(v) computed here or a few lines down is the same call on the same v).
    vec3 flow_w = vec3(sphere_n.xy, max(sphere_n.z, 0.0) + SS_FLOW_RISE);
#endif

#if SS_G5_SWIRL
    // <SS:Nexii> S3 G5_SWIRL (doc/atmo_magic_flow_field.md section 1 "Flow fields", review_opus.md's A-G5
    // verdict "ADOPT-W-FIX ... clamp swirlMag"): tangential 1/r field around the storm's owner centre, replacing
    // the constant-angle SS_STRIATION_ROT nudge (the #else branch below) with a field that curls TOWARD the
    // mesocyclone rather than rotating every puff's flow by the same fixed angle regardless of position.
    // rMeso floored at SS_MESO_SWIRL_MIN_M so the tangent never divides by a (near-)zero distance at the meso's
    // own axis - an eps-floor, not a normalize. swirlMag clamped at SS_MESO_SWIRL_MAX: unfloored,
    // meso*(SS_MESO_SWIRL_M/rMeso) reaches meso*15 at the floor radius (review's own measurement), which would
    // let rotation alone dictate the post-normalize direction with nothing else surviving it.
    vec2 ss_to_meso = ss_owner_delta;
    float ss_r_meso = max(length(ss_to_meso), SS_MESO_SWIRL_MIN_M);
    vec2 ss_swirl_tangent = ss_rot90(ss_to_meso) / ss_r_meso * storm_sample.rotSign;
    float ss_swirl_mag = min(storm_sample.meso * (SS_MESO_SWIRL_M / ss_r_meso), SS_MESO_SWIRL_MAX);
    flow_w.xy += ss_swirl_tangent * ss_swirl_mag;

    // Renormalize the FULL 3-D vector once, here - the fold above only ever touches .xy, so .z is exactly
    // whatever the SS_G2_BILLOW #if/#else pair above set it to: max(..., 0.0) + SS_FLOW_RISE, which is strictly
    // positive (SS_FLOW_RISE alone is already > 0) whatever the max() clamped away. A vector with a strictly
    // positive component has a strictly positive length, so THIS normalize can never see a zero vector - no
    // guard needed here, unlike G3's blend below, which genuinely can cancel z to exactly zero (this xy-only
    // add never touches z at all).
    flow_w = normalize(flow_w);
#else
    // <SS:Nexii> Optional cosmetic [interaction: ssstormcouplecore.h] (doc/atmo_magic_storm_dynamics.md section 3 "Supercell" - "persistent rotation drives base striations ... via a rotational detail-scroll bias"): nudges the detail flow's horizontal direction by the storm's own spin, meso-weighted so it strengthens toward the wall cloud and signed by rotSign so the rare anticyclonic hash spins the other way. A rotation of just the xy pair leaves flow_w's length untouched (z is unchanged, and rotation preserves the xy sub-vector's own length), so no renormalize is needed. Off-cell storm_sample.meso is 0 and this is the identity.
    // <SS:Nexii> S3: the normalize that used to open this whole block (`vec3 flow_w = normalize(vec3(...))`)
    // moved down here unchanged in VALUE - see the SS_G2_BILLOW #else branch's own note above. With both
    // SS_G2_BILLOW and SS_G5_SWIRL at 0 this reproduces today's exact sequence: build the raw vector, normalize
    // it, then rotate - byte-identical, just split across two statements instead of one nested expression.
    flow_w = normalize(flow_w);
    float striation_ang = storm_sample.meso * storm_sample.rotSign * SS_STRIATION_ROT;
    float striation_c = cos(striation_ang);
    float striation_s = sin(striation_ang);
    flow_w.xy = mat2(striation_c, -striation_s, striation_s, striation_c) * flow_w.xy;
#endif

#if SS_G3_OUTFLOW
    // <SS:Nexii> S3 G3_OUTFLOW (doc/atmo_magic_flow_field.md section 1 "Flow fields", review_opus.md's A-G3a
    // verdict "ADOPT-W-FIX ... the best structural idea in A"): anvil outflow - radial from the storm's owner
    // centre, blended toward the upper wind by downwind alignment (proto.h's f4_anvil, the ladder-proven form),
    // magnitude capped short (not absent) upwind via SS_ANVIL_UPWIND_CAP, so the back-shear flank still shows
    // some outward push rather than none. ss_safe_dir wherever a direction is needed from a vector that CAN be
    // zero (a fragment sat exactly on the owner's centre) - never a bare normalize(). storm_sample.anvil stands
    // in for the fuller `anvil_w` this function computes later (below the tower/map read, ~40 lines further
    // down): that value depends on n_map and the storm-widened tower window, neither read yet at this point in
    // the puff branch, and reordering the base-cut/tower sequence to read it early would touch code well outside
    // this stage's scope. storm_sample.anvil is already the coupled cell's own quantized-cell anvil term, the
    // same value anvil_w itself folds in via max() later, so an outflow direction or streak gain keyed on it
    // agrees structurally with where the fuller anvil_w will also read high. ss_radial_dir/ss_wind_align are NOT
    // redeclared here - they are the ones hoisted above `streak`'s own declaration (2026-09-06 opus S0-S3 fix
    // pass, finding 1), reused as-is so the outflow direction and the streak gain agree on the same alignment.
    vec2 ss_anvil_dir = ss_safe_dir(mix(ss_radial_dir, ss_wind, ss_wind_align));
    float ss_anvil_flow_mag = storm_sample.anvil * storm_sample.owner * mix(SS_ANVIL_UPWIND_CAP, 1.0, ss_wind_align);

    // NOT a bare normalize-of-a-mix: review_opus.md's own NaN finding is against exactly this shape of blend
    // (design A's G2/G3 combination fused the mix directly inside a single normalize() call), reproduced
    // here as the thing NOT to do. In THIS implementation, unlike design A's, flow_w.z is strictly positive
    // every time this block runs (both SS_G5_SWIRL branches above end by normalizing a vector whose z came from
    // the rise floor, and the swirl fold, when it runs, only ever touches .xy) - twin_flowfields.cpp's degenerate-
    // case tests confirm the blended vector never actually reaches exactly zero for any input this pipeline can
    // produce, because ss_anvil_flow_mag reaching 1.0 would need ss_wind_align to reach exactly 1.0, which
    // ss_safe_dir's eps floor keeps just short of for any finite ss_owner_delta. The guard is kept anyway as
    // defensive engineering, not because this exact formula is known to need it: a future change to the mag
    // formula, to SS_G5_SWIRL, or to the eps constant could reopen the case, and the guard costs one dot product
    // and a branch. What the guard DOES demonstrably matter for today is the ILL-CONDITIONED case review_opus.md
    // measured against the ORIGINAL flow (length ~0.21 at mag 0.5 near the equator) - a small-but-nonzero
    // denominator that swings the normalized direction wildly frame to frame; the guard's fallback keeps that
    // case bounded rather than amplifying it.
    vec3 ss_anvil_t = mix(flow_w, vec3(ss_anvil_dir, 0.0), ss_anvil_flow_mag);
    flow_w = (dot(ss_anvil_t, ss_anvil_t) > 1.0e-6) ? normalize(ss_anvil_t) : flow_w;

    // The streak gain itself is NOT applied here any more - see `streak`'s own declaration, well above this
    // block, where it now folds into the SAME variable wind_uv/pl_yz/pl_xz already read (2026-09-06 opus S0-S3
    // fix pass, finding 1). Applying it down here reached only flow_yz/flow_xz/flow_xy below, which shortened
    // the advected detail's travel instead of stretching the static base body - the opposite of both halves of
    // A-G3b's point (sharp upwind, fibrous downwind).
#endif

    // The same direction seen in each plane's own two axes. Whichever plane the triplanar weights favour, the detail is travelling the same way through the world. Scaled by boil, so the top of the
    // layer travels further per cycle than the base does. Safe to vary per fragment here in a way it never was on the rate: this multiplies a DISTANCE that resets every cycle, so it cannot
    // accumulate into the growing shear that scaling the rate caused.
    // <SS:Nexii> [interaction: ssdeckflowcore.h] EDGE MASK, LITERAL twin of SSDeckFlow::maskedReach (fifth build
    // report: "looking quite strange due to lack of masking around edges"): the advected octave's travel faded out
    // by the puff's OWN radial falloff, `rim` - the same term the carve a few lines below already multiplies the
    // density by, so the flow reaches zero exactly where the card's silhouette does. WHAT IT FIXES: the silhouette
    // is decided in the band r in [SS_PUFF_RIM, 1] where rim ramps, and the flow used to run at FULL travel right
    // through that band, so the edge of the mask writhed by the whole advection distance while the interior only
    // shuffled its texture. rim is 1.0 for every r <= SS_PUFF_RIM, so the core boils exactly as before.
    // WHAT IT DOES NOT FIX, stated because the obvious reading is wrong: the flow could never have drawn the
    // card's square outline. rim is already exactly zero for every r >= 1 and every point of the quad's boundary
    // has r >= 1 (the corners reach 1.414), so the alpha there was already exactly zero - measured over 400
    // perimeter points at worst-case noise, max alpha 0.000000000 (unit_deckflow.cpp perimeter_alpha_already_zero).
    // The rung that pins this one: max |advected - un-advected| around the perimeter at 16 boil phases is
    // 0.000000000 with the mask and 0.737767 without it, while the centre still moves (edge_mask_perimeter).
    float reach = min(SS_FLOW_M * boil / oct2_m, SS_FLOW_MAX_TILES) * rim;
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
#if SS_G2_VIGOR
    // <SS:Nexii> S3 G2_VIGOR (doc/atmo_magic_flow_field.md section 1 "Carve", review_opus.md's B-G2c verdict
    // "ADOPT ... the RIGHT way to spend C3 - and notably it does exactly what A-G1b tried and failed to do, by
    // turning the dial that already exists"): a puff under high storm boost and not already smothered by an
    // anvil overhead reads crisper - more of its own detail noise decides the silhouette - than a calm-sky puff
    // does. Contrast MULTIPLIER only: the erosion-carve swap this axis eventually feeds is S6, not this stage.
    // max(ss_anvil, storm_sample.anvil) - NOT storm_sample.anvil alone (2026-09-06 opus S0-S3 fix pass, finding
    // 2: the earlier landing substituted storm_sample.anvil for the fuller, map-refined `anvil_w` this function
    // computes later - anvil_w = max(ss_anvil, storm_sample.anvil) - on the ordering excuse that anvil_w's own
    // n_map/tower refinement isn't read yet at this point. That excuse only covers HALF of anvil_w: ss_anvil is
    // a plain uniform, readable anywhere, with no ordering dependency at all, and reading storm_sample.anvil
    // alone silently dropped it - so a puff under an AUTHORED anvil (doc/atmo_magic_phase8_show.md's authored
    // severe weather) sitting over a boosted storm cell went un-suppressed and read crisper where the design
    // says smothered. Folding in ss_anvil costs nothing - one extra token, no dependency on n_map or the tower
    // window - and this max() is exactly the anvil_w's own eventual max(), just evaluated early on its two
    // already-resident inputs rather than deferred to the refined value.
    float ss_vigor = clamp(storm_sample.boost - max(ss_anvil, storm_sample.anvil) * 0.6, 0.0, 1.0);
    density = clamp((noise - 0.5) * (SS_PUFF_CONTRAST * mix(1.0, 1.6, ss_vigor)) + shape, 0.0, 1.0) * rim;
#else
    density = clamp((noise - 0.5) * SS_PUFF_CONTRAST + shape, 0.0, 1.0) * rim;
#endif

    // ...and cut flat underneath - see SS_BASE_SOFT_M. Softened over a few tens of metres rather than a hard edge, because a real cloud base is ragged at the scale of the wisps hanging off it, just
    // not at the scale of the deck.
    density *= smoothstep(ss_base_z, ss_base_z + SS_BASE_SOFT_M, world_true.z);

    // <SS:Nexii> The convection noise map, read again at the fragment level. The CPU shaped the FIELD with this map - which columns stand up as towers, which fall into pockets - and this stage runs the same two reads the builder does, in the same direction: map HIGHS are towers, map LOWS are pockets. What the map must never do down here is shape the BOTTOM - that inversion (spiky undersides, flattened tops) is what the first cut did before the guard below, and it read as the map being applied upside down. Two cuts, both gated by the anvil weight so an ordinary convective sky keeps every ball it grew: The height ramp - the map blends toward 70% white as the fragment climbs the deck's last stretch toward the cirrus band, so near the lid EVERY column passes the tower window and the whole top consolidates into the anvil rather than only the strong columns' tops. The slope carve - the anvil's UNDERSIDE, deep where a tower feeds it, thinning to a sheet away from one - guarded so it can only bite in the deck's upper half. The base band is where the cloud keeps its body; an anvil carve that reaches the floor is what shredded the undersides. All three vertical windows - and the thick-base fill - come from the authored profile ramp when one is bound, from these built-ins when not. The ramp's four channels mean the author paints the whole vertical story in one strip: solid base, carving middle, white ramp to the anvil.
    float v_h = (world_true.z - ss_base_z) / max(ss_layer_thick, 1.0);
    vec4 prof = (ss_profile > 0.5) ? texture(bumpMap2, vec2(0.5, clamp(v_h, 0.0, 1.0))) : vec4(0.0);

    float anvil_w = ss_anvil;

    // The tower weight: the map, maxed with the profile's ramp-to-white - built-in 0.7 across
    // the top 30% when no strip is bound. n_map is the raw convection-map sample; it stays 0
    // with no map bound, exactly as the builder's own noiseFieldAt leaves raw_n at its
    // "not ready" sentinel (ssvolcloud.cpp) - so ramp_h alone still drives the ramp built-in.
    float n_map = 0.0;
    if (ss_noise_tile > 0.0)
    {
        // <SS:Nexii> Phase 4, revised 2026-09-05 [interaction: ssdeckframecore.h]: shape_air, NOT gate_air - this reverses the old F4/#10 fixup, which had it backwards (doc/atmo_magic_wind_profile.md
        // section 4 "Frame contract"; ssdeckframecore.h's header note calls out that exact draft: "The 4b draft that read n_map at gateAir was inverted"). This read feeds ss_storm_towerFromMap
        // (tower below) and the thick-base fill's n_map term, both of which classify a COLUMN - and a puff is a PLACEMENT, not a plain cell centre: the CPU reads the builder's gate/presence/n_map
        // at the unshifted cell c but then DRAWS that puff at c + O(z_puff) (placeWorld). An observer standing at the drawn position must undo O(z) to recover c, or it classifies the column the
        // lean happened to land the puff over rather than the one that grew it - up to the shear span apart. shape_air is exactly that undo (ss_frame_frameAir). The sheet's own n_map read (line
        // ~663) stays on gate_air because the sheet is base-anchored BY DEFINITION - it never evaluates O(z) at all, not because O(z0) == 0 there (see that call site's own comment for why that
        // older framing was wrong) - not because this rule is different for the sheet.
        // <SS:Nexii> [interaction: ssdecknoisecore.h] De-tiled (doc/atmo_magic_far_clouds.md section 2 step 4) - see ss_noise_mapDetiled. Still shape_air, per this block's own comment above.
        n_map = ss_noise_mapDetiled(shape_air);
    }
    float ramp_h = mix(0.7 * smoothstep(0.70, 1.25, v_h), prof.r, ss_profile);

    // <SS:Nexii> [interaction: ssstormcouplecore.h] review 3b NEW-1: only the storm's OWN anvil term folds in unconditionally here, outside the map branch - LITERAL twin of the CPU's cellShapeAt, which always folds cell_anvil through SSStormCouple::anvilWeight regardless of whether noiseFieldAt found a ready map (ssvolcloud.cpp), and storm_sample.anvil is 0 off-cell so an uncoupled deck is unaffected. S5: sampled at the QUANTIZED air-cell centre (ss_storm_samplePoint), not world_true.xy, so this fragment reads the same storm sample the builder used for its cell. storm_p/storm_sample are now computed once in main()'s shared frame block above (Phase 4 hoist, needed there for gate_air/shape_air and the gloom block's meso term below) rather than here - same values, same call. The map-driven tower and its early-anvil escalation stay gated behind ss_noise_tile>0.0 below, exactly as they read before the 3b hoist - a profile-only deck with no map bound gets no early anvil consolidation from the ramp, only from the storm term here.
    anvil_w = max(anvil_w, storm_sample.anvil);

    if (ss_noise_tile > 0.0)
    {
        // The tower weight: the map, maxed with the profile's ramp-to-white, under the (possibly
        // storm-widened) tower window - LITERAL twin of the builder's own SSStormCouple::towerFromMap
        // call in ssvolcloud.cpp; ss_storm_n 0 (the under deck) reads storm_sample.boost 0 and the
        // widened window collapses back to the raw ss_tower_ramp, so an uncoupled deck is unaffected.
        float tower = ss_storm_towerFromMap(max(n_map, ramp_h), ss_tower_ramp.x, ss_tower_ramp.y, storm_sample.boost);

        // The early anvil: the same ramp the builder runs, so the top of the deck takes the lid -
        // and with it both cuts - before the deck-wide anvil figure says so. LITERAL twin call
        // (phase-3c F3) instead of the inlined max/smoothstep sequence - see ss_storm_anvilWeight's
        // definition above and its core's own ss_churn-vs-convection caveat (ssstormcouplecore.h
        // anvilWeight). deckAnvil is ss_anvil, not the running anvil_w: max is associative, so
        // folding stormAnvil back in here (already applied outside the branch, above) is a no-op,
        // value-identical to the old inline sequence.
        anvil_w = ss_storm_anvilWeight(ss_anvil, ss_churn, tower, storm_sample.anvil);

        // The carve guard: profile green, built-in the upper half only.
        float base_guard = mix(smoothstep(0.20, 0.45, v_h), prof.g, ss_profile);

        float sheet_m = min(ss_layer_thick * 0.35, 420.0);
        float floor_z = mix(top_z - sheet_m, ss_base_z, tower);
        density *= mix(1.0, smoothstep(floor_z - 120.0, floor_z, world_true.z), anvil_w * base_guard);

        // The thick-base fill: profile alpha, none built-in. A density floor, fed by the map's
        // own mottle so the solid base still varies with the geography the deck was carved by.
        float fill = prof.a * clamp(0.6 + 0.8 * (n_map - 0.5), 0.0, 1.0);
        // <SS:Nexii> S0 fill bug fix (doc/atmo_magic_flow_field.md section 2, review_opus.md F-0.9/Stage 0): the
        // old `step(v_h, 1.0)` alone is 1 for v_h < 0 too (a one-sided ceiling, no floor), so an authored profile
        // with a non-zero alpha at v=0 re-filled density BELOW the base cut this fragment already applied above
        // (the smoothstep(ss_base_z, ss_base_z + SS_BASE_SOFT_M, ...) cut earlier in this branch), defeating the flat base cut and glowing under the deck's own floor. Bound to the [0,1]
        // band with `step(0.0, v_h)` added: v_h < 0 now contributes 0, [0,1] is bit-for-bit unchanged (step(0,x)
        // is 1 there), matched and asserted by V:\Scratch\atmo\tests\twin_flowchannel.cpp.
        density = max(density, fill * step(0.0, v_h) * step(v_h, 1.0));
    }

    // ...and flat on top too, once there is an anvil to flatten - see SS_TOP_SOFT_M. Faded in by
    // the anvil weight - the map's early ramp included - so an ordinary convective sky keeps its
    // rounded tops and only a driven one gets the table.
    // <SS:Nexii> [interaction: ssstormcouplecore.h] review 3b NEW-4 (doc/atmo_magic_storm_dynamics.md section 3, "Overshooting top vs the lid cut"): lid_top replaces the bare top_z in this cut and in the cap band below only - everything else in this shader keeps top_z. LITERAL twin of SSStormCouple::lidTopM, read at storm_sample (the quantized-cell sample above), so the lid rises only where the CPU actually lifted that cell's tallest sub-puff, never at the fragment's own true position.
    float lid_top = ss_storm_lidTopM(top_z, ss_layer_thick, storm_sample.overshoot);
    float lid = 1.0 - smoothstep(lid_top - SS_TOP_SOFT_M, lid_top, world_true.z);
    density *= mix(1.0, lid, anvil_w);

    // The torn cap band: profile blue where authored, the built-in metres-wide band under the
    // lid otherwise; the noise the puff carries decides what survives inside it.
    float cap_band = mix(smoothstep(lid_top - 260.0, lid_top - 30.0, world_true.z), prof.b, ss_profile);
    if (cap_band > 0.001 && (ss_profile > 0.5 || ss_noise_tile > 0.0))
    {
        float chunk = smoothstep(0.32, 0.52, noise);
        density *= mix(1.0, chunk, anvil_w * cap_band);
    }

    noise_v = noise;
    }
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
    // <SS:Nexii> MEASURED AND STATED, NOT FIXED (fourth build report, D1) [V:\Scratch\atmo\tests\unit_deck_radiance.cpp]: sphere_n is the FAKE SPHERE's normal and ss_sphere_normal builds that sphere
    // around `nrm`, the view ray, so the sphere's pole always points at the camera. Look up at a deck lit from above and every card's pole IS its anti-light point: wrap runs 0 at the card's own centre
    // to 0.5 at its limb, the body factor follows it from SS_FORM_DARK to the mid, and the thin-weighted fringe terms below are simultaneously smallest in the middle (thin = 1 - density) and largest in
    // the ring. Measured with the light overhead, this costs a puff's core 21.5% of its radiance between a 5-degree and an 89-degree view (1.546 -> 1.213) while nothing behind it dims, and the ring gain
    // (annulus / centre) rises 1.209 -> 1.277 over the same sweep. At a shallow view the identical construction produces the CORRECT lit-top / dark-bottom gradient, which is why the horizon rows read
    // fine. NOT CHANGED HERE, deliberately: the build report this was chased for turned out to be the advected octave's own once-a-second phase jump (D3, see `cycles` below and ssdeckboilcore.h), the
    // hypothesis that this term was the cause was retracted, and landing a second, unrequested change to the same pixels on the same build would make neither judgeable. The candidate, measured in the
    // same rung, is one line - ease wrap toward 0.5 by smoothstep(0.55, 0.95, abs(dot(nrm, ss_light_dir))), the identity for every side-on puff - and it holds the core flat over the sweep (1.546 ->
    // 1.578) instead of losing a fifth of it. The fuller answer is review_opus.md's own outstanding G2 item (i), which the billow block below already records as unfixed: build the sphere in the WORLD
    // frame so its axis stops tracking the camera at all.
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

    // <SS:Nexii> [interaction: ssstormcouplecore.h] Local gloom (doc/atmo_magic_storm_dynamics.md section 3 "Local gloom"): the mesocyclone's own wall-cloud darkening, multiplied over the SAME gloom above - both the puff path (this line) and the sheet path share this one gloom variable, and storm_sample.meso is the SAME storm field sample read at this fragment's own quantized cell point (main()'s shared frame block), so the veil directly under a wall cloud darkens by the identical amount with no seam between sheet and puffs. storm_sample.meso is 0 off-cell, so an uncoupled deck is unaffected.
    gloom *= (1.0 - SS_MESO_GLOOM * storm_sample.meso);

    // <SS:Nexii> S4 (doc/atmo_magic_flow_field.md section 2 S4): the switchable lighting variants, all gated on the SAME live ss_light_variant uniform SSAtmoCloudLightVariant drives - variant 0
    // touches NONE of ss_powder/ss_amb below (both stay at their identity values 1.0/vary_ss_amblit), so puff_light below is the exact original expression, bit-for-bit.
    bool ss_v_powder = (ss_light_variant == 1 || ss_light_variant == 3);
    bool ss_v_hg2 = (ss_light_variant == 2 || ss_light_variant == 3);
    bool ss_v_tint = (ss_light_variant == 3);

    // POWDER (variants 1/3): darkens the DIRECT-SUN body contribution only - see SS_POWDER_DEPTH's note for the exact curve and why it is ported raw, unnormalized, from scene.h's V_POWDER.
    float ss_powder = 1.0;
    if (ss_v_powder)
    {
        float ss_d = SS_POWDER_DEPTH * vary_color.g;
        ss_powder = exp(-ss_d) * (1.0 - exp(-2.0 * ss_d));
    }

    // FULL only (variant 3), and only under gloom (research_lighting.md #4's airlight-through-a-deep-backlit-storm "green sky" cue): the AMBIENT contribution tints toward a subtle storm green,
    // deepest where the puff is both heavily buried AND the deck's own gloom is severe - (1.0 - ss_gloom) is 0 at ss_gloom 1 (fair weather, untouched) and grows as the storm darkens, vary_color.g
    // is the same buried fraction POWDER reads above, so the cast is deepest in the buried bellies of dark storms, exactly as the doc's verdict describes it ("the severe green cast, deepest in
    // buried bellies of dark storms"). The sun contribution is untouched by this term - only ambient carries it, so a lit lid's direct sun still reads as the sunset's own colour.
    vec3 ss_amb = vary_ss_amblit;
    if (ss_v_tint && ss_gloom < 1.0)
    {
        vec3 ss_tint = mix(vec3(1.0), vec3(0.92, 1.0, 0.94), (1.0 - ss_gloom) * vary_color.g);
        ss_amb *= ss_tint;
    }

    vec3 sun_body = vary_ss_sunlit * vary_color.r * ss_powder;
    vec3 puff_light = (ss_amb + sun_body) * gloom;

    // The noise self-shade's mid, shared by the body and the graze light below - the lid keeps its texture whichever term is carrying it.
    float noise_mid = mix(mix(1.0 - SS_PUFF_SHADE, 1.0, noise_v), 0.83, form_flat);

    vec3 body = puff_light
              * mix(mix(SS_FORM_DARK, 1.0, wrap), 0.78, form_flat)
              * noise_mid;

    // <SS:Nexii> The graze light joins HERE, past the body's full wrap, rather than riding the form term as it used to. Inside the form sum it was multiplied by the sun's own wrap, and the sun's
    // wrap is exactly what a skimmed lid is not shaped by: at a grazing sun the whole horizon band is burning above the deck, and that sky lights the crown of a puff from every side - so the crest
    // came out as a warm stripe up the sun side of each lid puff with the anti-sun half held at the 0.55 floor, and the alpenglow the ramp exists to paint never read as a lit LID. A gentler wrap
    // of its own (SS_GRAZE_DARK) keeps the sun side warmest without ever putting a crown's far side in the dark; the same gloom and the same capped sunlit colour, so the crest stays the sunset's
    // own fire and a storm still eats it. UNTOUCHED by POWDER (research_lighting.md #1's own scope is the sun-facing body term, not every sunlit contribution) and by HG2 below - the graze is its
    // own light, riding vary_ss_sunlit directly, exactly as variant 0 does.
    body += vary_ss_sunlit * (vary_ss_top * gloom)
          * mix(mix(SS_GRAZE_DARK, 1.0, wrap), 0.78, form_flat)
          * noise_mid;

    // The bright fringe where the puff is thin enough for light to come through it - see SS_RIM. Fed by the capped vertex-stage sun light, so at a low sun the fringes carry the sunset's own
    // hue - the uniform this read before was the CPU replica that had already crushed to grey by then.
    // <SS:Nexii> S4 HG2 variant (variants 2/3): replaces this thin^3 rim with a two-lobe Henyey-Greenstein phase on the view/sun angle (research_lighting.md #3), so the bright fringe tracks the
    // sun's SIDE of the puff rather than glowing on every thin silhouette edge regardless of where the sun is. c = dot(-nrm, ss_light_dir): nrm is the existing to-eye normal (fragment-toward-
    // camera, `nrm = to_eye / drawn_dist` above), so -nrm is the camera-ray-into-the-scene direction (eye toward fragment and beyond) - the same convention scene.h's `camera.rayDir(x,y)` uses
    // against its own sunDir. When the sun sits BEHIND the puff from the viewer's side (looking toward the sun through the cloud), the ray onward from the fragment and the direction to the sun
    // both point further into the scene, so c runs toward +1 there - which is exactly where the forward lobe (g=0.8, weighted 0.55) peaks, so that geometry is the one that brightens: the silver
    // lining lands on the sun side, not on every edge. REPLACED, not added: the old wrap-weighted thin^3 term is skipped entirely on this branch.
    if (ss_v_hg2)
    {
        float ss_c = dot(-nrm, ss_light_dir);
        float ss_phase = mix(ss_hg2_phase(ss_c, -0.2), ss_hg2_phase(ss_c, 0.8), 0.55);
        body += vary_ss_sunlit * (SS_HG2_GAIN * ss_phase * thin * thin) * (1.0 - form_flat);
    }
    else
    {
        body += vary_ss_sunlit * (SS_RIM * wrap * thin * thin * thin) * (1.0 - form_flat);
    }

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

    // Bounded, then doubled. Everything feeding this is in EEP's HDR units - sunlight and ambient both run well past 1, and the rim term adds a whole sun colour on top - so the shading came out far
    // brighter than anything else in the frame. Nothing writes to a glow buffer here, but the bloom pass takes its bright-pass off the finished screen, and unbounded cloud sails straight over that
    // threshold. Hence the halo around every puff. The bound is what stops that, and it still does: ss_soft_clip_unit is strictly under 1 for every finite input, so `* 2.0` occupies exactly the
    // range the old `clamp(..., 0, 1)` did. WHAT CHANGED and why (fourth build report, D0): the bound used to be that hard clamp, and a hard clamp has ZERO derivative above 1 - so with a daylight
    // sky saturating the sum, cutting the direct-sun body term by 92% (SSAtmoCloudLightVariant's POWDER) produced an IDENTICAL pixel and the A/B dial appeared to do nothing at any value. The soft
    // clip keeps a non-zero derivative at every input, so the variants separate again; it is also hue-preserving where the per-channel clamp desaturated everything past 1 toward white, which is a
    // second reason bright deck did not match the sky beside it. See ss_soft_clip_unit above and ssairlightcore.h, which owns the curve.
    // <SS:Nexii> Stale claim corrected while here: this line used to say it was "bounded exactly the way the dome layer bounds itself". That is true of the cirrus BAND (cloudsF.glsl's bound is the
    // same expression, character for character) and FALSE of the SKY: skyF.glsl carries no per-channel clamp at all any more - it doubles and then caps the PEAK at 5.0, hue-preserving, with its own
    // note that a per-channel clamp "saturates anything past 5 to white ... so the light's own red came out white". The curve adopted here is the soft, hue-preserving version of exactly that cap.
    // NO scene-gamma term here, and its absence is a lesson: pow(c, 1/gamma) with an authored storm-sky gamma of a few tenths is an exponent of several - it crushed every mid-tone to black and left
    // white ridges with pink rims (red dies last), the whole deck reading as a burnt negative.
    // No fog pass after this any more: the atmosphere came in through the vertex-stage colours, the same door it enters the band by (the slab-ray transmittance in the cloud terms, the airlight
    // joining here - see ssVolCloudV.glsl). At the rim the transmittance takes the cloud terms to nothing and both deck and band APPROACH the same pure airlight. Softened from the old "converge to
    // the same pure airlight, so the handoff is exact by construction": measured, the band's airlight weight is (1 - T) and the dome's is (1 - T^0.25), so the gap rises to a mid-path maximum of
    // 1.14e-2 at 5 km before closing - 5.08e-3 at 20 km, 5.96e-8 at 400 km - and is bit-exact only once T underflows. Convergence is a limit, not an identity.
    vec3 shaded = ss_soft_clip_unit(body + vary_ss_airlight) * 2.0;

    frag_color = vec4(shaded, a);
}

