# Atmo Magic: surface material design competition

Status: **record** (2026-09-07, on `feature-atmo-magic`). A structured design
competition for the next-generation surface weather material system (wetness spectrum,
ice, granular accumulation, slope/vertical behaviour, moving objects and avatars).
Seventeen agents ran: fourteen designers — each championing a distinct paradigm against
a shared brief — and three judges (performance/memory, material fidelity, engineering
fit). The outcome this record fed is `doc/atmo_magic_surface_v2_plan.md`; the full
proposals and verdicts are preserved below unchanged.

## 1. Method

1. Fact-check pass over the current tree (worldfield 0.25 m band store, air spans,
   drainage solve, field window mechanics, gbuffer layout, shader infrastructure).
2. Fourteen designer agents, identical brief (§2), identical output contract
   (eleven fixed sections, ≤1100 words, file:line citations, no file writes). Each
   read the same code/docs first; docs were flagged as possibly stale.
3. Both judges' digests and score sheets were produced from compressed but faithful
   digests of all fourteen designs; judges scored blind to each other.

## 2. The shared brief (abridged)

Deferred renderer, GL 4.6 target (compute, SSBOs, bindless, persistent maps, MDI,
sparse textures; no core RT), RTX-class GPU, 16 CPU cores, determinism mandatory for
sim state. Existing Atmo Magic: `SSWorldField` (0.25 m column tiles, vertical bands,
air spans OUTDOORS/SHELTERED/INTERIOR, drainage solve, dirty-rect re-peels),
`SSSurfaceField` (camera-centred RGBA32F window ledger, CPU-stepped), three screen-space
passes (`ssSurfaceWetF/NormalF/AlbedoF`), `SSAvatarWet` capsule hack (to eliminate).
Content classes: static ROC-style user builds, terrain, moving objects, avatars + rigged,
flexi prims, alpha foliage, impostors. Required spectrum: liquid drops → tension-break
streaks → rivulets → sheet flow, puddles + impact ripples, porosity, drying;
freeze/thaw with crack relief and frost; granular accumulate/dissipate, wind
entrainment, drifts, avalanching, melt→wet coupling; slopes drain/erode at thresholds;
verticals hold clinging droplets breaking into runs. Debug-gated features with pristine
fallback.

## 3. Scoreboard

| # | Paradigm | Perf | Fidelity | Eng. | Avg |
|---|---|---|---|---|---|
| D7 | Shallow-water on surfaces | 8 | 9.0 | 8 | **8.3** |
| D12 | CPU bake + stream | 8 | 5.5 | 7 | 6.8 |
| D8 | Hero droplets + statistical micro | 7 | 7.0 | 6 | 6.7 |
| D6 | GPU-driven ECS + MDI | 7 | 6.5 | 6 | 6.5 |
| D3 | Clipmap cascade | 8 | 8.0 | 3 | 6.3 |
| D11 | Amortized temporal resampling | 7 | 4.0 | 8 | 6.3 |
| D4 | Sparse 3D shell volume | 5 | 7.5 | 5 | 5.8 |
| D10 | Surface MPM | 4 | 8.5 | 5 | 5.8 |
| D1 | Screen-space + id/motion/history | 6 | 5.5 | 5 | 5.5 |
| D9 | PBF droplets on an SDF | 4 | 9.0 | 3 | 5.3 |
| D5 | Per-object weather atlases | 6 | 5.5 | 4 | 5.2 |
| D14 | Forward material layers + VTF | 6 | 4.5 | 5 | 5.2 |
| D2 | Visibility buffer | 7 | 4.5 | 3 | 4.8 |
| D13 | World-space virtual texture | 5 | 4.0 | 3 | 4.0 |

D7 is the only design in all three judges' top three. Judge tops: A — D12, D7, D3;
B — D7, D9, D10; C — D11, D7, D12.

## 4. Judge findings (condensed)

- **Judge A (performance):** the fleet "prices the sunny day" — real cost lives in the
  invalidation path (teleports, rain onset, avatar churn), and the dense-forest
  alpha-overdraw benchmark breaks every per-pixel design (D1/D2/D4/D14). Best scaling:
  D12 (cost off the frame; worst case a 2 s artifact, never dropped ms), D7 (constant
  envelope), D3 (world-anchored footprint). Distrusted numbers: D9's SDF amortization,
  D5's VRAM math, D2's overdraw denominator.
- **Judge B (fidelity):** emergence wins — D7 (spill-level puddles, ice dams rerouting
  flow, Mei-capacity erosion, wall runs carrying real mass), D9 (every threshold a force
  balance), D10 (cohesion `c(1−wet)` puts melt coupling inside the constitutive law).
  Most dishonest claim: D1's "surface tension in a screen-space buffer". Unsolved by
  anyone: persistent world-anchored centimetre-scale liquid detail; frost/ice-crack
  physics nearly absent.
- **Judge C (engineering):** the cheap five (D11/D12/D7/D8/D6-p1) all keep the CPU
  ledger as truth; the expensive five (D1/D2/D13/D14) thread new channels through the
  writer tree. Fallback lies: D2 (dual GL3.3 path), D3 (CPU mirror = second truth),
  D14 (deletes proven passes under its own gate), D9 (staleness in the contract).
  The regret decision: letting ground truth drift into GPU-resident state.

## 5. Synthesis

The judges' hybrid affinities converge: **D11 → D7 → D10-scoped → D6-p1/D1-PoC → D8,
with D12 conditional** — recorded as the full plan in
`doc/atmo_magic_surface_v2_plan.md`. Mutual exclusions: D2 vs D1 (id channel), D4 vs D9
(`SOLID_VOLUME_3D` semantics), D3 vs the window model, D13 vs D5, D14 vs everything
post-deferred, D6-p2 vs the CPU-truth spine.

---

# Appendix A — Designer proposals

## D1 — Incumbent-evolved: "Screen-space with eyes"

**1. Core idea.** Keep weather application in the three post-gbuffer passes
(pipeline.cpp:9988-9990) over the CPU's deterministic ledger, and fix screen-space
blindness with three modern additions: a per-pixel 32-bit class/id attachment, a
motion-vector attachment for temporal reprojection, and one screen-space flow-history
buffer so streak/rivulet detail survives camera motion. The avatar capsule hack dies
because the passes finally know *which* pixels are bodies — id present routes to
per-id state; id absent means the ground answers.

**2. State & data model.** `ssIdRect`, R32UI full-res (8.3 MB @1080p): bits 0-23
sparse object id (0 = none), bits 24-26 class (STATIC/MOVING/AVATAR/FLEXI/FOLIAGE/
IMPOSTOR), bits 27-31 porosity quintile. Written as a fourth `frag_data` output by
every deferred fragment shader behind a `HAS_SS_ID` define; feature off → attachment
unbound, legacy path pristine. The saturated flag byte is left alone
(llshadermgr.cpp:647-651, gbufferUtil.glsl:55) — new attachment, not a repack.
`ssMvecRect`, RG16F full-res (8.3 MB): per-pixel velocity from current-vs-previous
clip positions computed in the gbuffer vertex shaders; avatars use previous-frame pose
matrices. `ssFlowHistory`, RG16F ping-pong (16.6 MB): screen-space runnel accumulator
(trail length, film fraction), reprojected through ssMvecRect; disoccluded texels
reset. Per-id state SSBO: sparse open-addressed table, 4096 slots × 16 B (soak, ice,
deposit, cake) = 64 KB — only non-static classes. Ledger untouched: four 256² RGBA32F
windows (sssurfacefield.cpp:110, 1340-1370) = 4 MB. Total added VRAM ≈ 34 MB @1080p.

**3. Simulation.** The field stays the one ground truth (tick, `stepCell`, SSGranular).
Liquid: wet/puddle depths are field state; the drop→streak→rivulet→sheet *spectrum* is
drawn, except tension-break, which lives in ssFlowHistory: each reprojected texel
accumulates trail length; when a drip cell's column exceeds capacity ∝ (1−slope)·wet,
the cap detaches (phase jump, Heartfelt stutter retained) and the trail merges
down-slope with its neighbour — a capacity/oversaturation rule; merged trails render
as rivulets; past the sheet threshold the lattice yields to the existing wave-map sheet
term. Granular: field lift/creep/lee carry unchanged; screen space adds saltation
shimmer and a repose gate with hysteresis; avalanching is `shedEdges` spill re-drawn as
rolling-grain scour normal. Freeze: field mIce/mFrost drive roughness targets and crack
relief (0.35 m lattice upgraded to Voronoi-edge hash); thaw credits mWet.

**4. Render path.** (1) gbuffer draws, now also emitting id + velocity (one extra
MRT). (2) Compute: id→SSBO gather; porosity quintile resolves. (3) The three passes in
existing order: each fetches id/class — STATIC/outdoor takes the field answer with its
exposure march unchanged; AVATAR id takes SSBO soak through the proven height-band
curves minus capsule geometry; MOVING blends field vs history; FOLIAGE/IMPOSTOR
excluded. Roughness uses the id porosity quintile instead of the 4-tap albedo-variance
guess. Normal pass keeps flatten, drops (9-cell walk), then draws tension runs from
ssFlowHistory as footprint-filtered ridges, rivulets as domain-warped trails, sheet
flow as today; rings unchanged. (4) Lighting untouched. (5) After `combineGlow`, the
history resolve pass reprojects and ages ssFlowHistory.

**5. Mobility matrix.** Static world/terrain: full fidelity. Moving objects: motion
vectors keep streak history glued; per-id SSBO wetness — field answers only via the
column test when parked. Avatars + rigged: capsules deleted; per-id soak drives the
existing curves; prev-pose velocity costs one extra matrix set — rigged attachment
drift breaks reprojection at fast animations. Flexi: class-flagged, per-prim velocity
only. Impostors: excluded outright.

**6. Slope & verticals.** Level: puddles from drainage pool cells + shore carve.
Slopes: D8 directions steer rivulets and meltwater; repose gate with hysteresis.
Verticals: existing drips gain clinging droplets that hold until capacity breaks into
runs traced down the history buffer; granular never clears the vertical gate; frost
rides the exposure term only.

**7. LOD & performance.** @1080p: id+velocity writes +0.4 ms; three passes ≈ 1.4 ms
(porosity gets cheaper); history ping-pong 0.35 ms. Added ≈ 2.2 ms GPU, behind
`SSAtmoSurfaceId`/`SSAtmoSurfaceHistory`. Past ~40 m the id field degrades to
class-only. Memory: 34 MB @1080p.

**8. GL 4.6.** SSBO id table; compute history reprojection and id downsample;
persistent-mapped readback via the existing SSGLReadback worker; bindless for the four
field windows.

**9. Integration.** Kept: ledger, tick, windows, passes, drop core, rings, pass order.
Deleted: the capsule path (ssSurfaceWetF.glsl:138-226, ssSurfaceNormalF.glsl:131-155
and 250-255, sssurfacefield.cpp:1695, ssavatarwet.* — soak migrates into the SSBO
updater). Added: ssidbuffer, ssflowhistory, `HAS_SS_ID` injection, one output line per
deferred shader, `gSSSurfaceHistoryProgram`, two settings.

