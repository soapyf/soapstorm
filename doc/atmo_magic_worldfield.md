# Atmo Magic: the shared world field (second proposal)

> **Implementation status (2026-09-01):** the capture service (`ssworldfield.h/cpp`,
> `SSWorldField`) is the sidecar of migration steps 1-2 and 4: staged band-sliced
> capture, the column store, `buildSurfaceGrid` with rain shadow's exact contract,
> the interest/channel registry, `coverageAt`/`surfaceTop`/`coverageDetail`,
> the dirty-rect re-peel path, and the async readback. The wet field reads it
> behind `SSWorldFieldSurfaceTop` (default off), and the soundscape's cover and
> burial come from the COVERAGE channel behind `SSWorldFieldCoverage` (default
> off). The air-connectivity flood fill (labels + occlusion depth) runs on the
> General worker queue per committed tile, snapshot-gated by geometry serial.
> The DRAINAGE_NETWORK core (priority-flood depression fill + pool mask + D8 on
> the filled surface, `buildDrainage`) ships as the surface field's pool source
> under the same `SSWorldFieldSurfaceTop` switch. A world field debug overlay
> (`RENDER_DEBUG_WORLD_FIELD`, the Atmo Magic Debug floater) shows
> the band surfaces, the air labels with occlusion depth, and the drainage
> topology. Still unbuilt: the wind solve's interior-skip (part of step 3; gated
> on multi-peel spans - see the architecture note at Part 3, SOLID_VOLUME_3D),
> wind capture absorption, WALKABLE (7), ACOUSTIC (8) and Design H (6).

This is a design, not a build log. The implementation status block above says
what exists. It supersedes
`doc/archive/atmo_magic_worldfield.md` (archived 2026-08-26 as an unbuilt
sketch). That sketch was written before the wind flowmap grew its probe carve,
its passage bridge, its multigrid solve, its async build stages and its
underside-forced slice placement, and before the snow architecture fixed the
granular transport and the whiteout layer in place. Those changes are the
ground truth this design starts from, and several of them change the answer.

The brief for this round, on top of the original one:

- Combine the capture pipelines into one capture of the world that every
  system reads, rather than three renders and a raycast bundle each asking
  "what is here".
- Improve **performance, detail and accuracy** together, not one at the cost
  of the others.
- The data should be good enough to build a **navmesh for pathfinding** from.
- **Runoff and drainage** should place and route water more accurately, across
  the whole sim, aware of what is above and below the surface it runs on.
- The data must describe **3D space properly**: underpasses, bridges, arcade
  decks, multi-storey courtyards — without special cases.
- A **better windflow**: occlusion-aware, skipping enclosed interiors instead
  of solving them, but still handling a wind that enters a large cave opening.
- **Very small lookups** for the common questions — indoor vs outdoor,
  covered vs not — at call-site cost near a table read.
- Enough structure for a **soundscape** system to look up propagation,
  occlusion and reverb character instead of raycasting, and to place
  shockwaves and thunder travel without raycasting either.
- **Partial updates.** A prim edit patches the affected area — capture,
  channels and derived data — never a whole-region rebuild.

## Where the code stands today

Five systems ask the scene "what is here and what can see what". The current
state, from the code (the archived docs predate several of these):

| System | Capture today | Cost | Shape it keeps |
|---|---|---|---|
| `SSRainShadowMap` | one ortho depth pass along the fall direction (1024², camera band +80/−160 m, avatars masked, one tile per region, 4 cached) | one ortho render + readback every 0.25 s at most | `SurfaceGrid`: 128² region-anchored heights + `SURF_*` flags, geometry serial |
| `SSWindFlowMap` | top-down ortho + 4 oblique probes (30°, up to 1536²) → hidden-underside evidence → adaptive slicing (histogram quantiles, underside peaks, track boundaries) → GPU voxelise + carve → CPU passage bridge → **multigrid** V-cycle pressure solve (≤5 levels, 128 Jacobi iters) → project + lee shelter → async readback on a worker | staged across frames, one tile at a time | per-region tile + 64 m margin, 4 m cells, 2–8 slabs; CPU-resident `mSolid`/`mFlow`/`mSurfaceTop` |
| `SSSurfaceField` | none — derives slope/edge/pool geometry from `SSRainShadowMap`'s grid (retrace gated on the geometry serial), integrates wet/snow/puddle per 2 m cell at a 0.25 s tick, sheds from edge cells through reservoirs | CPU, budgeted | field arrays + two GPU windows (field, flow) |
| `SSGranularTransport` *(planned, `atmo_magic_snow_architecture.md`)* | none — steps the surface field's snow channel: lift, creep, deposit, spill | CPU inside the field tick | reads the wind ground window; never touches geometry |
| `SSSoundscape` | 3 tilted up-rays + 4 cardinal side-rays, raycast every 50 ms cycle when the camera has moved 0.2 m or a second has passed; burial depth borrowed from `SSWindFlowMap::surfaceAt` | live raycasts, uncached, camera-relative | point classification only |

What has already converged, worth naming because the design should keep it:

- **Region-anchored tiles, region-local horizontal storage, absolute Z.** All
  three capture-shaped systems. Proven across region crossings and varregions.
- **Geometry serials as the retrace gate.** `SSRainShadowMap` bumps a serial on
  `markDirty` only; `SSSurfaceField` retraces on it. Captures that chase the
  camera or the wind do not retrace derived data. This is the partial-update
  mechanism, already working; the field generalises it.
- **Staged async builds.** `SSWindFlowMap::advanceBuild` spreads capture →
  reduce → solve → readback across frames with worker-thread readback and an
  abandon path. Any combined capture must be shaped like this, not like
  `SSRainShadowMap`'s synchronous single tile.
- **Evidence-only probe carving.** The oblique probes can only turn solid into
  open, never the reverse, and only when the probe ray actually hit something.
  The passage bridge then connects carved cells through thin solids along three
  axes. This is the codebase's proven answer to "a vertical capture cannot see
  a vertical surface".
