# Atmo Magic: the census navmesh (Recast/Detour over the declared-shape census)

Status 2026-09-09: §1-§9 record the homebrew design round and its verdicts; §10 is the Recast
benchmark that superseded it and §11 the owner's decision (Recast, 2D columns split into
height bands) and the viewer shell `ssnavmesh.h/.cpp`, written but not yet built. Read §10-§11
first; §3-§7 stay as the record of what the harness core (`V:\Scratch\navmesh`) still does and
why. It amends `atmo_magic_worldfield.md` (WALKABLE, Part 4) and
`atmo_magic_worldfield_competition.md` §11, whose 3D-tile decision §11 here revises.

## 1. What this replaces

The column+spans store in `ssworldfield.h/.cpp` is a render-sweep capture: horizontally
imprecise (bands, bisection), blind beneath its own landing surface, and it never mapped
interiors. It is dropped in favour of rasterizing the declared-shape census
(`ssworldfieldshapes.h`) into 3D tiles, Recast's model, with the sweep kept only as the
outdoors authority per §11. Consumers that read the store today: soundscape
(`acousticAt`, `probesAt`, `enclosureAt`), surface field, height fog, the info view and the
debug floater. They migrate to the tile reads in §6.

## 2. The shipped first pass is broken

`ssworldfieldtiles.cpp` (the in-viewer first pass, gated off by default) was ported verbatim
into the harness as the LEGACY policy and fails 13 of 68 synthetic checks:

- Box and sphere slab intersection computes the parameter `t` in metres, then clamps it to
  [0, 1] and scales by `TILE_M`. Any box whose bottom sits more than a metre above the tile
  floor vanishes; spheres scale wrong.
- Triangles with a degenerate xy projection (`fabsf(d) < 1e-9`) are skipped: every vertical
  wall of every mesh disappears.
- Column-centre sampling misses any wall thinner than a cell (0.25 m) that sits between
  centres.
- Published tiles are never pruned (`mTiles` only grows), and the 6-span cap collapses the
  thinnest gap at publish time, fusing floors before any consumer sees them.

None of the first pass survives except the tile key packing and the scheduling shape.

## 3. The raster core (harness `src/navraster.h`, port target `ssnavrastercore.h`)

Dependency-light: `stdtypes.h`, `<cmath>`, plain float arrays. Input is `NavShape`, a
float-array view of a census `Record` (the shell aliases `LLVector3` storage; cylinders need
`mAxisU/mAxisV` copied into axes 0 and 1 explicitly).

- **Scratch**: one dense 128³ U8 voxel block (2 MB), bits SOLID/PHANTOM/TERRAIN/DYNAMIC plus
  reserved WALKABLE bits. Touched z-range per column bounds both the clear and the extract.
  Span lists while filling were considered and rejected: the dense array keeps the fills
  branch-free and the measured extract cost is small.
- **CLIP policy** (the only one that ships): every primitive is clipped against each
  column's square, Recast's `rcRasterizeTriangle` progressive Sutherland–Hodgman, after a
  one-time clip against the tile's own box (a 2 km surround OBB otherwise stalls a tile for
  hundreds of milliseconds). Analytic shapes enter as convex face sets and are solid-filled
  between their lowest and highest clipped face (exact for convex bodies). Curved shapes are
  circumscribed polyhedra with an inflate factor taken from the patch half-diagonal, so the
  fill is conservative. Axis-aligned upright boxes fill directly. Triangle soups (mesh hulls,
  physics tessellations) are surface spans, as in Recast.
- **Extract**: run-length per column, sub-slab gaps (2 cells) merge, `mMaxSpans` stays 0
  (unlimited) at publish; capping is a read-side concern.
- **Guards**: non-finite AABBs are skipped and counted; shapes nominating more than 4096
  tiles (16³) are skipped by the scheduler and logged.

## 4. Storage

Per tile: CSR over 128² columns. Span is `{U8 bottom, U8 top, U8 flags}` (cells run 0..128),
column offsets are U16 with a per-tile escalation to U32 past 65535 spans. That halves the
measured 133 KB/tile to about 67 KB with no algorithm change. A block-tiered index (16×16
blocks of 8×8 columns with EMPTY / UNIFORM / TERRAIN_FLOOR / SPARSE modes) is measured in the
harness and adopted only if it beats that floor clearly on the nine-region sample; the
projection is 2 KB for empty or uniform tiles and under 20 KB for a terrain-crossing tile.

Memory ceiling at the default 192 m envelope is 1728 tiles: ~115 MB after the U8 change,
~30 MB if the block index lands. The owner sets the budget (§8).

## 5. Terrain

Baked into tiles as solid fill below the heightfield (BIT_TERRAIN), sampled at column
centres, fed by a per-tile 33×33 window at 1 m anchored at the tile's xy minimum. The shell
builds the window from `LLViewerRegion` (neighbour regions included where available). Terrain
is not a census `Record`: it has no UUID, no settle window and no provenance, and routing it
through the DYNAMIC machinery would be wrong. `castTerrain` stays for exact query-time
answers.

## 6. Walkable span graph, not a polymesh

The pipeline stops at a walkable-tagged span graph. Recast's distance field, watershed
regions, contours, polymesh and detail mesh are not built: SL's WALKABLE partition and the
planned SHELTERED/INTERIOR flood are the same open-air-graph problem over the same 3D
adjacency, and a second labelling would drift from the first. Contour/polymesh export stays
an unscheduled converter until a Detour-consuming feature exists.

- **Classification at raster time**: face normal against `mWalkableSlopeDeg` (45°) tags the
  topmost cell of each raster call with BIT_WALKABLE. After slab merge the OR'd bit means
  "walkable somewhere in this span", the same granularity Recast keeps.
- **Compact form on demand**: `compactColumn()` yields open (floor, ceiling) intervals
  between spans; nothing compact is stored.
- **Filters**: clearance (`mWalkableHeightCells` = 8, 2.0 m) and ledge/step
  (`mWalkableClimbCells` = 3, 0.75 m) against the four lateral neighbours. These are three
  separate constants; `mSlabCells` is a raster merge rule and is never reused as a
  locomotion parameter.