**10. Honest weaknesses.** (1) The id channel touches ~20 deferred shaders; missed
writers read stale texels. (2) Still screen-space: disocclusions reset streak state;
off-screen wetness does not exist. (3) Per-avatar prev-pose velocity needs skeleton
plumbing; recycled ids bleed stale soak unless refcounted. (4) Porosity quintile
coarse on PBR blends. (5) One liquid/one deposit look at a time.

**11. One-day PoC.** Allocate ssIdRect; only `avatarF.glsl` writes it; replace capsule
loops in both passes with an id fetch; feed SSAvatarWet's soak into the SSBO keyed by
id. Result: hack dead, attachment plumbing proven end-to-end.

## D2 — Visibility buffer resolve

**1. Core idea.** Add a per-pixel identity side channel — draw-id + triangle-id +
barycentrics — as two extra MRT attachments written by the existing deferred programs,
consumed *only* by the Atmo Magic surface passes. The weather resolve answers "which
exact object/triangle is this pixel" instead of guessing material class from albedo
variance, giving exact porosity, per-object exposure and per-object accumulation state
with pristine fallback behind a debug gate.

**2. State & data model.** Vis A `GL_RG32UI`: draw-id (16b, hi bit valid) | tri-id
(16b per-draw); `.y` = packed barycentrics (2×12b + 8b spare). Vis B `GL_R8` class
byte (porosity/sealed/exposure resolved at draw time). VRAM @1080p: 16.6 + 2 MB.
Blending forced off in the pools that write it. Fallback without 32-bit targets: pack
into `GL_RGBA8` (object-only identity). Barycentrics only disambiguate lattice
anchoring inside one triangle; the world-space anchoring error budget
(sssurfacedropcore.h:43-47) already tolerates it. Object table: SSBO 64k × 32 B
(2 MB): agent-space position, air label/enclosure, material class, per-object wet/
deposit scalars, atlas slot for statics.

**3. Simulation.** Ground truth unchanged (field ledger, fixed quanta). The vis layer
adds per-object *overprints*: statics get a 256² R16F atlas tile of high-frequency
residue (droplet clinging, rivulet seed heads, granular pile tops) relaxed by compute
with repose thresholds from the existing transport constants; Barnes/D8 drainage seeds
rivulet directions; cling-break runs reuse the Heartfelt stutter already transliterated
in sssurfacedropcore.h. Movers/avatars: one scalar row (wet, deposit, ice) driven by
exposure + a capsule solve, replacing SSAvatarWet's screen-space capsules. Porosity:
exact per draw from the material itself, replacing the 4-tap albedo-variance estimate.

**4. Render path.** Gbuffer programs gain two flat outputs + a flat varying draw-id
(`gl_PrimitiveID` in FS under GLSL 4.6, GS passthrough fallback; `gl_DrawID` free under
GL 4.6 MDI). CPU fills the object table per frame. A resolve pass (compute or
fullscreen) replaces `renderAlbedoPass` + `renderWetPass` internals: fetch identity →
SSBO row → sample field windows → evaluate the whole material law once → write
albedo/spec/normal deltas. The three passes' ordering constraint dissolves — one pass,
pristine inputs, exact porosity.

**5. Mobility matrix.** Static: atlas overprint, exact exposure, drainage-seeded runs.
Moving: scalar row + field-window sampling at current position; atlas state discarded
on rest. Avatars/rigged: draw-id row with wet/ice scalars; re-running skinning to fetch
attributes judged not worth it; capsules retire. Flexi: CPU-rebuilt dynamic VBs;
same-frame tri-ids resolve; moving-class state. Impostors: one quad = degenerate
identity; their own target, scalar path or nothing.

**6. Slope & verticals.** Slope logic keeps the gbuffer normal — now bit-stable per
*object*, so the drip-axis quantisation could drop from 16 azimuths. Verticals hold
clinging droplets in the static atlas; granular below repose; drain/erode via D8 +
creep.

**7. LOD & performance.** Vis write +12 B/px ≈ 3-5% frame on RTX at 4K. One resolve vs
three passes. Distance → class-byte-only rows; barycentrics noisy on small tris —
irrelevant, lattices are world-anchored.

**8. GL 4.6.** MDI + `gl_DrawID`, SSBO table, compute resolve, persistent-mapped
upload, bindless per-object atlas slots.

**9. Integration.** New ssvisbuffer + resolve program + `SSAtmoVisBuffer`. Touched:
addDeferredAttachments, resolve call site, ~15-20 FS+VS in llviewershadermgr.
sssurfacefield/ssworldfield stay read-only consumers.

**10. Honest weaknesses.** (1) Every gbuffer write path touched — the legacy+PBR
split's full surface area, for a benefit that mostly serves three weather passes.
(2) Alpha-blended surfaces poison the vis channel. (3) Barycentric value marginal —
lattices are world-anchored and already hardened. (4) GL 3.3 fallback is a second path
to maintain.

**11. One-day PoC.** `SSAtmoVisDebug`: one R16UI draw-id attachment; write it in three
shaders only (terrain, PBROpaque, diffuseIndexed); debug resolve tints pixels by class
and re-keys the drop lattice hash by object-id (drops stop swimming on moving carts).

## D3 — Clipmap cascade

**1. Core idea.** Replace the single camera window with a camera-anchored clipmap
pyramid: levels L0..L6, each a finite 256² (L0: 512²) texture whose origin snaps to its
own cell size, cell sizes 0.25, 0.5, 1, 2, 4, 8, 16 m — spans 128 m to 4 km. State is
painted and stepped by compute shaders on the GPU at shared-time quanta; any shader
stage samples by world position, choosing a level by pixel footprint. The single window
becomes L1's special case.

**2. State & data model.** Per level, a `GL_TEXTURE_2D_ARRAY` of pages: material RGBA32F
(Z, wet, snow, puddle), state RGBA8 (ice/frost/stain/age), flow RG16F+R8, cover R8. L0
adds a wall-shell array. VRAM ≈ 5 MB total. Level origins/cell sizes ride a small UBO.

**3. Simulation.** GPU compute, one dispatch chain per quantum, level-banded (Li steps
every 2^i quanta). Film flow: shallow-water film — height + per-edge flux, 2 Jacobi
sweeps, driven by painted Z and the world field's D8/pool view; depth thresholds give
drops → streaks → rivulets → sheet; the existing per-fragment lattices remain the
sub-texel look gated by film depth. Porosity baked per texel per edit region;
infiltration subtracts from film. Freeze/thaw: `stepCell` laws as compute,
deterministic integer hashes. Granular: angle-of-repose CA, per-texel flux to
4-neighbours, wind entrainment from the solved flow grid. Rain settle/melt/wash inputs
assembled CPU-side into a persistent-mapped UBO. The CPU keeps one duty: feeding paint
from SSWorldField (dirty-rect scissor).

**4. Render path.** The three passes get `ssClipFetch(worldXY, footprint)`: level by
`log2(fwidth)`, bilinear in-level, blend straddling levels. CPU consumers read a
persistent-mapped mirror. Ring buffer unchanged.

**5. Mobility matrix.** Static/terrain: painted per dirty rect. Moving: sampled for
received weather; their occlusion as capsule/box SSBOs subtracted in the exposure
compute; transient wet prints as decaying splats. Avatars: same; retires the capsule
bind hack. Flexi: sample-only. Foliage/impostors: sample-only; impostors read coarse
level in VS.

**6. Slope & verticals.** Horizontal levels carry painted Z + slope; drainage rides
spill/D8. Verticals: L0/L1 wall-shell array reusing SSWorldField's band peel — 8 layers
× 4 m bands, per-wall state in the drip frame, band resolved by Z with a ±band/2
validation. Walls hold drops that break into runs; granular above repose slides to the
base level below. Bands make multi-storey resolvable.

**7. LOD & performance.** Cost scales with camera-near area: L0 every quantum, L2 every
4, etc. Camera movement triggers ring updates — only the newly exposed L-shaped border
computes; state diffuses in from the next-coarser level. < 0.4 ms/frame compute; CPU
tick cost collapses toward zero.

**8. GL 4.6.** Compute + image load/store on arrays; SSBOs; persistent-mapped ring;
sparse 2D-array tiles for far levels; bindless for material sampling;
`glTextureBarrier` between paint and passes.

**9. Integration.** `SSAtmoClipmapCascade`; new ssclipfield + compute step; mirrors
`bindForShader`'s contract; LOCKSTEP consts in ssclipfieldcore.h.

**10. Honest weaknesses.** (1) Still 2.5D — 8 wall bands approximate true 3D.
(2) Camera-anchored non-determinism breaks texel-exact golden tests. (3) GPU-resident
state is a new failure class; needs a CPU coarse mirror. (4) Level-boundary seams pop
drying puddle edges. (5) Three new systems vs one stitch function.

**11. One-day PoC.** L0 only: compute-blit the existing CPU field arrays into a 512²
RGBA16F at 0.5 m, camera-snapped; `ssClipFetch` gated behind the setting; A/B against
the 2 m window on puddle shorelines and rings.

## D4 — Sparse 3D shell volume

**1. Core idea.** Weather state is stored on a sparse shell of voxels: only voxels
touching solid geometry exist. Structure comes from SSWorldField's span store (the
`SOLID_VOLUME_3D` channel, up to 6 solid spans per 0.25 m column), not a new capture.
The weather sim is a face-to-face cellular automaton across it. Overhangs, walls,
interiors and multi-storey structure fall out of 3D for free.

**2. State & data model.** 0.25 m voxels; brick = 8³; a brick exists iff it contains a
shell voxel. Per shell voxel, 12 B: wet film F16, pool/granular depth F16, ice/frost/
stain/age u8, face octant + quantised normal, flags (air label snapshot, water, pool,
mover-shadow). Thin bodies store two faces. Storage: hash-of-bricks in SSBOs (open
addressed, 64-bit world brick keys); `ARB_sparse_texture` explicitly rejected. Memory:
shell occupancy 3-8 voxels/column → 25-50 MB ≈ 15-35 k resident bricks near camera.

**3. Simulation.** Fixed-step compute, one brick per workgroup: gravity-projected flux
across face-neighbour slots; vertical faces transfer downward only, so a wall film
breaks into runs when thickness exceeds a hashed nucleation threshold — rivulets
emerge. Pools accumulate where down-face is solid and lateral flux ≈ 0 (Barnes pool/
spill seeds standing level). Granular: second flux channel with repose thresholds.
Ice/frost/stain/age step per voxel as `stepCell` does. Outflows accumulated into a
per-voxel inflow slot, applied after — order-independent, deterministic. Air labels
gate deposition. Rain input on OUTDOORS/SHELTERED up-faces.

**4. Render path.** Near window: gather bricks into a dense RGBA32F slice in the
existing lattice; the three passes change only their fetch function. Elsewhere: VS and
post shaders read the SSBO hash directly (`worldPosToVoxel`); a miss means no weather.
CPU `sample()` mirrors the field's Sample by hashing the same table.