- **Underside evidence.** Probe hits landing more than `HIDDEN_CLEARANCE`
  (1.5 m) below the column top are collected; histogram peaks over
  `UNDERSIDE_MIN_AREA` (24 m²) force slice boundaries at genuine underside
  altitudes (max 4). The capture already knows where under-overhang air starts;
  the shared store should keep that answer, not just wind's slices.

And what still hurts:

- Rain shadow's capture is camera-band-relative and single-layer; wind's is
  tile-static and multi-evidence. The same scene is rendered ortho twice with
  different bounds, and the cheaper of the two answers is the one drainage
  gets.
- `SSSoundscape` spends seven raycasts per cycle to learn what the wind tile's
  solid mask already knows, because that mask is solve-shaped (per-slab) rather
  than query-shaped (per point, any height).
- Drainage is per-edge-cell with a reservoir; it has no catchment ordering, no
  depression filling, and it cannot see a span below the top one — an
  underpass gutter and a road-level puddle field are invisible to it.
- Every consumer of "is this indoors" grows its own answer.
- A tile rebuild is tile-wide: one edited column re-captures, re-solves and
  re-reads back the whole region tile.

## What the store must answer

The questions, so the representation can be picked against them rather than
against the current systems' internals:

1. What is the topmost surface at (x, y) — and the next surface down, and the
   one after that? (precipitation landing, runoff, snow deposit, cover)
2. Is this point outdoors, sheltered, or buried, and by how much? (audio,
   effects, combat)
3. Is the space between two points open air? (occlusion, cover, propagation)
4. Where can an agent walk, and how do floors connect? (pathfinding, parkour,
   movement cost)
5. Where does water on surface X drain to, and where does it pool? (runoff,
   puddles, wet field)
6. Where is air, and which air is connected to open sky/outside? (wind solve
   scope, cave openings, fog, audio)
7. How does sound or a pressure wave travel from A to B, and what does a
   listener at P hear of an event at S? (soundscape, thunder, shockwaves)

Nothing above needs a voxel octree at runtime. Several of them want a dense
grid *briefly*. The next section is about picking the thing that archives well
and materialises the rest cheaply.

## Part 1 — the substrate: four designs

### Design A — column span store (the archived proposal, updated)

The archived proposal's structure, kept and extended: a region-anchored 2D grid
of columns, each column a short list of vertical spans (bottom, top, flags),
plus per-span open-above/open-below bits. One span per column is today's
rain shadow grid; multi-span is the generalisation.

Updates the current code forces on the sketch:

- The peel capture that produces spans must be **region-anchored and
  camera-independent** in its band. Rain shadow's capture band moves with the
  camera (±80/160 m); that is fine for a single surface the camera is near, but
  a shared multi-span store keyed to a moving band would churn every span
  every climb. The wind map already solved this: a tile band chosen from the
  sky track, with a probed ceiling. The shared capture adopts wind's band
  discipline, not rain shadow's.
- The probe carve must be stored **per column, per height**, not per slab: the
  wind solve wants slabs, acoustics wants "any z", and the archived doc's open
  question about probe-carve ownership lands here. A column's span list gains
  the probe verdicts as span-boundary evidence: a probe seeing past a face
  opens the interval it proved open, and the carve is stored as carved-open
  sub-spans rather than as flags on the whole column.