- **Multi-storey is free**: every span is its own node; there is no layer flattening to undo.
- **3D adjacency**: `neighborTile(tx,ty,tz,dx,dy,dz)`. Lateral stitching compares a tile's
  one-cell boundary ring with the neighbour's. Vertical: an open interval whose ceiling is the
  tile's +Z face has an unknown clearance; the condition is derived (topmost span top < 128,
  lowest span bottom > 0), never stored as flag bits, and resolved by chasing into the +Z tile
  with a depth cap (4) and an assume-blocked fallback. Edges into unpublished neighbours wait
  on a pending-stitch list keyed by the missing tile and resolve when it publishes.
- **Reads**: capped `spansAt(pos, out[], max)` and `solidAt(pos, mask)` for probe callers
  (default mask SOLID|TERRAIN so phantom never reads solid by accident); an uncapped
  zero-copy `columnSpans(tile, cx, cy)` is the only path the walkable builder is handed;
  `walkableAt(pos, agent_class, WalkNode&)` and an allocation-free `forEachOpenNeighbor`.
- **Door portals**: `EDGE_PORTAL_DOOR` is reserved in the edge kinds; nothing is behind it.
  Neither `Record` nor the offline data carries a prim name, so the classifier is port-side
  plumbing with no harness test.

## 7. Partial rebuilds, movers, phantom

- Attribution is the census bucket grid plus one cached previous-AABB pair per record.
  An edit, a move, a departure, or a provenance transition (an OBB placeholder replaced by the
  fetched hull, the commonest way a tile goes wrong with 60% of parts on OBBs) dirties the
  union of the old and new AABB tile sets. No per-span or per-tile owner tables.
- `TileMap::prune(keep)` and `evict(key)`; the scheduler diffs its key set against the
  published set every pass.
- Budget: a time budget per frame plus a per-candidate cost estimate from the bucket's
  shape/triangle count; projected-expensive tiles go to a low-priority queue. A tile is never
  published partially rasterized. Measured cost before the fixes: 1.25 ms per tile average,
  long tail to 12 ms; one edit re-rasterizes 1.8 tiles on average.
- DYNAMIC (moved within the settle window) stays excluded unconditionally; a mover's effect
  on walkability is a query-time gate against the live census.
- Phantom rasterizes behind BIT_PHANTOM only when scheduled to, and every reader passes a
  mask.
- One dedicated raster worker (not a pool) fed a copy-once census snapshot is the intended
  threading; the harness proves the core has no hidden shared state with a two-thread
  byte-identical run. Wiring it in the viewer is a later decision.

## 8. Owner decisions still open

1. Z-chase fallback past the depth cap: assume blocked (current default) or passable, and
   the cap in tiles.
2. Agent classes to bake (radius, height, step, slope). Four flag bits are free; the default
   build carries one class (2.0 m, 0.75 m step, 45°).
3. Whether name-based door classification is wanted for WALKABLE at all.
4. Phantom policy: out of the store, in behind a mask, or a second tile set.
5. Memory ceiling for the tile map at the default envelope (decides whether the block index
   is required).
6. Main-thread ms per frame, and whether a dedicated raster worker is acceptable.
7. Cell size: 0.25 m, or 0.125 m for parkour (8× scratch, invalidates the U8 encoding).
8. Terrain at region borders: can the shell source the neighbour heightfield for a border
   tile.
9. Who owns the mesh-fetch completion hook that turns a provenance transition into
   `markDirty`.
10. Whether a Detour-consumable polymesh export is ever needed.

## 9. Harness

`V:\Scratch\navmesh` links the viewer's own `llmath` (LLVolume tessellation,
`LLPhysicsShapeBuilderUtil`) and reads the Region Object Cache (`.roc`) so the census is the
viewer's exact classification of real content: nine Agni regions, 128k objects. Limits stated
plainly: mesh and sculpt assets are not cached offline, so those parts are OBBs exactly as the
viewer's pre-fetch path; ObjectPhysicsProperties never arrive offline, so every prim is the
geometry-only case. Indoor precision is proven on synthetic prim scenes; real regions give
scale and speed. Build: `powershell -ExecutionPolicy Bypass -File V:\Scratch\navmesh\build.ps1
-Target <test_raster|navbench|...>`.

## 10. Recast benchmark (2026-09-09, harness `src/recastbench.cpp`)

recastnavigation (zlib licence, `D:\recastnavigation`) was fed the same census through the same
tile schedule. Same machine, same nine regions, cs = ch = 0.25 m, agent 2.0 m / 0.5 m / 0.75 m
step / 45°.

| measure | homebrew CLIP core | Recast, 32 m cubes | Recast, 32 m columns (2D) |
|---|---|---|---|
| synthetic scenes (thin wall, room, 10 storeys) | pass | pass | pass |
| per tile, whole pipeline | 1.26 ms (raster + spans only) | 2.0 ms monotone, 3.0 ms watershed + detail | 57 ms, max 649 ms |
| rasterize only | 0.82 ms | 1.0 ms | 27 ms |
| published per tile | 66.7 KB spans | 2.2 to 2.6 KB navmesh | 13 KB navmesh |
| path query | none | 10 to 30 µs (`findPath`) | same |
| partial rebuild per edit | 5.2 ms (re-raster 1.8 tiles), worst 104 ms | 13.7 ms (full pipeline) | tile cache: 0.26 ms from cached layer, obstacle 0.14 ms |
| terrain-only ground connectivity | n/a | 63% in one component | 89% in one component |

Findings:

- Recast's `rcRasterizeTriangle` is the algorithm CLIP reimplements; it passes the same
  synthetic checks. Solids are hollow inside (surface spans only); CLIP's convex solid fill
  is the only geometric extra the homebrew has.