**5. Mobility matrix.** Statics/terrain: captured into the span store; own the state.
Moving: excluded from the capture; runtime shader term from air label + exposure.
Avatars: never in the volume; foot `sample()` drives per-avatar wetness ramps —
retires SSAvatarWet with a stronger signal (3D enclosure). Flexi: sample-only, snow-
load displacement read-only. Foliage: canopy-drip events dropped onto the shell
beneath. Impostors: read the hash in FS — correct wet look with zero state.

**6. Slope & verticals.** Face-relative flux: level faces pool; slopes drain and erode;
verticals hold film until nucleation breaks it into runs; an eave voxel whose down-flux
terminates emits a falling-drip event that lands on the first shell voxel below — the
shed cascade becomes simulated.

**7. LOD & performance.** Near ring (< 32 m) full 0.25 m sim; mid ring 1 m bricks at
¼ rate with analytic drying; far: existing 2D windows. LRU eviction weighted by state
mass; 40 k brick budget. Sim is one dispatch over active bricks, well under 1 ms.

**8. GL 4.6.** SSBO hash + brick pool, compute sim/gather, persistent-mapped staging,
plain bindings (bindless-free).

**9. Integration.** `SSAtmoSurfaceVolume` gate; ssshellvolume + ssshellcore; claims the
worldfield Interest; edits re-derive shell bricks from the dirty rect, preserving state
in surviving voxels.

**10. Honest weaknesses.** (1) Random-access FS cost: hash probe (2-4 dependent loads)
per fragment; near-window gather can lag a fast camera. (2) Occupancy spikes (dense
foliage) thrash eviction. (3) Face ambiguity: voxel-scale faces quantise corners; the
old drop/drip lattices survive as detail layers. (4) 0.25 m Z refinement multiplies
capture readbacks. (5) Evicted mid-solve state needs mass-preserving serialisation.

**11. One-day PoC.** CPU-only: derive shell voxels for one tile from the span store,
run the flux CA on 16 threads for the liquid channel, upload a dense 40 m window, hack
one fetch in ssSurfaceWetF. Measure build time, resident-voxel counts, wet
walls/undersides under a balcony.

## D5 — Per-object weather atlases

**1. Core idea.** Every *drawcall* that deserves weather owns a slot in one shared 2D
texture-array atlas: a baked geometry page (object-local position/normal/exposure) plus
simulated state pages (accumulation, wet film, ice, puddle). Rain is volumetric —
accumulation is `intensity·dt·exposure` against the baked exposure mask, not a
world-space brush. Pages bind to the object forever; moving objects carry their weather
for free.

**2. State & data model.** GPage RGBA16F 256²: local-space position + exposure·slope in
A — stored local so world pos = model matrix · local at use time; that is why moving
never invalidates the page. SPage RGBA16F: accumulation, wet film, ice, puddle. QPage
RGBA8: frost, stain/tint-id, age, drop-seed phase. Page table: persistent-mapped SSBO.
Allocation unit is the LLDrawInfo, keyed stable across frames; an `mWeatherPage` U16
joins the face/drawinfo. Budget: 96 layers × 3 arrays ≈ 50 MB.

**3. Simulation.** Compute over active pages in one dispatch; per texel per shared-time
quantum: settle, melt/freeze from the same temperature inputs the field tick uses,
drying by exposure and age, avalanche by height gradient over baked slope at repose,
wind drift projecting the region flow vector into the baked tangent. World coupling
stays in the ledger: SSWorldField supplies exposure baked into GPage (re-baked lazily);
`depositAt`/`vaporise` stay the ground's write paths.

**4. Render path.** One gap: the gbuffer has no object id — an 8-bit page-id attachment
(R8) written during the deferred geometry pass, exactly the per-draw state hook
`uploadMatrixPalette` establishes. The three passes branch: fetch page id → table →
sample SPage/QPage in UV from `vary_texcoord0` → apply today's laws. The baked normal
tilts drop caps; drop animation stays procedural and world-anchored — state in UV,
motion in world.

**5. Mobility matrix.** Static: page at allocation; lazy exposure re-bake. Terrain:
stays on the field (world-space already). Moving: local-space bake ⇒ state rides;
exposure re-bake throttled to 2 Hz. Avatars: per-attached-mesh pages baked in skin
space via the matrix-palette path; wet film + drips replace the capsules. Flexi /
alpha foliage / impostors: no page; existing screen-space fallback.

**6. Slope & verticals.** GPage carries the local normal: slopes shed by height-gradient
transport in-UV; verticals accumulate nothing but wet film, whose excess becomes drip
*initiation* in QPage's drop-seed channel — the sliding droplet renders world-anchored
off world-Z, so the UV-seam problem is dodged honestly. Optional v2: an allocation-time
seam-warp texture letting slow wet trails bleed island-to-island.

**7. LOD & performance.** Eligibility: < 64 m, screen size > ~5%, opaque, usable UVs
(scored at bake: texel world-size, overlap variance, repeat-tile count). Resolution
ladder 64/128/256; LRU; 96-layer cap. Sim cost scales with active texels — a squall
repaints ~50 active pages ≈ 3 M texels/step, trivial; idle pages cost zero.

**8. GL 4.6.** Compute + SSBO page table; `glTexStorage3D` arrays; persistent-mapped
table; `glFramebufferTextureLayer` bake. Bindless deliberately unused.

**9. Integration.** `SSWeatherAtlas*` settings; off ⇒ byte-identical. New
ssweatheratlas + bake/paint shaders; three touch points tagged.

**10. Honest weaknesses.** User UVs are hostile — atlas-eligible content may be a
minority; the R8 id attachment touches pipeline assumptions; pooled water crossing a
physical seam still piles at island borders (warp fix is R&D); page allocation is
client-local (seeds must come from object UUID); exposure staleness on movers.

**11. One-day PoC.** One 128² page, one cube prim: bake GPage+SPage, compute
accumulate/avalanche step, branch ssSurfaceAlbedoF on a hardcoded page id. Deliverable:
snow piling on a tilted box, draining to edges, melting — then the box moved sideways
proving the state rides.

## D6 — GPU-driven ECS + MDI

**1. Core idea.** Weather state becomes an ECS: one SSBO of per-object records + one
sparse texture-array atlas layer per object, written by compute, read by draws via
`gl_DrawID` indirection. Terrain stays with the existing screen-space field; the ECS
owns *objects*, the domain the window ledger structurally cannot address per-face.
Everything else survives unchanged and gains an id channel.

**2. State & data model.** `SSWeatherRecord` 64 B, SoA across three SSBOs: accum
(wet, puddleVol, depositDepth, age, stain), wet/ice/thermal (ice fraction, frost,
surface temp + thermal mass, dryness integral), identity (bindless albedo handle for
true-texture porosity, atlas handle, world AABB, class, geometry serial). Atlas layer
RGBA16F: wet, deposit, ice, flow-flux, from one sparse array. A one-off bake rasterises
world normal, height-above-base, sky exposure and porosity on creation/edits.

**3. Simulation.** Compute, one fixed step on the field's shared-time quanta. Passes:
(1) record step — thread per record, single-writer: thermal conduction, passive wet
in/out, freeze/thaw, age, stain; no atomics anywhere ⇒ bit-identical across clients.
(2) atlas step — film diffusion, downhill flux along the baked normal, puddle pooling
at curvature minima, D8-style flow accumulation. (3) compaction — fixed-order scan
builds the active-index list. Honest concession: cross-object water transfer is
approximated via the deterministic ring buffer, not atomics.

**4. Render path.** Phase 1 (no MDI): each LLDrawInfo gets a U32 mWeatherId bound as a
vertex attrib or 4-byte uniform — the cost class of `uploadMatrixPalette`/`gWindDir`.
Phase 2 (MDI, POOL_SIMPLE first): commands prebuilt per pool; `gl_BaseInstanceARB`
resolves the record. Identity to shading: weather-active pools write an R16
`ssWeatherId` gbuffer attachment from `gl_DrawID`; the three passes fetch record +
atlas by that id; terrain pixels read id 0 = existing ssFieldMap.

**5. Mobility matrix.** Statics/terrain: full record+atlas. Moving: scalars only;
atlas re-baked on pose delta past a threshold; teleports reset via serial bump.
Avatars: one scalar record each — reads `mAvatar` off LLDrawInfo. Flexi: scalar record;
vertex-wave authored drips. Foliage/impostors: porosity/wet scalar; impostors sample
their avatar's record.

**6. Slope & verticals.** The baked world-normal drives everything: |n·up| < 0.3
switches to the drip law; slopes shed via flux accumulation; eave lines emerge from
curvature + edge detection in the bake.

**7. LOD & performance.** Atlas res by distance/screen coverage; sparse pages freed
when idle-dry. Sim on the compacted active list only — 10 k objects with weather on
400 costs 400 threads. < 1 ms compute at 8 k active texels; zero CPU beyond the step
trigger.

**8. GL 4.6.** SSBOs, compute, `ARB_bindless_texture`, MDI + `gl_DrawID`,
`ARB_sparse_texture2`, image barriers per ssgpucull precedent.

**9. Integration.** `SSWeatherEcs` gate; phase 1 lands without touching draw
submission; phase 3 is the only MDI surgery. SSAvatarWet capsules retire in phase 1:
the avatar record *is* the capsule, better.

**10. Honest weaknesses.** (1) Arbitrary user UVs — movers degrade to scalars.
(2) CPU submission is the real ceiling; pools split by texture, so batching gains are
partial until bindless retires the sort key. (3) Determinism constrains the sim — no
cross-object transfer. (4) The id attachment is bandwidth the screen-space design
didn't pay. (5) Avatar rigged-surface weather is scalar-only.

**11. One-day PoC.** Reuse SSGPUCull's skeleton: 256-record SSBO, assignment from
POOL_SIMPLE's draws, one compute kernel stepping wet/ice scalars, R16 id written by a
patched deferred_simpleF, the albedo pass applying `ssWetAlbedoExponent` per-pixel.

## D7 — Shallow-water on surfaces

**1. Core idea.** Liquid weather is real hydraulics in two tiers. Static tier:
SSWorldField's drainage solve *is* the steady state of shallow water — Barnes priority
flood spills, pool membership, D8 routing give every puddle its equilibrium level for
free. Dynamic tier: a virtual-pipes shallow-water solver (Mei et al. 2007; Jákó-Tóth
sediment tilt) runs CPU-side over a camera-centred 0.25 m window, seeded by the static
topology and *parked* wherever its state matches the flood's equilibrium. The sim only
handles transients. On verticals the heightfield honestly dies, and a separate 1D
wall-film model takes over, feeding the existing drip lattice as its renderer.

**2. State & data model.** New `ssflowfield.h/.cpp` adds to the field a parallel
0.25 m active window (128² cells = 32 m): `h` film depth (`mPuddle` becomes its render
view), `fL/fR/fB/fT` pipe fluxes, velocity `u`, `ice` frozen fraction, `C` suspended
sediment, `bed` erosion offset. Wall ledger: sparse per-region map keyed
(azimuth 0..15, v-band 0.25 m, along 0.25 m) — the 16-azimuth quantisation *is*
`SSSurfaceDrop::DRIP_AZIMUTHS`, so sim and shader share one bit-stable frame; per entry
`h_wall`, downslope flux, run seeds. Uploads ride the existing window machinery.