- Underside evidence (today's `mHidden` peaks) becomes first-class: an
  underside hit is a span *bottom* candidate that a vertical peel cannot see.
  Capture stitches them in as span starts, area-bounded exactly as
  `placeSlices` does now.

**Adversarial review.**

- *Columns cannot represent horizontal occlusion.* A probe's carve is not
  column-shaped: "this probe saw past this point" is directional evidence that
  a per-column open/solid bit flattens. Today wind solves this by keeping the
  probe depth images and carving during the GPU init pass. A span store that
  only keeps the post-carve column shape has thrown the directionality away —
  and acoustics and cover queries want exactly that directionality.
- *Everything downstream still wants a dense read.* Wind's stencil solve needs
  a regular grid; drainage needs 8-neighbour surfaces; a navmesh wants
  region-partitioned space. Spans answer vertical queries perfectly and
  horizontal ones only by walking neighbours span-by-span.
- *Multi-storey accuracy.* 2–4 spans per column covers stacked flats; a
  genuinely deep build with irregular floors still blows the budget, and the
  overflow policy (truncate? merge?) silently defines accuracy.

### Design B — sparse voxel hash grid

Store the volume directly: a hash map of occupied 4 m or 8 m bricks, each a
small dense voxel block (OpenVDB-shaped; the structure used by SVOGI and id
Tech 6). True 3D, answers every question in the list, trivially sparse — a
mostly-empty sky region stores almost nothing.

**Adversarial review.**

- *It throws away what the capture gives for free.* The capture is depth
  peels; peels hand back per-pixel surface distances, which assemble into
  span lists for free. Turning them into voxels is extra work, and every
  downstream consumer then flattens back to heights or masks anyway.
- *Memory is honest but not cheap.* 4 m voxels over a 384 m domain at 8 m
  height is ~2.2M cells; a hash grid with per-cell overhead costs more than
  the dense tile it replaces until occupancy is genuinely sparse — and SL
  content is stacked flats, not sparse caves.
- *Nothing downstream wants to walk a tree per query.* Every current consumer
  samples a grid. A sparse structure needs a materialisation step before it is
  useful to any of them, which is design D with worse inputs.
- What survives: the **air-connectivity flood fill** (Part 3, wind) is a
  voxel-shaped operation and is run as a transient over the dense tile, not
  stored.

### Design C — one dense tile, everything from it

Generalise the wind solve's dense grid to be the only store: one
region-anchored 3D texture (say 192×192×8 at 4 m cells), captured, solved,
read back, and read by everyone. No spans, no channels, one shape.

**Adversarial review.**

- *Memory and readback scale with volume, not content.* An RGBA16F 192²×8
  volume is ~2.4 MB GPU and the same read back; fine — but every consumer now
  pays wind's resolution and band choice, and a channel that wants 1 m surface
  detail (drainage, snow) is stuck at 4 m cells or pays a second store anyway.
- *Vertical resolution is the wrong axis to fix at capture time.* Slices are
  content-adaptive (2–8); a store fixed at 8 slabs cannot answer "is this
  point under a roof" at better accuracy than the slab that contains it,
  which is exactly the regression the soundscape's current burial question
  cannot afford.
- *Partial updates mean re-uploading.* A one-prim edit dirties a dense tile;
  the patch unit is a texture region, which is fine for the solve but wrong as
  the only representation of, say, one edited wall's drainage.

### Design D — spans as the archive, dense as the answer (synthesis)

The pattern wind already proved, generalised and made the house rule:

**Capture sparse (spans), solve dense (a materialised slab grid), archive
sparse (columns), throw the dense copy away when the tile goes stale.**

- The **canonical store** is the multi-span column grid from Design A —
  small, region-anchored, partially updatable, the shape the capture
  natively produces.
- Each consumer channel is a **materialised view**, built from the spans on
  demand for the tiles something has an interest in, and dropped when that
  interest ends or the tile's geometry serial moves:
  - the wind solve's dense mask+velocity volume (already exists, unchanged),
  - a dense **air mask** for the solve, now connectivity-aware (below),
  - a **SDF/coverage field** for cheap indoor/outdoor and burial queries,
  - the drainage network, the navmesh tiles, the acoustic probe lattice.
- Directionality the columns cannot hold (probe evidence) stays with the
  capture, which re-runs the carve when materialising any 3D channel — the
  probe depth images are the source, the spans are the archive of the *answer*
  plus enough evidence (per-span openings) to answer vertical queries without
  re-running probes.

This keeps every good property the current systems independently proved:
region-anchored tiles, geometry serials, staged async builds, dense-solve-
and-throw-away. What it changes is who owns the capture: one service, one
store, five readers.

## Part 2 — the capture service

### Design E — one peel capture (recommended core)

A single staged, async, region-tiled capture service that all channels claim
data from. It is the wind build's stage machine, generalised:

1. **Vertical depth peels.** One ortho top-down pass, then peels that exclude
   what previous passes resolved, up to `SSWorldFieldMaxSpans` (default 4).
   Each peel is an ortho render + depth readback — machinery that already
   exists twice (`renderShadow` with a custom camera, twice in this
   codebase today). Result: per-column span lists, region-anchored, absolute
   Z, flags carried per span (solid/water/terrain/fallback).
2. **Oblique probes** — the existing four 30° cardinal probes, unchanged in
   mechanism, run against the shared store: evidence-only carving, one-sided
   (open-ward), hit-required. `HIDDEN_CLEARANCE`-qualified hits are underside
   evidence for span boundaries, not just carve votes.
3. **Connectivity pass** (new, CPU, per tile): label the open cells of the
   dense mask by flood fill from the tile boundary and from the sky. Air that
   reaches a horizontal tile edge or the sky is *outside-connected*; air that
   reaches neither but is bounded by spans on all sides is *interior* (a room,
   a cave); air connected to nothing (sealed) is *void*. This is the pass that
   turns the bridge hack into an answer: a bridge deck, an underpass and a
   cave mouth are all just openings the flood can walk through, at whatever
   slice resolution the store runs at.
4. **Commit** with a geometry serial, per-column dirty tracking, and the
   scissored re-peel path for dirty rects.

Everything else about the capture discipline stays as the current code proves
it: region + margin tiling, absolute Z, staged across frames, one tile in
build at a time, worker readback, settle-then-rebuild on edits, rebuild
triggers on AABB-tested geometry, ambient wind change, and sky-track change
(only for consumers whose band depends on it — the capture itself becomes
track-independent: it captures the whole band the spans occupy, and bands
become a per-consumer view, which retires the "rebuild when the camera
changes sky track" rebuild reason for the *store*; only the wind solve
re-slices).

**Adversarial review.**

- *Peel cost.* Each span level is a full ortho pass. Mitigation is the
  interest system: a region with only drainage enabled runs the single-pass
  peel rain shadow runs today; multi-span is paid only when a channel that
  needs depth (wind, coverage, navmesh, acoustics) holds an interest there.
- *Vertical surfaces between the first and second span.* Peels see tops;
  walls come from the probe carve, as today. The known failure (a probe-blind
  passage stays solid) is inherited unchanged, and the pass that fixes it
  (connectivity flood) also bounds its damage: a mislabelled solid pocket is
  interior, not outside-connected, so wind skips it and acoustics never
  places a probe in it.
- *LOD and asset timing.* Unchanged and honestly limited: the capture renders
  what is resident. Re-peel on sculpt/texture arrival is a rebuild trigger
  the shared service can finally own once, per tile, instead of each system
  guessing.

### Design F — physics-sweep capture (rejected, recorded)

Build the store from the physics representation (convex decomposition AABB
sweeps) instead of renders. Rejected for the reason the archived doc gives and
the current code confirms: the capture must see what a drop, an ear and the
wind see — including non-physical megaprims (1024 m sim-surrounds), alpha with
depth writes, and terrain — which is a *render* property. Physics AABBs stay
where they already are: in the dirty-detection path (`markDirty`'s
AABB testing), not in the capture.

### Design G — two-tier capture (recommended complement)

One region-resolution tier is the wrong compromise at varregion scale and for
detail. Run the peel at two tiers:

- **Near tier** — the camera's region plus its overlap margins, full cell
  size (4 m; surface channels resample to 1–2 m), multi-span.
- **Far tier** — regions within reach but off-camera, quarter resolution
  (16 m cells), single or two spans, enough for neighbour margins, distant
  shadow, coarse acoustic and "is there structure there" queries.

Region crossings swap tiers; a far build that comes into camera range
re-peels at near detail. The existing margin-overlap discipline keeps the
seam where it already is — a documented soft seam, same as today.

### Design H — worker-pool analytic refinement over copied geometry (recommended precision layer)

Everything above is a raster answer: it is only ever as precise as a texel of
an ortho depth capture, and that ceiling is load-bearing for the questions it
answers well — open/solid, indoor/outdoor, is-there-structure-there all
tolerate a few centimetres of fuzz because nothing renders exactly *at* the
boundary. An eave lip does not have that luxury: a stream is a ribbon a few
tens of centimetres wide, anchored at a point `SSRunoff::refineEdge` finds by
walking the *captured depth map* outward from the coarse trace cell until it
falls off the upper surface — which finds where the **capture** stopped
resolving a surface, not where the **roof** actually ends. Two things stack
against it right at the point it matters most: a texel straddling a real
edge holds one arbitrary sample, not a blend, and a fall direction that
meets the lip at a grazing angle foreshortens exactly the geometry the walk
is trying to resolve. The visible result is a drip or a stream anchored a
few centimetres on the wrong side of the tile it should be leaving from —
reads as clipping through the roof, because it is.

**The question underneath "where does the surface end" is actually "which
object ends here, and what shape is it" — and asking that first changes how
much of this needs to be expensive at all.** An eave is, definitionally, the
edge of *something* — the trace already knows a solid column stops here,
which means there is a specific prim or mesh whose own boundary the eave
line traces. Most roofs are not arbitrary triangle soup: a gabled roof is
two prisms, a hipped one a handful of cut boxes, a dome a sculpt or a
sphere-cut primitive. Every one of the built-in primitive shapes
(`LLVolumeParams` — box, cylinder, prism, sphere, torus, tube, ring, and
their path/profile cuts) has a **closed-form boundary**: given the object's
transform and its params, the exact XY extent at any Z is a handful of trig
and vector operations, not a search. There is nothing to refine for that
case — there is an answer to compute.

That reframes the whole design into **identify, then answer**, cheapest path
first:

1. **Identify the owning object and face.** The viewer already has the tool
   for this — `LLPickInfo`/`renderForSelect`'s colour-encoded object-ID
   buffer, the same technique mouse-picking uses to turn a screen pixel back
   into an `LLViewerObject*` and a face index. Rendered as a second target
   alongside the eave capture's own ortho depth pass (one extra attachment
   on a render that is already happening, not a second render), it turns
   every lip texel from "a height" into "a height, and the specific
   object and face that produced it."
