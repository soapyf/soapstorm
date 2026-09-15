# Atmo Magic — flow-field cloud upgrade (design, 2026-09-06 overnight run)

Goal: make the volumetric puff field read as one living cloud mass — neighbouring puffs merging (G1),
towers billowing like slow smoke (G2), anvils with real outflow structure (G3), a deck floor with scud /
wall-cloud / shelf detail (G4), and rotation that reads on supercells and funnels (G5) — by flow-map /
scrolling-layer tricks in the existing billboard pipeline, never raymarching. Method: three-way design
competition (fragment-only / vertex+CPU offload / density-buffer composite), an opus adversarial review of
all three, and a five-round CPU visual ladder (`V:\Scratch\atmo\flow\` — BRIEF.md, RESEARCH.md,
research_lighting.md, research_vfx.md, entries/, JUDGING.md, ref/ photo library, out/*.png). Every verdict
below is backed by a measured or rendered artifact, not an argument.

## 1. Settled verdicts

**Carve (G1/G2 texture).** The Nubis erosion remap replaces add+contrast ONLY in the round-2 formulation:
the base/sum must keep headroom below saturation (erosion cannot bite a saturated base), the noise gets a
~2.6x contrast pre-stretch before eroding (raw fbm hugs its mean and has gain only w/(1-w)), and a steep
opacity curve (smoothstep(0, 0.55, density)) restores solid cores. Vigor axis: young = fine-scale detail +
steep alpha (hard cauliflower lobes), decayed = coarse detail + directional smear (shift 0.35w, stretch
1+7w) + shallow alpha. Design A's original spec (erode the radial window at w 0.15, no alpha curve) is
rejected per the opus review — at calm-sky weights the noise controlled 0.8% of the radius. Introduce the
new carve blended by a weather/vigor weight so a calm sky keeps today's look until the new one has earned
the swap in a build.

**Phase (G2).** The advected detail's phase becomes PER-PUFF, carried on the vertex colour's b low half
[0, 0.49] (7 bits there; the shaft's b > 0.5 flag untouched). Per-cell hashed phase is rejected — a puff
quad spans 2-10 hash cells and the ladder rendered the hard seam a floor()-constant phase cuts through one
puff face (billow_v2_async panel 5). The per-quad phase is free precisely because the advected octave is
already per-quad discontinuous (flow_w derives from sphere_n). Bilinear-interpolated phase is the fallback
for channel-less paths (the sheet), C0 through the fract wrap but Jacobian-limited, so per-puff wins where
a channel exists. Advection reach stays <= 0.5 tiles: the ladder's difference strips show the dual-phase
crossfade flaring at mid-cycle when travel grows (the Vlachos limit made visible); per-puff phase also
desynchronizes those pulses across the field. A three-phase crossfade is the escalation if a build still
shows breathing — one more fetch, not taken yet.

**Flow fields (G2/G3/G5).** The billow field is `f1_ring` (flow/proto.h): two counter-rotating ring cores
below centre in the puff's meridional frame, at (±0.55, −0.15), plus a central updraft term, each ring
contribution rot90(d)*s*|d|/(eps+|d|^3) so it VANISHES at the core — never normalize a summed flow vector
(the seam and NaN class both the ladder and the opus review hit independently; design A's
normalize(mix(...)) had the same fault). The 0.5x curl-noise companion (`f3_ring_curl` = f1_ring + f2_curl)
is deliberately NOT shipped — it costs 8 extra fbm evaluations against S3's zero-new-fetch budget; f1_ring
alone is the settled verdict, and f3 is the stated escalation if a build wants more turbulence in the roll
(2026-09-06 opus S0-S3 fix pass, finding 3 — an earlier landing shipped the ring cores alone, at (±0.55,
0), missing both the −0.15 offset and the updraft term, while its own comments claimed a literal f1_ring
port; this line now matches what actually ships). Anvil outflow (`f4`): radial from the owner storm cell's
centre blended toward the upper wind by downwind alignment, magnitude capped short upwind (back-shear) —
all inputs already uniform-resident (ss_storm_a, ss_wind); the outflow direction reads `ss_owner_delta`,
the fragment's un-quantized world offset from the owner cell's own centre (`world_true.xy − o −
storm_sample.ownerC`) — the un-quantized companion of the point the storm field was sampled at
(`storm_p`), NOT `shape_air`, which is a pattern-read coordinate for a different purpose entirely (at
anvil altitude O(z) is of order a storm radius — opus finding, and using shape_air here would be wrong by
ss_drift + hero_shift, since storm centres neither drift with the deck nor take the hero shift). Meso
swirl (`f5`): tangential 1/r field around the owner centre signed by rotSign replaces the constant-angle
SS_STRIATION_ROT nudge; eps-floored, magnitude meso-weighted. Owner-cell snapping at influence boundaries
is an accepted pre-existing limitation (the meso/dir/rotSign fields already have it).

**Merge (G1).** The analytic neighbour-kernel merge is the adopted direction, proven on the ladder:
each fragment sums its OWN smooth kernel (2r^3/R^3 - 3r^2/R^2 + 1) with its neighbours' kernels evaluated
analytically (no fetches), runs the round-2 sum->erode chain on the SUM, and draws
alpha = 1 - (1 - fusedA)^share with share = own kernel / sum. The exponent form is EXACT under
back-to-front blending — measured 0.000 error against the single-pass ground truth — whenever overlapping
quads agree on the sum, because prod(1-a_i) = (1-A)^{sum s_i} = 1-A. Neighbour lists are built on the CPU
at build time by ranking overlap edges NEAREST-FIRST with mutual-capacity acceptance (cap 2-3); a
hash-ranked symmetric cap is rejected by measurement (it drops strong edges blind and performed worse than
no fix: 0.067 vs 0.011 mean error; strength-ranked collapsed it to 0.0014). Quads need ~1.6x headroom so
the fused silhouette fits. Transport (settled by the ladder's transport prototype, flow_transport.png): a HYBRID by LOD regime.
NEAR FIELD (Tier A, sub-puffs live): vertex-channel transport of 2-3 true neighbours — in-shader
reconstruction of subs 1-2 from the hash either omits them (mean error ~0.57 of a puff's peak density) or
needs a proxy that still leaves visible residue, costs 2.2-2.7x the sheet's existing per-fragment hash
budget before paying for the storm-sample and tessellation dependencies, and inherits the pre-existing
per-client puffs_per_cell divergence that CPU-resolved transport avoids. FAR FIELD (Tier B, one body per
macro cell): in-shader hash reconstruction — measured bit-exact there (nothing to omit) and zero vertex
data. The merge stages after the cosmetic wins either way.
Design C's offscreen density buffer is REJECTED for this pass: the opus review showed its cost premise
wrong (the carve, not the shade, owns ~all fetches) and its stage 1 unimplementable without
glBlendEquationi. Its CPU ground-truth math lives on inside the analytic merge.

**Channel transport fix (enables phase + merge + crowding).** Design B's raw texcoord payload is rejected
as written (fragment-side round() recovery destroys the interpolated quad coordinate — the puff vanishes;
opus Q2). The adopted fix: the VERTEX stage does the round()/decode while the attribute still carries
per-corner exact values, and vary_texcoord0 widens to vec4 (interpolators are vec4-granular; zero new
slots) carrying the clean quad coordinate plus decoded payload. The shaft branch's texcoord reads are
untouched by construction (decode only on the puff path).

**Lighting (scene-proven).** Powder-Beer on the existing buried depth: sun term *= exp(-d)(1-exp(-2d)),
d ~ 2.5 * buried — visibly deepens bellies at zero fetch cost (scene_r3 powder vs base A/B). Two-lobe
Henyey-Greenstein replaces the view-blind thin^3 fringe: phase = mix(HG(g=-0.2), HG(g=0.8), ~0.55) on
dot(view, sun), spent as sun * phase * thin^2 — the silver lining lands on the sun side only. Severe green
tint: a subtle high-buried ambient tint under storm gloom, cosmetic, judged in a build.

**Deck floor (G4), the A+B synthesis per the opus review.** Wall-cloud lowering: the CPU stretches the
card (safe — bakeGroundShadow reads cell occupancy, never puff geometry) AND the fragment's base cut lowers
capped to the geometry's true reach (A's 220 m fragment-only drop exceeded the ~164 m quad reach and would
have switched the cut off); the base veil fades under the lowering so it does not slice the wall. Scud:
CPU puffs hanging off already-gated parent puffs (no new gate, no shadow/precip tax), placed ABOVE the
lowered cut so they draw (B's originals sat below the cut, invisible) and excluded from the lightning
occlusion grid. Shelf: lineShelf-driven stacked striations, fragment-only. First: fix the PRE-EXISTING
bug the review found — `max(density, fill * step(v_h, 1.0))` re-fills below the base cut for any authored
profile (no lower bound on v_h).

**Funnels (G5).** The scene ablation says: the edge-softening mask (fresnel substitute in mask space) is
essential (its absence forks the silhouette into hard double lines); the noise trick group (per-layer
scroll differentials 1.0/0.6/0.5, 45-degree tilted pan, helical striation bands) earns its cost as a
group. All port to the production collar-quad renderer, whose shape functions the scene transliterated
(collarRadius / condensationShrink / radiusModulation, multi-vortex strictly as the radial alpha mask).
The landspout look (thin, laminar, low striation, dust collar) falls out of the same renderer at a
different parameter family — the dust-devil/gustnado scheduler already provides the non-mesocyclonic
spawns. Wedge bluntness (a no-taper collar that stays blunt under multi-vortex pinching) is an open tune.

## 2. Staged implementation plan (each stage independently landable, fallback flag, harness-first)

S0 Fix the thick-base fill bug (fragment one-liner + twin). Eyeball: authored-profile decks no longer
   glow under their own floor.
S1 Vertex-decode channel fix: vec4 vary_texcoord0, vertex-stage decode, payload zero — BIT-IDENTICAL
   output by construction (twin transliterates both stages). Zero visual change is the acceptance test.
S2 Per-puff phase on color.b low half + reach discipline. Eyeball: adjacent towers bloom out of step; no
   detail seams inside any puff face.
S3 Flow fields: ring billow, f1_ring (replaces radial flow_w), anvil outflow, meso swirl, vigor->detail
   contrast/erosion weight. All fragment-only, frame-contract-audited (shape_air), NaN-proofed by the
   no-normalize rule. Eyeball: towers roll out-and-over; anvil streams from the storm centre, fibrous
   downwind; striations curve around the meso.
S4 Lighting: powder-Beer + HG2 fringe (+ optional storm tint), behind one variant uniform for A/B in the
   build.
S5 Deck floor synthesis: fill-bug-safe wall lowering (CPU stretch + capped cut + veil fade), scud puffs,
   shelf bands.
S6 Carve swap: erosion-remap carve blended in by vigor/weather weight, calm-sky look preserved; then the
   analytic merge behind it once the transport prototype picks (a) or (b).
Funnel tricks land in the vortex renderer as their own slice (S3f) since they touch a different program.

Every stage: core formulas into ss*core.h headers where shells would otherwise compute (lesson 2), twins
for anything replicated, --changed tests only, opus review on any diff touching the replication set, and a
"what to look at in the build" list at the gate — the harness cannot see GLSL, so the user's build is the
only oracle for the on-screen result.

## 3. Cost envelope

Fragment: S2-S3 are ALU-only on the common path, zero new fetches (one conditional mammatus fetch inside
the existing cap-band branch). The ALU is NOT cheap and NOT gated off-cell: review_opus.md's hand count
against the shipped S3 blocks, all four flags on, comes to ~170 ALU worst case — and it is the COMMON case,
not a rare one, because there is no gate anywhere; a clear-sky puff (meso = anvil = owner = 0) still pays
the full ~170 to compute an identity (billow runs unconditionally, G5 normalizes a vector it added zero to,
G3 runs two ss_safe_dirs/a dot/two mixes/a second normalize to reproduce its input). The
`storm_sample.owner > 0.0` gate is the cheap win if this shows up in a frame-time pass (~90 ALU back on
every clear-sky fragment). S4 is ALU-only. S6 merge adds per-neighbour kernel
ALU (~12 ops x cap) and the transport cost chosen by the prototype. Vertex: decode + optional per-corner
outflow/swirl (moves trig off thousands of fragments; poor ROI on far tiny puffs — LOD-gate it). CPU
build: neighbour lists ~0.3-0.6 ms (cell-index walk), scud emission ~0.2 ms, both measured against
mLastBuildMS at the gate. No new textures, no new passes, no worker threads.

## 4. Alignment with the container/operator architecture (2026-09-07)

The main branch's phase-8 revision (doc/atmo_magic_phase8_show.md section 4·0, user 2026-09-06) settles
the model this work lives inside: the volumetric field is the MEDIUM, entities (storm anatomy archetypes,
kilometre-wide rain-cloud footprints) are OPERATORS on it — displacement / density / emission / shading,
closed-form, bounded, replicated, composed in a fixed producer order that observers invert in reverse.
Where this doc's work sits in that model, stage by stage:

**Boundary first: texture flow is NOT a displacement operator.** Everything S3 ships moves texture WITHIN
silhouettes the gate already owns — no puff moves, no observer read moves, so none of it enters the
operator composition chain or pays the inversion tax. That boundary is the cheapness the whole design
bought, and it must stay stated: the day a flow term displaces a READ of gate/presence/n_map, it has
become a displacement operator and owes the full producer/observer treatment.

**S2 phase + anatomy emission (8b):** the inheritance rule generalises — one billowing mass, one phase.
An anatomy PART (a turret, a mammatus lobe, the anvil sheet) hashes ONE phase from (entity id, part id)
and its emitted puffs inherit it, exactly as tessellation children inherit today. The updraft loop's
per-puff vertical-offset hash (8b section 4.4) is a SEPARATE salt — position loop and texture phase must
not share a hash or the loop's risers all crest their billow simultaneously.

**S3 vigor:** today's proxy (storm boost − anvil) upgrades to the entity's own age01 when anatomy lands —
young turret parts crisp, decaying parts fibrous — carried per part via the S1 payload channel. The
morphology (BRIEF: crisp-vs-fuzzy IS the vigor cue) finally gets its true driver.

**S3 anvil outflow:** the owner-cell centre stays the FABRIC fallback. When the anvil-sheet part (8b-2)
lands, its emitted puffs carry their own fan direction (tower top, downwind spread) on the payload
channel and the fragment prefers carried-over-derived. Same math, honest centre, no owner-cell snapping.

**S3 meso swirl vs the swirl displacement operator (8b): ONE FORMULA SITE.** The anatomy swirl rotates
puffs by θ(t)·w(|p−c|/r); our striation term rotates TEXTURE. If they disagree on rate or falloff the
texture slides against the geometry it decorates. When the swirl operator lands, SS_G5_SWIRL's tangential
field is re-derived FROM the operator's own θ/w (twin-shared), not kept as an independent 1/r field.

**S4 lighting:** applies to every fabric puff including anatomy emissions (they ride mForm/mBuried like
any puff). Rain-core darkness (the rain-cloud footprint's shading operator) multiplies the same gloom
chain meso gloom already does — composes, no conflict. The thickness split (depth vs DENSITY, section
4·0) will eventually feed the powder term's optical-depth proxy better than buried alone.

**S5 RESTAGED — superseded in part by 8b-3:** the wall cloud, tail cloud and shelf become anatomy parts,
and the tornado's inward/downward displacement operator makes the lowering the deck's own fabric drawn
into the funnel — strictly better than either the CPU card stretch or a capped fragment base-drop, so
NEITHER of those S5 items should land now. S5 shrinks to: (a) scud/pannus shreds — fabric cosmetics no
entity owns, still worth having; (b) shelf/whale's-mouth striation SHADING, reading whatever field term
the line archetype (8b-4) writes — the shading survives the operator's change of producer.

**S6 merge:** unchanged and strengthened. Anatomy concentrates hundreds of overlapping puffs on the hero
(tower rings, sheet fans) — exactly where kernel fusion pays. The CPU neighbour lists are built over
mPuffs regardless of what emitted each puff; the strength-ranked mutual cap needs no entity awareness.

**Rain-cloud entities:** footprint density operators raising coverage/drive/darkness over a moving
ellipse — the virga drive, precip and gloom reads this doc's work already touches all flow through those
same terms, so the flow work composes with rain-cloud footprints for free.

**Worktree sync note:** the main tree's dirty state has evolved past this worktree's fork copy (squall
cue authoring, keyframe span-removal, info-view look, the 4·0/8b doc revision). Flow changes here are
additive and marked (S0-S4, <SS:Nexii>, twin-pinned); expected conflict surface on re-sync is only
ssvolcloud.cpp/.h and the two ssVolCloud shaders. Re-sync by re-copying the main tree's files and
re-applying the flow diffs, or by folding flow into the main tree after its own gate — the user's call.

## 5. Open questions for the next rounds

Transport prototype (vertex channels vs in-shader hash reconstruction) for the merge; three-phase
crossfade if builds show the mid-cycle pulse; anvil "fish-scale" (per-puff outlines in the canopy — the
merge should eat this, verify in scene); wedge collar bluntness; static tower striation bands (the
mothership cue) as a carve term rather than motion-only; whale's-mouth underside (no published recipe
exists — original work, scene-first); Ghost-of-Tsushima-style cached paraboloid for the far field (Tier C
companion, out of scope tonight).