**3. Simulation.** Fixed to the deterministic clock (0.25 s tick), three substeps of
1/12 s. Stability: wave speed c = √(g·h) gives 0.7 m/s at a 5 cm puddle; the pipe
damping factor `1/(1+dt·K·g/(l·w))` removes the stiffness term, so 0.083 s substeps
hold with 2× margin; splashes never enter the heightfield (they are ring events).
Depth update via an order-independent inflow accumulator — the exact discipline
`mInflow` already uses — row-parallel over 16 cores, gather-only reads,
bit-deterministic. Sleep/wake: cells with flux < ε park at the flood's `spill − z`
answer; wake on rain, ice transition, splash, edit. Couplings: ice scales pipe area by
(1−ice); freeze/thaw events re-run the Barnes flood (event-driven) so a frozen pool
dams and overflows at a new spill; melt credits `h` and `C`; Mei capacity
`Cmax = Kc·|u|·sinα` drives erosion/deposition; Jákó-Tóth sediment tilt perturbs D8.
Repose reads the granular room with wet-modulated friction angle: cohesion raises it
damp, buoyancy lowers it saturated — drying then avalanches. Wind entrainment stays in
SSGranular lift thresholds, never hydraulic.

**4. Render path.** The three passes keep their shape. Film depth feeds wet/puddle;
`u` replaces the scroll direction; `ssWetFlowMinWet` becomes the physical film
threshold. Rivulets are `|u|`-high/`h`-low cells modulating the sheet wave texture into
narrow bright runs; suspended `C` tints puddle floors and stain. Wall runs reuse the
cap/trail code but the drip's phase comes from the sim's accumulated run distance
instead of `ssTime` — state-driven drips at last.

**5. Mobility matrix.** Statics/terrain: full macro sim. Moving: impacts and splash
rings; a drying trail via mWet. Avatars: capsules retired; feet sample the flow field;
wading displaces film as a one-tick flux impulse + splash spawn. Flexi: film rendered
from the frozen capture, flagged no-sim until recapture. Foliage: cover only.
Impostors: no film; underlying field still wets.

**6. Slope & verticals.** Pipes are honest to ~70° (head differences in XY stay
meaningful while z-drop dominates). Past that, the wall ledger takes over: per ring
cell, Nusselt laminar film `q = ρg·h³/3μ` drains down-v, wind diffuses along-u,
porosity absorbs. Film-thinning: `h_wall` decays by drainage; below breakup thickness
`h_crit ≈ 0.15 mm·(1−roughness)` the contact line fails — the entry converts its depth
into hashed run seeds (the 0.06 × 0.30 m drip cells), each carrying mass down at
accelerating speed and leaving the trail; runs landing at the wall's base credit the
macro field's `h`. Clinging films therefore thin, snap into runs, and re-wet
continuously.

**7. LOD & performance.** Tier 0 (> 64 m): static flood answer only. Tier 1
(16-64 m): parked equilibrium. Tier 2 (< 16 m): full solve; 16 k cells × 3 substeps ×
~40 flops ≈ 2 MFLOP/tick — sub-millisecond, single core; 256² escalation goes
row-parallel across 16 cores. Wall sim < 24 m. Active-set compaction deterministic.

**8. GL 4.6.** Persistent-mapped PBOs replace the four glTexSubImage2D calls per window
refresh; SSBO + compute escalation of the identical solver (gather-only, fixed ordering
= deterministic) if the window grows region-wide; sparse pages for the wall ledger if
promoted.

**9. Integration.** Claim `EChannel::DRAINAGE_NETWORK`; `buildDrainage` becomes the
static tier. New ssflowfield.*, core laws in ssflowcore.h twinned to GLSL. Debug
setting `SSAtmoFlowSim` (0 off / 1 static / 2 full) keeps the existing look as
fallback. `depositAt` becomes flux-borne.

**10. Honest weaknesses.** (1) Sub-cell physics is still art: 0.25 m cells cannot
resolve centimetre rivulet fingers. (2) Single landing surface: a balcony under a roof
shares the ground's answer until multi-peel per-span levels land. (3) Ice damming is
event-driven — between re-solves a growing dam holds wrong levels. (4) 0.25 s ticks
quantise fast transients. (5) 16-azimuth wall bins: runs visibly jump azimuth at
corners.

**11. One-day PoC.** (1) Feed the already-computed drainage into render: `h =
max(0, spill−z)` on pool cells, D8 slopes as flow dirs through the existing sheet path
— region-wide puddles at true spill level, visible drainage networks. (2) Add rain
input + pipe flux on a 64 m window, 2 substeps, single-thread, flux magnitude into the
flow window's spare channel. (3) Twin the pipe update in a unit; eyeball roof rain
reaching the eave, the yard pooling along its spill line, dry-down following channels.

## D8 — Hero droplets + statistical micro

**1. Core idea.** The macro layer already exists and stays: the per-region ledger plus
the two world-locked lattices (`sampleAt`, `dripAxis`) become the *statistical truth
everywhere*. On top, a sparse pool of N ≈ 512 compute-simulated "hero" droplets lives
within ~16 m of the camera, individually integrated, surface-constrained via
depth-buffer adhesion. Heroes render screen-space-fluid style (van der Laan et al.
2009: depth-layered smoothing, normals from smoothed depth gradient); the lattices keep
rendering the statistical micro at range. The tension-break that launches a run is a
per-cell probability in the macro; the hero pool is where those probabilities cash out
as visible bodies. Everything debug-gated with the existing item-1/item-2 path as
fallback.