2. **Primitive shape → closed form, on the main thread, no snapshot at
   all.** If that object's volume is one of the parametric types and its
   cuts aren't warped past where the closed form is worth deriving, the
   exact edge at the eave's height comes straight out of `LLVolumeParams`
   and the object's transform — a handful of vector ops per lip point,
   cheap enough to run inline in the trace itself. No copy, no worker, no
   race with live scene state, because nothing here touches the live object
   longer than the single read of its already-thread-unsafe params, done on
   the main thread where that's fine. This is very likely most of the
   answer: a warehouse roof, a gabled cottage, a walled courtyard — all
   primitives, all exact for free.
3. **Mesh, sculpt, or a cut past the closed form's reach → the worker pool.**
   Only here does anything need to be genuinely expensive, and only here is
   a triangle-level query actually earning its keep rather than solving a
   problem step 2 already solved for free. This is where the rest of this
   design applies, unchanged:
   - **Snapshot, not reference.** Once `SSAtmoMagic::settleEdits`' existing
     debounce (~3 s since last churn) clears a region, the mesh/sculpt
     objects step 2 couldn't answer for — a small, trace-identified subset,
     not every prim in the region — have their transform and a decimated
     triangle set (or physics hull, where one exists; the render mesh
     otherwise, LOD-matched to what the capture itself used) copied into a
     small, immutable, thread-safe snapshot. Copied, exactly the way the
     wind build already copies `mWindDir` for the same reason, scaled from
     one `LLVector3` to a per-object triangle buffer — `LLDrawable`/
     `LLVolume` are mutated by the main thread's update and rebuild
     pipeline, so a worker touching them live is the bug this avoids.
   - **A worker pool, not a worker.** The wind build's `postWorker` is
     deliberately one job at a time, because its stages share one set of
     scratch buffers and the state machine's whole safety argument rests on
     there being only one build in flight. Lip refinement has the opposite
     shape: every lip point's answer is independent of every other's, which
     is what makes it worth fanning across several workers rather than
     queuing them one behind another. Each job carries its own copy of
     whatever geometry its lip point is near and needs no scratch shared
     with any other job.
   - **The query.** A short ray-vs-triangle sweep against the object's own
     copied mesh — no raster resolution to hit a ceiling against, because
     the answer comes from the actual triangles the roof is made of. This
     is the same exact answer `lineSegmentIntersectWorldGeometry` gives
     live (and which `SSSoundscape`'s probes already call, at a scale — 7
     rays every 50 ms — that is why it never got used more widely); the
     snapshot and the pool are what make the same precision affordable at
     the hundreds of lip points a busy region's trace produces.
4. **Where it lands.** Either path replaces `refineEdge`'s raster walk for
   **eave lips specifically** — the one query in the whole store where the
   answer is drawn close enough to see the error. Everything else
   `SURFACE_TOP` answers (rain landing, coverage, the bulk of the drainage
   trace) stays on the raster path; sub-centimetre accuracy is wasted on a
   question nothing renders flush against. Graceful degradation matters at
   every tier: no ID-buffer hit, a shape the closed form can't cover, or a
   mesh snapshot not yet ready all fall back to today's raster answer rather
   than stalling the run waiting for a better one.

