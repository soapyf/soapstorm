# Atmo Magic: world-field acoustics (the ACOUSTIC channel, full design)

> **Status (2026-09-07, second pass): Parts 1-4 built.** The ring lattice is
> retired. Shipped now, all riding the flood's worker job over the span
> snapshot (`ss_wf_acoustic_build` in `ssworldfield.cpp`, pure core in
> `ssacousticcore.h`): the gap-anchored probe set (Part 1, anchor-column
> spiral included), the probe graph with validated links, apertures and
> portal flags (Part 2), `SSWorldField::traceSolid` and the real
> `occlusionGain` (Part 3's occlusion), per-source Dijkstra propagation
> through `SSWorldField::propagationQuery` - thunder's travel time, muffle
> and arrival direction are outputs now - the listener blend
> (`SSWorldField::probesAt`, own-gap plus graph-adjacent probes, feeding the
> soundscape's SPACE/size verdicts), and the tier A statistic bake per probe
> (8-direction wall profile, sky openness, bounded room flood, Sabine RT60,
> classes, travel-to-outdoors). Tier B ships behind
> `SSWorldFieldAcousticsQuality` = 1: per-probe ray bundles (128 rays, 8
> jittered bounces) against the 4x span mip, fanned as probe batches with
> individually serial-gated store-backs. The V10 ACOUSTICS info view draws
> the probes, the graph, the listener blend and the last thunder's direct
> line against its propagated path. Still open: disk bake (Part 4's
> persistence), source-side reverb wiring, and the lattice-density and
> traced-source-cap measurements the open questions list. This doc
> supersedes the ACOUSTIC bullet sketch in Part 3 of the worldfield doc.

The brief:

- **Occlusion** must be realtime — traced rays, answered the frame the
  question is asked, because sources and the listener both move.
- **Propagation** and **reflection/reverb** come from a grid of acoustic
  probes: per air gap, from just above the gap's floor span (small-avatar ear
  height, ~1.2 m) up to just under the ceiling span above (~0.4 m below it).
  Propagation and reflection carry through *connected* probes; analysed
  reverb is stored per probe and the listener blends between the connected
  probes near it.
- The analysis must be **bakeable** — computed when a tile commits, never per
  frame, so runtime is lookups — and must have a **high-quality tier that
  fans across worker threads** for people who want analysed rather than
  estimated reverb.

## Where the code stands

What the channel builds on, from the code rather than from the sketch:

| Piece | State today | What this design does with it |
|---|---|---|
| Span store | per column, ≤ `SS_WF_MAX_SPANS` (6) solid [bottom, top] spans, 0.25 m columns, gaps ≥ slab threshold | the geometry every trace and every probe placement reads; unchanged |
| Air flood | per-gap OUTDOORS/SHELTERED/INTERIOR labels + graph depth to open sky, one worker job, snapshot in, serial-gated out | probe labels, portal detection and travel-to-outdoors come from it free |
| Wall lattice | `ss_wf_acoustic`: 4 cardinals × fixed 4 m rings × ~8 m cells | subsumed — wall distances become a per-probe figure at the probe's real height, 8 directions; `acousticAt`'s contract is kept and answered from the nearest probe |
| `SSSoundscape::occlusionGain` | heuristic: cover + enclosure pick a constant between 0.6 and 0.22 when the source is beyond the listener's wall distance | replaced by the real trace (Part 3), same signature |
| `scheduleThunder(..., muffle)` | muffle is an input the storm system guesses | muffle and the extra travel delay become outputs of the propagation query |
| Probe cycle fallback | 7 raycasts per 50 ms cycle when the field has no answer | kept, untouched — the channel's staleness convention is already "no current answer → caller keeps its raycasts" |

The design splits into the probe set (what exists where), the graph (what
connects to what), the runtime queries (occlusion, propagation, blending),
and the analysis tiers (what a probe knows and how expensively it learned
it). A pure core (`ssacousticcore.h`, no llmath.h, POD in/out, harness-tested
in the scratch repo) holds placement, linking, the graph solve and the ray
bundle; `ssworldfield.cpp` owns snapshots, scheduling and storage, the same
split every other Atmo core follows.

## Part 1 — the probe set

### Design A — keep the fixed rings, densify them (rejected)

The shipped lattice's shape, with more rings and more directions. Rejected:
fixed rings sample altitudes nothing stands at — a ring inside a floor slab
answers for nobody, a 4 m ring spacing straddles a 2.5 m room so its one ring
sits at whatever height the modulo lands on. The rings were a stand-in for
"where would a listener be", and the span store already knows the real
answer: listeners stand on spans.

### Design B — gap-anchored probes (recommended; the brief's shape)

Probes anchor to the air gaps the store already keeps. Per lattice cell
(existing horizontal resolution: `lat_res` near 8 m, the
`llclamp(res·cell/8, 8, 64)` already in `scheduleFlood`), per air gap of the
cell's anchor column:

- **Ear probe** at `gap_bottom + 1.2 m` — small-avatar ear height above the
  floor span's top. This is where reverb is heard and where sources sit.
- **Ceiling probe** at `gap_top − 0.4 m`, only when the gap is roofed (a
  span stands above it) and tall enough that the two probes are distinct
  (gap height > ~2.4 m; below that one probe at the gap's midpoint serves
  both roles). The ceiling probe is what carries propagation *over* things —
  a courtyard wall, a mezzanine rail, the open top of a stall — and is the
  half of the brief's "to the ceiling of the span above" that fixed rings
  could never place reliably.
- **Intermediate probes** every ~4 m between ear and ceiling for tall gaps
  (atria, canyons, stairwells), so a vertical path is never a single 20 m
  hop through a wall the graph cannot see.
- **The sky-open top gap** gets the ear probe only. Outdoors air needs no
  vertical probe stack — its reverb is the outdoor bed, its openness is the
  coverage answer — but the ear probe must exist so outdoor sources and
  listeners have a graph entry point.

Probes are placed from the gap list alone, **ignoring the flood's labels**:
a sealed room (AIR_INTERIOR, or air the flood never reached) still gets
probes, because a listener teleporting into it still deserves its reverb.
Labels ride along as data, not as placement gates.

Per probe, stored: position (lattice cell + z), gap index, air label,
travel-to-outdoors (metres, from the classification's covered distance (DECIMETRES since 2026-09-11, so metres = depth x 0.1 - it used to be graph hops x cell size and so moved with SSWorldFieldCell)), and the
analysis block of Part 4. Order-of-magnitude count: 32² lattice cells × ~2
gaps × ~2 probes ≈ **4k probes per region**, ~64 B each ≈ 256 KB.

**Adversarial review.**

- *The anchor column is one column out of ~1024 in an 8 m lattice cell.* A
  centre column landing inside a pillar poisons the whole cell's probe stack
  (the shipped lattice has this today). Mitigation: spiral out from the
  centre over a handful of candidate columns and take the first whose gap
  list has air at ear height; record a "no representative column" cell as
  probe-less so its neighbours' interpolation covers it, rather than storing
  a pillar's answer.
- *8 m probes cannot resolve a doorway.* True and accepted, same as the
  worldfield doc's open question. The graph's links (Part 2) are validated
  by traced rays at store resolution, so a doorway *connects* correctly even
  when no probe stands in it; what suffers is reverb transition sharpness
  walking through the door, and the listener-side enclosure spectrum already
  covers that (it is per-column). Densifying the lattice is a dial, not a
  redesign.
- *Gap churn on edits.* A re-peel that splits or merges a gap renumbers
  probes; every cached graph solve and listener blend must gate on the same
  serial the lattice already gates on. This is the existing staleness
  discipline, just with more cached layers riding it (Part 3's caches all
  carry the serial).

## Part 2 — the probe graph

Edges, built with the probes in the same worker job:

- **Vertical links**: probes of the same gap in the same cell — connected by
  construction, cost = vertical distance.
- **Horizontal links**: for each 4-neighbour lattice cell pair, candidate
  links between probes whose gaps **vertically overlap by ≥ 0.7 m** (the
  crouch-clearance figure; below that, air technically connects but sound
  carrying through it should pay the aperture penalty, not travel freely).
  Each candidate is **validated by a span-store ray** from probe to probe —
  at 8 m spacing a wall thinner than a lattice cell is invisible to gap
  overlap alone, and the ray is what keeps the graph from teleporting sound
  through it. A blocked ray drops the link; the pair may still connect via
  another gap's probes or a longer path, which is the point of a graph.
- **Per link**: length, and an **aperture factor** — the vertical overlap
  height clamped against a horizontal clearance sample at the link's
  midpoint (nearest wall distance perpendicular to the link, from the probe
  analysis). A door-sized aperture passes nearly full energy; a mail-slot
  overlap is a heavy penalty.
- **Portal edges**: a link whose endpoints' labels differ across the
  OUTDOORS boundary (OUTDOORS ↔ SHELTERED/INTERIOR) is flagged as a portal —
  the edge thunder and the muffle question care about, free from the labels.

~4k nodes × ~6–10 edges each: the graph is a flat array of a few tens of
kilobytes, rebuilt whole on every flood (it is cheaper than the flood
itself).

**Adversarial review.**

- *Diagonal links.* Without them, a path across an open plaza zig-zags and
  overestimates length ~15–40 % depending on heading, which pollutes thunder
  delay. Options: 8-neighbour links (double the validation rays) or keep
  4-neighbour and post-smooth path length (string-pulling over the found
  path with the same traced-ray test). Recommendation: 4-neighbour + string
  pulling — validation rays are the build's dominant cost and path
  straightening is a per-solve triviality. Open question if plaza-scale
  error still shows.
- *The validation ray is centre-to-centre.* Two probes may be mutually
  visible around a pillar's edge yet the centre ray hits it; the link drops
  and the path detours a cell. Acceptable — the error bounds at one lattice
  cell and errs toward too much occlusion, the fail-safe direction for
  audio. Not worth multi-ray validation at bake time.

## Part 3 — runtime queries

### The occlusion trace (realtime, the brief's first ask)

`SSWorldField::traceSolid(a, b, F32& solid_m, S32& crossings)` — a 2D DDA
over the columns the segment crosses; per column, intersect the segment's
z-interval within that column against its span intervals; accumulate solid
metres and body crossings. Exact against the store, no scene raycast, main
thread, ~4 columns per metre at the 0.25 m cell.

- `SSSoundscape::occlusionGain` keeps its signature and becomes:
  transmission = `exp2(−solid_m / T)` (T the half-loss thickness, one
  material until span flags grow absorption classes), floored by the
  diffracted-path energy when a propagation solve exists for that source
  (below) — sound through a wall or around it, whichever survives better.
- **Budget.** A 64 m trace is ~256 column visits — trivial one at a time,
  not trivial × every source × every frame. The soundscape's existing cycle
  discipline applies: traces refresh on the 50 ms probe cycle, nearest/
  loudest sources first under a per-cycle cap, cached per source with the
  same 0.2 m move threshold the probe cycle already uses. A source beyond
  the cap keeps last cycle's figure.

### Propagation (per-source graph solve)

For each *traced source* (same capped set): snap the source to its gap's
probes in its lattice cell, run Dijkstra over the probe graph, edge cost =
length × aperture penalty (+ a fixed per-portal loss). Cached per source;
invalidated when the source or listener moves a cell, or the serial moves.
4k nodes solve in well under a millisecond — but the solve runs on the
worker queue anyway (it reads only the baked graph, which is immutable
between floods), so a burst of new sources never lands on a frame.

The listener reads its own connected probes (below) and gets, per source:

- **path length** — drives thunder/shockwave delay (`scheduleThunder`'s
  travel time uses path metres, not euclidean) and distance attenuation for
  heavily indirect paths;
- **path-vs-direct ratio and portal count** — becomes the **muffle** figure
  `scheduleThunder` currently receives as a guess;
- **arrival direction** — the direction from the listener's probe to its
  Dijkstra parent: sound entering through a doorway is *rendered from the
  doorway*. Applied to soundscape-owned emitters (thunder, beds, impact
  loops); exposed as an API for anything else.

### The listener blend (the brief's third ask)

The listener's reverb state is a blend over its **connected** probes only:
the probes of the listener's own gap in its cell plus graph-adjacent probes,
inverse-distance weighted — never a raw trilinear tap over the lattice,
because the nearest probe through a wall is exactly the one that must not
contribute (connectivity-aware interpolation, the standard baked-probe
failure mode). Blended figures: RT60, reverb level, room size class, wall
profile, sky openness. The result feeds the existing `ESpace`/`ESize`
machinery and the loop beds unchanged, the same consume-without-rewiring
migration `enclosureAt` already made; the raycast classification stays as
the fallback whenever `probesAt` returns false.

**Adversarial review.**

- *An elevated outdoor source* (a bird, a hovering emitter above the
  rooftops) snaps to the top gap's ear probe metres below it. Fine — the
  graph only matters when the direct trace is occluded, and an airborne
  source is almost never occluded; when it is (behind a tower), the top-gap
  probes route around it acceptably.
- *Dijkstra per source sounds expensive; it is not the risk.* The risk is
  cache invalidation thrash from a fast-moving source crossing cells every
  frame. The move threshold is per-cell, the solve is async, and a stale
  path for 50–100 ms is inaudible; state it and stop worrying.
- *Reverb send vs. source reverb.* A source in a cathedral heard from
  outside through the door should carry the cathedral's tail, not the
  porch's. Full source-side reverb is a per-source blend at the *source's*
  probes — available from the same solve, worth wiring for thunder and loud
  one-shots, overkill for footsteps. Dial, not architecture.

## Part 4 — analysis tiers and the bake

Both tiers run at tile commit, never per frame — that is the "bake" in this
design: the runtime only ever reads. Recompute rides the flood's exact
gates (snapshot in, geometry-serial check before store-back).

### Tier A — statistic bake (default when the channel is claimed)

In the flood's worker job, per probe, from the snapshot:

- **wall profile**: 8 horizontal directions at the probe's real z (subsumes
  the 4-cardinal lattice; `acousticAt` answers from the nearest probe);
- **sky openness**: from the gap label and the column stack above;
- **room volume / surface area**: bounded flood over the connected gap cells
  (radius-capped ~32 m), air-cell count × cell area × gap height, boundary
  count for area;
- **RT60**: Sabine — `0.161·V/(α·S)` with a default absorption per span-flag
  class (terrain soft, water hard, solid mid) — classified into
  `ESpace`/`ESize`;
- **travel-to-outdoors**: the flood's gap depth, already computed.

Cost: same order as the shipped lattice walk. This tier alone retires the
soundscape's classification raycasts.

### Tier B — stochastic analysis (opt-in quality, the multithreaded ask)

Per probe, a ray bundle against the span snapshot: K rays (default 128,
dial to 1024), up to 8 bounces, specular-ish reflection off span faces.
Collected per probe:

- **mean free path** → `4V/S` without Sabine's shoebox assumption — the
  honest V and S for L-shaped halls, colonnades, broken interiors;
- **decay curve** over bounces × per-bounce absorption → RT60 that reflects
  actual geometry, plus an **echo density** figure (early reflection
  sparseness: canyon slapback vs. room wash);
- **first-arrival profile** → dominant early-reflection delay and direction
  (the slap of a facing wall);
- **directional openness** (energy escaping to sky per direction octant).

The shape is what earns the worker pool: every probe's bundle is
independent, so tier B fans probe *batches* across the general queue as
separate jobs — the flood stays the one-at-a-time job it is, tier B is many
small ones behind it, each store-back serial-gated individually. Raw cost
honesty: 4k probes × 128 rays × 8 bounces × ~256 DDA steps ≈ 10⁹ column
tests per region — seconds of a core, tens of milliseconds of a pool for a
burst. Mitigations, in order: trace tier B against a 4×-coarsened span mip
(1 m columns — reverb statistics do not need 0.25 m walls), skip OUTDOORS
probes (tier A's sky answer is already right outdoors), and stage batches
behind the flood at a jobs-per-frame budget. With the mip and the outdoor
skip the realistic figure is ~10⁸ steps spread across a pool over a few
frames, per commit, opt-in.

### Persistence

Baking to disk across sessions is *not* in this design's first landing: the
geometry serial is a session counter, not a content identity, so a disk key
needs a content hash of the span store (the region-object-cache work is the
precedent and the likely shared machinery). Recorded as an open question;
the in-session bake already removes all steady-state runtime cost, which is
the brief's actual ask.

**Adversarial review.**

- *Specular bounces off axis-aligned span faces.* The store has no surface
  normals beyond up/down/column-side, so every bounce is axis-aligned and
  RT60 in a domed or faceted hall inherits raster bias. Acceptable for a
  parameter bake (Project Acoustics itself bakes on voxels); the fix, if
  ever needed, is jittered bounce directions (diffuse-ish), not real
  normals.
- *Absorption is invented.* One default per flag class is a guess; nothing
  in SL content says "curtain" vs "marble". True and permanent — no viewer
  data source exists. The dial is per-class constants; the design just must
  not pretend otherwise.
- *Tier B invalidation churn.* An edit-heavy region re-bakes 4k probes per
  settle. The dirty-rect machinery bounds it: probes whose lattice cell
  intersects the dirty rect re-analyse, the rest keep their block — the
  worldfield doc's "re-bake dirty probes only" line, now with a concrete
  unit (the probe batch).

## Part 5 — migration

1. **`traceSolid` + real `occlusionGain`.** No probes needed, immediate
   payoff, thunder muffle becomes an output. Independently shippable and
   testable against the debug overlay (draw the trace).
2. **Probe set + tier A bake** replace the ring lattice inside the flood
   job; `acousticAt` keeps its contract; the soundscape's SPACE/size
   classification reads probes with the raycast fallback intact.
3. **Graph + per-source propagation**: links at bake, Dijkstra at runtime,
   listener blend, thunder delay/muffle/direction from the solve.
4. **Tier B** behind `SSWorldFieldAcousticsQuality`, batch-fanned, mip-traced.
5. **Disk bake** — only if profiling says commit-time tier A/B costs matter
   in practice, and only after a content-hash key exists (ROC machinery).

Every step keeps the raycast fallback and the serial gates, so every
intermediate state degrades to today's behaviour, never below it.

## Costs

| Item | Estimate | Notes |
|---|---|---|
| Probe store | ~256 KB/region | ~4k probes × 64 B |
| Graph | ~150 KB/region | ~4k nodes × 6–10 links × 4 B |
| Tier A bake | ≈ shipped lattice walk | same job, same snapshot |
| Tier B bake | ~10⁸ DDA steps/region, pooled, opt-in | 4× span mip + outdoor skip; batches behind the flood |
| Occlusion trace | ~256 column visits / 64 m ray | per traced source per 50 ms cycle, capped |
| Dijkstra solve | < 1 ms / source | async, cached, cell-granular invalidation |
| Listener blend | a handful of loads per cycle | the "very small lookups" ask |

## Open questions

- **Lattice density vs. doorway fidelity** — inherited from the worldfield
  doc, unchanged: 8 m + connectivity-aware blending may be enough because
  the enclosure spectrum handles the walk-through-the-door transition;
  measure in real builds before densifying.
- **Diagonal links or string-pulling** — recommendation is string-pulling;
  revisit if plaza-scale thunder delay still reads wrong.
- **Absorption classes** — how many flag-derived classes are worth having
  when all constants are invented anyway.
- **Source-side reverb** — wire for thunder only, or for all traced
  sources? Cost is one extra blend per source; the question is audibility.
- **Traced-source cap** — how many sources per 50 ms cycle before the trace
  budget shows; needs the usual busy-region measurement.
- **Disk bake keying** — content hash over the span store; shared with the
  region-object-cache work or bespoke.

## Research notes

- **Project Acoustics** (Microsoft) — probe bake + runtime interpolation +
  portaling/obstruction split; this channel is the statistic/geometric
  version without the wave solver. Its documented failure modes
  (through-wall interpolation, probe placement in dead space) are the ones
  Parts 1 and 3 defend against.
- **Steam Audio** — baked static propagation, real-time direct-path
  occlusion as a separate cheap query: the same direct/diffracted split as
  Part 3's combination rule.
- **Thief-style portal propagation** — path-length and portal-count driven
  muffle/direction over a room graph; the probe graph is the same idea with
  rooms discovered by the flood instead of authored.
- **Sabine / Eyring, mean free path 4V/S** (Kosten) — tier A vs. tier B's
  statistical reverb: Sabine for the estimate, mean-free-path from traced
  rays when the shoebox assumption breaks.
- **String pulling / funnel** — path-length correction over grid-graph
  Dijkstra, standard in navmesh land, reused for delay accuracy.