- **Z-tiled cubes break Detour's portal model.** A floor cut by a Z boundary has no portal
  edge, so the ground network fragments where terrain crosses z = 32 (63% vs 89%). Recast
  wants 2D tiles with unbounded height; its span lists make height free. The §11 3D-tile
  decision holds for a solid-volume store but not for a Detour navmesh.
- 2D 32 m columns have a long tail (a column carrying a skybox at 3500 m costs up to 649 ms):
  a raster worker thread or 16 m tiles is required either way.
- DetourTileCache is the partial-rebuild story the design round was designing by hand:
  rasterize once into per-tile layers, rebuild the navmesh tile from the cached layer when an
  object or obstacle changes, movers as temporary obstacles. Raw layers are 134 KB per column
  uncompressed (three layers on average); the demo's FastLZ compresses them 5 to 10×.
- Connectivity in the real regions is content-bound offline: mesh buildings are solid boxes
  and their rooftops are islands (largest ground component 21 to 23% either way). Cross-tile
  stitching itself works (terrain-only paths cross tiles).

Recommendation: adopt Recast + Detour (+ DetourTileCache) for WALKABLE, fed from the census
by the `emitShape` adapter (150 lines), on 2D tiles with full-height span lists; keep a span
read for the flood/coverage consumers off the same `rcHeightfield` rather than a second
rasterizer. The homebrew core's remaining value is the convex solid fill and the analytic
terrain fill; both are cheap to keep as a pre-pass if a consumer needs solid-volume answers.
Owner decision needed before either path continues (see §8, plus: vendor Recast, or keep the
homebrew span store and add Recast only for navigation).

## 11. Decision and the viewer port (2026-09-09)

Owner decision: Recast. The homebrew raster core stays in the harness as a reference; the
viewer keeps none of it.

**Height bands instead of Z cubes.** The loss in dropping 3D tiles was granularity (a skybox
edit re-rasterizing its whole column) and the long tail. Both come back with bands: each
32 m column's geometry z-intervals (records plus terrain) merge across gaps smaller than
`SSNavMeshBandGap` (8 m); every merged run is one Recast build with that z-range and one
Detour layer. A band boundary lies in empty air by construction, so no floor is ever cut and
no portal is lost. Measured on region 0: 468 bands for 108 columns, ground connectivity
identical to full columns (89.1%), worst tile 31 ms instead of 70 ms; all nine regions 3319
bands, worst 302 ms instead of 649 ms. The remaining tail is dense content, which is why
builds run on the worker queue.

**The shell** (`indra/newview/ssnavmesh.h/.cpp`, `SSNavMesh` singleton, setting `SSNavMesh`,
needs `SSWorldFieldShapes`):

- Vendored `indra/recastnavigation` (Recast, Detour, DetourTileCache; zlib licence; upstream
  9f4ce644, 2026-02-27) as a static library target linked by newview. The licence notice is in
  `indra/newview/licenses-{win32,mac,linux}.txt` (Help > About > Licenses) and the one altered file
  is marked in the source, as the zlib licence requires.
- Ticks after the census in `llviewerdisplay.cpp`. On every census stamp change it schedules:
  records nominate columns and z-intervals, terrain adds a sampled 37×37 interval per column,
  bands are merged, and a band is rebuilt only when its geometry signature (hashed AABBs,
  class, provenance, triangle count, terrain heights) changed. Bands and columns that left the
  envelope are evicted from the tile cache and the navmesh.
- A build snapshots the band's triangles on the main thread (`emitRecord`: box, cylinder,
  ellipsoid as outward-wound faces, triangle soups as-is, terrain as two triangles per metre)
  and posts the Recast pipeline to the General work queue (rasterize, filters, compact,
  erode, heightfield layers, zlib-compressed tile cache layers). The main thread swaps the
  layers into the DetourTileCache and rebuilds the column's navmesh tiles. Two builds per
  frame, two in flight, a generation counter drops late results after a teardown.
- Coordinates: agent space is re-based into a frame pinned to the region origin captured at
  init, so a border crossing shifts positions rather than tile keys; Recast is y-up, the map
  is (x, z, -y), Detour tile y is -ty-1.
- DYNAMIC records become box obstacles, re-synced per census; `update()` pumps the tile cache
  once per frame (one affected tile rebuilt from its cached layer, 0.26 ms in the benchmark).
- Queries: `nearestPoint`, `onNavMesh`, `findPath` (straight path, partial flag). Debug view
  7 draws polygon edges coloured by band.

Not yet done, in order: the first build (owner), a floater/info-view stat line, the
flood/coverage span read off the heightfield (the SHELTERED/INTERIOR consumer), agent
classes and door portals (§8), and retiring the column+spans store once its soundscape
readers have navmesh-side replacements.

## 12. Census cost (2026-09-10)

First build in the viewer showed `FTM_SS_SHAPES_CENSUS` spiking to 700 ms: the census rebuilt
whole on one frame every 8 to 10 s (age trigger, 48 m anchor drift, dirty sphere) and again
lazily inside any query that found it stale, re-tessellating and re-baking every part each
time. Two changes in `ssworldfieldshapes.cpp`:

- **Part cache.** Every part's baked records are kept under a signature of everything that
  shaped them (transform, volume parameters, physics type, phantom, hidden, mesh
  decomposition state). A rebuild reuses any part whose signature holds and only
  re-tessellates what changed. Triangle soups are shared pointers, so reuse copies a pointer.
  Entries expire 60 s after their last sighting.
- **Time-sliced build.** The scan fills a pending census under `SSWorldFieldShapesBudgetMS`
  (3 ms) per frame and swaps it in whole; readers use the previous snapshot meanwhile and
  the census stamp moves only at the swap, so the navmesh schedules once per completed
  build. A region switch mid-scan abandons the pending snapshot. Queries no longer rebuild
  inline; a stale census only opens the sliced build.

The navmesh's own scheduling no longer samples the land at every metre of every column per
census (268k lookups); it samples every 4 m to detect change and the band build still uses
the full grid.

## 13. Tile seams (2026-09-10)

Two seam defects showed in the viewer overlay, both fixed and both measured in the harness through
the tile-cache path the viewer uses (`recastbench --tilecache --bands 8`):