This is a companion to Designs E–G, not a substitute: the peel-and-flood
substrate stays the source of *what exists and where*; this is a narrow,
opt-in precision pass over the small set of points the substrate's own
resolution isn't good enough for, and most of that pass should turn out to
be step 2 — cheap, exact, and synchronous — with the worker pool doing real
work only on the minority of eaves that are actually mesh. The same
identify-then-answer shape is the natural tool for anything else in the
store that turns out to want better-than-texel accuracy later (a parkour
ledge lip, a close-range cover edge), without it needing to be built for
those yet.

**Adversarial review.**

- *Copy cost and staleness.* A triangle buffer per mesh lip-owning object is more
  to copy than one `LLVector3`, and it has to be re-copied whenever that
  object's geometry serial moves — bounded by the settle debounce already
  gating everything else here, but worth budgeting (object count, triangle
  count per snapshot) before assuming it's free.
- *Not every object has a cheap mesh to copy.* Sculpts and mesh objects at a
  low resident LOD, or a physics shape simplified past what the visual edge
  needs, both degrade the refinement's own accuracy — the raster fallback is
  not just a safety net for missing data, it's the answer whenever the copy
  itself is coarser than the capture.
- *Worker pool sizing is a scheduling question, not a design one.* How many
  concurrent jobs, and against which general work queue, is the same
  trade-off the viewer already makes everywhere else it fans work out; it
  does not need a bespoke answer here.
- *The ID buffer has the same edge problem one level up.* A texel straddling
  two objects' boundary still picks one ID, arbitrarily — which is fine here
  in a way it isn't for depth: getting the *object* wrong at one texel means
  falling back to the mesh path (or the raster answer) for that one lip
  rather than computing a confidently wrong closed-form edge, since a
  misidentified object's params describe the wrong shape entirely rather
  than a slightly-off one.
- *Not every primitive cut has a clean closed form.* A simple box or prism
  does; a heavily twisted, tapered, and profile-cut tube approaches "just
  raycast it" territory anyway. The dividing line between "derive the
  closed form" and "treat it as the mesh case" is a real decision, not a
  given — and getting it wrong only costs a slower answer, never a wrong
  one, if the mesh path is always the fallback.

## Part 3 — channels

The column store is cheap and always on when Atmo Magic is on. Everything
below is materialised lazily, per (region, channel), only while a consumer
holds an interest handle, and discarded on last release or serial change.

| Channel | Built from | Consumers today |
|---|---|---|
| `SURFACE_TOP` | top span per column + flags | precipitation landing, wet/snow/puddle field, runoff, snow transport |
| `SOLID_VOLUME_3D` | spans + probe carve + connectivity | wind solve, any 3D mask query |
| `COVERAGE` | span stack + connectivity | indoor/outdoor, burial, shelter; audio, effects, combat |
| `DRAINAGE_NETWORK` | per-span priority-flood + flow routing | runoff, puddles, streams, surface field feeds |
| `WALKABLE` | span-top filters (slope/step/height clearance) | pathfinding/navmesh, parkour, footfall |
| `ACOUSTIC` | probe lattice over `SOLID_VOLUME_3D` air + `COVERAGE` | soundscape beds, reverb, occlusion, thunder/shockwaves |

### SURFACE_TOP

Today's `SurfaceGrid` verbatim — one span's own fields — so `SSSurfaceField`,
the runoff shed, the wet pass, the granular transport and both GPU windows are
untouched consumers with unchanged contracts. The only change is upstream:
the grid is built from the shared store's top spans instead of a private
capture, and `refineEdge`/`resolveColumn` become reads of the same store at
full capture resolution.

### SOLID_VOLUME_3D and the sparse air solve

> **Architecture note (2026-09-01) - what gates the interior-skip.** The shipped
> capture stores ONE surface per band per column. Air under that surface - a
> room, an underpass, a stilt space - shares its band-cell with the surface and
> reads SOLID, so the connectivity flood can only see air in bands the surface
> never touched. Consequences, all verified against the code rather than the
> sketch:
>
> - The flood's INTERIOR labels fire only for sealed air with a full band of
>   headroom (a warehouse mezzanine, a roofed atrium spanning 16 m+ of open
>   band above its floor). Courtyards, underpasses and single-storey rooms are
>   outside the store's reach entirely.
> - Feeding those labels into the wind solve is therefore a no-op for sealed
>   halls (the top-down mask already says solid, and probes cannot see what
>   their rays cannot reach) and actively harmful wherever the carve
>   legitimately opened a space the coarse band labels sealed (a gateway hall,
>   a stilt floor - connected by construction, since a probe saw in). The
>   carve already implements "skip what is not outside-seen"; trustable
>   interior-skip needs the multi-peel span store first.
> - The preconditions for the interior-skip and per-span drainage are the same:
>   the band-sliced capture gains a second peel per band (per-column spans
>   within each band), and `mBandTop`/`mBandFlags` become span lists. Until
>   then, `buildDrainage` runs over the landing surface only - which is the
>   level the surface field actually consumes, so step 5's core ships now and
>   its per-span generalisation lands with the peel.

The wind solve keeps its shape (voxel init → multigrid pressure projection →
project + shelter → readback), with two changes the shared store makes possible:

- **Solve the outside-connected air, not the volume.** The connectivity pass
  labels air cells; the solve seeds and iterates only cells marked
  outside-connected (a compute dispatch masked by label, the same way the
  solid mask already gates cells). Enclosed interiors — buildings' rooms,
  sealed boxes — are *skipped*, not solved: their ambient is the ambient of
  their slab, exposure 0, no Jacobi work. This is the "sparse point cloud"
  ask in its practical form: a label field, not a point cloud, because the
  labels come free from the same flood fill the bridges use.