**2. State & data model.** Macro additions to Field: per-cell `mFilm` (metres of liquid
on sloped/vertical cells), `mCling` (clinging droplet mass on verticals), `mHero` (mass
checked out to heroes, suppressing lattice occupancy beneath). Windows upload them in
the flow window's spare channels. Hero state: one SSBO `{pos, vel, mass, radius, seed,
state}` — state ∈ CLING, RUN, FREE. Seeds use the integer-hash idiom so spawn is
deterministic against the fixed-step shared clock.

**3. Simulation.** Macro: per tick, inflow from wet deposition; if `mFilm > h_c(up_align)`
a hash-seeded roll launches a run with probability `p = k·dt·(mFilm − h_c)/h_c`, moving
mass down-slope into the inflow accumulator. Verticals: `mCling` grows with deposition;
hold capacity per drip cell is hash-jittered, so break-off fires stochastically per
cell — the statistical twin of the Heartfelt stutter. Heroes: compute pass at 120 Hz
shared-clock quanta; RUN heroes advect along the flow window with a per-hero hash
deviation; merge when within radius (mass-weighted); breakoff when mass exceeds the
cell's capacity → FREE → becomes a landing ripple-quad event via the existing impact
path. Spawn: the spawn walk reads ledger cells above the tension threshold — the
`forEachLiftCell` pattern — and checks out mass into `mHero`.

**4. Render path.** Heroes splat as point sprites into a half-res R16F hero-depth + R8
thickness target, depth-offset by cap height. Two bilateral smoothing passes
(depth-layered), then one pass computes view-space normals from the smoothed gradient.
Composite: wherever hero depth ≈ scene depth (ε-band), the normal pass blends the hero
normal over `flat_world` and the wet pass tightens specular by thickness — reusing the
cap-tilt plumbing of item 1. Screen-space fluid normals are safe here: the gradient is
of the hero layer's own smoothed depth, never of scene depth scaled by |p|.

**5. Mobility matrix.** Statics/terrain: full macro + lattices; heroes inside 16 m.
Moving: gbuffer adhesion only; ledger rebuild flags retire heroes whose depth anchor
moves — they hand mass back and vanish statistically. Avatars: none; capsule containment
early-out. Flexi: heroes only while screen-visible, else dry. Foliage: flag-gated out;
wet pass only. Impostors: albedo tint only.

**6. Slope & verticals.** `h_c = γ₀ + γ₁(1−up_align)` makes tension-break continuous
across the slope spectrum: level cells pool, 45° cells run as sheets, walls accumulate
`mCling` until the per-cell hash roll fires a RUN hero on the quantised-azimuth frame —
macro and hero share the frame. Wind lean enters via the same shear term as the existing
lattice.

**7. LOD & performance.** Heroes ≤ 16 m, 512 × 120 Hz ≈ 0.4 ms compute; lattices
per-cell CPU tick (~0.5 ms); splat+smooth+composite ≈ 0.6 ms half-res. Hero count
scales with a particle-budget share. Fallback restores items 1/2 verbatim.

**8. GL 4.6.** Compute sim with atomic free-list SSBO; persistent-mapped buffer for
debug readback only; `gl_DrawArraysIndirect` to splat only live heroes; image
load/store for `mHero` ledger writes.

**9. Integration.** New ssherodrops.{h,cpp} + stages; composite hooks into the normal
pass's post-ring section. Handoff: a RUN hero dying writes mass to `mWet`/`mFilm`, sets
a 2-cell fade where lattice occupancy eases in under a per-cell `mHero` shadow. No
`LLPerlinNoise` anywhere; all hashes integer-mix.

**10. Honest weaknesses.** (1) Silhouette/disocclusion: the ε-depth match fails at rim
edges; a tracked hero pops out with only the mass write-back surviving. (2) Adhesion
error at range; shrinking heroes to ≤ 16 m hides it but draws a content cliff.
(3) Topology-blind merging: depth-layered smoothing merges droplets on opposite faces
of a thin wall. (4) Determinism erosion: GPU merge order nondeterministic — acceptable
only because heroes are sub-second lives. (5) Double-representation mismatch: occluded
hero + visible lattice cell briefly reads a hole in the film.

**11. One-day PoC.** (a) SSBO pool of 256, compute spawn from cells above threshold,
gbuffer-snap adhesion, down-slope advection — no merging. (b) Half-res depth splat +
one smoothing pass + normal-from-gradient composite debug-gated into ssSurfaceNormalF
after item 1. (c) Handoff = fade + mWet write. Benchmark against the item-1 baseline.

## D9 — Position-based fluids on surfaces

**1. Core idea.** Droplets are XPBD particles living on a **signed distance field of
the world**, not on a heightfield. The SDF is the one thing SSWorldField cannot be (a
0.25 m column of spans has no wall normals), so a GPU SDF is built as a *materialised
view* over the span store: fold spans into a sparse 3D occupancy texture, run a
two-pass Jump Flood distance transform per tile. Vertical walls get real normals from
the SDF gradient; SSWorldField remains the seed and the edit fan-out. Everything wet
above the particle scale is mass-conserving: film → droplet → rivulet → puddle, and
back.

**2. State & data model.** Per particle, three SSBO records (ping-pong): pos+radius,
vel+mass, meta (phase, age, surface-id epoch, flags frozen/pool). Surface frame is
*derived, never stored*: normal `n = ∇SDF(pos)`; tangent from the bit-stable quantised
azimuth of `dripAxis` so frames don't shimmer. Movers carry a surface-velocity delta.
Global: freelist, grid cell counts, dynamic-SDF list (≤ 32 OBB/capsule records).
Determinism: no atomics in the order-dependent path — counting-sort keys rebuilt every
substep; merge ties break on stable particle id.

**3. Simulation.** Fixed substeps: 3 per shared-time quantum with catch-up. Per substep:
predict (gravity · tangent, wind), then 4 XPBD iterations over constraints, then
project, then v = Δx/dt. Constraints: C_surface (|SDF(p)| = 0 film or = r droplet);
C_cohesion (Macklin attraction kernel — surface tension, merging, mass-weighted
absorb); C_friction (static below μ·|g⊥|, else kinetic drag — slope threshold emerges);
C_adhesion (on |n·ẑ| < 0.3 faces, hold until `mass > m_break ≈ 2πrσ`; then detach →
free-fall until re-projection). Breakoff, stick-slip stutter, and sheet-vs-drop regime
all fall out; render keeps `dripReach`'s footprint law. Solver: one compute dispatch;
neighbour search: 3D uniform grid over the camera cell, rebuilt per substep.

**4. Render path.** Three ranges, one composer: < 32 m, point-sprite splats into a
dedicated RGBA16F droplet target (depth-tested), composited inside the normal pass
alongside static drops/drips. 32-64 m, neighbour chains render as oriented elongated
splats = rivulets, mass-scaled. > 64 m, nothing: the lattice and the field carry it. No
screen-space fluid mesh — overdraw and silhouette artifacts at these feature sizes
aren't worth it.

**5. Mobility matrix.** Statics/terrain: SDF from worldfield tile; edits re-peel → SDF
refit. Moving prims: per-mover OBB SDF proxy + interpolated surface velocity added at
projection; particles ride by constraint, not parenting. Avatars: capsule SDFs;
wetness transfers to a per-avatar film — this *replaces* SSAvatarWet with actual drip
physics. Flexi: per-frame vertex soup in an SSBO, spatial-hash closest-point soft
constraint; ≤ 8 active within 32 m. Foliage/impostors: non-constraining.

**6. Slope & verticals.** Slopes: friction cone per porosity. Verticals: adhesion
budget `m_break ≈ 2πrσ`; exceed → detach and fall; below it the particle creeps with
stick-slip — exactly the drip-law stutter transliterated into dynamics instead of a
wall-clock function. Walls finally get the treatment the heightfield can't give.

**7. LOD & performance.** Near 0-24 m ≤ 24 k particles; mid 24-64 m ≤ 4 k rivulet
nodes; far = field. Collapse points: SDF build (a 512² tile × 64 bands + JFA ≈ 3-5 ms
GPU, amortised one tile/frame), dynamic-SDF count > 32, close-range splat overdraw,
CPU↔GPU sync at the ledger boundary. The solver isn't the limit; the **oracle is**.

**8. GL 4.6.** Compute (solver, sort, SDF refit), SSBOs, sparse 3D textures for SDF
bricks, persistent-mapped telemetry, MDI for splat draws.

**9. Integration.** Claims `EChannel::SOLID_VOLUME_3D` — it already exists unused.
Gate `SSAtmoSurfacePBF`. Coupling: film wet > capacity spawns droplets; settling
deposits back via mWet/mPuddle; freeze sets frozen flags ↔ mIce; granular is a second
phase sharing the grid with friction/repose constraints. Ledger sync: one-frame-lagged
GPU→CPU event upload, ≤ 1 k events/frame.

**10. Honest weaknesses.** (1) 0.25 m SDF quantisation: railings, thin caps, terrain
micro-relief below the oracle; near-camera statics need a per-object mesh-SDF tier —
the design's biggest missing piece. (2) Non-determinism across GPUs: FP reduction order
differs per vendor; merge events and freeze timing can diverge; only shared-clock
quanta survive exactly. (3) Movers are approximations — particles visibly float or sink
on curved vehicles. (4) Splat/deferred compositing mis-sorts at silhouettes; per-
particle sorting is a real cost at 24 k.

**11. One-day PoC.** CPU-only, 3 k particles, terrain+static tops via `surfaceTop()` as
projector. Constraints: C_surface, C_friction, merge; breakoff on eave edges. Render
through the existing ripple-quad path. Prove: rain → beads → rivulets on slopes →
breakoff at eaves → puddle absorb.

## D10 — Surface MPM (continuum granular)

**1. Core idea.** Granular weather is a continuum, not a ledger of hacks. Keep the
two-level architecture the codebase already trusts — a far-field depth/thermal ledger on
the column lattice, and a near-field solver — but replace the near-field's creep/
empirics with a GPU **2.5D Material Point Method**: particles are material patches of
snow/sand/ash advected over the terrain surface, with snow plasticity (Stomakhin 2013),
Drucker-Prager sand (Klár 2016), and a per-particle thermal field feeding melt mass
into the existing wet/puddle ledger. MPM runs only where continuum motion is happening.

**2. State & data model.** Ledger (far field): existing channels become the MPM's
boundary condition and mass bank; mSnow is depth; melt debits into mWet/mPuddle; freeze
reads mIce. MPM window (near field): a camera-centred 64×64 patch of the 0.25 m lattice
(16 m box), an SSBO of particles `{pos.xy, vel.xyz, h, mass, F (2×2 in-plane), Jp, T,
material_id, age}`; grid side: two double-buffered node SSBOs. A per-cell particle
count (atomicAdd head array) with capacity 4 — a particle is a 0.25 m² patch of a depth
slice (~1 cm min). Depth ≤ 2 cm or sleeping cells never spawn particles; typical
occupancy 12-40 k, hard cap 65,536. Window origin/cell uniforms mirror `ssFieldOrigin`
so passes fetch either source identically.

**3. Simulation.** Kernels per fixed step: (1) seed/recycle — spawn where ledger depth
> threshold, absorb where depth → 0; (2) P2G — quadratic B-spline scatter;
(3) grid update — gravity projected on the surface, wind traction from the flowmap,
thermal diffusion in-plane, then freeze the grid where T < 0 and ice-fraction > θ
(velocity zeroed — ice locks); (4) G2P + constitutive return-mapping — Stomakhin snow
energy-hardening, Drucker-Prager yield with friction angle = preset repose, cohesion
`c·(1 − wet)` collapsing as melt wets; (5) ledger exchange — net depth change per
column atomicAdds into the window's ledger texture, the single write path, the GPU
analogue of `depositAt`; melt converts particle mass to wet/puddle credit. Timeline:
steps land on exact quanta; CFL at 0.25 m and ≤ 15 m/s gives Δt ≈ 12 ms; 2 substeps of
8 ms per frame, amortized — a frame may run zero steps when the window sleeps.

**4. Render path.** MPM writes a deposit depth/displacement texture (RG16F) at window
resolution; the three passes sample it instead of the ledger where valid, falling back
otherwise. Normal displacement = depth-gradient tilt added after `ssDepositMask`;
sparkle/grain stay procedural. Ice: the thermal/ice channel drives the freeze look plus
a procedural crack pattern over locked particles. Footprints: moving-object capsules
rasterize a per-frame compression/carve mask; a compute pass applies it as depth
displacement + grid impulse, retiring SSAvatarWet's capsule hack.

**5. Mobility matrix.** Statics/terrain: full. Moving: carve/impulse masks + compaction
(footprints, plows); pushed heaps avalanche. Avatars: same mask path. Flexi: ledger
credit only. Foliage: capture-pass flags exclude. Impostors: excluded — ledger depth
stays the far lie.

**6. Slope & verticals.** Repose *emerges* from Drucker-Prager — a slope steeper than
the friction angle yields until it isn't — replacing the incumbent heuristic.
Verticals: gravity's in-plane projection pulls particles down the face; a cohesion term
(damp/frozen) supports clinging up to a thickness threshold — beyond it the yield
surface fails and the sheet peels off. Over-the-eave spill is the honest 2.5D
compromise: material sheds along the face, not ballistic off it.

**7. LOD & performance.** Tier 0 CPU incumbent — fallback and low spec. Tier 1 — 32×32
window, 2 particles/cell, 1 substep. Tier 2 — 64×64, 4 particles/cell, 2 substeps.
Worst case ≈ 65 k particles × 2 substeps ≈ 1.5 ms. Window follows the camera; the far
ledger handles everything outside it, so cost is constant, not scene-scaled.

**8. GL 4.6.** Compute + SSBOs; fence-guarded persistent-mapped staging; shader
atomics for P2G and ledger exchange; MDI for debris sprites.

**9. Integration.** New ssmpm; claims the SURFACE_TOP channel; `markDirty` invalidates
seeded cells. `SSAtmoMPMQuality` gates the tier; the ledger remains the single ground
truth — MPM is a write-path peer of `depositAt`, never a bypass. `liftAt` stays the one
lift authority; entrained mass converts to the existing drift pool. Thermal input is
`SSAtmoMagic::temperatureC` + exposure.

**10. Honest weaknesses.** (1) 2.5D loses real 3D: no snow bridges, no slab overhang,
no ballistic ejection. (2) Window locality lies at the seam. (3) Patches, not strata:
no crust-over-powder layering. (4) GPU atomics break the "identical across viewers"
guarantee. (5) MPM is overkill for calm weather — the machinery earns its keep only in
storms and traffic.

**11. One-day PoC.** One compute shader: 32×32 window, 2 particles/cell,
Drucker-Prager only, 1 substep, writing depth into a throwaway texture the albedo pass
samples behind `SSAtmoMPMQuality`. Test: seed a heap, apply a wind impulse, verify the
avalanche stops at repose and the pass renders the displaced surface.

## D11 — Amortized temporal resampling (ReSTIR for ground state)

**1. Core idea.** Treat per-cell weather state as a streaming signal and the 0.25 s
full-grid tick as an *importance-sampled* event. Each quantum, a bounded set of cells
wins a reservoir draw and gets a full `stepCell`+transport step; every other cell is
*stale* and is carried by a cheap analytic predictor. The predictor is the same closed
form the CPU already evaluates (`1 - exp(-rate·dt)`), so staleness becomes predicted
freshness, not ghosting. Selection is deterministic: a per-cell hash threshold on an
activity score, never a rank or index trim.

**2. State & data model.** Keep every existing Field vector untouched; add per cell
`mStamp` (U32 quantum last fully stepped), `mAct` (F32 activity score), one tolerance
class byte. Per region, the full vectors are already the checkpoint; slices commit in
row bands so a mid-sweep crash leaves a quantum-consistent prefix. The window gains a
fifth texture `ssFieldAgeMap` RGBA8: staleness in quanta, predicted rate, activity,
class. `ssFieldOrigin`/lattice stay identical, so shader fetch sites don't move.

**3. Simulation.** Importance = |predicted wet gain| + |puddle gain| + exposure change
+ camera proximity falloff + wind-event boost + phase-danger: any cell within ±1 °C of
0 °C, or whose ice crosses 0, is forced full-step — phase transitions and look-Mix
promotions are never interpolated. What interpolates: wet, puddle, stain, age, snow
depth — all monotone relaxations with bound `|true − interp| ≤ rate · staleQuanta ·
0.25 s`; admit staleness while bound < ε (0.02 wet, 2 mm puddle). What never
interpolates: pool membership (drainage is a static per-geometry-serial solve), repose/
spill events, scorch holds — a `vaporise` strike forcibly promotes its disc this
quantum. Execution: 16 cores drain a priority worklist of K ≈ 4096 cells/tick;
`SSGranular::step` unchanged but run only on resampled cells plus their inflow donors.
Failure taxonomy: (a) burst rain arrives late — a preset-change or gust front stamps
the whole field resync; (b) puddle edges crawl — cells near the mask knee get tolerance
halved, full-stepping at 2× rate; (c) rivulets must not ghost — drip/runnel detail is
procedural wall-clock animation, never state, so it cannot resample by construction.

**4. Render path.** The three passes keep their gbuffer contract; their window fetches
simply return fresher-or-staler per region, and each shader adds one line: advance the
fetched wet/puddle by `rate · staleness` from `ssFieldAgeMap`. No motion vectors needed
— the passes are stateless redraws, every lattice world-anchored; a camera cut
invalidates no history. The only temporal state is the impact ring buffer; a cut
detector resets it plus extrapolation uniforms for one frame.

**5. Mobility matrix.** Statics/terrain: full beneficiaries. Moving: geometry retrace
zeroes cells on serial change — promote on zeroing. Avatars: nothing (capsules
retired; rain occlusion reads the cover window). Flexi: too unstable for ledgers —
procedural wetting only. Foliage/impostors: not in the surface grid.

**6. Slope & verticals.** Slopes: D8 directions and spill levels are precomputed, not
per-tick state, so erosion cells resample on gain only. Verticals: held drops and runs
are the drip lattice — procedural, hash-anchored — and edge shedding is already
amortised by cursor; the cursor becomes the staleness sweeper that retires stale shed
edges.

**7. LOD & performance.** Region = 65,536 cells; today every cell pays every 0.25 s.
Steady rain shows < 5% active cells: K = 4096/tick full-steps every active cell every
tick while distant quiet cells refresh on a 4-16 s hash-scheduled cadence.
`updateWindow()` re-packs all four textures per frame; that pack moves into the
worklist pass — only touched rows are repacked and uploaded via persistent-mapped PBO
sub-ranges.

**8. GL 4.6.** Compute shader drains a worklist SSBO for the GPU-side predictor +
window pack (pure function of state — deterministic, but the CPU stays authoritative:
shaders predict, never own state, per the SSWhiteout one-way rule). Persistent-mapped
coherent ring for uploads; sparse tiles per claimed region; SSBO worklist with atomic
compaction. CPU fallback keeps GL 3.3.

**9. Integration.** `SSAtmoSurfaceResample` gates; off = pristine full tick. Debug
staleness heat overlay beside `renderRunoffDebug()`. External writers keep working —
each external write bumps the cell's priority. `markDirty` re-peels fan out to priority.

**10. Honest weaknesses.** (1) Rate stationarity: the ε bound assumes tomorrow's rates
equal the last step's; every mitigation is a heuristic resync, and a wrong guess shows
as a late front. (2) Two models of the sim: the in-shader predictor can drift from CPU
truth, requiring twinned constants and a test rig. (3) GPU window pack breaks the
CPU-resident window contract: CPU consumers would need a readback or duplicate pack.
(4) Determinism pressure: the priority order must be a total order on
(region, cell, quantum) with hash tiebreaks.

**11. One-day PoC.** Add mStamp/mAct; gate the per-cell loop on the hash threshold
with forced promotions; repack only touched rows into ssFieldAgeMap; extend one shader
with the staleness advance; paint staleness in a debug view. Target > 10× tick cut at
steady rain with no visible difference at 4 m.

## D12 — CPU bake + streaming

**1. Core idea.** Weather is a **baked product, like a lightmap**. The 16-core pool
steps the ground-truth ledger and then *bakes* a per-region material-descriptor texture
set; the GPU never solves anything — it samples world-anchored baked textures, exactly
like the wet pass samples the camera window today. The precedent is already in the
repo: the air flood runs on the general worker queue from a snapshot, generation- and
serial-gated, and edits re-peel scissored dirty rectangles. SSBakeField industrializes
both: every tick is a snapshot → parallel step → parallel bake → streamed upload. The
static world is the ideal customer; movers get a thin overlay. No new threads — jobs
ride `LL::WorkQueue`.

**2. State & data model.** Ground truth stays CPU SoA per region (the Field vectors).
Sim grid: 1 m, 256² (today GEOM_RES=128 is 2 m; halve the cell). The bake is the
*materialization*: T0 `bakeLiquid` RGBA16F 512² (0.5 m): wet film, puddle depth,
deposit depth, ice. T1 `bakeMat` RGBA8: deposit type, age, frost, porosity×enclosure.
T2 `bakeFlow` RG16F: octahedral flow direction + magnitude. W `wallStrips`: packed
vertical atlas. Full 0.25 m (1024²) reserved for the near ring: a camera-centred 128 m
window re-baked at 0.25 m from the 0.5 m bake plus own-hash noise (LLPerlinNoise seeds
rand() — banned). Everything world-anchored per region; no camera-window rebuild.

**3. Simulation.** Fixed-step 0.25 s ticks exactly as built. Job graph per tick:
(1) tick jobs — region striped into 32-row blocks, each over the *previous* step's
snapshot; outflows accumulate into per-cell inflow, applied after all outflows —
order-independent, bitwise identical regardless of scheduling. (2) bake jobs — on the
post-tick snapshot; only dirty rects re-bake; in active weather ⅛ of the region
re-bakes per tick (full refresh every 2 s, staggered). (3) upload jobs — main thread
drains a completion queue, issues dirty-rect `glTextureSubImage2D` from the persistent
ring, fenced. Drainage reuse: `buildDrainage`'s Barnes fill / D8 baked once per
geometry serial — puddle masks and rivulet routing are bake inputs, never per-tick
solves. Cost: 65,536 cells/region/step × ~50 FLOP × 4 regions × 4 Hz ≈ 52 MFLOP/s —
trivial against 16 AVX2 cores. SIMD: SoA loops vectorize 8-wide; the D4 creep exchange
stays scalar but blocked. Working set ~3.4 MB/region fits L3.

**4. Render path.** The three passes keep their structure but sample the bake instead
of the four 256² windows: resolve region from the origin uniform, fetch
bakeLiquid/bakeMat/bakeFlow, feed the existing law functions. POM, sparkle and drop
detail stay shader-side, so the bake stays low-frequency. No per-frame updateWindow CPU
fill, no four full-texture uploads.

**5. Mobility matrix.** Statics/terrain: baked fully; edits re-bake the dirty rect in
≤ 2 s. Moving: thin per-object 64² overlay texture (wet/icing accumulator stepped on
the sim clock), composited in the wet/albedo pass; no world coupling. Avatars: same
overlay — SSAvatarWet's hack retires in favour of this. Flexi: overlay only. Foliage:
no bake; screen-space droplets only. Impostors: bake sampled at anchor.

**6. Slope & verticals.** Slopes native: D8 routing plus slope baked into bakeFlow.
Verticals: for each band (4 m) and each column whose spans show a vertical face, a
**wall strip** — a packed 32×128 texel block in a 4096² RGBA16F atlas with an R16UI
indirection texture. Wall sim is 1D per strip (film falls, breaks into runs, feeds the
ground bake at the strip's base), run as bake jobs on the same pool, step-locked.
Sparse allocation keeps the atlas under ~24 MB live.

**7. LOD & performance.** Residency: camera region at 0.25 m + 512² full region +
8 neighbours at 256² = ~30 MB GPU, LRU. Uploads budget 4 MB/s sustained via persistent
ring; capped at 2 region-introductions/frame; teleport floods stream bottom-up.

**8. GL 4.6.** DSA, immutable targets, **persistent-mapped 4-deep PBO ring**
(`glBufferStorage` persistent+coherent) with `glFenceSync` per slot;
`glTextureBarrier` only for debug overlays. MDI for drop lattices. Compute deliberately
unused for state.

**9. Integration.** SSBakeField claims SURFACE_TOP + DRAINAGE_NETWORK via the
refcounted Interest model. Gated by `SSAtmoBakeWeather`; off = existing path untouched.

**10. Honest weaknesses.** (1) Verticals are a bolt-on — the wall atlas is where bugs
will breed. (2) Bake latency vs fast events: 2 s full-state visibility; vaporise needs
an urgent out-of-band dirty upload. (3) The dynamic overlay is thin and lying — movers/
avatars/flexi get zero world coupling (an avatar under an eave wets the same in rain),
yet crowded real scenes are mostly non-static content. (4) Triple storage triples
memory and adds a coherence surface. (5) Region-transition spikes contend with the
renderer for PCIe.

**11. One-day PoC.** SSBakeField for the camera's region only: tick the existing Field,
one bake job (512² RGBA16F splat), one persistent-mapped PBO upload, albedo pass
samples the bake under the gate. Measure tick parallel speedup, upload ms, visual
parity.

## D13 — World-space virtual texture

**1. Core idea.** Weather state is a world-space scalar field, so give it a software
virtual texture: a page table over world XY (three axes for triplanar), pages resident
in a physical pool, filled on demand from the CPU ground-truth ledgers. The decisive
observation is that Atmo Magic already proves the macro/procedural split: drops, drips,
ripples and sparkle are *stateless* hashed lattices gated by field channels. Only
quantities that integrate over time — wet, puddle, snow, ice, frost, stain, age — need
storage. So the VT carries centimetre-scale *nothing*: it carries 0.25 m macro state,
and detail stays procedural, seeded by the same integer hash. Pages never require
re-rendering scene content — the killer cost of classic megatextures — only a
re-blocked copy of arrays that already exist.

**2. State & data model.** Macro page: 128² texels @ 0.25 m = 32 m per page. Two
attachments per page: A RGBA16F (surface Z, wet, puddle, snow), B RGBA8 (ice, frost,
stain, age, packed flags). 192 KB/page; sources are verbatim copies of the Field/
Geometry vectors and worldfield span stores. Page table: per axis (XY, XZ, YZ) a mip
pyramid of indirection R32UI/64-bit entries: packed physical layer+slot, state word,
geometry serial for staleness. Physical pool: one GL_TEXTURE_2D_ARRAY per attachment,
4096² × 8 layers = 1024 slots, ~167 MB. The incumbent 256² camera window survives as
the always-resident fallback — nothing ever samples "unmapped".

**3. Simulation.** Unchanged: the per-region CPU ledger stays the one ground truth.
The VT is a *materialized view*: pages marked dirty from the same signals that mark
windows today (tick deltas, markDirty, dirty rects) and re-gathered — a 128² page
gather is µs of memcpy on the worker queue; a full visible set refreshes inside one
idle() at negligible cost. No GPU sim; determinism for free.

**4. Render path.** The three surface passes swap `ssFieldFetch` for a VT fetch:
quad → axis selection → table lookup → bindless page sample. `ssSurfaceFieldF.glsl` is
the single include all three link, so the change is contained there plus the shell's
uploads. Feedback: the wet pass atomically ORs (pageId, minMip) into a small SSBO hash;
a compute pass compacts it into a request list, read back via SSGLReadback's shared-
context worker. The main thread enqueues page fills; the 16-core pool gathers them;
`glCopyImageSubData`/`glTexSubImage3D` per slot. No per-page scene rendering anywhere.

**5. Mobility matrix.** Statics/terrain: full VT membership. Moving: excluded; they
receive weather procedurally from the VT sample at their root XY. Avatars: same overlay
path — SSAvatarWet capsules retired; skin wets/drips by sampling the VT at avatar
position. Flexi/foliage: overlay; root-position sample × procedural streaks.
Impostors: one VT sample baked into their tint.

**6. Slope & verticals.** XY pages serve up-facing and mildly sloped surfaces.
Verticals use XZ/YZ page spaces sharing the pool: a wall texel's state is derived at
gather time from the worldfield column at its foot — wetness climbs by splash height,
frost by exposure — and roof-eave drip seeds come from the D8/pool drainage solve: a
pool member's spill column marks the eave, and the wall page beneath inherits a run
source. Axis selection per-fragment by |normal| dominance; empty vertical space costs
zero.

**7. LOD & performance.** Full region at 0.25 m = 1024² = 64 pages/mip; four visible
regions ≈ 400 resident; pool of 1024 holds 2.5× headroom. Fill: ~5 ms one-time,
amortized; steady-state re-gathers only dirty/feedback pages (< 1%/frame). Streaming
mips fall back to the window texture, so LOD transitions are free. Miss on camera
cuts: 1-2 frames, masked by the fallback and teleport prefetch.

**8. GL 4.6.** Bindless resident handles (stable per-layer, so the table carries
handles), SSBO page table + feedback hash, compute for feedback compaction,
persistent-mapped request/upload buffers with fence polling, `glCopyImageSubData`. No
sparse textures (dense pool is simpler and deterministic), no MDI.

**9. Integration.** `SSSurfaceVT` gate; off-path keeps window sampling byte-identical
(the fallback texture *is* the window, updated by the existing updateWindow code). VT
pages are a new consumer of `SSWorldField::claim` interests. No gbuffer format changes.

**10. Honest weaknesses.** (1) Infrastructure gravity: bindless + table indirection
enters every deferred program via the shared include — a renderer-wide commitment;
bindless on Mesa/old iGPU is notorious. (2) Feedback latency: 1-2 frame request lag —
fast camera whips show fallback detail. (3) Vertical pages are a half-truth: wall state
is synthesized from foot columns, not measured per-wall. (4) Two sources of truth
re-creates the staleness class the worldfield fights with serials. (5) 167 MB pool is
real VRAM competing with the BC7 store.

**11. One-day PoC.** XY axes only, no mips, no feedback: one 4096² RGBA16F pool, page
table as a uniform array of 64 region-fixed pages filled synchronously from mFields;
point ssSurfaceFieldF at the table; keep drops/drips procedural. A/B against the window
path in the wet pass alone.

## D14 — Forward material layers + vertex weather

**1. Core idea.** The field stays the CPU-stepped ground truth; what changes is where
field→pixels happens. Every deferred material writer gets a shared weather layer: the
**vertex stage** VTF-samples the field windows to produce an interpolated weather
magnitude and a conservative dry/wet gate, the **fragment stage** re-fetches the field
at its own (vertex-interpolated, not depth-reconstructed) agent position and blends
wet/snow/ice/frost/deposit layers straight into the albedo/ORM/normal gbuffer writes.
The three screen-space passes are deleted under the gate; identity is native to each
draw, so moving content carries its answer for free.

**2. State & data model.** Nothing new. Reuse all four windows, the looks and the ring
buffer. Deleted state: mScratch/mScratchNormal, renderWetPass/renderAlbedoPass bodies,
SSAvatarWet. New plumbing only: per-frame `ssAgentFromView` mat4, per-draw CPU gate
uniform, vertex-stage field bindings pinned to a dedicated texture unit.

**3. Simulation.** Untouched. Tick order, settle, stepCell, wash, granular transport
all stay. The drop/drip/ring laws remain LOCKSTEP with sssurfacedropcore.h — but
transliterated once into a single include (`ssWeatherF.glsl`) instead of into per-pass
files, reusing pitchM/hash/cellAt.

**4. Render path.** `attachShaderFeatures` gains `hasWeather`, attaching
ssWeatherV.glsl + ssWeatherF.glsl beside textureUtilV — same attach-point precedent.
VS: agent position from view-space varying plus VTF fetch of wet/snow/puddle/ice →
packed varying magnitude. FS: one function with two entries — `ssWeatherPBR` (reads
true ORM) and `ssWeatherLegacy` (spec colour/gloss) — sharing identical inner laws:
porosity exponent darkening, roughness tightening, liquid metal tint, deposit
matte+sparkle, ice cracks, frost grain, puddle flatten, drops/drips/rings. Because
porosity is computed where the material's pristine textures are in hand, the degraded
`ssPorosityFromAlbedo` fallback retires; PBR paths read roughness directly.

**5. Mobility matrix.** Statics/terrain: sampled where drawn; wet bakes into the
gbuffer once, no per-frame pass cost. Moving: magnitude re-sampled per frame at current
position — a cart entering rain wets as it drives. Avatars: avatarF + rigged variants
inherit via `make_rigged_variant`; capsules deleted; skin = fixed porosity constant.
Flexi: VS samples after displacement — cloth wets by where it hangs. Alpha foliage:
mask-discard path, fixed leaf porosity. Impostors: one magnitude from the foot vertex,
tint-only.

**6. Slope & verticals.** Interpolated varyings kill the depth-reconstruction error
class the anchoring rule was built to survive (190 mm position drift, 0.24 rad
derivative normals) — the azimuth-quantised drip axis becomes trivially satisfied.
Puddles: level gate + field value + shore noise, flattened toward world up inside the
material FS. Rivulets/erosion: flow window's slope channels bias streak direction;
verticals take the triplanar drip lattice, clinging droplets breaking into runs past a
wet threshold, wind-skewed. Deposits: the Zucconi mask with lee settling; field-side
transport unchanged.

**7. LOD & performance.** Dry draws pay **zero shader cost**: a CPU AABB query on the
field gates the draw uniform, which most draws fail. Wet triangles pay 1 VTF + 1 FS
re-fetch + ~40 ALU, versus today's three fullscreen passes for every pixel regardless
of weather — cost scales with wet coverage. The FS re-fetch (only when gated) restores
0.25 m detail inside huge terrain triangles.

**8. GL 4.6.** The de-risk: **no new texture, format, or filter mode** — ssFieldMap is
already sampled by the shipping passes; VTF only relocates the stage. GL 3.3 min-spec
guarantees ≥ 16 vertex texture units; runtime guard falls back to gate-off. Explicit
sampler `layout(binding=…)` pins the vertex unit clear of material samplers. Nothing
exotic is load-bearing.

**9. Integration.** `hasWeather = true` on diffuse/terrain/pbropaque/pbralpha/
pbrterrain/avatar/impostor/foliage variants. Bind: SSSurfaceField::bindForShader +
looks + rings per frame; per-draw gate beside the matrixPalette precedent. Settings:
`SSAtmoWeatherForward`, `SSAtmoWeatherDebug`. Fallback: gate off = the proven three
passes, bit-identical. Puddle flattening and rings move in-material; ripple quads keep
the far field.

**10. Honest weaknesses.** (1) No wetness memory on movers — a tarp carried from rain
into a barn dries instantly. (2) Overdraw multiplies cost — O(pixels × layers); dense
foliage pays per card, and overlapping alpha cards double-darken where the old pass
weathered the resolved gbuffer once. (3) Vertex gate conservatism on 16 m triangles.
(4) Coverage holes at content classes without material hooks (particles, water).
(5) Deleting proven passes removes the safety net and grows the twin-test surface.

**11. One-day PoC.** Gate `SSAtmoWeatherForward`. Add ssAgentFromView + ss_agent_pos to
diffuseV.glsl; attach a minimal ssWeatherF.glsl with ssFieldFetch, wet darkening +
gloss tighten only. Enable on gDeferredDiffuseProgram + terrain; early-out the two
passes when gated. Validate: dry world unchanged, wet road reads, crate wets as it
moves, flip-to-fallback pristine.

---

# Appendix B — Judge verdicts

## Judge A — performance, memory, scalability

**Score table.** D1 6 — 34 MB closes at 1080p, but cost is per-pixel forever (~133 MB
at 4K), and history dies on every transparent — the forest is its blind spot. D2 7 —
18.6 MB honest; real resolve win, but 9 B/px written inside the most overdrawn pass in
the frame. D3 8 — ~5 MB constant, ring amortization, CPU→~0 is the right shape. D4 5 —
dependent hash probe in every material fragment = latency poison; occupancy spikes.
D5 6 — active-texel amortization correct, but the 96-page cap means eviction/repaint
storms; VRAM claim doesn't close. D6 7 — single-writer, no atomics = deterministic *and*
cheap; MDI pays off exactly in dense forests; movers degrade gracefully. D7 8 — sub-ms
CPU claim passes flop math; fixed O(window) cost; never has a bad scene because it
never scales with the scene. D8 7 — 512 droplets is trivial (~1 ms total is honest).
D9 4 — "3-5 ms/tile, 1 tile/frame" reads as a standing tax; avatars invalidate SDFs
continuously. D10 4 — float P2G atomics are order-nondeterministic; constant 1.5 ms
even in calm weather. D11 7 — 10× is arithmetic-honest; closed-form prediction is
*exact* for linear drying. D12 8 — only design with ~zero steady-state GPU frame cost;
pays in 2 s latency, not fps. D13 5 — 167 MB pool for macro-only 0.25 m data is
dominated by D3's 5 MB cascade for the same job. D14 6 — zero attachments + deleted
passes is real, but the admitted worst case (100% wet × alpha foliage) *is the
showcase scenario*.

**Honest-number audit (top 3).** D9: a 256² JFA should cost ~0.3 ms, so 3-5 ms/tile
implies 1024² tiles — worse amortization than stated. D5: 96 pages × 3 arrays gives
75-400 MB, not 50 MB. D2: 3-5% is a calm-scene number; forest alpha overdraw plus
~17-20 writer paths makes the honest forest number closer to 5-8%. (Runner-up: D1's
2.2 ms scales ×4 at 4K and assumes valid motion vectors for geometry that doesn't
write them.)

**Worst-case stress.** (a) Dense foliage forest: D12, D7, D3, D11, D6. (b) 40-avatar
dance floor: D12, D7, D8, D11, D3. (c) Teleport storm, 8 regions: D12, D7, D3, D6,
D13. Casualties everywhere: D1 history wipe, D4 hash rehash, D5 96-page repaint.

**Top 3.** D12 — moved the cost off the frame entirely; worst case a visible artifact,
not dropped ms. D7 — smallest defensible constant envelope; never scales with the
scene. D3 — world-anchored constant footprint beats every per-pixel and per-object
competitor.

**Hybrid affinity.** D7 + D11: one shared cell store — D11's worklist/predictor *is*
D7's parked-cell sleep mechanism; pure composition, zero redundant representation.
D3 + D6: clipmap as macro boundary condition, D6's single-writer compute as per-texel
detail. D12 + D2: D12's streamed atlases need exactly one deferred fetch path — D2's
single resolve pass *is* that path.

**Biggest trap.** The fleet prices the sunny day: the real cost lives in the
invalidation path, where history wipes, page thrash, hash rehash, SDF re-flood and
worklist spikes all spike simultaneously — in the same frames the renderer is already
re-loading 8 regions. And three designs add dependent per-fragment work to the hottest
shaders, which multiplies with alpha overdraw: the dense forest is the benchmark that
breaks half the fleet and the one nobody quoted a number for.

## Judge B — material & physics fidelity

**Score table.** D1 5.5 — "tension-break" as screen-space trail capacity is physics
vocabulary on a view-dependent visual; granular is shimmer, not saltation. D2 4.5 —
exact ORM porosity is the one real fidelity win; rivulets reduced to seeds. D3 8.0 —
genuine film sim + repose-CA on a world clipmap; walls get a real state structure.
D4 7.5 — rivulets, spill-level pools, eave drip cascades genuinely emerge; freeze
missing from the story. D5 5.5 — UV-space repose is metric-wrong under UV stretch;
walls never hold runs. D6 6.5 — conduction-coupled thermal is solid; refusing
cross-object transfer strands water at seams. D7 9.0 — the only design where puddles
obey true spill levels, freeze reroutes flow (ice dams), erosion uses Mei capacity, and
wall runs carry real mass down a Nusselt film. D8 7.0 — the only genuine cm droplets
driving real impact rings; rest declared-statistical. D9 9.0 — every threshold is a
force balance: friction-cone repose, Tate's-law breakoff, stick-slip as dynamics; film-
as-particles won't scale. D10 8.5 — best granular: real plasticity/yield with cohesion
c(1−wet); drops/ripples absent. D11 4.0 — temporal hygiene, not physics, but
"never interpolate across 0 °C" is the rule everyone should steal. D12 5.5 — real 1D
wall film; serial bake freezes hydrology at bake time. D13 4.0 — splash-height wall
wetting is invented physics and wets the wrong side of walls (admitted). D14 4.5 —
renderer polish with exact per-material porosity; "break into runs past wet threshold"
is paint-by-wetness.

**Spectrum coverage matrix** (F full / P partial / A absent):

| Design | Drops | Streaks→Rivulets | Sheet | Puddles+Ripples | Freeze/Thaw | Granular+Cplg |
|---|---|---|---|---|---|---|
| D1 | P | P | P | P | P | P |
| D2 | P | P | A | P | A | A |
| D3 | P | P | **F** | **F** | **F** | **F** |
| D4 | P | **F** | **F** | **F** | A | **F** |
| D5 | P | P | A | P | P | P |
| D6 | A | P | P | P | **F** | A |
| D7 | P | **F** | **F** | **F** | **F** | P |
| D8 | **F** | P | P | **F** | A | A |
| D9 | **F** | **F** | P | **F** | P | **F** |
| D10 | A | A | P | P | **F** | **F** |
| D11 | A | A | A | P | P | A |
| D12 | A | P | P | P | P | P |
| D13 | P | P | A | P | P | A |
| D14 | P | P | A | P | A | A |

**Physics-honesty awards.** Best emergent: D9 — slope threshold *is* the friction
cone, vertical breakoff *is* the capillary budget, stick-slip is dynamics; zero painted
thresholds. Honourable: D10, D4. Most dishonest claim: D1 — surface tension cannot
live in a view-dependent buffer; trail capacity ≠ capillary instability. Runner-up:
D13's splash-height wetting of indoor faces.

**Top 3.** D7 — the full liquid spectrum delivered by mechanisms: priority-flood/D8
drainage, true spill-level puddles, Mei-capacity erosion, the fleet's only freeze→flow
coupling. D9 — fidelity by emergence, one mass-conserving system. D10 — granular a
generation ahead; loses the top spot because drops/ripples are absent.

**Hybrid affinity.** D7 liquid + D10 granular — the natural marriage: wetness feeds
cohesion c(1−wet); emergent repose replaces the friction-angle knob; melt mass drains
into the pipes. D7 film + D9 detail — where the Nusselt film thins below critical
thickness, spawn D9 particles instead of hashed run seeds; the mass handoff at wall
base is already specified in D7. D3 clipmap + D8 heroes — film-depth gate is the spawn
condition; hero rings write back as ripple disturbances. D4 eave events → D7 wall
ledger. D11's never-interpolate-phase rule → everyone.

**Biggest unsolved gap.** Persistent, world-anchored centimetre-scale liquid detail at
world coverage — every design fakes it procedurally, budgets it to heroes, or pays
particle prices that can't scale to sheet flow. Runner-up: ice crack relief and frost
are effectively uncovered — D1's Voronoi cracks is the only attempt.

## Judge C — engineering fit, migration risk, robustness

**Score table.** D1 5 — spends the full ~20-writer edit on a *disposable* side channel;
id-recycling bleed and missed-writer ghosts are precisely the stale-state class this
fork exists to kill. D2 3 — every writer ×2 (GL3.3 dual path) is permanent dual
maintenance; `gl_DrawID` needs 4.6 on hardware that doesn't have it; alpha-poisoning
unresolved. D3 3 — GPU-resident stepped state + CPU mirror is two ground truths by
construction, and camera-anchored texel alignment deliberately breaks the texel-exact
golden harness the team already runs. D4 5 — best-grounded GPU variant (deterministic
hash of the existing span store; the SOLID_VOLUME_3D claim is a designed extension
point), but gather lag, occupancy thrash and eviction serialization make bookkeeping
rival the sim. D5 4 — determinism-aware (UUID-seeded page hashes) and movers handled
honestly, but an R8 page-id attachment re-touches all writers and seam-warp is open
R&D. D6 6 — the only digest with a real phase ladder — phase 1 is genuinely small;
but "single-writer compute keeps determinism" is device-local truth only, and
movers-scalar-only concedes the correctness axis that matters most. D7 8 — rides the
existing window/upload machinery and the already-proven order-independent inflow
pattern; CPU fixed-step native; one LOCKSTEP ledger. D8 6 — smallest honest GPU layer;
macro stays CPU-deterministic, hero nondeterminism conceded and short-lived; fallback
is today's passes verbatim. D9 3 — five new interacting systems, admitted cross-vendor
FP erosion, 1-frame-lagged sync puts staleness *in the contract*, and it contests the
same SOLID_VOLUME_3D semantics D4 wants. D10 5 — depositAt single-write-path is the
structurally cleanest erosion and footprint masks genuinely kill SSAvatarWet — but
atomics concede bitwise cross-viewer equality, the house spine. D11 8 — smallest delta
on every axis; predictor under the twin-test discipline; CPU stays authoritative; the
bet (rate-stationarity) is narrow and testable. D12 7 — industrializes the discipline
the repo already trusts; GPU stays a dumb consumer so upstream merge hygiene is
near-zero; the tax is the triple-storage coherence surface. D13 3 — renderer-wide
bindless indirection, two sources of truth vs serials, bindless on Mesa/old-iGPU
notorious, 167 MB regressive on exactly the hardware that matters. D14 5 — the single
attach-point include is the right *shape*, but deleting the three proven passes *under
the gate* converts the feature flag into a cliff, and VTF is zero-precedent in this
tree.

**Blast radius (fewest → most).** D11 (~2-3 C++ files, 1 shader line, 1 window);
D12 (~8-10 files, ~0-2 shader edits); D7 (~6-8 files); D8 (~8-10 files);
D6-p1 (3-5 files); D4 (~8-12); D5 (~10-14); D3 (~10-15 + 3 interacting systems);
D10 (~10-14); D1 (~20 shader edits); D14 (~10-16 material FS); D13 (every deferred
program + pool + feedback infra); D2 (every writer + a full GL3.3 duplicate). Shape:
the cheap five all keep the CPU ledger as truth; the expensive five all thread new
channels through the writer tree.

**Fallback honour roll.** Genuinely "off = byte-identical": D11 (by construction),
D7 (flowfield zeroed, existing passes keep current uniforms), D12 (GPU is a consumer —
"off" is "no uploads"), D8 (fallback is *the same code users run today*), D6 phase 1.
A lie or second-path burden: D2, D3 (the "fallback" is a reconciliation protocol),
D9 (stale state contractual), D14 (no degradation rung between the new path and broken
materials), D13 (two truths make "off" a consistency question).

**Determinism audit.** Preserves/strengthens: D7, D11, D12, D4, D5 (UUID-seeded).
Erodes, honestly conceded: D8 (short-lived), D9 (the most honest erosion statement),
D10 (atomics). Erodes, hand-wavy: D6 phase 2 ("single-writer keeps determinism" is
per-device only), D3. Neutral (visual-side only): D1, D2, D14. D13 is the exception —
sim-adjacent truth risks leaking into a cache.

**Top 3.** D11 — the only design whose off-path is byte-identical *by construction*
and which extends the twin-test discipline instead of routing around it. D7 — adds the
one thing the surface system actually lacks (a flow sim core) while inheriting the
existing window/upload machinery and proven order-independence pattern. D12 — movers/
flexi get explicitly uncoupled overlays — the stale-ghost question is answered in the
design rather than deferred.

**Hybrid affinity.** One roadmap (phase order): D11 (gating + staleness, zero visual
delta) → D7 (flow sim core on the same tick/window rails) → D12 (bake+stream when
coverage outgrows the window) → D8 (hero droplets as a cosmetic GPU layer over a
now-stable CPU truth). Parallel track: D10's footprint masks to delete ssavatarwet.
Composable throughout: D6 phase 1 as the per-draw weather-id index. Mutually exclusive
forks: D2 vs D1 (rival id side-channels); D4 vs D9 (first claimant sets
SOLID_VOLUME_3D semantics); D3 vs the window discipline; D13 vs D5 (rival texture
caches); D14 vs D8-style gbuffer adhesion; D6 phase 2 vs D7/D11/D12 (where the sim
lives).

**The single decision this fork will regret making carelessly.** Deciding where the
ground truth lives by drift instead of by decree — letting any design migrate
authoritative state into GPU-resident/compute-stepped structures. Everything this repo
is built on — LOCKSTEP constants, the twin harness, texel-exact goldens, the no-rand
rule — is funded by one property: *CPU is the only truth, fixed-step*. Nearly free to
keep now; a sim rewrite to restore. (Runner-up, the reason D14 scores 5 and not 7:
deleting the three proven passes *under* the gate turns the house's fallback convention
into a cliff.)