- **Corners at four heights.** Each band rasterized into a heightfield whose floor was that
  column's own `lo - 1`, so the 0.25 m height cells sat at a different offset per tile and a
  shared corner rounded differently in each. Band z-ranges now snap outward to the global
  0.25 m lattice (`ssnavmesh.cpp` schedule, mirrored in the bench).
- **Mid-edge wedges.** The tile-cache contour builder places a border vertex only where the
  tile's own regions change, so one tile follows the terrain with a mid-edge vertex while its
  neighbour draws one straight edge, and the two diverge by the terrain's curvature over up to
  32 m. Detour still links them within the climb tolerance, but the surface is misdrawn and
  border height reads are off. The vendored `DetourTileCacheBuilder.cpp` is now ALTERED (marked
  in the file and in the library's CMakeLists): `tessellatePortalEdges` splits every portal
  segment at world-aligned 16-cell (4 m) intervals, taking each inserted vertex's height from
  the region's own cells at that corner. Tiles are a whole number of intervals wide, so both
  sides of a border carry vertices at identical positions with identical heights. Measured on
  region 0, terrain only: 461 of 461 portal edges link (was 326 of 326 before, at coarser
  granularity), ground connectivity 89.0%; with content 85.6% link, the rest being surfaces
  that genuinely end on a border. Polygon count rises about 18%.

A first attempt interpolated the inserted heights linearly along the edge; the builder only snaps a
vertex to real cell heights when the guess is within the climb tolerance, so on curved terrain the
guess survived and produced 4 m disagreements. The harness's `reportConnectivity` prints unlinked
portal edges with their endpoints, which is what exposed it.

Also in this round: terrain change detection for scheduling moved from height samples to the land
patches (min/max height plus the sim's last update time per 16 m patch), which is exact and never
churns; the overlay flashes a band white for a second when its layers land, so rebuilds are visible.

## 14. Terrain-only navmesh: the settle bug, and the console (2026-09-10)

The viewer's navmesh mapped terrain and nothing else. Recast was reading the census; the census was
handing it nothing: `SSWorldFieldShapes::trackRest` tested `!state.mSeen` as "first sighting", but
`mSeen` is the pruning flag every build resets to false before its scan, so every build re-marked
every root DYNAMIC. Nothing ever settled, every object was excluded from the bands and carved out
as a box obstacle instead. A `mKnown` flag now carries "has a history"; `mSeen` stays the pruning
flag. The bug predates the navmesh; the old tile raster excluded everything the same way and
nobody could tell because it drew nothing anyway.

Also from this round: convex hull soups (mesh decompositions, prim physics hulls) are oriented
outward per hull at census time, since Recast only marks up-facing triangles walkable and hull
winding is whatever the decomposition produced.

**Console.** `ssfloaternavmesh.h/.cpp` + `floater_ss_atmo_navmesh.xml`, opened from the Atmo
floater's Navmesh button, laid out after the stock Pathfinding view / test floater. Status:
columns, bands, pending and in-flight builds, polygons, layer memory, builds and last build time,
obstacles; census records, triangles, cached parts, current/rebuilding/stale, and what the last
schedule saw (records, dynamic, phantom). View tab: `SSNavMeshShow` (fills + edges),
`SSNavMeshShowLinks` (cyan linked / red open border edges), `SSNavMeshShowFlash` (rebuilt bands
whiten for a second), `SSNavMeshShowObstacles` (mover boxes), `SSNavMeshShowCensus` (the shapes),
and Rebuild all. The overlay draws from the pipeline whenever any switch is on, independent of the
world-field mask; world-field view 7 still forces the polygons. Test path tab: start and end from
the avatar or the camera, Find path, Clear; the polyline draws orange when complete and yellow
when Detour stopped at the closest reachable point.

## 15. Rest classification: pathfinding role, ROC ledger, watching (2026-09-10)

`SSWorldFieldShapes::trackRest` now decides "landscape or mover" on a three-rung ladder, and the
console's census line reports how many roots each rung decided:

1. **The sim's pathfinding role**, from flags every object update carries. A pathfinding
   character or a physical root is a mover whatever it is doing right now. A linkset with
   `FLAGS_AFFECTS_NAVMESH` was explicitly set to a static role (Walkable, Static obstacle,
   Material or Exclusion volume) and is landscape from its first sighting. Legacy linksets nobody
   touched read as Movable obstacle and carry neither, which is the common case: builders rarely
   set pathfinding roles, so this rung is right when present and usually absent.
2. **The Region Object Cache ledger** (`SSROCLedger::restVerdict`), the same evidence promotion
   weighs: a hard-disqualified record (physical, character once) or one re-sighted elsewhere is a
   mover; a promoted record, or one confirmed still on two or more region entries, is landscape
   now rather than after a settle window. No record, or a single entry, is no opinion.
3. **Watching**: the existing settle window (4 s at rest across rebuilds), which is all a
   first-visit region has. Anything a higher rung called landscape that then moves still falls
   to DYNAMIC through this rung on the next rebuild.

The practical effect: on a return visit to a region the ROC knows, the navmesh has its objects
from the first census instead of the second.

**Next: cache the navmesh in the ROC.** The band layers are already compressed, keyed by column
and band, and guarded by a geometry signature, so a `SSROC_SECTION_NAVMESH` in the region file
could carry them and let a return visit publish bands straight from disk, rasterizing only what
the fresh schedule's signature disagrees with. Two things must change first: the band signature
must be stable across sessions, which means replacing the terrain patch update time (session
frame time) with a hash of the patch heights and pinning the local frame to the region origin
rather than the origin captured at init; and the section needs the ROC's usual per-section CRC,
size bound and skip-if-unknown handling so an older build ignores it. Owner call on the disk
budget: a fully built envelope is a few MB per region of zlib layers.

## 16. What the census asks the sim for (2026-09-10)

Owner decisions this round, reversing the census's original "never request anything" stance:

- **Physics shapes.** A part whose shape type is unknown is asked for, through the same batched
  `GetObjectPhysicsData` capability the physics-shape overlay uses (calling `getPhysicsShapeType`
  files the id; the object list sends one request per region per frame for every stale id). The
  arrival changes the part's signature, so the next census re-tessellates it off its OBB.
- **Navmesh roles.** A linkset carrying `FLAGS_AFFECTS_NAVMESH` was explicitly given a static
  role, which is a strong signal; the exact role (Walkable, Static obstacle, Material volume,
  Exclusion volume) comes from one `ObjectNavMeshProperties` request per region, issued when a
  flagged root turns up without a known role and no sooner than a minute after the last. The
  reply fills a root-keyed table, marks the census for rebuild, and every record of the linkset
  carries the role. The navmesh honours it: static obstacles rasterize with no walkable area
  (solid, never floor), exclusion volumes become convex cuts applied to the compact heightfield
  after erosion, walkable and material volumes contribute as ordinary geometry.
- **Not census at all.** Volume-detect and temporary-on-rez linksets, and phantom ones: they
  are not physics shapes. The one phantom that stays is an exclusion volume, which carries no
  geometry but must reach the navmesh; the phantom layer therefore holds exclusion volumes only,
  and the census's own segment casts skip them.

The console's census line reports roles known (and whether a request is in flight) and physics
shapes asked for in the last build.

## 17. Whole-region coverage, solid fill, overlay controls (2026-09-10)

- **Range.** The navmesh was scheduled inside the census envelope (192 m around the camera) and
  evicted outside it, so it vanished as you moved: wrong for a surface the weather reads, where
  snow and puddles would come and go with the camera. Now `SSNavMeshRange` (512 m) sets the
  scheduling radius, the census widens its envelope to match while the navmesh is on
  (`SSWorldFieldShapes::envelopeRange`, triangle budget raised to 6M), columns are scheduled only
  when they lie wholly inside the envelope so every band sees all its records, and a built
  column beyond the envelope keeps its bands until its region leaves the world. The vendored
  library builds with `DT_POLYREF64` (28 tile bits, 20 poly bits), the tile budget is 16384, and
  the overlay draws every published tile with no distance cull.
- **Solid fill.** Recast rasterizes surfaces, so a solid body on the ground kept a walkable
  island inside it: the terrain span merged with the body's bottom face and read as floor with
  the body's height as headroom. Every convex record (analytic prims, mesh bounding boxes,
  decomposition hulls, one record per hull now) is filled solid per column with `rcAddSpan`,
  from its lowest clipped face to its highest, walkable only when the topmost face faces up
  within the slope limit. Tessellated soups, which may be hollow by design, stay surfaces. This
  is the homebrew core's convex fill, back where it earns its keep.
- **Overlay.** World toggle (off wipes the frame as the stock console does), X-ray vision
  (the occluded navmesh drawn shaded and fainter with a greater-than depth test), interior
  edges much fainter than border links, and ellipsoid census records drawn as three great
  circles rather than a box.
- **Phantom.** Phantom linksets leave the census entirely (previous section); trees seen in the
  overlay were from a build before that change.

## 18. Cells: 0.125 m, 16 m columns (2026-09-10)

Owner decision: 0.25 m cells were too coarse for user content (doorways, stairs, thin ledges;
the agent radius alone does not fix it). Cells are now 0.125 m, and because DetourTileCache
stores a layer's width in a byte (255 cells at most), columns shrink to 16 m so a tile stays at
128 cells across. A power of two keeps every lattice line exact across neighbours. Region sizes
are expressed in metres and converted per build (2 m minimum region, 5 m merge); the terrain
window is 21×21; the tile budget is 32768; build throttles default to 4 per frame and 4 in
flight. Border vertices are inserted every 16 cells, now 2 m.

Measured in the harness on region 0 through the tile-cache path with bands, full content:

| | 0.25 m / 32 m | 0.125 m / 16 m |
|---|---|---|
| columns, bands | 108, 1022 layers | 365, 3606 layers |
| raster + compact + erode per column | 18.5 ms | 21.0 ms |
| total build for the region | 2.0 s | 7.7 s |
| polygons | 11.6k | 31.1k |
| portal edges linked | 85.6% | 98.3% |
| raw layer data (before zlib) | 48 MB | 169 MB |

A 16 m column at 0.125 m has the same cell count as a 32 m column at 0.25 m, so the cost per
build is unchanged and the total scales with the column count, about 3.4× here. Border linking
improves markedly because half-cell height disagreements shrink with the cell. Memory is the
thing to watch: the console's "MB of layers" line reports the zlib-compressed resident size, and
`SSNavMeshRange` is the dial if a 512 m radius proves too much on dense mainland.

## 19. Fewer rebuilds, navmesh into the void (2026-09-10)

- **Persistent band slots.** Bands were keyed by their ordinal in the column, so a skybox
  appearing or a mover settling renumbered every band above it and rebuilt them for nothing. A
  new band now takes over the slot of the existing band its z-range overlaps most, and only an
  unmatched band takes a free slot; the Detour layer index follows the slot. The only rebuilds
  left are geometry changes in the band itself, a mover settling into it, a physics shape or
  navmesh role arriving, or a land patch update.
- **The void.** Sim surrounds and off-sim decor (a root inside the region, the build out past
  the edge) are navmesh too. Eviction kept a band only within one tile of a loaded region, so
  a void column inside the envelope was built and evicted every schedule. A band now lives
  while it was scheduled this pass wherever it is, or while it sits within
  `SSNavMeshVoidMargin` (256 m) of a loaded region; only a region leaving the world, or a band
  inside the envelope that the schedule no longer produces, drops it.

## 20. Crash in the tile-cache poly mesh, and the seam fix that was only half working (2026-09-10)

The first 0.125 m build crashed on the main thread in `canRemoveVertex` inside
`DetourTileCacheBuilder.cpp` with a stack-cookie failure. Two findings, both fixed in the vendored
file (marked ALTERED in the source and in the library's CMakeLists):

- **Upstream bug.** `canRemoveVertex` declares its edge scratch as `MAX_REM_EDGES` shorts but
  writes three shorts per edge, as its sibling `removeVertex` correctly sizes it. Once a vertex
  flagged for removal is shared by 16 polygons the writes run off the stack. That fan is exactly
  what a tessellated border produces: a strip of collinear border vertices triangulated against one
  interior vertex. The buffer is now `MAX_REM_EDGES*3` and the loop declines the removal when the
  edge count would exceed it.
- **The seam fix was being undone.** After triangulation the poly mesh removes every border vertex
  that sits on a straight portal run inside one region (`getCornerHeight` sets the 0x80 flag when
  the corner's cells share one portal direction and one region). That is precisely the vertex
  `tessellatePortalEdges` inserts, so most of section 13's lattice vertices were stripped again
  before the tile reached Detour. The height pass now keeps a flagged vertex whose along-border
  coordinate is a multiple of the 16-cell lattice; the neighbour carries the same vertex, so it is
  a shared vertex, not clutter. Everything else upstream removes is still removed.

The harness now reports how many portal edges have both endpoints on the lattice
(`reportConnectivity`, "on the 16-cell lattice"). Region 0 through the tile-cache path with content
at 0.125 m cells, 16 m columns:

| | before | after |
|---|---|---|
| portal edges | 22.4k | 63.7k |
| linked | 98.3% | 99.0% |
| both ends on the lattice | 42.4% | 75.3% |
| polygons | 31.1k | 54.7k |
| ground polys in the richest component | 20.6% | 25.1% |
| navmesh from layer, per tile | 1.6 ms | 2.4 ms |

The remaining quarter of portal edges have an endpoint at a genuine region change or a contour
corner, which the neighbour also carries. Polygon count rises 76%: every kept border vertex splits
a polygon, and the merge step cannot merge across them. Layer memory is unchanged (it is the
compressed heightfield, not the polygons). If that cost matters more than 2 m seam fidelity,
`DT_SS_PORTAL_TESS_CELLS` is the dial: 32 cells (4 m) halves the inserted vertices.

**Bend test (same day).** Keeping every lattice vertex fanned each flat border into slivers: eight
collinear vertices ear-clipped against one far vertex, visible on any platform. A flagged lattice
vertex is now kept only where the region's own height there leaves the chord between its lattice
neighbours by more than a cell (second difference over the lattice). Both tiles read that from
adjacent cells, so they keep the same vertices; where they disagree at the threshold the wedge is
under a cell. Region 0 with content: 37.9k polygons (upstream 31.1k, keep-all 54.7k), 98.5% of
portal edges linked, 1.9 ms per tile from layer. Flat borders return to a single straight edge.

What the lattice does and does not do: Detour links two tiles' portal edges by their overlap in the
plane and a height tolerance of the walkable climb; shared vertices are not required, which is why
the linked percentage barely moves across these variants. The lattice makes both tiles describe a
curved border with the same polyline, so the drawn surface and border height reads agree and the
wedge never exceeds the climb. On region 0 terrain the link count was 100% with or without it.

**Heap corruption on Rebuild all (same day).** The next run died inside `operator new` under the
terrain tangent builder with the heap reporting corruption: a victim, not a culprit. The culprit is
the second half of the same upstream bug family. `removeVertex` declares `tverts` as three bytes per
hole vertex and fills four, so a hole of more than 36 vertices overruns into `tpoly`; the garbage
indices survive triangulation into the polygon list, and `buildMeshAdjacency` then indexes its
heap arrays with them. Big holes are exactly what a border vertex shared by many polygons leaves
when it is removed, so the tessellation made both bugs reachable. Fixed by sizing `tverts` at four
per vertex (marked in the source). Also found on the way: `dtTileCache` lists at most 32 layers per
column when building or touching tiles at a column, silently ignoring the rest; a column of 12
bands can hold up to 192, so the list is now 256 (`DetourTileCache.cpp`, marked). The harness
result is unchanged by both, as expected: 37.9k polygons, 98.5% linked.

## 21. Bug sweep: dead region knobs, island cull, terrain signature, layer cap (2026-09-10)

Three findings from a review pass over the pipeline, all in `ssnavmesh.cpp`:

- **`minRegionArea`, `mergeRegionArea` and `maxEdgeLen` never did anything.** They are `rcConfig`
  fields for the classic path (`rcBuildRegions`, `rcBuildContours`), and this pipeline goes
  `rcBuildHeightfieldLayers` then `dtBuildTileCacheRegions` / `dtBuildTileCacheContours`, which take
  neither. The tile-cache region builder does its own monotone partition and merges each region into
  its largest same-area neighbour where that leaves no hole, so merging is not missing; what is
  missing is the classic path's cull of small isolated regions. Region merging is per tile in both
  paths and never crosses a border, so this was not behind the section 13 or 20 seam defects. The
  three fields are gone. In their place `cullLayerIslands` floods each `rcHeightfieldLayer` over its
  same-layer connections before compression and nulls any component under `SS_NAV_MIN_ISLAND_M`
  squared (2 m, 256 cells at 0.125 m) that touches neither the layer's edge nor a layer portal. Both
  exceptions matter: an island at the edge may be the sliver of a surface the neighbour tile carries,
  and one with a portal bit continues on another layer of the same tile. Culling either would open
  a seam of exactly the kind section 13 closed.
- **Terrain change signature stopped 4 m short.** The window `emitTerrain` rasterizes spans -2 m to
  +18 m around a 16 m column; the signature sampled the land patches at -2, +6, +14 m, so a terraform
  confined to the +16..+18 m strip (the next patch over) baked into the tiles but never changed the
  signature, and the band stayed stale until something else touched the column. Samples are now at
  -2, +8, +18.
- **Layers past the per-band cap were dropped in silence.** A band can produce more than
  `SS_NAV_MAX_LAYERS_PER_BAND` (16) walkable layers when floors stack within the 8 m band gap. The
  build now reports the overflow, `publish` warns with the band's column and count, and
  `SSNavMesh::layersDropped()` accumulates it for the console.

## 22. The world field's cells come from the navmesh (2026-09-10, rewritten 2026-09-11)

The queue's "flood/coverage span read off the heightfield". It landed on 2026-09-10 as a source
switch — the navmesh pushed each published band's sheet at the world field, which spliced it into a
store of its own — and was finished on 2026-09-11 by deleting that store. The navmesh's bands now
**own** the world field's geometry, and the field pulls rather than being pushed to. Full design:
`doc/atmo_magic_worldfield.md`.

- **Sheet on the worker** (`extractSpanSheet`, `ssnavmesh.cpp`). After rasterization and the
  Recast filters, before the compact heightfield, every raster span of the column's 16 m interior
  is read off the `rcHeightfield`: two cells by two into 0.25 m cells, gaps under the field's slab
  (0.25 m) closed, the list clipped to six by folding its thinnest gaps. The span the land sits in
  is flagged terrain (`SURF_FALLBACK`) and raised to the water surface with `SURF_WATER` where the
  land is under water; schedule extends the terrain interval to the water level so that surface is
  inside the band. The sheet rides the `Result` beside the layers and costs the worker under a
  millisecond.
- **The band keeps it** (`Band::mSheet`, a `shared_ptr<const SpanSheet>`). This is the change of
  2026-09-11. A sheet is written once and never edited; a rebuilt band swaps in a whole new one.
  That immutability is what lets a reader hold a `shared_ptr` and read the sheet from any thread
  for as long as it holds it — which is the whole reason the world field no longer needs a store or
  a deep copy. `removeBand` drops the sheet with the band; `sheetsHeld()` counts them.
- **The field pulls** (`SSNavMesh::collectSheets`). Main thread, one call per region: every live
  band whose column centre falls inside the region, with the agent-space corner the field needs to
  place its cells, plus a stamp mixing each band's key and geometry signature. `shared_ptr` copies
  only — no geometry is copied. The stamp is the field's change detector: an identical stamp after
  a settle means nothing was rebuilt and the published classification still describes the world.
  `SSWorldField::navSpans`, `feedWorldField` and `refeedRegion` are gone with the push model, and
  with them the "a tile created after the navmesh built the region never gets filled" problem they
  existed to work around.
- **Settle** (`SSNavMesh::regionSettled`, unchanged). No band of a column inside the region queued
  or building and no schedule pending. The field adds its own 0.5 s debounce on top, because a
  region under edit republishes bands in a trickle and `regionSettled` goes true between them.
- **Switch.** `SSNavMesh::sheetsWanted()` — the world field's master setting (`SSWorldField`)
  alone. Off, bands keep no sheet and the field classifies nothing. `SSWorldFieldFromNavMesh` is
  gone: there is no other source to switch to.

What this does not do: move any reader to Detour, or let the world field see `DYNAMIC` movers
(they ride the tile cache as obstacles and never enter a layer or a sheet, so they do not occlude,
shelter or enclose). Phantom and no-physics prims are invisible to both, by `navIgnores`.

## 23. Diagnostics: Dump selection and Mark location (2026-09-10)

Two buttons on the navmesh console's View tab, both logging under the tag `SSNavMeshDump` and
showing the verdict in the console:

- **Dump selection** (`SSWorldFieldShapes::dumpObject`). For every selected linkset: the root's
  flags as the census reads them, the navmesh role and whether it was fetched, the rest ladder rung
  and the tracked rest state, the envelope distance; then every part's physics type, hidden test,
  mesh physics state (decomposition, hulls, physics mesh, header), the part cache entry with each
  record's class, provenance, layer, triangle count and box, and how the navmesh treats that record
  (solid fill, surface raster, block, obstacle, skipped). It re-evaluates addPart's skip rules
  read-only and names the one that fired. Then `SSNavMesh::dumpAt` over the records' box: which
  columns, whether scheduled, what is queued or building, every band slot with z-range, tiles and
  age, and a nearest-point probe over the box top.
- **Mark location under the camera centre** (`SSFloaterNavMesh::onMarkLocation`). A ray from the
  camera centre finds the first object or the land. The hit is dumped from every side: the object's
  own dump, the census records meeting a 3 m box (`dumpRecordsAt`), the world field column
  (`SSWorldField::dumpColumn`: tile state and source, spans with flags, gap labels, the point's air
  verdict), the navmesh bands, and every portal edge within 4 m with its link state and endpoints
  (`dumpLinksAt`), which is what a red line needs. The overlay draws a magenta post at the mark.

The dump mirrors addPart's decisions rather than sharing code with it; when a rule changes, change
both.

## 24. The missing buildings: inverted census boxes (2026-09-10)

The first Dump selection run answered the "navmesh ignores a building" reports from sections 17
and 20. Three rotated prims (two skyscraper foundations, one solid block) were filed by the census
as box records whose world AABB had min above max on x, for example 177.9 to 144.1. The census
buckets a record by its AABB and every query walks bucket ranges from min to max, so an inverted box
lands in no bucket range any query visits: the navmesh, the segment casts and the wall profiles
all behaved as if the prim did not exist, and the navmesh followed the land underneath.

The cause was the AABB extent in `addOBB`, `addEllipsoid` and `addCylinder`: each summed the body's
axis vectors multiplied component-wise by the half-size vector, a signed sum. For any rotation that
flips an axis (a prim turned past 90 degrees about z) a world component of the sum goes negative and
min and max swap. The extent is now, per world axis, the sum of |axis component| times that axis'
half size (`ss_obb_extent`). Unrotated and lightly rotated prims were never affected, which is why
the defect looked random. The part cache is in memory only, so a restart refiles everything under
the fixed extent. The dump now prints "INVERTED AABB" on any record it finds in that state.

## 25. Mark location, first run: floating polygons, spinner churn (2026-09-10)

The owner marked a hillside where the navmesh floats over the land. The dump: the nearest navmesh
point sat 1.3 m above the terrain span the world field holds for that column; the tile's world
field serial had moved 63 times in nine minutes and its air serial was still 0; and the column's
sky band (z 355 to 374, no walkable layer) had published seven seconds earlier. Three findings.

- **Polygons are planes.** The tile cache path builds no detail mesh, so Detour reads a polygon's
  height off its plane, and a hexagon merged across a crest cuts through the hill. Recast's classic
  path fixes this with `rcBuildPolyMeshDetail` over the compact heightfield, which the tile cache
  never has. The layer it does have still holds every cell's height, so `SSNavMeshProcess::detail`
  dresses the decompressed layer as a one-span-per-cell compact heightfield and the tile's polygons
  as an `rcPolyMesh` (regions marked multiple, so each polygon's height patch seeds from its own
  centre), and runs Recast's own detail builder: samples every metre, a vertex wherever the surface
  leaves the plane by more than 0.25 m. The vendored tile cache gained one hook for this,
  `dtTileCacheMeshProcess::detail`, called between `process` and `dtCreateNavMeshData`. Detour's
  nearest-point and height queries then follow the land, and the overlay draws the detail
  triangles. It runs on the main thread inside the tile build; the console now shows the publish
  cost beside the worker cost.
- **Spinners churned everything.** The census took a part's rotation from its drawable, which
  carries the client-side spin of `llTargetOmega`. The root never moves, so the rest ladder called
  a spinning platform landscape, but its records' boxes changed every census, so its band's
  signature changed every census and rebuilt, and every rebuild fed the world field, moved the
  tile's serial and invalidated the flood in flight, forever. A part spinning under its own or an
  ancestor's angular velocity is now a mover, filed at its unspun pose so its obstacle box is
  stable.
- **Land hits looked like objects.** The camera ray hits the land as its surface-patch object; the
  mark now treats that as the land, and the dump names non-volume roots instead of judging them.

**Agent radius 0 (owner decision, same day).** `SSNavMeshAgentRadius` defaults to 0 m and the erode
pass is skipped at 0. The navmesh is the world field's replacement surface, not (yet) something an
agent walks, so nothing is shaved off walls; downstream consumers rebuild themselves on the
high-accuracy surface instead of the column store. A pathfinding consumer raises the radius when it
arrives.

**Detail mesh measured** (`V:\Scratch\navmesh\src\recastprobe_detail.cpp`, the viewer's shim verbatim
on a synthetic 8 m hill 1.5 m tall, one polygon, 400 height queries through Detour):

| | flat polygons | with detail |
|---|---|---|
| mean height error | 0.68 m | 0.10 m |
| worst | 1.50 m | 0.29 m |

The worst case sits just past the 0.25 m tolerance between two samples; the cell is 0.125 m.
Keep the probe and the viewer's `SSNavMeshProcess::detail` in step.

**Unspun pose, corrected after review.** `getRotation()` on a spinning object already includes the
accumulated client-side spin (the viewer composes sim rotation times `mAngularVelocityRot` on every
update and every tick), so reading it back was not unspun at all. The helper now divides the
accumulator out per link, which is exact, and the box of a spinner is stable across censuses.

## 26. Flat tiles as fans: the bend test moves to insertion (2026-09-10)

Flat, unobstructed tiles came out as fans of slivers along two of their four sides. Section 20's
bend test ran in the height pass, after every lattice vertex had been inserted, and relied on the
poly mesh's vertex removal to take the flat ones out. Removal declines exactly these: ear clipping
fans a straight run of collinear border vertices against one far vertex, `mergePolys` will not merge
across a collinear vertex (its convexity test wants a strict turn), and `removeVertex` refuses a
vertex whose polygons would leave two or fewer edges, which is a vertex between two fan triangles.
Which sides fanned depended on the ear order, hence two sides of every tile.

The test now runs at insertion (`portalBendsAt`, shared by `tessellatePortalEdges` and the height
pass): a lattice line gets a vertex only where the region's own height there leaves the chord
between its lattice neighbours by more than a cell, a neighbour the region does not reach counting
as level. A flat run gets no vertices at all, so the removal question never arises; a bent run gets
the same vertices on both sides of the border, as before. Also fixed on the way: the tessellation
read its segment's end vertex after moving it to open the gap.

Region 0 through the tile-cache path, 0.125 m cells: 25.0k polygons (upstream 31.1k, keep-all 54.7k,
height-pass bend test 37.9k), 98.2% of portal edges linked, 1.2 ms per tile from layer. Fewer
polygons than upstream because a flat border now carries no vertex at all, where upstream's own
removal still left the odd fan.

## 27. No floor under the land (2026-09-10)

The overlay caught walkable polygons inside a grassy hill: the sheet cut through the slope while its
flanks rode the surface. Nothing in the pipeline ever put a floor under the land on purpose, but
nothing forbade it either, and three paths could make one. A convex record buried in the land
(a foundation, a pond liner) is filled solid per column, and its top face below the surface read as
floor. A tessellated soup rasterizes every triangle, undersides included. And a band whose z-range
ends mid-hill clipped the terrain raster at the band top, leaving a walkable shelf inside the hill.

The guarantee now lives where every span has already been built and no filter can resurrect it:
`nullUnderTerrain` (worker, after `rcFilterWalkableLowHeightSpans`, before the span sheet) walks the
heightfield and nulls the area of any walkable span whose top sits under the land by more
than `SS_NAV_TERRAIN_SLOP_M` (0.5 m). The ceiling comes from the build's own terrain nodes, sampled
at the raster cell's four corners and maxed, so the terrain's own spans — rasterized at or above
that ceiling by construction — always survive; the slop only absorbs the grid's bilinear reading
against the soup's triangle chords and the 0.25 m cell quantization. Spans are left in place, only
their area nulled: the span sheet the world field classifies ignores areas, and a span flagged terrain
reaches the world floor and swallows what is below it there anyway. Land under water keeps its
lakebed floor — that is at the land, not under it. Publish warns once per band when the nulled count
changes (`SSNavMesh` tag), so a buried foundation announces itself instead of vanishing silently.