- **Cave and underpass openings work by construction.** Openings are
  connectivity, not carve: a cave mouth wider than a cell connects the cave's
  air to the outside flood, and the solve's existing boundary handling (open
  at edges, walls at solids, zero-gradient across solid faces) does the rest.
  The current bridge pass (gap-bridging along three axes) remains as a
  repair stage for the sub-cell gaps the 4 m mask always has, but it no
  longer has to invent passages the spans already know about.
- **Occlusion as a first-class output.** Exposure exists today; add a per-cell
  *occlusion depth* (graph distance from outside-connected air) — one flood
  fill, stored in the mask's spare channel — which is the number every
  "how enclosed is this point" consumer actually wants, including audio.
- The band/slice machinery, the pressure solve, the lee shelter and the gust
  layer are untouched. The gust field stays a frozen-turbulence modulation at
  sample time; no one re-solves for gusts.

### COVERAGE

Per column, per span: `open_above`, `open_below`, and the burial depth of
everything beneath the lowest transitively sky-open span. Answers:

- `isIndoor(pos)` — is the point under a span without open-above?
- `burialDepth(pos)` — floor-count-aware, from the span stack (today's
  single subtraction against the wind tile's column top cannot tell one
  storey from three).
- `shelter(pos)` — the existing exposure scalar, now available everywhere,
  not only inside a solved wind tile.
- `coverAt(pos, height)` — solid between the point and the sky at a given
  height; the crouch-cover query combat wants, from the same stack walk.

This channel is what retires `SSSoundscape`'s seven live raycasts (below).

### DRAINAGE_NETWORK

The current shed is per-edge-cell with a reservoir, fed by a capture slope and
a delivery correction. The shared store makes it a proper hydrology pass at
negligible extra cost, on every span level that has a surface:

1. **Priority-flood depression filling** (Barnes et al. 2014 — O(n), one
   pass with a heap): fill each span surface's sinks so every cell has a
   descent path; unfilled spills become the **pool mask** (this generalises
   today's local `mPool` dips-check and the puddle mask into actual
   catchments).
2. **D8/D∞ flow directions** on the filled surface, per span level. The
   existing eave rule (a discontinuity steeper than ~63° and ≥0.75 m ends the
   surface) stays; it is the same trick, now applied with the span above as
   the thing the water falls *from* — an eave is a step down *in Z within one
   column or between columns at the same span level*, which multi-span makes
   unambiguous for the first time.
3. **Flow accumulation in descending-height order** (already the shape of the
   current trace) yields catchment per cell and per edge; eaves, pool rims
   and water-plane cells terminate. Runs (flood-filled edge groups, the
   archived runoff doc's shape) come back as the grouping layer for streams
   when the per-lip cursor model runs out of fidelity — the field's
   reservoir/store math is unchanged either way.
4. **The wet/puddle/snow field keeps its contract**: it still reads a
   `SurfaceGrid`-shaped geometry, still integrates per cell, still feeds the
   shed and the granular transport. What changes is who owns the surface it
   integrates on (the shared store) and that pools now come from real
   depressions instead of a noise mask over a slope test.
5. Bridges and underpasses are free: each span level is its own catchment.
   Water on the skyway drains the skyway; water under it drains the street;
   today's single-top-surface trace cannot even represent that.

### WALKABLE (navmesh)

The span store is Recast's heightfield in all but name — Recast rasterises
triangles into per-column spans and everything downstream (walkable filter,
region partition, contours, polymesh) works on spans. The mapping:

1. **Walkable filter.** A span top is walkable where the step to a
   neighbouring column's span top is within the agent's step height and the
   span is above the drop threshold (ledge filtering); slope comes from the
   same neighbour deltas the surface field already computes. Flags for water,
   and for span tops too steep, are the same flags drainage reads.
2. **Multi-storey.** Each span level is a navmesh layer; vertical links
   (stairs, ramps) are found where a column's walkable spans connect by
   slope-and-height continuity — the same data the parkour channel would
   read. Recast's plain pipeline is single-layer; the layered approach is
   Recast-with-layer-separation on the span field, which is precisely what
   the span list gives without the classic " Recast flattens multiple
   levels" failure.
3. **Tiling and partial rebuild** are Detour TileCache's exact model: tiles
   rasterise independently, an edit re-rasterises the affected tiles'
   spans and re-bakes them, obstacles compose as temporary rasters. The
   dirty-column machinery the store already has is the tile-cache update
   signal.
4. **Clients of this channel**: local AI/parkour/combat (client-side, no
   server dependency), and coverage answers for SLMC-style checks. The
   **server's** pathfinding navmesh (what the sim builds and what
   `llpathfinding*` displays) is a different artefact from a different
   source of truth; this channel does not replace it and must not be
   confused with it. If a client-side mesh ever wants to be *authoritative*,
   that is a sim-side feature with the same representation — the design
   simply keeps the store able to export spans in a form a Recast build
   consumes.

### ACOUSTIC

The soundscape today raycasts 3 up + 4 out on a 50 ms cycle, classifies
SPACE_OUTDOOR/SHELTERED/SMALL/MEDIUM/BIG, and borrows the wind tile's column
top for burial. Replace the classification with lookups, keep the smoothing:

- **Probe lattice**: one probe per air cell of a coarse grid (8–16 m) over
  outside-connected and interior air, from `SOLID_VOLUME_3D`. Per probe,
  precomputed at bake (recomputed on tile rebuilds, not per frame):
  - **sky openness** — from `COVERAGE` (the 3 up-rays' answer, exact);
  - **room volume and wall distance profile** — air-cell count and span
    distances in the four cardinals (the 4 side-rays' answer, per probe);
  - **reverb character** — Sabine-style estimate from room volume and an
    approximate surface-area-to-absorption ratio derived from span density;
    classified into the existing SPACE/ESize enums so the current loop-bed
    machinery is unchanged;
  - **travel time to open air** (the burial/portal measure) — graph distance
    from the probe to the nearest outside-connected cell.
- **Runtime query**: nearest probes (the lattice is a flat array; 2–4 taps),
  interpolate, done. Indoors/outdoors, burial, room size and reverb class are
  a handful of loads — the "very small lookups" ask.
- **Occlusion and shockwaves**: the air cells and their connectivity form a
  graph (neighbour + diagonal + vertical links where spans permit). A
  **travel-time field** (uniform-cost Dijkstra over that graph, per event
  origin, computed once and cached while the event is interesting) replaces
  straight-line distance for thunder scheduling and gives shockwaves their
  actual routes: around buildings, through alleys, into courtyards. Occlusion
  gain for a source→listener pair is the count/depth of solid spans the
  straight line crosses, read from the store — no raycast. This is the
  Project-Acoustics shape (baked per-probe acoustic parameters, runtime
  interpolation) scaled to what a viewer can afford: parameter bake, not wave
  simulation.
- **Windblown snow interplay**: the soundscape's regime crossfade and the
  whiteout pass keep reading what they read today (regime signal, ground
  window). The acoustic channel adds what none of them has: muffle by
  structure between source and listener, using the same occlusion walk.

## Part 4 — partial updates and patching

The store's update story is one gate and four patch paths.

**One gate.** `SSAtmoMagic::settleEdits` already debounces object edits
(~3 s, reset on churn) and fans out `markDirty` to both maps. It becomes the
single `markDirty(pos, radius)` into the field; the field fans out to every
materialised channel. Per-channel consumers can also dirty themselves
(wind on wind change, rain shadow on fall-direction swing — both exist).

**Patch granularity.**

- **Capture**: an edit's AABB marks dirty *columns*; the re-peel is scissored
  to the dirty rect (the peel is a render with a smaller ortho frustum —
  cheap), splicing new spans into the tile's columns. Column-granular, not
  tile-granular; a one-prim edit is a few hundred columns, not 65k.
- **Geometry serials** per tile, as today, but now bumped only when *spans*
  change, not when the camera leaves the band — the shared store is
  band-independent, so climbing 100 m no longer retouches the surface grid at
  all (today's rain shadow recaptures on band exit; the drainage retrace gate
  correctly ignores it, but the work is still spent).
- **Derived channels** re-materialise lazily from the new spans, each with
  its own cadence and granularity:
  - `SURFACE_TOP`/`DRAINAGE`: dirty-rect retrace (the surface field's
    retrace gate becomes rect-scoped; reservoirs survive, shared out by
    catchment as today).
  - `SOLID_VOLUME_3D`: the wind tile re-solves whole (a pressure solve is
    global), but on the existing throttled, settle-gated schedule; the
    sparse air mask makes the re-solve cheaper than today because enclosed
    rooms are skipped.
  - `WALKABLE`: tile-cache re-bake of affected navmesh tiles only.
  - `ACOUSTIC`: re-bake dirty probes only; listeners keep interpolating from
    neighbours meanwhile, which is why probe staleness is invisible.
  - `COVERAGE`: per-column derived figures are recomputed with their spans —
    no whole-tile invalidation (the archived doc's "finer granularity"
    open question resolves to: columns, because coverage is per-column).
- **Cross-region**: the 64 m margin overlap means an edit near a border marks
  the overlapping margin of the neighbour's tile dirty too — the same rule
  both capture systems apply today, now in one place.

**Cost targets.** A prim edit should cost: one scissored re-peel (one small
ortho pass), a few hundred column rewrites, and dirty marks. Channel
re-materialisation is scheduled by interest and budget (one channel-tick per
frame, the way the field cursor and wind stages already work), never a same-
frame cascade.

## Part 5 — migration, in an order that never breaks the intermediate state

1. **The capture service exists beside the systems, producing `SURFACE_TOP`.**
   It is rain shadow's capture (same pass, same band) hosted in the new staged
   pipeline; `buildSurfaceGrid` reads the store. Rain shadow keeps its
   tile/band/serial semantics; behaviour is identical, plumbing is shared.
2. **Multi-span peel lands behind a setting, nothing consumes it yet.**
   Prove the peel-and-exclude cost and span budget against real builds.
3. **`SOLID_VOLUME_3D` absorbs wind's capture stages.** `CAPTURE_TOP`,
   `CAPTURE_PROBE`, `REDUCE`, `BRIDGE` become "ask the field"; the multigrid
   solve, project, shelter and gust machinery are untouched. The
   underside-histogram slice forcing moves into the store (span evidence),
   and slice placement keeps reading it. The air-connectivity pass lands
   here with the "skip enclosed interiors" optimisation.
4. **`COVERAGE` absorbs the soundscape probes.** `coverageAt`/`burialDepth`
   replace the 7 raycasts and the `surfaceAt` borrow. This is the first step
   whose payoff is a feature, not just a deleted pipeline.
5. **`DRAINAGE` replaces the edge-cell trace.** Priority-flood + D8 per span
   level; eaves and pools feed the existing reservoir/shed code unchanged;
   the puddle mask's slope test becomes filled-depression membership.
6. **Design H lands behind its own setting, after step 5, identify tier
   first.** It needs the trace to already know which cells are lips before
   it has anything to refine. The ID buffer and the closed-form primitive
   path are the first half — cheap, synchronous, no worker involved — and
   are worth shipping and measuring alone before the mesh snapshot and
   worker pool are built at all, since they may turn out to cover most real
   content by themselves. `refineEdge` falls back to today's raster walk
   wherever neither tier answers. The raster answer stays correct on its
   own the whole time — this step only ever tightens it.
7. **`WALKABLE`** lands as a channel (Recast over spans, tile cache).
8. **`ACOUSTIC`** probes bake; the soundscape's probe cycle becomes a lookup.
9. Snow never moves: the field windows, `sampleGround`, `liftAt`,
   `forEachLiftCell` and the granular tick keep their exact contracts; only
   the provider of their geometry changes underneath them in step 1.

Every step is independently revertable, and steps 3–4 are where the known
limitations of today's docs stop being private-capture limits and become
tuning dials on the shared one.

## Costs

Rough order, per 256 m region (varregions scale with area, and the near/far
tier caps the damage):

| Item | Estimate | Notes |
|---|---|---|
| Span store | ~1–2 MB/region | 128² columns × 2–4 spans × 12 B |
| Peel capture | N ortho renders + readbacks per tile, staged | N = span depth (1–4); scissored on patches |
| Air mask + solve volume | ~2–7 MB GPU transient | today's solve grid, reused; sparse masking reduces Jacobi work |
| Coverage arrays | ~100 KB/region | per-column derived flags/depths |
| Drainage network | ~500 KB/region | per-span D8 + runs; rebuilt on geometry serial only |
| Navmesh tiles | ~1–4 MB/region | Recast polymesh at agent scale, cached, tile-cache patched |
| Acoustic probes | ~10 KB/region | 16 m lattice, few floats per probe |
| Edge ID buffer | one extra render target on the eave capture | reused every peel, not a separate pass |
| Edge refinement snapshots | a few hundred KB/region, transient, mesh lips only | primitive lips are a closed-form read, no snapshot; only mesh/sculpt lips reach the worker pool |

The GPU solve keeps its current budget (one tile at a time, staged, throttled);
the peel adds passes but retires two standalone ortho captures, and the
sparse-air mask reduces the solve's live cell count by whatever the build
encloses.

## Open questions

- **Span budget under real content.** 2–4 spans is the working hypothesis
  from the archived sketch and still unmeasured against dense SLMC-scale
  builds; `SSWorldFieldMaxSpans` is the dial and the overflow policy
  (collapse bottom spans first) must be chosen against real content.
- **Peel depth vs probe res.** Probes at 2× grid res already cost four
  1536² captures in the worst case; whether the shared service raises probe
  count (8 directions), resolution, or span peels first is an empirical
  question once one capture feeds everything.
- **Interior vs sealed.** The connectivity pass needs a defensible rule for
  "room with a window": interior rooms still deserve wind *audio* (draughts
  through openings) even when the solve skips them. Likely answer: solve
  interior volumes at reduced iterations when a COVERAGE consumer is
  registered, skip when only wind is.
- **Navmesh agent parameters and cross-region stitching.** Agent radius/step
  and the margin overlap needed for Detour stitching across regions are
  tuning, but decide them before the first navmesh tile ships.
- **Acoustic lattice density vs bake cost.** 16 m probes with interpolation
  covers SPACE classification; portal-grade reverb (doorways) wants denser
  probes or portal tagging — unresolved, decide against real builds.
- **Who owns the carve.** Same question as the archived doc, updated: probes
  carve the shared store now, so the store's span flags must answer
  "open at height z", not just at span boundaries — a small interpolation
  rule, to be fixed before wind migrates.
- **How much of real content is primitive vs. mesh at the eave.** This is
  the number that decides whether Design H's worker pool is a core piece of
  the system or a rarely-used fallback — measure it against real builds
  (SLMC regions specifically) before sizing anything around it.
- **What the closed-form/mesh dividing line actually is.** Which cut/path
  combinations still get a clean analytic edge and which don't is a
  `LLVolumeParams`-shaped question that wants answering against real content
  rather than derived from first principles.
- **What Design H's mesh snapshot actually holds.** Physics hull, decimated
  render mesh, or something built specifically for this — unresolved, and
  the choice trades snapshot cost against how much better the answer
  actually is than the raster one; decide against measured
  `refineEdge` error, not a guess at it.
- **Worker pool scope.** Whether mesh lip refinement gets its own bounded
  pool or shares the viewer's general work queue is a scheduling decision
  that should follow from measuring how many mesh lips a busy region's
  trace actually produces per retrace, not precede it — likely a much
  smaller number than "every lip" once primitives are handled separately.

## Research notes

- **Recast / Detour TileCache** — navmesh from rasterised spans is the
  industry pipeline (voxelise → filter → regions → contours → polymesh);
  tiled navmeshes exist for exactly the re-bake-on-edit and streaming
  behaviour asked for here. The span store *is* Recast's heightfield, which
  is why WALKABLE is a channel and not a project. (recastnavigation,
  Docs/1_How it works; DetourTileCache.)
- **Project Acoustics** (Microsoft) — voxelise the scene, simulate/bake
  per-probe acoustic parameters (occlusion, portaling, reverb), interpolate
  probes at runtime. The ACOUSTIC channel is the scaled-down, statistic-based
  version of the same shape: the lattice, the per-probe bake, runtime
  interpolation — without the wave solver.
- **Steam Audio baking** — baked static-geometry propagation with dynamic
  listener; same probe/parameter split.
- **Portal/graph propagation** (Thief: Deadly Shadows and successors) —
  propagation over the navigation structure; the wind connectivity pass and
  the navmesh give the same graph for free.
- **Layered depth images / depth peeling** — the capture primitive; N peels
  give N spans per column, which is exactly the span store's production
  method.
- **Sparse voxel hierarchies (OpenVDB, SVO/DAGs)** — the right tool when true
  3D sparsity is the content (volumetric fog, full cave systems); rejected
  here as the canonical store because the content is stacked flats, but the
  air-connectivity flood fill is the same idea applied where it pays.
- **Priority-flood depression filling + D8/D∞** (Barnes et al. 2014;
  Tarboton 1997) — the hydrology-standard O(n log n) drainage computation on
  a heightfield, and the correct upgrade for the puddle/eave/catchment
  machinery.
- **Pressure projection** — the wind solve's divergence-free guarantee is
  what makes every downstream advection (precipitation, particles, snow
  creep, the whiteout's semi-Lagrangian air) stable; any wind redesign keeps
  it.
- **Mean free path / Sabine estimation** — the cheap reverb characterisation
  (volume, surface area, absorption) that makes per-probe reverb parameters
  bakeable without a wave solver.
